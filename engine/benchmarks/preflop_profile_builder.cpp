// RFC 0009 W4b: offline preflop profile builder.
//
// Trains a heads-up preflop flop-terminal profile at a declared stack depth
// with the full 1,326-combo range per seat, using the EquityFrontierEvaluator
// (exact all-in-at-flop equity) as the frontier value, then exports and
// publishes a schema-v2 artifact with rules_id "rfc0009-unified-preflop-v1".
//
// Usage: bigshark-preflop-profile-builder <output-dir> [iterations] [stack-bb]
// Default: 100,000 iterations, 100 BB stack.
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
#include <vector>

using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::gto;
using namespace bs::artifacts;

namespace {

GameDef make_preflop_def(int stack_bb) {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  const Chips stack = static_cast<Chips>(stack_bb * 2);  // BB = 2 chips
  def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::Flop;
  return def;
}

std::vector<std::vector<WeightedHand>> all_combos() {
  std::vector<std::vector<WeightedHand>> ranges(2);
  ranges[0].reserve(1326);
  ranges[1].reserve(1326);
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      ranges[0].push_back({{c0, c1}, 1.0});
      ranges[1].push_back({{c0, c1}, 1.0});
    }
  }
  return ranges;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <output-dir> [iterations] [stack-bb]\n"
                 "  defaults: 100000 iterations, 100 BB stack\n",
                 argv[0]);
    return 1;
  }
  const std::filesystem::path out_dir = argv[1];
  const std::uint64_t iterations = argc > 2 ? std::atoll(argv[2]) : 100000;
  const int stack_bb = argc > 3 ? std::atoi(argv[3]) : 100;

  std::filesystem::create_directories(out_dir);

  const GameDef def = make_preflop_def(stack_bb);
  const auto ranges = all_combos();
  const AbstractTree tree(def, ActionAbstraction::identity());
  const EquityFrontierEvaluator frontier;

  std::size_t action_nodes = 0;
  for (const auto& n : tree.nodes())
    if (n.is_action())
      ++action_nodes;

  std::printf("Preflop profile: %d BB, %zu combos/seat, %llu iterations\n", stack_bb,
              ranges[0].size(), static_cast<unsigned long long>(iterations));
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

  // Write manifest.
  const auto manifest = out_dir / "manifest.json";
  std::ofstream mf(manifest);
  mf << "{\n";
  mf << "  \"artifact\": \"preflop-profile.db\",\n";
  mf << "  \"sha256\": \"" << published.sha256_hex << "\",\n";
  mf << "  \"file_bytes\": " << published.file_bytes << ",\n";
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
  mf << "  \"frontier_evaluator\": \"exact-equity-all-in-flop\",\n";
  mf << "  \"rules_id\": \"rfc0009-unified-preflop-v1\",\n";
  mf << "  \"schema_version\": 2\n";
  mf << "}\n";
  mf.close();
  std::printf("Manifest: %s\n", manifest.string().c_str());

  return 0;
}
