// RFC 0009 W4b (extended by RFC 0010): offline preflop profile builder.
//
// Trains a preflop flop-terminal profile at a declared stack depth with the
// full 1,326-combo range per seat, using the EquityFrontierEvaluator (exact
// all-in-at-flop equity) as the frontier value, then exports and publishes a
// schema-v2 artifact. Heads-up (the default) keeps rules_id
// "rfc0009-unified-preflop-v1"; three-or-more-way profiles (RFC 0010) use
// "rfc0010-multiway-preflop-v1".
//
// Usage: bigshark-preflop-profile-builder <output-dir> [iterations] [stack-bb] [seats]
// [abstraction] Default: 100,000 iterations, 100 BB stack, 2 seats (heads-up), identity
// abstraction. "coarse" selects the RFC 0008 DeclaredOnly menu (no forced min-bet/jam), which keeps
// the 3+ way tree buildable at deeper stacks.
//
// This is an offline tool, NOT a CTest. The artifact is written to
// <output-dir>/preflop-profile.db with a manifest.json alongside.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::gto;
using namespace bs::artifacts;

namespace {

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
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <output-dir> [iterations] [stack-bb] [seats] [abstraction]\n"
                 "  defaults: 100000 iterations, 100 BB stack, 2 seats (heads-up), identity\n"
                 "  seats 3..10 trains an RFC 0010 multiway profile\n"
                 "  abstraction: 'identity' (default) or 'coarse' (RFC 0008 DeclaredOnly)",
                 argv[0]);
    return 1;
  }
  const std::filesystem::path out_dir = argv[1];
  const std::uint64_t iterations = argc > 2 ? std::atoll(argv[2]) : 100000;
  const int stack_bb = argc > 3 ? std::atoi(argv[3]) : 100;
  const int seats = argc > 4 ? std::atoi(argv[4]) : 2;
  if (seats < 2 || seats > 10) {
    std::fprintf(stderr, "seats must be in 2..10 (got %d)\n", seats);
    return 1;
  }
  const std::size_t player_count = static_cast<std::size_t>(seats);
  const bool coarse = argc > 5 && std::string(argv[5]) == "coarse";
  if (argc > 5 && !coarse && std::string(argv[5]) != "identity") {
    std::fprintf(stderr, "abstraction must be 'identity' or 'coarse' (got '%s')\n", argv[5]);
    return 1;
  }
  if (coarse) {
    // The nseat trainer's validate_request() accepts only the identity action
    // abstraction; the coarse DeclaredOnly menu is future work (the 3+ way
    // identity tree caps at ~16 BB under the 2M-node limit).
    std::fprintf(stderr,
                 "coarse abstraction is not yet supported by the nseat trainer "
                 "(identity gate); use 'identity'\n");
    return 1;
  }

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
  const EquityFrontierEvaluator frontier;

  std::size_t action_nodes = 0;
  for (const auto& n : tree.nodes())
    if (n.is_action())
      ++action_nodes;

  std::printf("Preflop profile: %d BB, %zu seats, %zu combos/seat, %llu iterations\n", stack_bb,
              player_count, ranges[0].size(), static_cast<unsigned long long>(iterations));
  std::printf("Tree: %zu nodes (%zu action), %zu bytes\n", tree.size(), action_nodes,
              tree.accounted_bytes());

  NSeatTrainerLimits limits;
  limits.wall = std::chrono::minutes(30);

  const auto start = std::chrono::steady_clock::now();
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, iterations, 20261003, limits, &frontier);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double wall_s = std::chrono::duration<double>(elapsed).count();

  const char* phase = trained.termination == NSeatTerminationPhase::Complete    ? "Complete"
                      : trained.termination == NSeatTerminationPhase::WallClock ? "WallClock"
                                                                                : "ResourceLimit";
  std::printf("Training: %s, %llu iterations, %zu info sets, %zu bytes, %.2f s\n", phase,
              static_cast<unsigned long long>(trained.completed_iterations),
              trained.information_sets, trained.accounted_bytes, wall_s);

  if (trained.termination == NSeatTerminationPhase::ResourceLimit) {
    std::fprintf(stderr, "training hit a resource limit; refusing to publish\n");
    return 1;
  }

  // Export to concrete seat-indexed rows (schema v2: no card_id for preflop).
  const SeatTrainingResult exported = export_seat_policy(trained, tree, ranges);
  std::printf("Export: %zu stored rows\n", exported.policy.rows().size());

  // Publish.
  const auto checkpoint = out_dir / "checkpoint.db";
  const auto policy = out_dir / "preflop-profile.db";
  std::filesystem::remove(checkpoint);
  std::filesystem::remove(policy);

  SeatCheckpointProvenance provenance;
  provenance.validation = ValidationState::Unvalidated;
  create_checkpoint(checkpoint, exported, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policy);
  const ArtifactProbe probe = probe_artifact(policy);
  std::filesystem::remove(checkpoint);

  std::printf("Published: %s (%llu bytes, SHA-256 %s)\n", published.path.string().c_str(),
              static_cast<unsigned long long>(published.file_bytes), published.sha256_hex.c_str());
  std::printf("Probe: %llu info sets, %llu actions, %llu key words\n",
              static_cast<unsigned long long>(probe.information_sets),
              static_cast<unsigned long long>(probe.action_count),
              static_cast<unsigned long long>(probe.total_key_words));

  // Heads-up keeps the RFC 0009 rules identifier so the published artifact
  // stays byte-identical; 3+ way profiles use the RFC 0010 identifier.
  const char* rules_id =
      player_count == 2 ? "rfc0009-unified-preflop-v1" : "rfc0010-multiway-preflop-v1";

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
  mf << "  \"termination\": \"" << phase << "\",\n";
  mf << "  \"tree_nodes\": " << trained.nodes << ",\n";
  mf << "  \"action_nodes\": " << action_nodes << ",\n";
  mf << "  \"information_sets\": " << trained.information_sets << ",\n";
  mf << "  \"stored_rows\": " << exported.policy.rows().size() << ",\n";
  mf << "  \"accounted_bytes\": " << trained.accounted_bytes << ",\n";
  mf << "  \"wall_seconds\": " << wall_s << ",\n";
  mf << "  \"card_abstraction\": \"preflop-169\",\n";
  mf << "  \"action_abstraction\": \"" << action_abstraction.id().to_string() << "\",\n";
  mf << "  \"frontier_evaluator\": \"exact-equity-all-in-flop\",\n";
  mf << "  \"rules_id\": \"" << rules_id << "\",\n";
  mf << "  \"schema_version\": 2\n";
  mf << "}\n";
  mf.close();
  std::printf("Manifest: %s\n", manifest.string().c_str());

  return 0;
}
