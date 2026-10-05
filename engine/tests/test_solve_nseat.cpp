// RFC 0009 W2a L4 conformance for the seat-parameterized MCCFR trainer.
//
// Five gates:
//  1. Two-seat parity: on one tiny 2-seat identity tree the NSeatCfr route and
//     the HeadsUpCfr route converge to the same river subgame. The n-seat
//     trainer is external-sampling only, so its policy has sparse coverage
//     over the 49*48 runout lanes of a flop-rooted game and cannot be exactly
//     evaluated there; the gate therefore roots the n-seat game at the river
//     (board_size 5), where the tree has no chance nodes and external sampling
//     covers every action node. The heads-up reference is trained on the
//     equivalent flop+fixed-runout game by FULL traversal (complete,
//     deterministic coverage), and its river rows -- conditioned on the unique
//     flop/turn check-down history that reaches this river with pot 2 and 1/1
//     stacks -- are rekeyed into the (bucket, node) key space. A
//     full-traversal evaluator over the river tree then scores BOTH policies
//     on the same subgame and compares their profile values and NashConv
//     within a declared tolerance.
//  2. Three-seat update-rule oracle: ONE visible sweep (the debug seam) is
//     compared, row by row and BITWISE (regrets, sums, visits, menus), against
//     an independently coded enumeration of the same documented update rule --
//     the oracle derives its own joint deal, chance ordinal, regret matching,
//     kFull average and RM+ update from the caller-owned streams and never
//     calls the trainer's walk.
//  3. Determinism: the same request reproduces identical rows and PRNG state;
//     a different seed changes the sampled trajectory.
//  4. Refusals: the typed family of every unsupported shape, exactly as the
//     stage-4 route already reports it.
//  5. Zero-sum terminals: fold leaves and showdown leaves settle exactly
//     (asserted inside the production walk and inside the oracle walk, so any
//     violation throws out of these tests).
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/solve.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
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

namespace {

using namespace bs;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;

int card(const char* name) {
  return cardId(std::string(name));
}

// --- shared fixtures ---------------------------------------------------------

// The golden heads-up family (test_solve_conformance.cpp:65): top overpairs,
// rooted flop 2c3d7h, 1/1 stacks, dead contributions {1,1}. The 1-chip stacks
// keep the materialized tree small (31,124 nodes) and leave every later street
// a runout, so the exact evaluator can score both routes' sealed policies.
GameDef heads_up_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

std::vector<WeightedHand> heads_up_range0() {
  return {{{card("Ah"), card("Ac")}, 2}, {{card("8d"), card("8c")}, 3}};
}

std::vector<WeightedHand> heads_up_range1() {
  return {{{card("As"), card("Ad")}, 5}, {{card("Qd"), card("Qc")}, 7}};
}

// The parity gate's ranges. On the river board 2c3d7hKs9c each seat's two
// holdings fall in DISTINCT CategoryTiersV1 buckets (one pair + two pair), so
// the n-seat trainer's (bucket, node) key holds one row per exact hand: the
// bucket-pooled game it solves is exactly the heads-up exact-hand game, and
// the rekey's emplace-keep-first never aliases two holdings onto one row.
// Both holdings per seat are strong made hands so the two routes converge
// cleanly (a weak high-card holding opens borderline folding spots where the
// two routes' partial-convergence exploitability estimates diverge). Every
// holding is off the board and mutually card-distinct, so all four joint
// deals are card-compatible (the sampler's rejection loop and the evaluator's
// enumeration both see the full 2x2 grid).
std::vector<WeightedHand> river_range0() {
  return {{{card("Ah"), card("Ac")}, 2}, {{card("9d"), card("Kh")}, 3}};
}

std::vector<WeightedHand> river_range1() {
  return {{{card("As"), card("Ad")}, 5}, {{card("9h"), card("Kc")}, 7}};
}

// The tiny 3-seat fixture (the same shape test_abstract_tree_fidelity builds
// and counts at 102,827 nodes): rooted flop, 1/1/1 stacks, dead {1,1,1}.
GameDef three_seat_def() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

// Two disjoint combos per seat with hole cards off the rooted board, so every
// joint combination is card-compatible (the sampler still runs its rejection
// loop, and the oracle independently reproduces it).
std::vector<std::vector<WeightedHand>> three_seat_ranges() {
  return {
      {{{card("Qh"), card("Kh")}, 2}, {{card("Ts"), card("Js")}, 3}},
      {{{card("Qd"), card("Kd")}, 5}, {{card("Th"), card("Jh")}, 7}},
      {{{card("Qs"), card("Ks")}, 11}, {{card("Td"), card("Jd")}, 13}},
  };
}

// The river-rooted parity fixture: the same 2-seat family, but the board
// carries five cards so the n-seat external-sampling policy covers every
// action node (no runout chance to leave sparse). The heads-up reference is
// the flop+fixed-runout game below; its river subgame, conditioned on a
// flop/turn check-down (the only history that reaches the river with pot 2
// and 1/1 stacks), is the SAME game the n-seat policy solves directly.
GameDef river_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  def.board_size = 5;
  return def;
}

HeadsUpGame river_game() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = river_range0();
  game.ranges[1] = river_range1();
  game.fixed_runout = {card("Ks"), card("9c")};
  return game;
}

SolveRequest base_request(const AbstractTree& tree) {
  SolveRequest req;
  req.tree = &tree;
  req.mode = SolveMode::ExternalSampling;
  req.limits.time = std::chrono::milliseconds(600'000);
  return req;
}

// --- the independently coded update-rule oracle ------------------------------
//
// Written from the documented conventions, in this test, without calling the
// trainer's walk. It re-derives every random draw from the caller-owned
// streams in the pinned order, so it selects exactly the trajectory the
// production walk should select, and then reproduces its arithmetic.

struct OracleRow {
  std::vector<Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
  std::uint64_t visits = 0;
};
using OracleTable = std::map<NSeatInformationKey, OracleRow>;

double oracle_unit_draw(SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::size_t oracle_bounded(SplitMix64& rng, std::size_t bound) {
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  for (;;) {
    const std::uint64_t value = rng.next_u64();
    if (value >= threshold)
      return static_cast<std::size_t>(value % limit);
  }
}

std::size_t oracle_weighted(SplitMix64& rng, const std::vector<double>& weights) {
  const double point = oracle_unit_draw(rng);
  double cumulative = 0.0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < weights.size(); ++i) {
    if (weights[i] <= 0.0)
      continue;
    cumulative += weights[i];
    last = i;
    if (point < cumulative)
      return i;
  }
  return last;
}

// The RFC 0006 scalable joint deal, re-derived: board-filtered ascending
// cumulative marginals per seat, one unit draw per seat per attempt, a full
// restart on an inter-seat conflict.
std::vector<std::array<int, 2>> oracle_joint_deal(
    const std::vector<std::vector<WeightedHand>>& ranges, const std::vector<int>& board,
    SplitMix64& rng) {
  const std::size_t seats = ranges.size();
  std::array<bool, 52> board_used{};
  for (int c : board)
    board_used[c] = true;
  std::vector<std::vector<std::array<int, 2>>> combos(seats);
  std::vector<std::vector<double>> cumulative(seats);
  for (std::size_t seat = 0; seat < seats; ++seat) {
    double running = 0.0;
    for (const WeightedHand& hand : ranges[seat]) {
      if (board_used[hand.cards[0]] || board_used[hand.cards[1]])
        continue;
      running += hand.weight;
      combos[seat].push_back(hand.cards);
      cumulative[seat].push_back(running);
    }
  }
  for (;;) {
    std::vector<std::array<int, 2>> proposal;
    bool conflicted = false;
    for (std::size_t seat = 0; seat < seats && !conflicted; ++seat) {
      const double target = oracle_unit_draw(rng) * cumulative[seat].back();
      std::size_t index = 0;
      while (cumulative[seat][index] <= target)
        ++index;
      const std::array<int, 2> candidate = combos[seat][index];
      for (const std::array<int, 2>& chosen : proposal)
        for (int c : chosen)
          if (c == candidate[0] || c == candidate[1])
            conflicted = true;
      if (!conflicted)
        proposal.push_back(candidate);
    }
    if (!conflicted)
      return proposal;
  }
}

std::vector<double> oracle_sigma(const OracleRow& row) {
  std::vector<double> sigma(row.actions.size());
  double positive = 0.0;
  for (double r : row.regrets)
    positive += std::max(0.0, r);
  if (positive > 0.0) {
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      sigma[i] = std::max(0.0, row.regrets[i]) / positive;
  } else {
    for (double& p : sigma)
      p = 1.0 / static_cast<double>(row.actions.size());
  }
  return sigma;
}

OracleRow& oracle_touch(OracleTable& rows, const NSeatInformationKey& key,
                        const std::vector<Action>& menu) {
  auto it = rows.find(key);
  if (it != rows.end()) {
    if (it->second.actions != menu)
      throw std::runtime_error("oracle row address merged nodes with different menus");
    return it->second;
  }
  OracleRow row;
  row.actions = menu;
  row.regrets.assign(menu.size(), 0.0);
  row.sums.assign(menu.size(), 0.0);
  return rows.emplace(key, std::move(row)).first->second;
}

std::vector<int> oracle_legal_runout(const GameState& state,
                                     const std::vector<std::array<int, 2>>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const std::array<int, 2>& hole : holes)
    used[hole[0]] = used[hole[1]] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

std::size_t oracle_board_ordinal(const GameState& state, int card) {
  std::size_t ordinal = 0;
  for (int c = 0; c < card; ++c) {
    bool on_board = false;
    for (int b : state.board())
      if (b == c)
        on_board = true;
    if (!on_board)
      ++ordinal;
  }
  return ordinal;
}

struct OracleCtx {
  const AbstractTree* tree = nullptr;
  const std::vector<std::array<int, 2>>* holes = nullptr;
  OracleTable* rows = nullptr;
  std::size_t traverser = 0;
  SplitMix64* action = nullptr;
  SplitMix64* chance = nullptr;
};

double oracle_walk(OracleCtx& ctx, const GameState& state, std::size_t node_index,
                   double own_reach) {
  const TreeNode& node = ctx.tree->node(node_index);
  switch (state.phase()) {
    case Phase::Folded: {
      const TerminalPayload& payload = ctx.tree->terminal(node.terminal);
      std::int64_t sum = 0;
      for (std::int64_t utility : payload.chip_utility)
        sum += utility;
      if (sum != 0)
        throw std::runtime_error("oracle fold payout is not zero-sum");
      return static_cast<double>(payload.chip_utility[ctx.traverser]);
    }
    case Phase::Showdown: {
      const TerminalPayload& payload = ctx.tree->terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = (*ctx.holes)[payload.live_order[i]];
      const ContributionSettlement settlement = state.settle_showdown(live);
      std::int64_t sum = 0;
      for (std::size_t s = 0; s < state.player_count(); ++s)
        sum += settlement.chip_utility[s];
      if (sum != 0)
        throw std::runtime_error("oracle showdown payout is not zero-sum");
      return static_cast<double>(settlement.chip_utility[ctx.traverser]);
    }
    case Phase::Deal: {
      const std::vector<int> cards = oracle_legal_runout(state, *ctx.holes);
      const std::size_t pick = oracle_bounded(*ctx.chance, cards.size());
      const std::size_t ordinal = oracle_board_ordinal(state, cards[pick]);
      GameState next = state.after_card(cards[pick]);
      return oracle_walk(ctx, next, node.children[ordinal], own_reach);
    }
    case Phase::Action:
      break;
    case Phase::Frontier:
      // RFC 0007: the oracle does not evaluate flop-terminal games; the
      // trainer's frontier support is tested separately.
      throw std::runtime_error("oracle does not support frontier leaves");
  }
  const std::size_t actor = *state.actor();
  const std::vector<Action>& menu = node.actions;
  std::vector<int> board(state.board().begin(), state.board().end());
  NSeatInformationKey key;
  key.own_card_bucket = static_cast<std::uint32_t>(abstraction::card_bucket(
      abstraction::CardBucketKind::CategoryTiersV1, (*ctx.holes)[actor], board));
  key.node_index = node_index;
  OracleRow& row = oracle_touch(*ctx.rows, key, menu);
  const std::vector<double> sigma = oracle_sigma(row);
  if (actor != ctx.traverser) {
    const std::size_t sampled = oracle_weighted(*ctx.action, sigma);
    GameState next = state.after_action(actor, menu[sampled]);
    return oracle_walk(ctx, next, node.children[sampled], own_reach);
  }
  for (std::size_t a = 0; a < menu.size(); ++a)
    row.sums[a] += own_reach * sigma[a];
  ++row.visits;
  std::vector<double> children(menu.size(), 0.0);
  for (std::size_t a = 0; a < menu.size(); ++a) {
    GameState next = state.after_action(actor, menu[a]);
    children[a] = oracle_walk(ctx, next, node.children[a], own_reach * sigma[a]);
  }
  double value = 0.0;
  for (std::size_t a = 0; a < menu.size(); ++a)
    value += sigma[a] * children[a];
  for (std::size_t a = 0; a < menu.size(); ++a) {
    const double updated = row.regrets[a] + (children[a] - value);
    row.regrets[a] = updated > 0.0 ? updated : 0.0;
  }
  return value;
}

// One oracle sweep over the caller-owned streams: the joint deal first, then
// the in-walk opponent-action and chance draws, in the pinned order.
void oracle_sweep(const AbstractTree& tree, const std::vector<std::vector<WeightedHand>>& ranges,
                  std::size_t traverser, NSeatTraversalStreams& streams, OracleTable& rows) {
  const std::vector<int> board(tree.def().board.begin(),
                               tree.def().board.begin() + tree.def().board_size);
  const std::vector<std::array<int, 2>> holes =
      oracle_joint_deal(ranges, board, streams.joint_deal);
  OracleCtx ctx;
  ctx.tree = &tree;
  ctx.holes = &holes;
  ctx.rows = &rows;
  ctx.traverser = traverser;
  ctx.action = &streams.opponent_action;
  ctx.chance = &streams.chance_card;
  const GameState root(tree.def());
  oracle_walk(ctx, root, tree.root_index(), 1.0);
}

bool oracle_tables_equal(const OracleTable& oracle,
                         const std::map<NSeatInformationKey, NSeatRawRow>& trained) {
  if (oracle.size() != trained.size())
    return false;
  auto jt = trained.begin();
  for (const auto& [key, row] : oracle) {
    if (jt == trained.end() || !(jt->first == key))
      return false;
    if (jt->second.actions != row.actions || jt->second.regrets.size() != row.regrets.size() ||
        jt->second.sums.size() != row.sums.size())
      return false;
    for (std::size_t i = 0; i < row.regrets.size(); ++i)
      if (jt->second.regrets[i] != row.regrets[i])  // raw ==
        return false;
    for (std::size_t i = 0; i < row.sums.size(); ++i)
      if (jt->second.sums[i] != row.sums[i])  // raw ==
        return false;
    if (jt->second.visits != row.visits)
      return false;
    ++jt;
  }
  return jt == trained.end();
}

// --- river-subgame parity machinery ------------------------------------------
//
// The n-seat trainer is external-sampling only: on a flop-rooted game its
// policy visits a sparse subset of the 49*48 runout lanes, so an exact
// evaluator (which requires a row for every queried information set) cannot
// score it. The parity gate therefore roots the n-seat game at the river
// (board_size 5), where the tree has no chance nodes and external sampling
// covers every action node. The heads-up reference is the equivalent
// flop+fixed-runout game trained by FULL traversal (complete, deterministic
// coverage). Its river rows are conditioned on the flop/turn history; the
// unique history that reaches this river with pot 2 and 1/1 stacks is a
// check-down on both earlier streets, so the river subgame the heads-up
// policy solves there is exactly the game the n-seat policy solves directly.
//
// The fixture ranges make this "exactly" literal: each seat's two holdings
// fall in DISTINCT CategoryTiersV1 buckets on this board (one pair + one high
// card), so the n-seat trainer's (bucket, node) key holds one row per exact
// hand and its bucket-pooled game IS the heads-up exact-hand game. With two
// same-bucket holdings the rekey's emplace-keep-first would alias one holding
// onto the other's row and the gate would compare a pooled policy against an
// aliased one -- the distinct-bucket fixture is what keeps the parity honest.

// Advances a heads-up state from the flop root through a check-down on the
// flop and turn to the river action phase. With 1-chip stacks any flop or
// turn bet is all-in and ends the hand, so check-down is the ONLY history
// that reaches the river with pot 2 and 1/1 stacks.
HeadsUpState checked_down_river(const HeadsUpGame& game) {
  HeadsUpState state(game.root);
  const Action check{ActionType::Check, 0};
  for (int street = 0; street < 2; ++street) {
    if (state.phase() != Phase::Action || state.actor() != 0)
      throw std::runtime_error("checked_down_river: expected seat 0 to act");
    state = state.after_action(0, check);
    if (state.phase() != Phase::Action || state.actor() != 1)
      throw std::runtime_error("checked_down_river: expected seat 1 to act");
    state = state.after_action(1, check);
    if (state.phase() != Phase::Deal)
      throw std::runtime_error("checked_down_river: expected a deal phase");
    state = state.after_card(*game.fixed_runout[street]);
  }
  if (state.phase() != Phase::Action || state.actor() != 0)
    throw std::runtime_error("checked_down_river: expected river action by seat 0");
  if (state.board().size() != 5)
    throw std::runtime_error("checked_down_river: expected a five-card board");
  return state;
}

using RiverTable = std::map<NSeatInformationKey, NSeatPolicyRow>;

// Rekey the heads-up policy's river-subgame rows into the n-seat (bucket,
// node) key space. Walks the river-rooted tree with the check-down river
// state, emitting one row per actor holding that is card-compatible with the
// board, addressed by the documented (bucket, node) pair and valued by the
// heads-up row at the matching information key. A missing heads-up row is a
// loud failure: full traversal covers every river node, so a gap means the
// check-down conditioning drifted from the trained policy.
RiverTable rekey_heads_up_river(const HeadsUpGame& game, const AbstractTree& tree,
                                const HeadsUpPolicy& hu, const HeadsUpState& river_root) {
  RiverTable table;
  struct Frame {
    std::size_t node;
    HeadsUpState state;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{tree.root_index(), river_root});
  while (!stack.empty()) {
    Frame frame = std::move(stack.back());
    stack.pop_back();
    const TreeNode& node = tree.node(frame.node);
    if (node.kind == NodeKind::TerminalFold || node.kind == NodeKind::TerminalShowdown)
      continue;
    if (node.kind == NodeKind::Chance)
      throw std::runtime_error("rekey: a river-rooted tree must not branch on chance");
    const std::vector<Action> menu = abstract_actions(frame.state, game.sizes);
    if (menu != node.actions)
      throw std::runtime_error("rekey: menu drifted from the tree node");
    const std::size_t actor = *frame.state.actor();
    std::vector<int> board(frame.state.board().begin(), frame.state.board().end());
    for (const WeightedHand& holding : game.ranges[actor]) {
      const std::array<int, 2> hand = holding.cards;
      bool conflict = false;
      for (int c : board)
        conflict = conflict || c == hand[0] || c == hand[1];
      if (conflict)
        continue;
      const PolicyRow* hu_row = hu.lookup(frame.state, hand);
      if (hu_row == nullptr)
        throw std::runtime_error("rekey: heads-up river policy is incomplete");
      if (hu_row->actions != menu)
        throw std::runtime_error("rekey: heads-up row menu drifted");
      const std::uint32_t bucket = static_cast<std::uint32_t>(
          abstraction::card_bucket(abstraction::CardBucketKind::CategoryTiersV1, hand, board));
      NSeatPolicyRow row;
      row.actions = hu_row->actions;
      row.probabilities = hu_row->probabilities;
      table.emplace(NSeatInformationKey{bucket, static_cast<std::uint64_t>(frame.node)},
                    std::move(row));
    }
    for (std::size_t a = node.actions.size(); a-- > 0;)
      stack.push_back(
          Frame{node.children[a], frame.state.after_action(node.actor, node.actions[a])});
  }
  return table;
}

// --- a full-traversal evaluator over the river-rooted tree -------------------
//
// Enumerates every card-compatible joint deal and walks the ENTIRE river
// action tree for each, querying the sealed (bucket, node) table at every
// action node. The river game has no runout chance, so a trained policy must
// cover every queried set; a missing row is a loud failure, never a fill.
// `responder` is the seat whose value is returned; `best` takes the argmax at
// the responder's own nodes (best response) and mixes with the row
// probabilities elsewhere (profile). Opponent nodes always mix with the
// profile in BOTH modes, exactly as the shipped heads-up evaluator does.

struct RiverDeal {
  std::array<std::array<int, 2>, 2> hands;
  double probability = 0;
};

std::vector<RiverDeal> enumerate_river_deals(const HeadsUpGame& game) {
  std::vector<RiverDeal> deals;
  double mass = 0;
  for (const WeightedHand& a : game.ranges[0]) {
    for (const WeightedHand& b : game.ranges[1]) {
      bool conflict = false;
      for (int c : a.cards)
        for (int d : b.cards)
          if (c == d)
            conflict = true;
      if (conflict)
        continue;
      const double p = a.weight * b.weight;
      deals.push_back({std::array<std::array<int, 2>, 2>{a.cards, b.cards}, p});
      mass += p;
    }
  }
  for (RiverDeal& deal : deals)
    deal.probability /= mass;
  return deals;
}

double river_walk(const AbstractTree& tree, const GameState& state, std::size_t node_index,
                  const std::array<std::array<int, 2>, 2>& holes, const RiverTable& rows,
                  std::size_t responder, bool best) {
  const TreeNode& node = tree.node(node_index);
  if (node.kind == NodeKind::TerminalFold)
    return static_cast<double>(tree.terminal(node.terminal).chip_utility[responder]);
  if (node.kind == NodeKind::TerminalShowdown) {
    const auto& payload = tree.terminal(node.terminal);
    std::vector<std::array<int, 2>> live(payload.live_count);
    for (std::size_t i = 0; i < payload.live_count; ++i)
      live[i] = holes[payload.live_order[i]];
    return static_cast<double>(state.settle_showdown(live).chip_utility[responder]);
  }
  if (node.kind == NodeKind::Chance)
    throw std::runtime_error("river eval: a river-rooted tree must not branch on chance");
  const std::size_t actor = *state.actor();
  const std::vector<Action>& menu = node.actions;
  std::vector<int> board(state.board().begin(), state.board().end());
  const std::uint32_t bucket = static_cast<std::uint32_t>(
      abstraction::card_bucket(abstraction::CardBucketKind::CategoryTiersV1, holes[actor], board));
  const auto it = rows.find(NSeatInformationKey{bucket, static_cast<std::uint64_t>(node_index)});
  if (it == rows.end())
    throw std::runtime_error("river eval: uncovered information set");
  const NSeatPolicyRow& row = it->second;
  if (row.actions != menu)
    throw std::runtime_error("river eval: row menu drifted from the tree node");
  if (actor != responder) {
    double value = 0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      value += row.probabilities[a] * river_walk(tree, state.after_action(actor, menu[a]),
                                                 node.children[a], holes, rows, responder, best);
    return value;
  }
  if (best) {
    double value = -std::numeric_limits<double>::infinity();
    for (std::size_t a = 0; a < menu.size(); ++a)
      value = std::max(value, river_walk(tree, state.after_action(actor, menu[a]), node.children[a],
                                         holes, rows, responder, best));
    return value;
  }
  double value = 0;
  for (std::size_t a = 0; a < menu.size(); ++a)
    value += row.probabilities[a] * river_walk(tree, state.after_action(actor, menu[a]),
                                               node.children[a], holes, rows, responder, best);
  return value;
}

struct RiverEvaluation {
  std::array<double, 2> profile{};
  std::array<double, 2> br{};
  double nash_conv = 0;
};

RiverEvaluation evaluate_river(const HeadsUpGame& game, const GameDef& def,
                               const AbstractTree& tree, const RiverTable& rows) {
  const std::vector<RiverDeal> deals = enumerate_river_deals(game);
  RiverEvaluation ev;
  for (std::size_t s = 0; s < 2; ++s) {
    for (const RiverDeal& deal : deals) {
      GameState state(def);
      ev.profile[s] += deal.probability * river_walk(tree, state, tree.root_index(), deal.hands,
                                                     rows, s, /*best=*/false);
      ev.br[s] += deal.probability *
                  river_walk(tree, state, tree.root_index(), deal.hands, rows, s, /*best=*/true);
    }
    ev.nash_conv += ev.br[s] - ev.profile[s];
  }
  return ev;
}

// --- 1. two-seat parity on the river subgame ---------------------------------

int test_two_seat_parity() {
  const GameDef def = river_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const HeadsUpGame game = river_game();
  const HeadsUpState river_root = checked_down_river(game);
  const std::uint64_t seed = 20260929;

  // Heads-up reference: FULL traversal on the flop+fixed-runout game. Full
  // traversal visits every node (complete coverage) and is deterministic, so
  // every river row conditioned on the check-down history is present.
  TrainingLimits hu_limits;
  hu_limits.time = std::chrono::minutes(5);
  const TrainingResult hu = HeadsUpTrainer(game).train(300, hu_limits);
  CHECK(hu.status == TrainingStatus::Complete);

  // The n-seat route through solve() on the river-rooted tree. External
  // sampling covers every river action node (no runout chance), and the
  // trainer is roughly two orders of magnitude faster than the heads-up
  // sampled route, so it runs many more iterations for a like-for-like
  // value comparison.
  SolveRequest ns_request;
  ns_request.tree = &tree;
  ns_request.mode = SolveMode::ExternalSampling;
  ns_request.solver = SolverKind::NSeatCfr;
  ns_request.ranges = {river_range0(), river_range1()};
  ns_request.iterations = 30'000;
  ns_request.seed = seed;
  const SolveResult ns_result = solve(ns_request);
  CHECK(!ns_result.is_heads_up());
  const NSeatTrainingResult& ns = ns_result.nseat();
  CHECK(ns.termination == NSeatTerminationPhase::Complete);
  CHECK(ns.completed_iterations == 30'000);
  CHECK(ns.algorithm_revision == kNSeatAlgorithmRevision);
  CHECK(ns_result.action_id() == abstraction::identity_action_id());
  CHECK(!ns.policy.rows().empty());
  CHECK(same_game_def(ns.policy.game(), def));
  CHECK(ns.seed == seed);

  // Rekey the heads-up policy's river-subgame rows (conditioned on the
  // check-down history) into the (bucket, node) key space, then score BOTH
  // policies on the same river tree with the full-traversal evaluator. Byte
  // equality is NOT claimed (streams, average weighting and row keys differ
  // by design); the tolerance is declared and justified here: the root pot
  // is 2 chips, the profile-gap tolerance is 2% of the pot (0.04 chips) and
  // the NashConv tolerance is 4% of the pot (0.08 chips), roughly an order
  // of magnitude above the measured gaps on this machine (printed below) and
  // well below the gap a materially worse policy shows on this fixture.
  const RiverTable hu_river = rekey_heads_up_river(game, tree, hu.policy, river_root);
  const RiverEvaluation hu_eval = evaluate_river(game, def, tree, hu_river);
  const RiverEvaluation ns_eval = evaluate_river(game, def, tree, ns.policy.rows());
  const double pot = static_cast<double>(def.pot);
  std::printf("heads-up river: profile={%.6f,%.6f} br={%.6f,%.6f} nash_conv=%.6f\n",
              hu_eval.profile[0], hu_eval.profile[1], hu_eval.br[0], hu_eval.br[1],
              hu_eval.nash_conv);
  std::printf("n-seat river:   profile={%.6f,%.6f} br={%.6f,%.6f} nash_conv=%.6f\n",
              ns_eval.profile[0], ns_eval.profile[1], ns_eval.br[0], ns_eval.br[1],
              ns_eval.nash_conv);
  for (int p = 0; p < 2; ++p) {
    const double gap = std::abs(ns_eval.profile[p] - hu_eval.profile[p]);
    std::printf("profile gap seat %d: %.6f (tolerance %.6f)\n", p, gap, 0.02 * pot);
    CHECK(gap <= 0.02 * pot);
  }
  const double conv_gap = std::abs(ns_eval.nash_conv - hu_eval.nash_conv);
  std::printf("nash_conv gap: %.6f (tolerance %.6f)\n", conv_gap, 0.04 * pot);
  CHECK(conv_gap <= 0.04 * pot);

  // The n-seat policy's own lookup surface: exact hits and exact misses.
  {
    GameState root(def);
    CHECK(root.phase() == Phase::Action);
    const std::size_t actor = *root.actor();
    const std::array<int, 2> own = actor == 0 ? std::array<int, 2>{card("Ah"), card("Ac")}
                                              : std::array<int, 2>{card("As"), card("Ad")};
    const NSeatPolicyRow* row = ns.policy.lookup(tree, root, actor, own);
    CHECK(row != nullptr);
    CHECK(!row->actions.empty());
    CHECK(row->probabilities.size() == row->actions.size());
    double total = 0.0;
    for (double p : row->probabilities) {
      CHECK(p >= 0.0 && p <= 1.0);
      total += p;
    }
    CHECK(std::abs(total - 1.0) < 1e-12);
    // A non-acting seat is an exact miss at this state.
    CHECK(ns.policy.lookup(tree, root, 1 - actor, own) == nullptr);
    // An own card on the board is an exact miss.
    CHECK(ns.policy.lookup(tree, root, actor, {card("2c"), card("Ac")}) == nullptr);
    // A same-shape state from a different-board river game is an exact miss:
    // its observable ledger matches no node in this tree (every legal action
    // from the root IS in the tree, so an off-tree state needs a different
    // board, not an illegal action).
    GameDef other_def = def;
    other_def.board[4] = card("9d");
    GameState other(other_def);
    CHECK(other.phase() == Phase::Action);
    CHECK(ns.policy.lookup(tree, other, *other.actor(), own) == nullptr);
  }
  return 0;
}

// --- 2. three-seat update-rule oracle ----------------------------------------

int test_three_seat_oracle() {
  const GameDef def = three_seat_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_ranges();

  // One visible sweep per traverser; the oracle re-derives the whole episode.
  for (std::size_t traverser = 0; traverser < 3; ++traverser) {
    NSeatTraversalStreams trained_streams{
        derive_nseat_stream(4242, NSeatStreamPurpose::JointDeal, 0, traverser),
        derive_nseat_stream(4242, NSeatStreamPurpose::OpponentAction, 0, traverser),
        derive_nseat_stream(4242, NSeatStreamPurpose::ChanceCard, 0, traverser)};
    NSeatTraversalStreams oracle_streams{
        derive_nseat_stream(4242, NSeatStreamPurpose::JointDeal, 0, traverser),
        derive_nseat_stream(4242, NSeatStreamPurpose::OpponentAction, 0, traverser),
        derive_nseat_stream(4242, NSeatStreamPurpose::ChanceCard, 0, traverser)};

    std::map<NSeatInformationKey, NSeatRawRow> trained;
    debug_run_one_nseat_sweep(tree, ranges, traverser, trained_streams, trained);
    OracleTable oracle;
    oracle_sweep(tree, ranges, traverser, oracle_streams, oracle);
    CHECK(!trained.empty());
    CHECK(!oracle.empty());
    if (!oracle_tables_equal(oracle, trained)) {
      std::printf("oracle mismatch on traverser %zu: rows oracle=%zu trained=%zu\n", traverser,
                  oracle.size(), trained.size());
      auto jt = trained.begin();
      for (const auto& [key, row] : oracle) {
        if (jt == trained.end() || !(jt->first == key)) {
          std::printf("  first key divergence: bucket=%u node=%llu\n", key.own_card_bucket,
                      static_cast<unsigned long long>(key.node_index));
          break;
        }
        bool differs = false;
        for (std::size_t i = 0; i < row.regrets.size() && !differs; ++i)
          differs = jt->second.regrets[i] != row.regrets[i];
        for (std::size_t i = 0; i < row.sums.size() && !differs; ++i)
          differs = jt->second.sums[i] != row.sums[i];
        if (differs || jt->second.visits != row.visits) {
          std::printf("  first row divergence: bucket=%u node=%llu visits %llu vs %llu\n",
                      key.own_card_bucket, static_cast<unsigned long long>(key.node_index),
                      static_cast<unsigned long long>(row.visits),
                      static_cast<unsigned long long>(jt->second.visits));
          break;
        }
        ++jt;
      }
      CHECK(false);
    }

    // The sweep must have exercised the update rule: a whole 3-seat sweep with
    // two-card ranges cannot leave every accumulator at zero.
    bool any_regret = false, any_sum = false, any_visit = false;
    for (const auto& [key, row] : trained) {
      (void)key;
      any_visit = any_visit || row.visits > 0;
      for (double r : row.regrets)
        any_regret = any_regret || r > 0.0;
      for (double s : row.sums)
        any_sum = any_sum || s > 0.0;
    }
    CHECK(any_visit);
    CHECK(any_regret);
    CHECK(any_sum);
  }
  return 0;
}

// --- 3. determinism ----------------------------------------------------------

int test_determinism() {
  const GameDef def = three_seat_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_ranges();
  const NSeatTrainerLimits limits{};

  const NSeatTrainingResult first = train_nseat(tree, ranges, 64, 777, limits);
  const NSeatTrainingResult second = train_nseat(tree, ranges, 64, 777, limits);
  CHECK(first.termination == NSeatTerminationPhase::Complete);
  CHECK(second.termination == NSeatTerminationPhase::Complete);
  CHECK(first.completed_iterations == 64 && second.completed_iterations == 64);
  CHECK(first.prng_state == second.prng_state && first.prng_state != 0);
  CHECK(first.visits == second.visits && first.information_sets == second.information_sets &&
        first.accounted_bytes == second.accounted_bytes);
  CHECK(first.policy.rows().size() == second.policy.rows().size());
  auto jt = second.policy.rows().begin();
  for (const auto& [key, row] : first.policy.rows()) {
    CHECK(jt != second.policy.rows().end());
    CHECK(jt->first == key);
    CHECK(jt->second.actions == row.actions);
    CHECK(jt->second.visits == row.visits);
    CHECK(jt->second.probabilities == row.probabilities);  // raw ==
    ++jt;
  }
  CHECK(jt == second.policy.rows().end());

  // A different seed takes a different sampled trajectory: the first sweep's
  // raw rows differ from the same-seed sweep's (checked via the visible seam),
  // and the finished policy differs.
  NSeatTraversalStreams streams_a{
      derive_nseat_stream(777, NSeatStreamPurpose::JointDeal, 0, 0),
      derive_nseat_stream(777, NSeatStreamPurpose::OpponentAction, 0, 0),
      derive_nseat_stream(777, NSeatStreamPurpose::ChanceCard, 0, 0)};
  NSeatTraversalStreams streams_b{
      derive_nseat_stream(778, NSeatStreamPurpose::JointDeal, 0, 0),
      derive_nseat_stream(778, NSeatStreamPurpose::OpponentAction, 0, 0),
      derive_nseat_stream(778, NSeatStreamPurpose::ChanceCard, 0, 0)};
  std::map<NSeatInformationKey, NSeatRawRow> rows_a, rows_b;
  debug_run_one_nseat_sweep(tree, ranges, 0, streams_a, rows_a);
  debug_run_one_nseat_sweep(tree, ranges, 0, streams_b, rows_b);

  const auto to_oracle = [](const std::map<NSeatInformationKey, NSeatRawRow>& raw) {
    OracleTable converted;
    for (const auto& [key, row] : raw)
      converted.emplace(key, OracleRow{row.actions, row.regrets, row.sums, row.visits});
    return converted;
  };
  CHECK(!oracle_tables_equal(to_oracle(rows_a), rows_b));

  const NSeatTrainingResult seeded_b = train_nseat(tree, ranges, 64, 778, limits);
  bool policy_differs = seeded_b.policy.rows().size() != first.policy.rows().size();
  if (!policy_differs) {
    auto bt = seeded_b.policy.rows().begin();
    for (const auto& [key, row] : first.policy.rows()) {
      CHECK(bt != seeded_b.policy.rows().end());
      if (!(bt->first == key) || bt->second.probabilities != row.probabilities) {
        policy_differs = true;
        break;
      }
      ++bt;
    }
  }
  CHECK(policy_differs);

  // The seam is reproducible under caller-owned streams: the same streams run
  // twice produce byte-identical rows.
  NSeatTraversalStreams streams_again{
      derive_nseat_stream(777, NSeatStreamPurpose::JointDeal, 0, 0),
      derive_nseat_stream(777, NSeatStreamPurpose::OpponentAction, 0, 0),
      derive_nseat_stream(777, NSeatStreamPurpose::ChanceCard, 0, 0)};
  std::map<NSeatInformationKey, NSeatRawRow> rows_again;
  debug_run_one_nseat_sweep(tree, ranges, 0, streams_again, rows_again);
  CHECK(oracle_tables_equal(to_oracle(rows_a), rows_again));

  // The derivation is pinned: an inline re-implementation of the documented
  // FNV-1a derivation must produce the same stream as derive_nseat_stream.
  {
    const auto fnv = [](const std::string& bytes) {
      std::uint64_t hash = 0xcbf29ce484222325ULL;
      for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
      }
      return hash;
    };
    const std::string role = std::string("bsnseat|") + "2|" + "4242|" + "7|" + "1";
    SplitMix64 expected(fnv(role) ^ 0x42534E5354414D32ULL);
    SplitMix64 derived = derive_nseat_stream(4242, NSeatStreamPurpose::OpponentAction, 7, 1);
    for (int i = 0; i < 8; ++i)
      CHECK(expected.next_u64() == derived.next_u64());
  }
  return 0;
}

// --- 4. refusals -------------------------------------------------------------

int test_refusals() {
  // True iff solve(req) throws unsupported_tree_shape and nothing in the
  // invalid_argument family.
  auto expect_shape = [](const SolveRequest& req) {
    bool typed = false, invalid = false;
    try {
      solve(req);
    } catch (const unsupported_tree_shape&) {
      typed = true;
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    return typed && !invalid;
  };
  // True iff solve(req) throws std::invalid_argument (and not the shape type).
  auto expect_invalid = [](const SolveRequest& req) {
    try {
      solve(req);
    } catch (const unsupported_tree_shape&) {
      return false;
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };

  const GameDef hu = heads_up_def();
  const AbstractTree hu_tree(hu, abstraction::ActionAbstraction::identity());
  const GameDef three = three_seat_def();
  const AbstractTree three_tree(three, abstraction::ActionAbstraction::identity());

  // Wrong range count: shape refusal on both routes.
  {
    SolveRequest req = base_request(three_tree);
    req.ranges = {heads_up_range0()};  // one range for three seats
    req.iterations = 1;
    CHECK(expect_shape(req));
    req.solver = SolverKind::NSeatCfr;
    CHECK(expect_shape(req));
    SolveRequest two = base_request(hu_tree);
    two.solver = SolverKind::NSeatCfr;
    two.ranges = {heads_up_range0()};
    two.iterations = 1;
    CHECK(expect_shape(two));
    SolveRequest hu_solver = base_request(hu_tree);
    hu_solver.solver = SolverKind::HeadsUpCfr;
    hu_solver.ranges = {heads_up_range0()};
    hu_solver.iterations = 1;
    CHECK(expect_shape(hu_solver));
  }

  // 3-seat tree with HeadsUpCfr explicitly selected: the current typed
  // refusal, unchanged.
  {
    SolveRequest req = base_request(three_tree);
    req.solver = SolverKind::HeadsUpCfr;
    req.ranges = three_seat_ranges();
    req.iterations = 1;
    CHECK(expect_shape(req));
  }

  // A two-seat river root (board_size 5) is a legal game the n-seat route
  // solves, but the heads-up route must refuse it with the typed shape error:
  // HeadsUpRoot is structurally flop-rooted, so projecting would silently
  // drop board[3..4]. Auto on a two-seat tree routes to heads-up, so it
  // refuses too.
  {
    const GameDef river = river_def();
    const AbstractTree river_tree(river, abstraction::ActionAbstraction::identity());
    SolveRequest req = base_request(river_tree);
    req.solver = SolverKind::HeadsUpCfr;
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 1;
    CHECK(expect_shape(req));
    req.solver = SolverKind::Auto;
    CHECK(expect_shape(req));
    req.solver = SolverKind::NSeatCfr;
    SolveResult result = solve(req);
    CHECK(!result.is_heads_up());
    CHECK(result.nseat().completed_iterations == 1);
  }

  // Non-identity action abstraction: refuses on both routes.
  {
    abstraction::SizeSchedule custom = abstraction::default_size_schedule();
    custom[0].bets = {{1, 2}, {3, 4}, {3, 2}};  // differs from identity
    abstraction::ActionAbstraction nonidentity(custom);
    CHECK(nonidentity.id() != abstraction::identity_action_id());
    AbstractTree nid_hu(hu, nonidentity);
    SolveRequest req = base_request(nid_hu);
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 1;
    req.solver = SolverKind::NSeatCfr;
    CHECK(expect_shape(req));
    req.solver = SolverKind::Auto;
    CHECK(expect_shape(req));
    AbstractTree nid_three(three, nonidentity);
    SolveRequest req3 = base_request(nid_three);
    req3.ranges = three_seat_ranges();
    req3.iterations = 1;
    CHECK(expect_shape(req3));
  }

  // Declared-coarse action abstraction: accepted on both routes.
  {
    abstraction::ActionAbstraction coarse =
        abstraction::ActionAbstraction::declared(abstraction::default_size_schedule(),
                                                 abstraction::CoverSeeds::DeclaredOnly);
    CHECK(coarse.id() != abstraction::identity_action_id());
    AbstractTree coarse_hu(hu, coarse);
    SolveRequest req = base_request(coarse_hu);
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 1;
    req.solver = SolverKind::NSeatCfr;
    SolveResult result = solve(req);
    CHECK(!result.is_heads_up());
    CHECK(result.nseat().completed_iterations == 1);
    // Auto with 3 seats routes to the n-seat trainer.
    AbstractTree coarse_three(three, coarse);
    SolveRequest req3 = base_request(coarse_three);
    req3.ranges = three_seat_ranges();
    req3.iterations = 1;
    req3.solver = SolverKind::Auto;
    SolveResult result3 = solve(req3);
    CHECK(!result3.is_heads_up());
    CHECK(result3.nseat().completed_iterations == 1);
  }

  // Zero iterations: invalid_argument on every route that gets that far.
  {
    SolveRequest req = base_request(hu_tree);
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 0;
    req.solver = SolverKind::NSeatCfr;
    CHECK(expect_invalid(req));
    req.solver = SolverKind::Auto;
    CHECK(expect_invalid(req));
    SolveRequest req3 = base_request(three_tree);
    req3.ranges = three_seat_ranges();
    req3.iterations = 0;
    CHECK(expect_invalid(req3));
  }

  // Null tree: invalid_argument (kept from stage 4).
  {
    SolveRequest req;
    req.solver = SolverKind::NSeatCfr;
    CHECK(expect_invalid(req));
  }

  // Fixed runout conditioning on the n-seat route: typed shape refusal (the
  // conditioning would otherwise be silently dropped).
  {
    SolveRequest req = base_request(three_tree);
    req.ranges = three_seat_ranges();
    req.iterations = 1;
    req.runout = {card("Js"), std::nullopt};
    CHECK(expect_shape(req));
  }

  // FullTraversal mode on the n-seat route: typed refusal (the model has no
  // full-traversal driver), never a silent substitution.
  {
    SolveRequest req = base_request(three_tree);
    req.ranges = three_seat_ranges();
    req.iterations = 1;
    req.mode = SolveMode::FullTraversal;
    CHECK(expect_shape(req));
  }

  // Malformed ranges (empty seat range, unsorted combo, zero weight) are
  // invalid_argument, the malformed-request family.
  {
    SolveRequest req = base_request(three_tree);
    req.iterations = 1;
    req.ranges = three_seat_ranges();
    req.ranges[1].clear();
    CHECK(expect_invalid(req));
    req.ranges = three_seat_ranges();
    req.ranges[0][0].cards = {card("Kh"), card("Qh")};  // unsorted
    CHECK(expect_invalid(req));
    req.ranges = three_seat_ranges();
    req.ranges[2][0].weight = 0.0;
    CHECK(expect_invalid(req));
  }

  // The explicit two-seat NSeatCfr route SOLVES (it is permitted), so the
  // shape refusals above are not an artifact of the route being absent.
  {
    SolveRequest req = base_request(hu_tree);
    req.solver = SolverKind::NSeatCfr;
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 1;
    SolveResult result = solve(req);
    CHECK(!result.is_heads_up());
    CHECK(result.nseat().completed_iterations == 1);
  }

  // The Auto route on the 3-seat tree reaches the n-seat trainer (it is no
  // longer a refusal).
  {
    SolveRequest req = base_request(three_tree);
    req.ranges = three_seat_ranges();
    req.iterations = 1;
    SolveResult result = solve(req);
    CHECK(!result.is_heads_up());
    CHECK(result.nseat().completed_iterations == 1);
  }

  // The heads-up route's discriminant accessor fails loudly when misused.
  {
    SolveRequest req = base_request(hu_tree);
    req.ranges = {heads_up_range0(), heads_up_range1()};
    req.iterations = 1;
    SolveResult result = solve(req);
    CHECK(result.is_heads_up());
    bool threw = false;
    try {
      (void)result.nseat();
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
  }
  return 0;
}

// RFC 0010: a 3-seat preflop flop-terminal game trains end-to-end with the
// EquityFrontierEvaluator. Over enough iterations the walk samples preflop
// folds, reaching FlopDeal leaves with two live seats (and the folded seat's
// dead money), exercising the N-way frontier path inside the trainer.
int test_three_seat_flop_terminal() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {6, 6, 6, 0, 0, 0, 0, 0, 0, 0};  // 3 BB
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted[small_blind_seat(def)] = 1;
  def.blinds_posted[big_blind_seat(def)] = 2;
  def.terminal = TerminalDepth::Flop;

  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  CHECK(tree.size() > 0);

  const auto ranges = three_seat_ranges();
  const gto::EquityFrontierEvaluator frontier;
  const NSeatTrainerLimits limits{};
  const NSeatTrainingResult trained = train_nseat(tree, ranges, 200, 4242, limits, &frontier);
  CHECK(trained.termination == NSeatTerminationPhase::Complete);
  CHECK(trained.information_sets > 0);
  return 0;
}

}  // namespace

int main() {
  if (test_three_seat_oracle() != 0)
    return 1;
  if (test_determinism() != 0)
    return 1;
  if (test_refusals() != 0)
    return 1;
  if (test_two_seat_parity() != 0)
    return 1;
  if (test_three_seat_flop_terminal() != 0)
    return 1;
  std::printf("test_solve_nseat PASS\n");
  return 0;
}
