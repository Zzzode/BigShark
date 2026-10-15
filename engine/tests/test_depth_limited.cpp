// RFC 0024: depth-limited resolving unit tests.
//
// Covers the five unit-test areas from the RFC verification plan:
//   1. DepthLimit tree construction: TerminalFrontier leaves at street
//      boundaries, no Action nodes beyond the limit, root-street offset.
//   2. FrontierEvaluator generalization: EquityFrontierEvaluator on 4/5-card
//      boards (river enumeration, direct showdown settlement).
//   3. Trainer walk dispatch: a depth-limited tree trains successfully with a
//      frontier evaluator (TerminalFrontier leaves route to terminal_utility,
//      which calls the evaluator).
//   4. Trainer validation gate: a frontier-leaf tree with a null evaluator
//      throws; a full tree with a null evaluator succeeds.
//   5. SubgameResolver depth-limited path: resolve_uniform builds a
//      depth-limited tree, passes the frontier evaluator to train_nseat, and
//      returns a valid candidate.
//
// The depth-limit/decision-street constraint (max_streets=1 rejected for
// river decisions) is tested in test_subgame_resolver.cpp, where the blueprint
// fixture already exists.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/resolver.hpp>
#include <bs/subgame_resolver.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

#define CHECK_CLOSE(a, b, tol)                                                             \
  do {                                                                                     \
    if (std::fabs(static_cast<double>(a) - static_cast<double>(b)) > (tol)) {              \
      std::printf("CHECK_CLOSE failed: %s=%.12f vs %s=%.12f (%s:%d)\n", #a,                \
                  static_cast<double>(a), #b, static_cast<double>(b), __FILE__, __LINE__); \
      return 1;                                                                            \
    }                                                                                      \
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

// ---- node counting helpers ------------------------------------------------

struct NodeCounts {
  std::size_t action = 0;
  std::size_t chance = 0;
  std::size_t terminal_fold = 0;
  std::size_t terminal_showdown = 0;
  std::size_t terminal_frontier = 0;
  std::size_t flop_deal = 0;
};

NodeCounts count_nodes(const AbstractTree& tree) {
  NodeCounts c;
  for (const auto& n : tree.nodes()) {
    switch (n.kind) {
      case NodeKind::Action:
        ++c.action;
        break;
      case NodeKind::Chance:
        ++c.chance;
        break;
      case NodeKind::TerminalFold:
        ++c.terminal_fold;
        break;
      case NodeKind::TerminalShowdown:
        ++c.terminal_showdown;
        break;
      case NodeKind::TerminalFrontier:
        ++c.terminal_frontier;
        break;
      case NodeKind::FlopDeal:
        ++c.flop_deal;
        break;
    }
  }
  return c;
}

bool has_action_street(const AbstractTree& tree, Street street) {
  for (const auto& n : tree.nodes()) {
    if (n.kind == NodeKind::Action && n.street == street)
      return true;
  }
  return false;
}

// ---- GameDef fixtures ------------------------------------------------------

// 2-seat turn-rooted game, 1-chip stacks, dead {1,1}, pot 2.
// Board: 2c 3d 7h Js. Terminal=River so the state opens action.
GameDef make_turn_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 2-seat flop-rooted game, 1-chip stacks, dead {1,1}, pot 2.
// Board: 2c 3d 7h. Terminal=River.
GameDef make_flop_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 2-seat river-rooted game, 1-chip stacks, dead {1,1}, pot 2.
// Board: 2c 3d 7h Js 9c. Terminal=River.
GameDef make_river_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  def.board_size = 5;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 2-seat preflop game, 6-chip stacks, blinds 1/2, pot 3.
GameDef make_preflop_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {6, 6, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted[0] = 1;  // SB (button in heads-up)
  def.blinds_posted[1] = 2;  // BB
  def.terminal = TerminalDepth::River;
  return def;
}

// Two disjoint combos per seat, off the turn board 2c 3d 7h Js.
// Card id = rank*4+suit, suit: s=0 h=1 d=2 c=3.
std::vector<std::vector<WeightedHand>> make_test_ranges() {
  return {
      {{{card("Ah"), card("Ac")}, 1.0}},
      {{{card("Qd"), card("Qc")}, 1.0}},
  };
}

// ---- 1. DepthLimit tree construction tests ---------------------------------

// DepthLimit::streets(0) throws.
int test_depth_limit_streets_zero_throws() {
  bool threw = false;
  try {
    (void)DepthLimit::streets(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// A full tree (no depth limit) has no frontier leaves.
int test_full_tree_no_frontier_leaves() {
  const GameDef def = make_turn_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  CHECK(!tree.has_frontier_leaves());
  const auto c = count_nodes(tree);
  CHECK(c.terminal_frontier == 0);
  return 0;
}

// A turn-rooted tree with max_streets=1 has TerminalFrontier leaves at the
// river boundary and no river Action nodes.
int test_turn_rooted_depth_limit_one() {
  const GameDef def = make_turn_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity(), {},
                          DepthLimit::streets(1));
  CHECK(tree.has_frontier_leaves());
  const auto c = count_nodes(tree);
  CHECK(c.terminal_frontier > 0);
  CHECK(c.action > 0);
  // Turn action nodes exist; river action nodes do not.
  CHECK(has_action_street(tree, Street::Turn));
  CHECK(!has_action_street(tree, Street::River));
  return 0;
}

// A flop-rooted tree with max_streets=1 has TerminalFrontier leaves at the
// turn boundary and no turn or river Action nodes.
int test_flop_rooted_depth_limit_one() {
  const GameDef def = make_flop_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity(), {},
                          DepthLimit::streets(1));
  CHECK(tree.has_frontier_leaves());
  const auto c = count_nodes(tree);
  CHECK(c.terminal_frontier > 0);
  CHECK(c.action > 0);
  // Flop action nodes exist; turn and river action nodes do not.
  CHECK(has_action_street(tree, Street::Flop));
  CHECK(!has_action_street(tree, Street::Turn));
  CHECK(!has_action_street(tree, Street::River));
  return 0;
}

// A preflop-rooted game with a DepthLimit throws (Street enum ordering breaks
// the offset arithmetic).
int test_preflop_rooted_depth_limit_throws() {
  const GameDef def = make_preflop_def();
  bool threw = false;
  try {
    AbstractTree tree(def, abstraction::ActionAbstraction::identity(), {}, DepthLimit::streets(1));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// ---- 2. FrontierEvaluator generalization tests ------------------------------

// Card ids: rank*4+suit, suit: s=0 h=1 d=2 c=3.
constexpr int c2s = 0, c2h = 1, c2d = 2, c2c = 3;
constexpr int c3c = 7;
constexpr int c9c = 31;
constexpr int cKs = 44;
constexpr int cAh = 49, cAd = 50;

TerminalPayload make_ledger(std::size_t n, std::span<const bool> folded,
                            std::span<const std::int64_t> contributed) {
  TerminalPayload tp;
  tp.player_count = n;
  for (std::size_t s = 0; s < n; ++s) {
    tp.seats[s].folded = folded[s];
    tp.seats[s].contributed = contributed[s];
  }
  return tp;
}

std::vector<double> equity_from_values(std::span<const double> values,
                                       std::span<const std::int64_t> contributed) {
  double pot = 0;
  for (std::int64_t c : contributed)
    pot += static_cast<double>(c);
  std::vector<double> equity(values.size());
  for (std::size_t s = 0; s < values.size(); ++s)
    equity[s] = (values[s] + static_cast<double>(contributed[s])) / pot;
  return equity;
}

// 4-card board (turn): quads vs full house. Seat 0 always wins regardless of
// the river card. The evaluator enumerates all C(pool,1) river combos.
int test_equity_frontier_turn_board() {
  const std::array<int, 4> board = {c2c, c2d, c2h, cKs};
  const std::array<std::array<int, 2>, 2> hands = {{{c2s, c3c}, {cAh, cAd}}};
  const std::array<bool, 2> folded = {false, false};
  const std::array<std::int64_t, 2> contributed = {2, 2};
  const auto ledger = make_ledger(2, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(board, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 1.0, 1e-12);
  CHECK_CLOSE(eq[1], 0.0, 1e-12);
  // Pot = 4; seat 0 nets +2, seat 1 nets -2.
  CHECK_CLOSE(values[0], 2.0, 1e-9);
  CHECK_CLOSE(values[1], -2.0, 1e-9);
  return 0;
}

// 5-card board (river): quads vs full house. Direct showdown settlement, no
// enumeration.
int test_equity_frontier_river_board() {
  const std::array<int, 5> board = {c2c, c2d, c2h, cKs, c9c};
  const std::array<std::array<int, 2>, 2> hands = {{{c2s, c3c}, {cAh, cAd}}};
  const std::array<bool, 2> folded = {false, false};
  const std::array<std::int64_t, 2> contributed = {2, 2};
  const auto ledger = make_ledger(2, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(board, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 1.0, 1e-12);
  CHECK_CLOSE(eq[1], 0.0, 1e-12);
  CHECK_CLOSE(values[0], 2.0, 1e-9);
  CHECK_CLOSE(values[1], -2.0, 1e-9);
  return 0;
}

// ---- 3. Trainer walk dispatch tests -----------------------------------------

// A depth-limited tree trains successfully with a frontier evaluator. This
// exercises the walk dispatch: TerminalFrontier leaves route to
// terminal_utility, which calls ctx.frontier->evaluate.
int test_depth_limited_tree_trains_with_frontier() {
  const GameDef def = make_turn_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity(), {},
                          DepthLimit::streets(1));
  CHECK(tree.has_frontier_leaves());
  const auto ranges = make_test_ranges();
  const EquityFrontierEvaluator frontier;
  const NSeatTrainerLimits limits{};
  const NSeatTrainingResult result = train_nseat(tree, ranges, 50, 4242, limits, &frontier);
  CHECK(result.termination == NSeatTerminationPhase::Complete);
  CHECK(result.information_sets > 0);
  return 0;
}

// ---- 4. Trainer validation gate tests ----------------------------------------

// A depth-limited tree with a null frontier evaluator throws.
int test_depth_limited_tree_throws_without_frontier() {
  const GameDef def = make_turn_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity(), {},
                          DepthLimit::streets(1));
  CHECK(tree.has_frontier_leaves());
  const auto ranges = make_test_ranges();
  const NSeatTrainerLimits limits{};
  bool threw = false;
  try {
    (void)train_nseat(tree, ranges, 10, 4242, limits, nullptr);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// A full tree (no frontier leaves) with a null frontier evaluator succeeds.
// This verifies the validation gate extension does not regress existing
// behavior: only trees with frontier leaves (or flop-terminal games) require
// a frontier evaluator.
int test_full_tree_trains_without_frontier() {
  const GameDef def = make_river_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  CHECK(!tree.has_frontier_leaves());
  const auto ranges = make_test_ranges();
  const NSeatTrainerLimits limits{};
  // Should not throw.
  const NSeatTrainingResult result = train_nseat(tree, ranges, 10, 4242, limits, nullptr);
  CHECK(result.termination == NSeatTerminationPhase::Complete);
  return 0;
}

// ---- 5. SubgameResolver depth-limited path tests -----------------------------

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

// resolve_uniform with a turn decision and max_streets=1 produces a valid
// candidate. The resolver builds a depth-limited tree, passes the frontier
// evaluator to train_nseat, and returns a legal action.
int test_resolver_depth_limited_turn() {
  const GameState state = make_turn_state(2);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const EquityFrontierEvaluator frontier;
  resolver.set_frontier_evaluator(&frontier);

  ResolveLimits limits;
  limits.iterations = 100;
  limits.time = std::chrono::seconds(30);
  limits.max_nodes = 5000000;
  limits.max_information_sets = 1000000;
  limits.public_seed = 0x5151515151515151ULL;
  limits.depth_limit = DepthLimit::streets(1);

  const auto result = resolver.resolve_uniform(state, no_history, limits);
  if (result.status != SubgameResolveStatus::Solved)
    std::fprintf(stderr, "depth-limited turn: status=%s\n", bs::resolver::to_string(result.status));
  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  if (check_valid_candidate(result) != 0)
    return 1;
  return 0;
}

}  // namespace

int main() {
  // 1. DepthLimit tree construction
  if (test_depth_limit_streets_zero_throws() != 0)
    return 1;
  if (test_full_tree_no_frontier_leaves() != 0)
    return 1;
  if (test_turn_rooted_depth_limit_one() != 0)
    return 1;
  if (test_flop_rooted_depth_limit_one() != 0)
    return 1;
  if (test_preflop_rooted_depth_limit_throws() != 0)
    return 1;
  // 2. FrontierEvaluator generalization
  if (test_equity_frontier_turn_board() != 0)
    return 1;
  if (test_equity_frontier_river_board() != 0)
    return 1;
  // 3. Trainer walk dispatch
  if (test_depth_limited_tree_trains_with_frontier() != 0)
    return 1;
  // 4. Trainer validation gate
  if (test_depth_limited_tree_throws_without_frontier() != 0)
    return 1;
  if (test_full_tree_trains_without_frontier() != 0)
    return 1;
  // 5. SubgameResolver depth-limited path
  if (test_resolver_depth_limited_turn() != 0)
    return 1;
  std::printf("test_depth_limited PASS\n");
  return 0;
}
