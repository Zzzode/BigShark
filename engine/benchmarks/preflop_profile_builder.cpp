// RFC 0009 W4b (extended by RFC 0010, RFC 0013): offline preflop profile builder.
//
// Trains a preflop flop-terminal profile at a declared stack depth with the
// full 1,326-combo range per seat. By default the EquityFrontierEvaluator
// (exact all-in-at-flop equity) is the frontier value. When a declared
// postflop table (.fdt, RFC 0013) is supplied and the stack is deeper than
// 15 BB, the PostflopFrontierEvaluator replaces it, using CFR-solved
// flop-rooted river-terminal values from the table. At <= 15 BB the equity
// frontier is retained (jam/fold is near-optimal at short stacks).
//
// Heads-up (the default) keeps rules_id "rfc0009-unified-preflop-v1";
// three-or-more-way profiles (RFC 0010) use "rfc0010-multiway-preflop-v1".
//
// Usage: bigshark-preflop-profile-builder <output-dir> [iterations] [stack-bb]
// [seats] [abstraction] [frontier-table-path]
// Default: 100,000 iterations, 100 BB stack, 2 seats (heads-up), identity
// abstraction, equity frontier. "coarse" selects the RFC 0008 DeclaredOnly
// menu (no forced min-bet/jam), which keeps the 3+ way tree buildable at
// deeper stacks. frontier-table-path: path to a .fdt declared postflop
// table (RFC 0013); default is the equity frontier.
//
// This is an offline tool, NOT a CTest. The artifact is written to
// <output-dir>/preflop-profile.db with a manifest.json alongside.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::gto;
using namespace bs::artifacts;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

// Process peak RSS, not the trainer's portable accounted-byte budget.
std::int64_t peak_rss_bytes() {
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return -1;
#if defined(__APPLE__)
  return static_cast<std::int64_t>(usage.ru_maxrss);
#else
  return static_cast<std::int64_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return -1;
#endif
}

GameDef make_preflop_def(int stack_bb, std::size_t player_count) {
  GameDef def{};
  def.player_count = player_count;
  def.button = 0;
  def.big_blind = 2;
  const Chips stack = static_cast<Chips>(stack_bb * 2);  // BB = 2 chips
  def.stacks = {stack, stack, stack, stack, stack, stack, stack, stack, stack, stack};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  // Blinds follow the same convention as GameDef validation and the GameState
  // constructor: heads-up the button posts the SB, 3+ the blinds sit clockwise
  // of the button (RFC 0010 exports the helpers as the single source of truth).
  def.blinds_posted[small_blind_seat(def)] = 1;
  def.blinds_posted[big_blind_seat(def)] = 2;
  def.terminal = TerminalDepth::Flop;
  return def;
}

std::vector<std::vector<WeightedHand>> all_combos(std::size_t player_count) {
  std::vector<std::vector<WeightedHand>> ranges(player_count);
  for (auto& range : ranges)
    range.reserve(1326);
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      for (auto& range : ranges)
        range.push_back({{c0, c1}, 1.0});
    }
  }
  return ranges;
}

}  // namespace

int main(int argc, char** argv) {
  // Parse optional flags first, then positional arguments.
  std::size_t num_threads = 1;
  std::size_t cache_capacity = 128;
  PostflopCacheLimits cache_limits;
  std::vector<char*> positional;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--threads" && i + 1 < argc) {
      num_threads = static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (arg == "--cache-capacity" && i + 1 < argc) {
      cache_capacity = static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (arg == "--cache-bytes" && i + 1 < argc) {
      const std::string value = argv[++i];
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), cache_limits.max_policy_bytes);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          cache_limits.max_policy_bytes < cache_limits.max_load_bytes) {
        std::fprintf(stderr, "--cache-bytes requires an integer of at least %zu bytes\n",
                     cache_limits.max_load_bytes);
        return 1;
      }
    } else {
      positional.push_back(argv[i]);
    }
  }
  const int pargc = static_cast<int>(positional.size()) + 1;
  std::vector<char*> pargv;
  pargv.push_back(argv[0]);
  for (char* p : positional)
    pargv.push_back(p);

  if (pargc < 2) {
    std::fprintf(stderr,
                 "usage: %s <output-dir> [iterations] [stack-bb] [seats] [abstraction]\n"
                 "       [frontier-table-path] [policy-dir] [--threads N] [--cache-capacity N]\n"
                 "       [--cache-bytes N]\n"
                 "  defaults: 100000 iterations, 100 BB stack, 2 seats (heads-up), identity\n"
                 "  seats 3..10 trains an RFC 0010 multiway profile\n"
                 "  abstraction: 'identity' (default) or 'coarse' (RFC 0008 DeclaredOnly)\n"
                 "  frontier-table-path: .fdt declared postflop table (RFC 0013);\n"
                 "    default is the equity frontier (all-in-at-flop)\n"
                 "  policy-dir: directory with .nsp policy files for on-the-fly\n"
                 "    evaluation of missing table entries (RFC 0013 Stage 2)\n"
                 "  --threads N: parallel CFR with N threads (default 1)\n"
                 "  --cache-capacity N: retained policy entry cap (default 128)\n"
                 "  --cache-bytes N: policy/load accounted-byte cap (default 1073741824)",
                 argv[0]);
    return 1;
  }
  const std::filesystem::path out_dir = pargv[1];
  const std::uint64_t iterations = pargc > 2 ? std::atoll(pargv[2]) : 100000;
  const int stack_bb = pargc > 3 ? std::atoi(pargv[3]) : 100;
  const int seats = pargc > 4 ? std::atoi(pargv[4]) : 2;
  if (seats < 2 || seats > 10) {
    std::fprintf(stderr, "seats must be in 2..10 (got %d)\n", seats);
    return 1;
  }
  const std::size_t player_count = static_cast<std::size_t>(seats);
  const bool coarse = pargc > 5 && std::string(pargv[5]) == "coarse";
  if (pargc > 5 && !coarse && std::string(pargv[5]) != "identity") {
    std::fprintf(stderr, "abstraction must be 'identity' or 'coarse' (got '%s')\n", pargv[5]);
    return 1;
  }
  const std::string frontier_table_path = pargc > 6 ? pargv[6] : "";
  const std::string policy_dir = pargc > 7 ? pargv[7] : "";
  // The equity frontier is retained for <= 15 BB (jam/fold is near-optimal at
  // short stacks) and when no table is supplied (backward-compatible default).
  const bool use_postflop = !frontier_table_path.empty() && stack_bb > 15;

  const auto pipeline_started = Clock::now();
  std::filesystem::create_directories(out_dir);

  const GameDef def = make_preflop_def(stack_bb, player_count);
  const auto ranges = all_combos(player_count);
  // The identity abstraction seeds the legal minimum and all-in cap at every
  // aggressive node (MinAndCap); the 3+ way action tree then exceeds the node
  // cap past ~16 BB. The coarse DeclaredOnly menu (RFC 0008) drops those forced
  // seeds so deeper multiway profiles stay buildable.
  const ActionAbstraction action_abstraction =
      coarse ? ActionAbstraction::declared(default_size_schedule(), CoverSeeds::DeclaredOnly)
             : ActionAbstraction::identity();
  const AbstractTree tree(def, action_abstraction);

  // Frontier selector: equity by default, postflop-declared when a table is
  // supplied at > 15 BB. The table and evaluator are heap-owned (large,
  // non-copyable); the pointer selects which evaluator the trainer uses.
  EquityFrontierEvaluator equity_frontier;
  std::unique_ptr<NSeatDeclaredFrontierTable> declared_table;
  std::unique_ptr<PostflopFrontierEvaluator> postflop_evaluator;
  std::unique_ptr<OnTheFlyPostflopEvaluator> onthefly_evaluator;
  std::string table_identity;
  const FrontierEvaluator* frontier = &equity_frontier;
  const char* frontier_tag = "exact-equity-all-in-flop";
  if (use_postflop) {
    declared_table = std::make_unique<NSeatDeclaredFrontierTable>(
        NSeatDeclaredFrontierTable::load(frontier_table_path, &table_identity));
    if (!policy_dir.empty()) {
      onthefly_evaluator = std::make_unique<OnTheFlyPostflopEvaluator>(
          *declared_table, std::filesystem::path(policy_dir), cache_capacity, cache_limits);
      frontier = onthefly_evaluator.get();
      frontier_tag = "postflop-onthefly-v1";
    } else {
      postflop_evaluator = std::make_unique<PostflopFrontierEvaluator>(*declared_table);
      frontier = postflop_evaluator.get();
      frontier_tag = "postflop-declared-table-v1";
    }
  }

  std::size_t action_nodes = 0;
  for (const auto& n : tree.nodes())
    if (n.is_action())
      ++action_nodes;

  std::printf("Preflop profile: %d BB, %zu seats, %zu combos/seat, %llu iterations\n", stack_bb,
              player_count, ranges[0].size(), static_cast<unsigned long long>(iterations));
  std::printf("Tree: %zu nodes (%zu action), %zu bytes\n", tree.size(), action_nodes,
              tree.accounted_bytes());
  if (num_threads > 1)
    std::printf("Parallel: %zu threads\n", num_threads);
  if (use_postflop) {
    std::printf("Frontier: %s (%zu entries, identity: %s)\n", frontier_tag, declared_table->size(),
                table_identity.c_str());
  } else {
    std::printf("Frontier: %s\n", frontier_tag);
  }

  NSeatTrainerLimits limits;
  // The existing twelve-hour safety envelope does not certify throughput or
  // strategy quality. Complete means only that all requested samples finished.
  limits.wall = std::chrono::hours(12);

  const auto start = std::chrono::steady_clock::now();
  const NSeatTrainingResult trained =
      num_threads > 1
          ? train_nseat_parallel(tree, ranges, iterations, 20261003, limits, frontier, num_threads)
          : train_nseat(tree, ranges, iterations, 20261003, limits, frontier);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double wall_s = std::chrono::duration<double>(elapsed).count();

  const char* phase = trained.termination == NSeatTerminationPhase::Complete    ? "Complete"
                      : trained.termination == NSeatTerminationPhase::WallClock ? "WallClock"
                                                                                : "ResourceLimit";
  std::printf(
      "Training: %s, %llu samples, %llu epochs, batch=%zu, revision=%u, "
      "%zu info sets, %zu bytes, %.2f s\n",
      phase, static_cast<unsigned long long>(trained.completed_iterations),
      static_cast<unsigned long long>(trained.completed_epochs), trained.batch_size,
      trained.algorithm_revision, trained.information_sets, trained.accounted_bytes, wall_s);
  if (onthefly_evaluator) {
    const auto stats = onthefly_evaluator->statistics();
    std::printf(
        "Frontier cache: hits=%zu misses=%zu table_hits=%zu computed=%zu "
        "equity=%zu policy_loads=%zu templates=%zu\n",
        onthefly_evaluator->cache_hits(), onthefly_evaluator->cache_misses(),
        onthefly_evaluator->table_hits(), onthefly_evaluator->computed(),
        onthefly_evaluator->equity_fallbacks(), stats.policy_loads, stats.template_loads);
    std::printf("Frontier accounting: policy=%zu peak=%zu cap=%zu templates=%zu\n",
                stats.policy_bytes, stats.peak_policy_bytes, cache_limits.max_policy_bytes,
                stats.template_bytes);
    std::printf(
        "Frontier cumulative timing: policy_load=%.2f ms template_load=%.2f ms "
        "extract=%.2f ms\n",
        stats.policy_load_ms, stats.template_load_ms, stats.extract_ms);
  }

  if (trained.termination == NSeatTerminationPhase::ResourceLimit) {
    std::fprintf(stderr, "training hit a resource limit; refusing to publish\n");
    return 1;
  }

  // Training has joined its workers and all evaluation statistics are recorded.
  // Release frontier caches before export and publication retain policy copies.
  onthefly_evaluator.reset();
  postflop_evaluator.reset();
  declared_table.reset();

  // Export to concrete seat-indexed rows (schema v2: no card_id for preflop).
  const auto export_started = Clock::now();
  const SeatTrainingResult exported = export_seat_policy(trained, tree, ranges);
  const double export_s = seconds_since(export_started);
  std::printf("Export: %zu stored rows\n", exported.policy.rows().size());

  // Publish.
  const auto checkpoint = out_dir / "checkpoint.db";
  const auto policy = out_dir / "preflop-profile.db";
  std::filesystem::remove(checkpoint);
  std::filesystem::remove(policy);

  SeatCheckpointProvenance provenance;
  provenance.validation = ValidationState::Unvalidated;
  const auto checkpoint_started = Clock::now();
  create_checkpoint(checkpoint, exported, provenance);
  const double checkpoint_s = seconds_since(checkpoint_started);
  const auto publication_started = Clock::now();
  const PublishedPolicy published = publish_policy(checkpoint, policy);
  const double publication_s = seconds_since(publication_started);
  const auto probe_started = Clock::now();
  const ArtifactProbe probe = probe_artifact(policy);
  const double probe_s = seconds_since(probe_started);
  std::filesystem::remove(checkpoint);

  std::printf("Algorithm: %s\n", probe.manifest.algorithm_revision.c_str());

  std::printf("Published: %s (%llu bytes, SHA-256 %s)\n", published.path.string().c_str(),
              static_cast<unsigned long long>(published.file_bytes), published.sha256_hex.c_str());
  std::printf("Probe: %llu info sets, %llu actions, %llu key words\n",
              static_cast<unsigned long long>(probe.information_sets),
              static_cast<unsigned long long>(probe.action_count),
              static_cast<unsigned long long>(probe.total_key_words));

  // Rules identifiers describe the game, separately from training identity.
  const char* rules_id =
      player_count == 2 ? "rfc0009-unified-preflop-v1" : "rfc0010-multiway-preflop-v1";

  // The measured pipeline ends before writing its diagnostic JSON manifest.
  const double pipeline_s = seconds_since(pipeline_started);
  const auto peak_rss = peak_rss_bytes();
  std::printf(
      "Pipeline: export=%.3f s checkpoint=%.3f s publication=%.3f s "
      "probe=%.3f s total=%.3f s peak_rss_bytes=%lld\n",
      export_s, checkpoint_s, publication_s, probe_s, pipeline_s, static_cast<long long>(peak_rss));

  // Write manifest.
  const auto manifest = out_dir / "manifest.json";
  std::ofstream mf(manifest);
  mf << "{\n";
  mf << "  \"artifact\": \"preflop-profile.db\",\n";
  mf << "  \"sha256\": \"" << published.sha256_hex << "\",\n";
  mf << "  \"file_bytes\": " << published.file_bytes << ",\n";
  mf << "  \"player_count\": " << player_count << ",\n";
  mf << "  \"stack_bb\": " << stack_bb << ",\n";
  mf << "  \"combos_per_seat\": " << ranges[0].size() << ",\n";
  mf << "  \"iterations_requested\": " << iterations << ",\n";
  mf << "  \"iterations_completed\": " << trained.completed_iterations << ",\n";
  mf << "  \"completed_samples\": " << trained.completed_iterations << ",\n";
  mf << "  \"completed_epochs\": " << trained.completed_epochs << ",\n";
  mf << "  \"batch_size\": " << trained.batch_size << ",\n";
  mf << "  \"algorithm_revision\": \"" << probe.manifest.algorithm_revision << "\",\n";
  mf << "  \"termination\": \"" << phase << "\",\n";
  mf << "  \"tree_nodes\": " << trained.nodes << ",\n";
  mf << "  \"action_nodes\": " << action_nodes << ",\n";
  mf << "  \"information_sets\": " << trained.information_sets << ",\n";
  mf << "  \"stored_rows\": " << exported.policy.rows().size() << ",\n";
  mf << "  \"accounted_bytes\": " << trained.accounted_bytes << ",\n";
  mf << "  \"wall_seconds\": " << wall_s << ",\n";
  mf << "  \"export_seconds\": " << export_s << ",\n";
  mf << "  \"checkpoint_seconds\": " << checkpoint_s << ",\n";
  mf << "  \"publication_seconds\": " << publication_s << ",\n";
  mf << "  \"probe_seconds\": " << probe_s << ",\n";
  mf << "  \"pipeline_seconds\": " << pipeline_s << ",\n";
  mf << "  \"peak_rss_bytes\": " << peak_rss << ",\n";
  mf << "  \"card_abstraction\": \"preflop-169\",\n";
  mf << "  \"action_abstraction\": \"" << action_abstraction.id().to_string() << "\",\n";
  mf << "  \"frontier_evaluator\": \"" << frontier_tag << "\",\n";
  if (num_threads > 1)
    mf << "  \"parallel_threads\": " << num_threads << ",\n";
  if (use_postflop) {
    mf << "  \"frontier_table_path\": \"" << frontier_table_path << "\",\n";
    mf << "  \"frontier_table_sha256\": \"" << sha256_file_hex(frontier_table_path) << "\",\n";
    mf << "  \"frontier_table_identity\": \"" << table_identity << "\",\n";
  }
  mf << "  \"rules_id\": \"" << rules_id << "\",\n";
  mf << "  \"schema_version\": 2\n";
  mf << "}\n";
  mf.close();
  std::printf("Manifest: %s\n", manifest.string().c_str());

  return 0;
}
