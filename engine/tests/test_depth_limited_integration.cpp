// RFC 0024: depth-limited resolving integration tests.
//
// Covers the integration-test areas from the RFC verification plan:
//   1. 3-seat depth-limited subgame: verify the tree is significantly
//      smaller than the full tree and the resolver produces a legal action.
//   2. Full-tree vs depth-limited comparison: solve the same subgame with
//      the full tree (exact) and with depth-limited resolving +
//      EquityFrontierEvaluator (approximate), and compare the results.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/resolver.hpp>
#include <bs/subgame_resolver.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using namespace bs;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::resolver;
using bs::gto::EquityFrontierEvaluator;

int card(const char* name) {
  return cardId(std::string(name));
}

// Build a street-start GameState on the turn (board_size=4) for N seats.
GameState make_turn_state(std::size_t seats, Chips stack = 10) {
  GameDef def{};
  def.player_count = seats;
  def.button = seats == 2 ? 0 : seats - 1;
  def.big_blind = 2;
  for (std::size_t s = 0; s < seats; ++s) {
    def.stacks[s] = stack;
    def.contributions[s] = 1;
  }
  def.pot = static_cast<Chips>(seats);
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return GameState(def);
}

// Build the GameDef for a 3-seat turn-rooted game.
GameDef make_3seat_turn_def() {
  GameDef def{};
  def.player_count = 3;
  def.button = 2;
  def.big_blind = 2;
  def.stacks = {10, 10, 10, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// Verify that the candidate is non-empty and every row is a valid probability
// distribution over the node's ordered abstract actions.
int check_valid_candidate(const SubgameResolveResult& result) {
  CHECK(result.completed_iterations > 0);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());
  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions == result.node_actions);
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(std::abs(sum - 1.0) < 1e-9);
  }
  return 0;
}

// ---- 1. 3-seat depth-limited subgame --------------------------------------

// A 3-seat turn-rooted subgame with max_streets=1 produces a significantly
// smaller tree than the full tree, and the resolver produces a valid candidate.
int test_3seat_depth_limited_subgame() {
  // Build the full tree and the depth-limited tree.
  const GameDef def = make_3seat_turn_def();
  TreeLimits tree_limits;
  tree_limits.max_nodes = 10'000'000;

  const AbstractTree full_tree(def, abstraction::ActionAbstraction::identity(), tree_limits);
  const AbstractTree dl_tree(def, abstraction::ActionAbstraction::identity(), tree_limits,
                             DepthLimit::streets(1));

  const std::size_t full_nodes = full_tree.nodes().size();
  const std::size_t dl_nodes = dl_tree.nodes().size();

  std::printf("3-seat: full tree=%zu nodes, depth-limited=%zu nodes (%.1fx reduction)\n",
              full_nodes, dl_nodes, static_cast<double>(full_nodes) / dl_nodes);

  // The depth-limited tree should be significantly smaller.
  CHECK(dl_nodes < full_nodes);
  CHECK(dl_nodes < full_nodes / 2);  // At least 50% reduction

  // Solve the depth-limited tree.
  const GameState state = make_turn_state(3);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const EquityFrontierEvaluator frontier;
  resolver.set_frontier_evaluator(&frontier);

  ResolveLimits rlimits;
  rlimits.iterations = 50;
  rlimits.time = std::chrono::seconds(30);
  rlimits.max_nodes = 5000000;
  rlimits.max_information_sets = 1000000;
  rlimits.public_seed = 0x5151515151515151ULL;
  rlimits.depth_limit = DepthLimit::streets(1);

  const auto result = resolver.resolve_uniform(state, no_history, rlimits);
  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  if (check_valid_candidate(result) != 0)
    return 1;

  return 0;
}

// ---- 2. Full-tree vs depth-limited comparison ------------------------------

// Solve the same 2-seat turn-rooted subgame with the full tree (exact) and
// with depth-limited resolving (approximate). Both should produce valid
// candidates. The depth-limited solve should use fewer nodes and information
// sets.
int test_full_vs_depth_limited_comparison() {
  const GameState state = make_turn_state(2);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  const EquityFrontierEvaluator frontier;

  // Solve with the full tree (no depth limit, no frontier evaluator).
  SubgameResolver full_resolver;
  ResolveLimits full_limits;
  full_limits.iterations = 50;
  full_limits.time = std::chrono::seconds(30);
  full_limits.max_nodes = 5000000;
  full_limits.max_information_sets = 1000000;
  full_limits.public_seed = 0x5151515151515151ULL;

  const auto full_result = full_resolver.resolve_uniform(state, no_history, full_limits);
  CHECK(full_result.status == SubgameResolveStatus::Solved);
  if (check_valid_candidate(full_result) != 0)
    return 1;

  // Solve with depth-limited resolving (max_streets=1, frontier evaluator).
  SubgameResolver dl_resolver;
  dl_resolver.set_frontier_evaluator(&frontier);
  ResolveLimits dl_limits = full_limits;
  dl_limits.depth_limit = DepthLimit::streets(1);

  const auto dl_result = dl_resolver.resolve_uniform(state, no_history, dl_limits);
  CHECK(dl_result.status == SubgameResolveStatus::Solved);
  if (check_valid_candidate(dl_result) != 0)
    return 1;

  std::printf(
      "2-seat comparison: full=%zu nodes/%zu infosets, depth-limited=%zu nodes/%zu infosets\n",
      full_result.nodes, full_result.information_sets, dl_result.nodes, dl_result.information_sets);

  // The depth-limited solve should use fewer nodes and information sets.
  CHECK(dl_result.nodes < full_result.nodes);
  CHECK(dl_result.information_sets < full_result.information_sets);

  // Both should have the same node_actions (same action menu at the root).
  CHECK(full_result.node_actions == dl_result.node_actions);

  return 0;
}

}  // namespace

int main() {
  if (test_3seat_depth_limited_subgame() != 0)
    return 1;
  if (test_full_vs_depth_limited_comparison() != 0)
    return 1;
  std::printf("test_depth_limited_integration PASS\n");
  return 0;
}
