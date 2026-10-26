// RFC 0011 Stage 3: NashConv measurement for the turn-rooted subgame resolver.
//
// Builds representative 2-seat, 3-seat, and 4-seat, turn-rooted and
// river-rooted River-terminal subgames, solves with CFR (train_nseat), and
// computes NashConv by enumerating every card-compatible joint deal and every
// legal river card. Chance nodes are enumerated exactly (the river chance is
// bounded at 44 cards for 2 seats, 42 for 3, 40 for 4), so the only
// approximation is the CFR policy itself. Reports tree size, solve time,
// coverage, and exploitability at N iterations, plus a uniform-policy
// baseline.
//
// The best response is information-set-consistent: joint deals are grouped by
// the traverser's own holding (which, with the public history, defines the
// information set), and ONE action is chosen per group, pooling all opponent
// holdings. This matches the exact_best_response_utility algorithm in
// engine/src/stage6/exact_oracle.cpp. The previous per-deal walker (which
// maximized separately under fixed joint private cards) was an omniscient
// upper bound, not a best response, and has been replaced.
//
// The fixture is a flop check-through on 2c 3d 7h that reaches the turn Js
// (and optionally the river 9c), with 10 BB behind and a 1 BB pot per seat.
// The action abstraction is a declared-only schedule with one 1/2-pot bet and
// one 1/2-pot raise per street, which keeps the tree small enough for exact
// enumeration while still exercising the full turn → river → showdown
// structure.
//
// Usage: bigshark-subgame-nash-conv [iterations] [seed]
//        bigshark-subgame-nash-conv --t3 [iterations] [seed]
//        bigshark-subgame-nash-conv --t3-dl [iterations] [seed]
//        bigshark-subgame-nash-conv --t3-dl-cap [iterations] [seed]
//        bigshark-subgame-nash-conv --t3-dl-cap-nc [iterations] [seed]
//
// --t3        : full-tree feasibility scan (2..6 seats, 10/25/100 BB)
// --t3-dl     : depth-limited feasibility scan (same fixtures, max_streets=1)
// --t3-dl-cap  : depth-limited + raise-capped scan (max_streets=1, max_raises=1)
// --t3-dl-cap-nc: same as --t3-dl-cap but with NashConv quality measurement
//
// This is a manual offline measurement tool, NOT a CTest.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/nash_conv_units.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/prng.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::card_bucket;
using bs::abstraction::CoverSeeds;
using bs::abstraction::SizeSchedule;
using bs::abstraction::StreetSizes;
using bs::gto::EquityFrontierEvaluator;
using bs::gto::FrontierEvaluator;

namespace {

// ---------------------------------------------------------------------------
// Card and range helpers
// ---------------------------------------------------------------------------

int card(const char* name) {
  return bs::cardId(std::string(name));
}

std::array<int, 2> hand(const char* c0, const char* c1) {
  int a = card(c0);
  int b = card(c1);
  return {std::min(a, b), std::max(a, b)};
}

bool hands_collide(const std::array<int, 2>& a, const std::array<int, 2>& b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

bool blocks_board(const std::array<int, 2>& h, const std::array<int, 5>& board,
                  std::uint8_t board_size) {
  for (std::uint8_t i = 0; i < board_size; ++i)
    if (h[0] == board[i] || h[1] == board[i])
      return true;
  return false;
}

// Direct showdown utility for a 5-card (river) board. Bypasses the
// EquityFrontierEvaluator's cache/mutex/key-allocation overhead, which dominates
// when millions of frontier leaves are evaluated (the evaluator's own equity
// computation for a 5-card board is just this hand evaluation). Returns one
// utility per seat: equity * pot - contributed.
std::vector<double> direct_showdown_utility(std::span<const int> board,
                                            std::span<const std::array<int, 2>> holes,
                                            const bs::tree::TerminalPayload& payload) {
  const std::size_t n = holes.size();
  std::uint32_t best = 0;
  std::array<std::uint32_t, kMaxUnifiedSeats> scores{};
  for (std::size_t s = 0; s < n; ++s) {
    if (payload.seats[s].folded)
      continue;
    const int cards[7] = {holes[s][0], holes[s][1], board[0], board[1],
                          board[2],    board[3],    board[4]};
    scores[s] = bs::evaluate(cards, 7).score;
    if (scores[s] > best)
      best = scores[s];
  }
  int tied = 0;
  for (std::size_t s = 0; s < n; ++s)
    if (!payload.seats[s].folded && scores[s] == best)
      ++tied;
  const double share = 1.0 / static_cast<double>(tied);
  double pot = 0.0;
  for (std::size_t s = 0; s < n; ++s)
    pot += static_cast<double>(payload.seats[s].contributed);
  std::vector<double> values(n);
  for (std::size_t s = 0; s < n; ++s) {
    const double equity = (!payload.seats[s].folded && scores[s] == best) ? share : 0.0;
    values[s] = equity * pot - static_cast<double>(payload.seats[s].contributed);
  }
  return values;
}

// Legal runout cards: {0..51} minus the board and every known hole card.
// Folded seats carry the {-1,-1} sentinel and are skipped: under the uniform
// unconditioned fold-range model their two unknown cards are exchangeable
// with the runout, so the marginal runout distribution is uniform over the
// cards not fixed to the board or live seats (see collect_joint_deals for the
// symmetry proof).  Do not use this shortcut with a conditioned fold-range.
std::vector<int> legal_runout_cards(const GameState& state,
                                    const std::vector<std::array<int, 2>>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const auto& h : holes) {
    if (h[0] >= 0)
      used[h[0]] = true;
    if (h[1] >= 0)
      used[h[1]] = true;
  }
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

// The card's index in the ascending board-only list {0..51}\state.board().
// The tree's chance children enumerate exactly that list.
std::size_t board_only_ordinal(const GameState& state, int card) {
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

// ---------------------------------------------------------------------------
// Representative fixtures
// ---------------------------------------------------------------------------

// 2-seat, 10 BB behind, 1 BB pot, flop check-through to the turn Js.
GameDef make_subgame_def_2p_turn() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;  // seat 1 acts first postflop in heads-up
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {20, 20};
  def.contributions = {1, 1};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 3-seat, 10 BB behind, 1 BB pot each, flop check-through to the turn Js.
// Button=2 so seat 0 acts first postflop, matching the multiway convention.
GameDef make_subgame_def_3p_turn() {
  GameDef def{};
  def.player_count = 3;
  def.button = 2;  // seat 0 acts first postflop
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {20, 20, 20};
  def.contributions = {1, 1, 1};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 4-seat, 10 BB behind, 1 BB pot each, flop check-through to the turn Js.
// Button=3 so seat 0 acts first postflop, matching the multiway convention.
GameDef make_subgame_def_4p_turn() {
  GameDef def{};
  def.player_count = 4;
  def.button = 3;  // seat 0 acts first postflop
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {20, 20, 20, 20};
  def.contributions = {1, 1, 1, 1};
  def.pot = 4;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// T3: parameterized N-seat fixture. Reproduces the 2/3/4-seat 10 BB fixtures
// exactly when called with (n, 10, false) and extends the same shape to 5/6
// seats and 25/100 BB normal stacks. Button is the last seat so seat 0 acts
// first postflop, matching the multiway convention.
GameDef make_subgame_def_np(std::size_t seat_count, int stack_bb, bool river_rooted) {
  GameDef def{};
  def.player_count = seat_count;
  def.button = static_cast<int>(seat_count - 1);
  def.big_blind = 2;
  def.ante = 0;
  for (std::size_t i = 0; i < seat_count; ++i) {
    def.stacks[i] = static_cast<Chips>(2 * stack_bb);
    def.contributions[i] = 1;
  }
  def.pot = static_cast<Chips>(seat_count);
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), river_rooted ? card("9c") : 0};
  def.board_size = river_rooted ? 5 : 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// 2-seat, river-rooted: flop check-through, turn check-through, river 9c.
GameDef make_subgame_def_2p_river() {
  GameDef def = make_subgame_def_2p_turn();
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  def.board_size = 5;
  return def;
}

// 3-seat, river-rooted: flop check-through, turn check-through, river 9c.
GameDef make_subgame_def_3p_river() {
  GameDef def = make_subgame_def_3p_turn();
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  def.board_size = 5;
  return def;
}

// 4-seat, river-rooted: flop check-through, turn check-through, river 9c.
GameDef make_subgame_def_4p_river() {
  GameDef def = make_subgame_def_4p_turn();
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  def.board_size = 5;
  return def;
}

// 3-seat, turn-rooted, seat 2 folded on the flop. The folded seat's 1 BB
// contribution is dead money. Two live seats play for the 3 BB pot.
GameDef make_subgame_def_3p_turn_folded() {
  GameDef def = make_subgame_def_3p_turn();
  def.folded[2] = true;
  return def;
}

// 3-seat, river-rooted, seat 2 folded on the flop. Two live seats play
// through the turn and reach the river.
GameDef make_subgame_def_3p_river_folded() {
  GameDef def = make_subgame_def_3p_river();
  def.folded[2] = true;
  return def;
}

// Declared-only schedule: one 1/2-pot bet and one 1/2-pot raise per street.
// This keeps the tree small enough for exact NashConv enumeration while
// exercising the full turn → river → showdown structure.
SizeSchedule make_coarse_schedule() {
  SizeSchedule schedule{};
  StreetSizes postflop;
  postflop.bets = {{1, 2}};
  postflop.raises = {{1, 2}};
  schedule[static_cast<std::size_t>(Street::Flop)] = postflop;
  schedule[static_cast<std::size_t>(Street::Turn)] = postflop;
  schedule[static_cast<std::size_t>(Street::River)] = postflop;
  return schedule;
}

// 10 representative hands per seat, none blocking the board 2c 3d 7h Js.
// Covers pairs, suited/offsuit broadway, and suited connectors.
std::vector<std::array<int, 2>> make_representative_range() {
  std::vector<std::array<int, 2>> range;
  range.push_back(hand("Ac", "Ad"));  // AA
  range.push_back(hand("Kc", "Kd"));  // KK
  range.push_back(hand("Qc", "Qd"));  // QQ
  range.push_back(hand("Jc", "Jd"));  // JJ
  range.push_back(hand("Tc", "Td"));  // TT
  range.push_back(hand("9c", "9d"));  // 99
  range.push_back(hand("Ac", "Kc"));  // AKs
  range.push_back(hand("Ac", "Qc"));  // AQs
  range.push_back(hand("Ac", "Kd"));  // AKo
  range.push_back(hand("9c", "8c"));  // 98s
  return range;
}

// ---------------------------------------------------------------------------
// NashConv computation over all card-compatible joint deals (N-way)
// ---------------------------------------------------------------------------

struct NashConvResult {
  double nash_conv = 0.0;
  double exploitability = 0.0;
  double exploitability_bb = 0.0;
  std::size_t joint_deals = 0;
  std::size_t fallbacks = 0;
  std::vector<double> strategy_value;
  std::vector<double> br_value;
};

// Multi-traverser walker: walks the tree ONCE per joint deal and returns every
// seat's utility in a fixed-size array. This replaces the old per-traverser
// enumerate_joint_deals, which walked the tree seat_count times per deal and
// re-evaluated every frontier leaf each time. The frontier evaluator is the
// dominant cost at depth-limited leaves, so batching all seats into one walk
// cuts frontier evaluations by a factor of seat_count.
struct MultiValueWalker {
  const AbstractTree& tree;
  const NSeatPolicy* policy = nullptr;  // null = uniform baseline
  std::size_t fallbacks = 0;
  const FrontierEvaluator* frontier = nullptr;

  std::array<double, kMaxUnifiedSeats> walk(const GameState& state, std::size_t node_index,
                                            const std::vector<std::array<int, 2>>& holes) {
    std::array<double, kMaxUnifiedSeats> result{};
    const auto& node = tree.node(node_index);
    const std::size_t n = holes.size();

    if (node.kind == NodeKind::TerminalFold) {
      const auto& util = tree.terminal(node.terminal).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }

    if (node.kind == NodeKind::TerminalShowdown) {
      const auto& payload = tree.terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = holes[payload.live_order[i]];
      const auto& util = state.settle_showdown(live).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }

    if (node.kind == NodeKind::TerminalFrontier) {
      const auto& payload = tree.terminal(node.terminal);
      const auto board = std::vector<int>(state.board().begin(), state.board().end());
      if (board.size() == 5) {
        // River: direct showdown, bypass frontier evaluator cache overhead.
        const auto utils = direct_showdown_utility(board, holes, payload);
        for (std::size_t i = 0; i < n; ++i)
          result[i] = utils[i];
      } else {
        if (!frontier)
          throw std::runtime_error(
              "multi-value walker: TerminalFrontier without frontier evaluator");
        const auto utils = frontier->evaluate(board, holes, payload);
        for (std::size_t i = 0; i < n; ++i)
          result[i] = utils[i];
      }
      return result;
    }

    if (node.is_chance()) {
      const auto legal = legal_runout_cards(state, holes);
      for (int c : legal) {
        const std::size_t ordinal = board_only_ordinal(state, c);
        if (ordinal >= node.children.size())
          throw std::runtime_error("multi-value walker: chance ordinal out of range");
        auto child = walk(state.after_card(c), node.children[ordinal], holes);
        for (std::size_t i = 0; i < n; ++i)
          result[i] += child[i];
      }
      const double inv = 1.0 / static_cast<double>(legal.size());
      for (std::size_t i = 0; i < n; ++i)
        result[i] *= inv;
      return result;
    }

    if (!node.is_action())
      throw std::runtime_error("multi-value walker: unexpected node kind");

    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;
    const auto board = std::vector<int>(state.board().begin(), state.board().end());

    // Policy probabilities for the actor (same regardless of traverser).
    std::vector<double> probs(menu.size(), 1.0 / static_cast<double>(menu.size()));
    if (policy) {
      const auto bucket =
          static_cast<std::uint32_t>(card_bucket(kNSeatCardKind, holes[actor], board));
      const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node_index)};
      const auto it = policy->rows().find(key);
      if (it != policy->rows().end()) {
        const auto& row = it->second;
        if (row.actions != menu)
          throw std::runtime_error("multi-value walker: row menu drifted from tree node");
        probs = row.probabilities;
      } else {
        ++fallbacks;
      }
    }

    for (std::size_t a = 0; a < menu.size(); ++a) {
      auto child = walk(state.after_action(actor, menu[a]), node.children[a], holes);
      for (std::size_t i = 0; i < n; ++i)
        result[i] += probs[a] * child[i];
    }
    return result;
  }
};

#ifdef _OPENMP
#include <omp.h>
#endif

// Compute strategy values for ALL seats in one tree walk per joint deal.
// Parallelized across joint deals with OpenMP.
void compute_strategy_values(const AbstractTree& tree, const NSeatPolicy* policy,
                             const GameDef& def,
                             const std::vector<std::vector<std::array<int, 2>>>& deals,
                             std::vector<double>& strategy_sum, std::size_t& total_fallbacks,
                             const FrontierEvaluator* frontier = nullptr) {
  const GameState root(def);
  const std::size_t seat_count = def.player_count;
#ifdef _OPENMP
  const int num_threads = omp_get_max_threads();
#else
  const int num_threads = 1;
#endif

  std::vector<std::vector<double>> sums_per_thread(num_threads,
                                                   std::vector<double>(seat_count, 0.0));
  std::vector<std::size_t> fallbacks_per_thread(num_threads, 0);

#pragma omp parallel for schedule(dynamic)
  for (std::ptrdiff_t d = 0; d < static_cast<std::ptrdiff_t>(deals.size()); ++d) {
#ifdef _OPENMP
    const int tid = omp_get_thread_num();
#else
    const int tid = 0;
#endif
    const auto& holes = deals[d];
    MultiValueWalker walker{tree, policy, 0, frontier};
    auto utils = walker.walk(root, tree.root_index(), holes);
    for (std::size_t p = 0; p < seat_count; ++p) {
      if (def.folded[p])
        continue;
      sums_per_thread[tid][p] += utils[p];
    }
    fallbacks_per_thread[tid] += walker.fallbacks;
  }

  strategy_sum.assign(seat_count, 0.0);
  total_fallbacks = 0;
  for (int t = 0; t < num_threads; ++t) {
    for (std::size_t p = 0; p < seat_count; ++p)
      strategy_sum[p] += sums_per_thread[t][p];
    total_fallbacks += fallbacks_per_thread[t];
  }
}

// Forward declarations: information-set-consistent best response and joint
// deal collection (defined below).
std::vector<std::vector<std::array<int, 2>>> collect_joint_deals(
    const std::vector<std::array<int, 2>>& range, std::size_t seat_count,
    const std::array<bool, kMaxUnifiedSeats>& folded);
double compute_pooled_br_value(const AbstractTree& tree, const NSeatPolicy* policy,
                               const std::vector<std::vector<std::array<int, 2>>>& deals,
                               const GameDef& def, std::size_t traverser,
                               const FrontierEvaluator* frontier);

NashConvResult compute_nash_conv(const AbstractTree& tree, const NSeatPolicy* policy,
                                 const std::vector<std::array<int, 2>>& range, const GameDef& def,
                                 const FrontierEvaluator* frontier = nullptr) {
  const std::size_t seat_count = def.player_count;

  // Filter out hands that block the board — they cannot be dealt in a real
  // game and would produce incorrect showdown settlements.
  std::vector<std::array<int, 2>> live_range;
  for (const auto& h : range) {
    if (!blocks_board(h, def.board, def.board_size))
      live_range.push_back(h);
  }

  // Collect joint deals once; shared across strategy-value and BR walks.
  const auto deals = collect_joint_deals(live_range, seat_count, def.folded);

  // Strategy values: one tree walk per joint deal, all seats at once.
  std::vector<double> strategy_sum;
  std::size_t total_fallbacks = 0;
  compute_strategy_values(tree, policy, def, deals, strategy_sum, total_fallbacks, frontier);

  NashConvResult result;
  result.strategy_value.resize(seat_count);
  result.br_value.resize(seat_count);
  result.joint_deals = deals.size();
  result.fallbacks = total_fallbacks;

  // Per-traverser best response is independent: parallelize across traversers.
  // Each thread writes to a distinct element of strategy_value/br_value.
  double nash_conv = 0.0;
#pragma omp parallel for reduction(+ : nash_conv) schedule(dynamic)
  for (std::size_t p = 0; p < seat_count; ++p) {
    if (def.folded[p]) {
      result.strategy_value[p] = 0.0;
      result.br_value[p] = 0.0;
      continue;
    }
    result.strategy_value[p] = strategy_sum[p] / static_cast<double>(deals.size());
    // Information-set-consistent best response: pool joint deals by the
    // traverser's own holding and choose one action per information set.
    result.br_value[p] = compute_pooled_br_value(tree, policy, deals, def, p, frontier);
    nash_conv += result.br_value[p] - result.strategy_value[p];
  }
  result.nash_conv = nash_conv;
  const bs::solver::NashConvNormalisation units = bs::solver::normalise_nash_conv(def, nash_conv);
  result.exploitability = units.exploitability;
  result.exploitability_bb = units.exploitability_bb;
  return result;
}

// ---------------------------------------------------------------------------
// Information-set-consistent best response (pooled walk)
// ---------------------------------------------------------------------------
//
// The per-deal walker above chooses the best action separately for each joint
// deal, allowing the traverser to condition on opponents' unrevealed cards.
// That is an omniscient upper bound, not a best response. The pooled walk
// below groups joint deals by the traverser's OWN holding (which, together
// with the public history, defines the information set) and chooses ONE
// action per group, pooling all opponent holdings. This matches the
// exact_best_response_utility algorithm in engine/src/stage6/exact_oracle.cpp.

// Fixed-size holes avoid a per-view heap allocation at every tree node.
// kMaxUnifiedSeats is 10, so a BRView is ~96 bytes — cheap to copy in bulk.
struct BRView {
  std::array<std::array<int, 2>, kMaxUnifiedSeats> holes{};
  std::size_t seat_count = 0;
  double reach = 0.0;  // probability of reaching this public node
};

struct PooledValue {
  double value = 0.0;  // unnormalized sum of reach-weighted traverser utility
  double mass = 0.0;   // total reach mass accumulated
};

// Pooled best-response walk for ONE traverser holding shared by every view.
// At traverser action nodes, tries each menu action and picks the one with the
// best POOLED value (one action for the whole information set). At opponent
// action nodes, branches on each action with per-view policy probabilities.
// At chance nodes, splits each view's reach over available runout cards.
// At terminals, accumulates per-view utility weighted by reach.
PooledValue pooled_br_walk(const GameState& state, std::size_t node_index,
                           std::vector<BRView> views, const AbstractTree& tree,
                           const NSeatPolicy* policy, std::size_t traverser,
                           const FrontierEvaluator* frontier) {
  PooledValue out;
  std::vector<BRView*> live;
  for (BRView& v : views)
    if (v.reach > 0.0)
      live.push_back(&v);
  if (live.empty())
    return out;

  const auto& node = tree.node(node_index);

  if (node.kind == NodeKind::TerminalFold) {
    const double util = static_cast<double>(tree.terminal(node.terminal).chip_utility[traverser]);
    for (BRView* v : live) {
      out.mass += v->reach;
      out.value += v->reach * util;
    }
    return out;
  }

  if (node.kind == NodeKind::TerminalShowdown) {
    const auto& payload = tree.terminal(node.terminal);
    for (BRView* v : live) {
      std::vector<std::array<int, 2>> live_holes(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live_holes[i] = v->holes[payload.live_order[i]];
      const double util =
          static_cast<double>(state.settle_showdown(live_holes).chip_utility[traverser]);
      out.mass += v->reach;
      out.value += v->reach * util;
    }
    return out;
  }

  if (node.kind == NodeKind::TerminalFrontier) {
    const auto& payload = tree.terminal(node.terminal);
    const auto board = std::vector<int>(state.board().begin(), state.board().end());
    const bool river = board.size() == 5;
    if (!river && !frontier)
      throw std::runtime_error("pooled-br: TerminalFrontier leaf without frontier evaluator");
    for (BRView* v : live) {
      double util;
      if (river) {
        // Direct showdown: bypass frontier evaluator cache/mutex overhead.
        std::array<std::array<int, 2>, kMaxUnifiedSeats> hands{};
        for (std::size_t i = 0; i < v->seat_count; ++i)
          hands[i] = v->holes[i];
        const auto utils = direct_showdown_utility(
            board, std::span<const std::array<int, 2>>(hands.data(), v->seat_count), payload);
        util = utils[traverser];
      } else {
        std::array<std::array<int, 2>, kMaxUnifiedSeats> hands{};
        for (std::size_t i = 0; i < v->seat_count; ++i)
          hands[i] = v->holes[i];
        const auto utils = frontier->evaluate(
            board, std::span<const std::array<int, 2>>(hands.data(), v->seat_count), payload);
        util = utils[traverser];
      }
      out.mass += v->reach;
      out.value += v->reach * util;
    }
    return out;
  }

  if (node.is_chance()) {
    // Precompute per-view used masks and available counts once.
    std::vector<std::array<bool, 52>> view_used(live.size());
    std::vector<std::size_t> view_avail(live.size());
    for (std::size_t i = 0; i < live.size(); ++i) {
      std::array<bool, 52> used{};
      for (int c : state.board())
        used[c] = true;
      for (std::size_t s = 0; s < live[i]->seat_count; ++s) {
        const auto& h = live[i]->holes[s];
        if (h[0] >= 0)
          used[h[0]] = true;
        if (h[1] >= 0)
          used[h[1]] = true;
      }
      std::size_t avail = 0;
      for (int c = 0; c < 52; ++c)
        if (!used[c])
          ++avail;
      if (avail == 0)
        throw std::runtime_error("pooled-br: chance node with no available card");
      view_used[i] = used;
      view_avail[i] = avail;
    }
    for (int card = 0; card < 52; ++card) {
      std::vector<BRView> children;
      children.reserve(live.size());
      for (std::size_t i = 0; i < live.size(); ++i) {
        if (view_used[i][card])
          continue;
        BRView child = *live[i];  // fixed-size copy: no per-view heap alloc
        child.reach /= static_cast<double>(view_avail[i]);
        children.push_back(child);
      }
      if (children.empty())
        continue;
      const std::size_t ordinal = board_only_ordinal(state, card);
      if (ordinal >= node.children.size())
        throw std::runtime_error("pooled-br: chance ordinal out of range");
      PooledValue child = pooled_br_walk(state.after_card(card), node.children[ordinal],
                                         std::move(children), tree, policy, traverser, frontier);
      out.value += child.value;
      out.mass += child.mass;
    }
    return out;
  }

  if (!node.is_action())
    throw std::runtime_error("pooled-br: unexpected node kind");

  const std::size_t actor = *state.actor();
  const auto& menu = node.actions;
  const auto board = std::vector<int>(state.board().begin(), state.board().end());

  if (actor == traverser) {
    // One action for the whole information set. Try each menu action and
    // pick the one with the best pooled traverser value.
    bool have_best = false;
    PooledValue best;
    for (std::size_t a = 0; a < menu.size(); ++a) {
      std::vector<BRView> children;
      children.reserve(live.size());
      for (BRView* v : live)
        children.push_back(*v);  // fixed action, reach unchanged
      PooledValue child = pooled_br_walk(state.after_action(actor, menu[a]), node.children[a],
                                         std::move(children), tree, policy, traverser, frontier);
      if (!have_best || child.value > best.value) {
        best = std::move(child);
        have_best = true;
      }
    }
    return best;
  }

  // Opponent node: branch on each action, multiply each view's reach by the
  // policy probability (which depends on the opponent's own hole cards).
  for (std::size_t a = 0; a < menu.size(); ++a) {
    std::vector<BRView> children;
    children.reserve(live.size());
    for (BRView* v : live) {
      // Get policy probability for this opponent action.
      const auto bucket =
          static_cast<std::uint32_t>(card_bucket(kNSeatCardKind, v->holes[actor], board));
      const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node_index)};
      double prob = 0.0;
      if (policy) {
        const auto it = policy->rows().find(key);
        if (it != policy->rows().end()) {
          const auto& row = it->second;
          if (row.actions == menu && a < row.probabilities.size())
            prob = row.probabilities[a];
        }
      } else {
        prob = 1.0 / static_cast<double>(menu.size());
      }
      if (prob > 0.0) {
        BRView child = *v;
        child.reach *= prob;
        children.push_back(child);
      }
    }
    if (children.empty())
      continue;
    PooledValue child = pooled_br_walk(state.after_action(actor, menu[a]), node.children[a],
                                       std::move(children), tree, policy, traverser, frontier);
    out.value += child.value;
    out.mass += child.mass;
  }
  return out;
}

// Collect all card-compatible joint deals into a flat vector.  Folded seats
// carry a {-1,-1} sentinel: their hole cards are unknown and are NOT
// enumerated here.  This is an explicit analytic marginalization under the
// uniform unconditioned fold-range model, not an omission.  Symmetry proof:
// after fixing the board and every live seat's holes, the R remaining cards
// are exchangeable between the folded seat's (unknown) two cards and every
// runout position — C(R-k,2) / [C(R,2)·prod_j(R-2-j)] = 1/[R·(R-1)…] — so
// each runout sequence has the same marginal probability it would have if
// the folded seat were never dealt.  legal_runout_cards() realizes that
// marginal by skipping the sentinel.  A conditioned fold-range breaks the
// symmetry and requires explicit enumeration of the folded seat's range;
// that model is not used by these fixtures.
std::vector<std::vector<std::array<int, 2>>> collect_joint_deals(
    const std::vector<std::array<int, 2>>& range, std::size_t seat_count,
    const std::array<bool, kMaxUnifiedSeats>& folded) {
  std::vector<std::vector<std::array<int, 2>>> deals;
  std::vector<std::array<int, 2>> holes;
  holes.reserve(seat_count);
  std::function<void(std::size_t)> recurse = [&](std::size_t current_seat) {
    if (current_seat == seat_count) {
      deals.push_back(holes);
      return;
    }
    if (folded[current_seat]) {
      holes.push_back({-1, -1});
      recurse(current_seat + 1);
      holes.pop_back();
      return;
    }
    for (const auto& h : range) {
      bool collides = false;
      for (std::size_t s = 0; s < current_seat; ++s) {
        if (folded[s])
          continue;
        if (hands_collide(h, holes[s])) {
          collides = true;
          break;
        }
      }
      if (!collides) {
        holes.push_back(h);
        recurse(current_seat + 1);
        holes.pop_back();
      }
    }
  };
  recurse(0);
  return deals;
}

// Compute the information-set-consistent best response value for one traverser.
// Groups joint deals by the traverser's own holding, then walks each group
// with the pooled walker. Returns the normalized best response value.
double compute_pooled_br_value(const AbstractTree& tree, const NSeatPolicy* policy,
                               const std::vector<std::vector<std::array<int, 2>>>& deals,
                               const GameDef& def, std::size_t traverser,
                               const FrontierEvaluator* frontier) {
  const GameState root(def);
  if (deals.empty())
    return 0.0;

  // Group deals by the traverser's own holding.
  std::map<std::pair<int, int>, std::vector<BRView>> groups;
  for (const auto& holes : deals) {
    const auto& own = holes[traverser];
    BRView view;
    view.seat_count = def.player_count;
    view.reach = 1.0;
    for (std::size_t i = 0; i < def.player_count; ++i)
      view.holes[i] = holes[i];
    groups[{std::min(own[0], own[1]), std::max(own[0], own[1])}].push_back(view);
  }

  double total_value = 0.0;
  double total_mass = 0.0;
  for (auto& [key, views] : groups) {
    (void)key;
    PooledValue g = pooled_br_walk(root, tree.root_index(), std::move(views), tree, policy,
                                   traverser, frontier);
    total_value += g.value;
    total_mass += g.mass;
  }
  if (total_mass <= 0.0)
    return 0.0;
  return total_value / total_mass;
}

// ---------------------------------------------------------------------------
// Coverage measurement
// ---------------------------------------------------------------------------

struct CoverageResult {
  std::size_t total_action_nodes = 0;
  std::size_t covered_action_nodes = 0;
  std::size_t total_information_sets = 0;
  std::size_t covered_information_sets = 0;
};

CoverageResult compute_coverage(const AbstractTree& tree, const NSeatPolicy& policy,
                                const std::vector<std::array<int, 2>>& range) {
  CoverageResult result;
  for (const auto& node : tree.nodes()) {
    if (!node.is_action())
      continue;
    ++result.total_action_nodes;
    bool covered = false;
    for (const auto& hand : range) {
      const auto board = std::vector<int>(tree.def().board.begin(),
                                          tree.def().board.begin() + tree.def().board_size);
      const auto bucket = static_cast<std::uint32_t>(card_bucket(kNSeatCardKind, hand, board));
      const NSeatInformationKey key{bucket, node.index};
      if (policy.rows().count(key)) {
        covered = true;
        break;
      }
    }
    if (covered)
      ++result.covered_action_nodes;
  }
  result.total_information_sets = policy.rows().size();
  result.covered_information_sets = policy.rows().size();
  return result;
}

// ---------------------------------------------------------------------------
// Fixture runner
// ---------------------------------------------------------------------------

int run_fixture(const std::string& label, const GameDef& def,
                const std::vector<std::array<int, 2>>& range, const SizeSchedule& schedule,
                std::uint64_t iterations, std::uint64_t seed, int solve_wall_seconds = 120,
                bool skip_nash_conv = false, DepthLimit depth_limit = DepthLimit::none(),
                const FrontierEvaluator* frontier = nullptr,
                const ActionAbstraction* action_override = nullptr) {
  const std::size_t seat_count = def.player_count;
  const bool river_rooted = def.board_size == 5;

  std::printf("\n========================================================================\n");
  std::printf("  %s\n", label.c_str());
  std::printf("========================================================================\n");
  std::printf("  seats:        %zu (%s)\n", seat_count,
              river_rooted ? "river-rooted" : "turn-rooted");
  std::size_t folded_count = 0;
  for (std::size_t p = 0; p < seat_count; ++p)
    if (def.folded[p])
      ++folded_count;
  if (folded_count > 0)
    std::printf("  folded:       %zu seat(s) (dead money)\n", folded_count);
  std::printf("  stacks:       %.0f BB each (%.0f chips)\n",
              def.stacks[0] / static_cast<double>(def.big_blind),
              static_cast<double>(def.stacks[0]));
  std::printf("  pot:          %.1f BB (%lld chips)\n",
              def.pot / static_cast<double>(def.big_blind), static_cast<long long>(def.pot));
  std::printf("  abstraction:  declared-only, 1/2-pot bet + 1/2-pot raise\n");
  std::printf("  iterations:   %llu\n", static_cast<unsigned long long>(iterations));
  std::printf("  seed:         %llu\n", static_cast<unsigned long long>(seed));
  std::fflush(stdout);

  // Build the tree.
  ActionAbstraction default_action =
      ActionAbstraction::declared(schedule, CoverSeeds::DeclaredOnly);
  const ActionAbstraction& action = action_override ? *action_override : default_action;
  TreeLimits tree_limits;
  std::unique_ptr<AbstractTree> tree_ptr;
  const auto build_start = std::chrono::steady_clock::now();
  try {
    tree_ptr = std::make_unique<AbstractTree>(def, action, tree_limits, depth_limit);
  } catch (const tree_resource_exhausted& e) {
    // Exceeding the 1 GiB TreeLimits::max_bytes cap is a measurement result,
    // not a benchmark failure: report it as a finding and move on.
    std::fprintf(stderr, "  tree build exceeded the 1 GiB cap: %s\n", e.what());
    std::fprintf(stderr,
                 "  FINDING: the %zu-seat %s subgame tree does NOT fit the 1 GiB "
                 "TreeLimits::max_bytes cap at this abstraction; the resolver "
                 "cannot build it without a smaller action schedule.\n",
                 seat_count, river_rooted ? "river-rooted" : "turn-rooted");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  tree build failed: %s\n", e.what());
    return 1;
  }
  const auto build_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - build_start);
  const AbstractTree& tree = *tree_ptr;

  std::printf("\nTree:\n");
  std::printf("  nodes:        %zu\n", tree.size());
  std::printf("  depth:        %zu\n", tree.depth());
  std::printf("  build time:   %.1f ms\n", build_elapsed.count() / 1.0);
  std::printf("  accounted:    %.1f KiB\n", tree.accounted_bytes() / 1024.0);
  std::printf("  1 GiB cap:    fits (%.1f MiB of 1024 MiB)\n",
              tree.accounted_bytes() / (1024.0 * 1024.0));

  std::size_t action_nodes = 0, chance_nodes = 0, fold_leaves = 0, showdown_leaves = 0,
              frontier_leaves = 0;
  for (const auto& node : tree.nodes()) {
    switch (node.kind) {
      case NodeKind::Action:
        ++action_nodes;
        break;
      case NodeKind::Chance:
        ++chance_nodes;
        break;
      case NodeKind::TerminalFold:
        ++fold_leaves;
        break;
      case NodeKind::TerminalShowdown:
        ++showdown_leaves;
        break;
      case NodeKind::TerminalFrontier:
        ++frontier_leaves;
        break;
      default:
        break;
    }
  }
  std::printf("  action:       %zu\n", action_nodes);
  std::printf("  chance:       %zu\n", chance_nodes);
  std::printf("  fold leaves:  %zu\n", fold_leaves);
  std::printf("  showdown:     %zu\n", showdown_leaves);
  if (frontier_leaves > 0)
    std::printf("  frontier:     %zu (depth-limited)\n", frontier_leaves);
  std::fflush(stdout);

  // Build ranges (filtered by blocks_board).
  std::vector<std::vector<WeightedHand>> ranges(seat_count);
  for (std::size_t p = 0; p < seat_count; ++p) {
    for (const auto& h : range) {
      if (blocks_board(h, def.board, def.board_size))
        continue;
      ranges[p].push_back({h, 1.0});
    }
  }
  std::printf("  live ranges:  %zu / %zu hands\n", ranges[0].size(), range.size());
  std::fflush(stdout);

  // Solve with CFR.
  NSeatTrainerLimits trainer_limits;
  trainer_limits.wall = std::chrono::seconds(solve_wall_seconds);

  const auto solve_start = std::chrono::steady_clock::now();
  NSeatTrainingResult train_result;
  try {
    train_result = train_nseat(tree, ranges, iterations, seed, trainer_limits, frontier, nullptr);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  train_nseat failed: %s\n", e.what());
    return 1;
  }
  const auto solve_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - solve_start);

  std::printf("\nCFR Solve:\n");
  std::printf("  termination:  %s\n",
              train_result.termination == NSeatTerminationPhase::Complete    ? "complete"
              : train_result.termination == NSeatTerminationPhase::WallClock ? "wall-clock"
                                                                             : "resource-limit");
  std::printf("  iterations:   %llu\n",
              static_cast<unsigned long long>(train_result.completed_iterations));
  std::printf("  info sets:    %zu\n", train_result.information_sets);
  std::printf("  visits:       %zu\n", train_result.visits);
  std::printf("  wall time:    %.1f ms\n", solve_elapsed.count() / 1.0);
  std::fflush(stdout);

  // Coverage.
  const auto coverage = compute_coverage(tree, train_result.policy, range);
  std::printf("\nCoverage:\n");
  std::printf("  action nodes: %zu / %zu covered (%.1f%%)\n", coverage.covered_action_nodes,
              coverage.total_action_nodes,
              100.0 * coverage.covered_action_nodes /
                  std::max<std::size_t>(1, coverage.total_action_nodes));
  std::printf("  info sets:    %zu sealed\n", coverage.covered_information_sets);
  std::fflush(stdout);

  // NashConv: CFR policy.
  if (skip_nash_conv) {
    std::printf("\nNashConv: skipped (feasibility scan)\n");
    return 0;
  }
  std::printf("\nNashConv (information-set-consistent BR):\n");
  std::fflush(stdout);
  const auto cfr_result = compute_nash_conv(tree, &train_result.policy, range, def, frontier);
  std::printf("  joint deals:  %zu\n", cfr_result.joint_deals);
  std::printf("  fallbacks:    %zu\n", cfr_result.fallbacks);
  for (std::size_t p = 0; p < seat_count; ++p) {
    std::printf("  player %zu:     strategy=%+.2f  br=%+.2f  gap=%+.2f\n", p,
                cfr_result.strategy_value[p], cfr_result.br_value[p],
                cfr_result.br_value[p] - cfr_result.strategy_value[p]);
  }
  std::printf("  CFR NashConv:       %.2f chips\n", cfr_result.nash_conv);
  std::printf("  CFR Exploitability: %.2f chips (%.3f BB)\n", cfr_result.exploitability,
              cfr_result.exploitability_bb);

  // NashConv: uniform baseline.
  const auto uniform_result = compute_nash_conv(tree, nullptr, range, def, frontier);
  std::printf("\n  Uniform NashConv:       %.2f chips\n", uniform_result.nash_conv);
  std::printf("  Uniform Exploitability: %.2f chips (%.3f BB)\n", uniform_result.exploitability,
              uniform_result.exploitability_bb);

  // Reduction.
  const double reduction = uniform_result.exploitability - cfr_result.exploitability;
  const double reduction_bb = uniform_result.exploitability_bb - cfr_result.exploitability_bb;
  const double reduction_pct = 100.0 * reduction / std::max(1e-9, uniform_result.exploitability);
  std::printf("\n  Exploitability reduction: %.2f chips (%.3f BB, %.1f%%)\n", reduction,
              reduction_bb, reduction_pct);

  return 0;
}

// RFC 0036 M1 six-seat enumerable sub-range. The ten-hand T3 range has only
// one six-seat disjoint assignment on the turn board and none on the river
// board (the river card 9c blocks both nine-containing hands, leaving five
// non-colliding pairs). This twelve-hand range keeps the same pair-heavy
// structure but drops the nine dependence through ranks 4/5/6/8, so six
// pairwise disjoint holdings exist on the river too. It stays small enough
// for exact joint-deal enumeration; the full-range six-seat case uses the
// sampled estimator separately (RFC 0036 stage 0).
std::vector<std::array<int, 2>> make_rfc0036_six_range() {
  std::vector<std::array<int, 2>> range;
  range.push_back(hand("Ac", "Ad"));  // AA
  range.push_back(hand("Kc", "Kd"));  // KK
  range.push_back(hand("Qc", "Qd"));  // QQ
  range.push_back(hand("Jc", "Jd"));  // JJ
  range.push_back(hand("Tc", "Td"));  // TT
  range.push_back(hand("8c", "8d"));  // 88
  range.push_back(hand("Ac", "Kc"));  // AKs
  range.push_back(hand("Ac", "Qc"));  // AQs
  range.push_back(hand("Ac", "Kd"));  // AKo
  range.push_back(hand("8c", "6c"));  // 86s
  range.push_back(hand("5h", "4h"));  // 54s
  range.push_back(hand("6h", "5h"));  // 65s
  return range;
}

// RFC 0036 M1: production limped root. 100 BB buy-in (200 chips at big blind
// 2), every seat limps for 2 chips, so the root pot holds 2 chips per seat
// and every stack behind is 198. Same convention as the deployed six-way
// flop library root (RFC 0033). Board 2c 3d 7h Js [9c].
GameDef make_rfc0036_production_root(std::size_t seat_count, bool river_rooted) {
  GameDef def{};
  def.player_count = seat_count;
  def.button = static_cast<int>(seat_count - 1);
  def.big_blind = 2;
  def.ante = 0;
  for (std::size_t i = 0; i < seat_count; ++i) {
    def.stacks[i] = 198;
    def.contributions[i] = 2;
  }
  def.pot = static_cast<Chips>(2 * seat_count);
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), river_rooted ? card("9c") : 0};
  def.board_size = river_rooted ? 5 : 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

struct M1Sample {
  bool declined = false;
  std::string reason;
  std::uint64_t completed_iterations = 0;
  std::size_t information_sets = 0;
  std::size_t joint_deals = 0;
  double max_gap_bb = 0.0;  // largest single live-seat BR gap, in BB
  double sum_gap_bb = 0.0;  // sum of live-seat gaps (= NashConv), in BB
};

// RFC 0036 M1: train one production shape and compute its in-model NashConv
// with the information-set-consistent pooled walker on an enumerable
// sub-range (the representative range, shared with the T3 campaign).
// `depth` is none() for the bounded river tree and streets(1) for the
// street-start turn tree. A build past the 1 GiB builder cap is a decline.
M1Sample measure_rfc0036_m1(const GameDef& def, const ActionAbstraction& action, DepthLimit depth,
                            const std::vector<std::array<int, 2>>& range, std::uint64_t iterations,
                            std::uint64_t seed, const FrontierEvaluator* frontier,
                            int solve_wall_seconds) {
  M1Sample sample;
  std::unique_ptr<AbstractTree> tree_ptr;
  try {
    TreeLimits tree_limits;
    tree_ptr = std::make_unique<AbstractTree>(def, action, tree_limits, depth);
  } catch (const std::exception& e) {
    sample.declined = true;
    sample.reason = std::string("build: ") + e.what();
    return sample;
  }
  const AbstractTree& tree = *tree_ptr;

  std::vector<std::vector<WeightedHand>> ranges(def.player_count);
  for (std::size_t p = 0; p < def.player_count; ++p) {
    for (const auto& h : range) {
      if (blocks_board(h, def.board, def.board_size))
        continue;
      ranges[p].push_back({h, 1.0});
    }
  }

  NSeatTrainerLimits trainer_limits;
  trainer_limits.wall = std::chrono::seconds(solve_wall_seconds);
  NSeatTrainingResult trained;
  try {
    trained = train_nseat(tree, ranges, iterations, seed, trainer_limits, frontier, nullptr);
  } catch (const std::exception& e) {
    sample.declined = true;
    sample.reason = std::string("train: ") + e.what();
    return sample;
  }
  sample.completed_iterations = trained.completed_iterations;
  sample.information_sets = trained.information_sets;

  const NashConvResult nc = compute_nash_conv(tree, &trained.policy, range, def, frontier);
  sample.joint_deals = nc.joint_deals;
  for (std::size_t p = 0; p < def.player_count; ++p) {
    if (def.folded[p])
      continue;
    const double gap_bb =
        (nc.br_value[p] - nc.strategy_value[p]) / static_cast<double>(def.big_blind);
    sample.max_gap_bb = std::max(sample.max_gap_bb, gap_bb);
    sample.sum_gap_bb += gap_bb;
  }
  return sample;
}

// RFC 0036 M1 sweep: bounded river (G1) and street-start turn (G2) production
// shapes, seats 2..6, at increasing iteration budgets, three seeds each. The
// gate (RFC 0036) is per-seat gap < 0.05 BB and sum < 0.15 BB; the calibration
// table then asks whether the smallest passing iteration count fits the
// envelope. Prints CSV; this is evidence for the stage-0 go/no-go, not a
// ctest.
int run_rfc0036_m1_sweep(std::uint64_t max_iterations, std::uint64_t base_seed) {
  const ActionAbstraction capped = ActionAbstraction::identity(/*max_raises=*/1);
  const EquityFrontierEvaluator frontier;
  const std::vector<std::uint64_t> iteration_levels = {100, 300, 1000, 3000, 10000};
  const std::array<std::uint64_t, 3> seed_offsets{0, 1, 2};

  std::printf(
      "shape,seats,iterations,seed,completed,infosets,joint_deals,max_gap_bb,sum_gap_bb,note\n");
  const auto ten_hand_range = make_representative_range();
  const auto six_seat_range = make_rfc0036_six_range();
  for (std::size_t seats = 2; seats <= 6; ++seats) {
    const std::vector<std::array<int, 2>>& range = seats == 6 ? six_seat_range : ten_hand_range;
    // G1: chance-free bounded river tree, exact showdown leaves.
    const GameDef river = make_rfc0036_production_root(seats, /*river_rooted=*/true);
    for (std::uint64_t iters : iteration_levels) {
      if (iters > max_iterations)
        continue;
      for (std::uint64_t off : seed_offsets) {
        const M1Sample s = measure_rfc0036_m1(river, capped, DepthLimit::none(), range, iters,
                                              base_seed + off, nullptr, 120);
        if (s.declined)
          std::printf("river-cap1,%zu,%llu,%llu,DECLINED,,,,,%s\n", seats,
                      static_cast<unsigned long long>(iters),
                      static_cast<unsigned long long>(base_seed + off), s.reason.c_str());
        else
          std::printf("river-cap1,%zu,%llu,%llu,%llu,%zu,%zu,%.6f,%.6f,\n", seats,
                      static_cast<unsigned long long>(iters),
                      static_cast<unsigned long long>(base_seed + off),
                      static_cast<unsigned long long>(s.completed_iterations), s.information_sets,
                      s.joint_deals, s.max_gap_bb, s.sum_gap_bb);
        std::fflush(stdout);
      }
    }
    // G2: street-start turn tree, streets(1), equity frontier at the cut.
    const GameDef turn = make_rfc0036_production_root(seats, /*river_rooted=*/false);
    for (std::uint64_t iters : iteration_levels) {
      if (iters > max_iterations)
        continue;
      for (std::uint64_t off : seed_offsets) {
        const M1Sample s = measure_rfc0036_m1(turn, capped, DepthLimit::streets(1), range, iters,
                                              base_seed + off, &frontier, 120);
        if (s.declined)
          std::printf("turn-cap1-s1,%zu,%llu,%llu,DECLINED,,,,,%s\n", seats,
                      static_cast<unsigned long long>(iters),
                      static_cast<unsigned long long>(base_seed + off), s.reason.c_str());
        else
          std::printf("turn-cap1-s1,%zu,%llu,%llu,%llu,%zu,%zu,%.6f,%.6f,\n", seats,
                      static_cast<unsigned long long>(iters),
                      static_cast<unsigned long long>(base_seed + off),
                      static_cast<unsigned long long>(s.completed_iterations), s.information_sets,
                      s.joint_deals, s.max_gap_bb, s.sum_gap_bb);
        std::fflush(stdout);
      }
    }
  }
  return 0;
}

int run_rfc0036_m1_envelope(std::uint64_t base_seed) {
  // The exact affordable iteration counts at the 1,600 ms p95 envelope from
  // the calibration table (iterations = (1600 - build_ms) / ms_per_iteration):
  //   river 5 seats 3,740 | river 6 seats 1,510
  //   turn  5 seats 2,690 | turn  6 seats   970
  // These are the BEST in-model quality the bounded paths can reach; if M1
  // fails the 0.05 BB/seat and 0.15 BB sum gates here, no routing occurs.
  struct Point {
    std::size_t seats;
    bool river;
    std::uint64_t iters;
  };
  const std::vector<Point> points = {
      {5, true, 3740},
      {6, true, 1510},
      {5, false, 2690},
      {6, false, 970},
  };
  const auto ten_hand_range = make_representative_range();
  const auto six_seat_range = make_rfc0036_six_range();
  const ActionAbstraction capped = ActionAbstraction::identity(/*max_raises=*/1);
  const EquityFrontierEvaluator frontier;
  const std::array<std::uint64_t, 3> seed_offsets{0, 1, 2};

  std::printf(
      "shape,seats,iterations,seed,completed,infosets,joint_deals,max_gap_bb,sum_gap_bb,note\n");
  for (const Point& p : points) {
    const GameDef def = make_rfc0036_production_root(p.seats, p.river);
    const std::vector<std::array<int, 2>>& range = p.seats == 6 ? six_seat_range : ten_hand_range;
    for (std::uint64_t off : seed_offsets) {
      const M1Sample s =
          measure_rfc0036_m1(def, capped, p.river ? DepthLimit::none() : DepthLimit::streets(1),
                             range, p.iters, base_seed + off, p.river ? nullptr : &frontier, 1800);
      const char* shape = p.river ? "river-cap1" : "turn-cap1-s1";
      if (s.declined)
        std::printf("%s,%zu,%llu,%llu,DECLINED,,,,,%s\n", shape, p.seats,
                    static_cast<unsigned long long>(p.iters),
                    static_cast<unsigned long long>(base_seed + off), s.reason.c_str());
      else
        std::printf("%s,%zu,%llu,%llu,%llu,%zu,%zu,%.6f,%.6f,\n", shape, p.seats,
                    static_cast<unsigned long long>(p.iters),
                    static_cast<unsigned long long>(base_seed + off),
                    static_cast<unsigned long long>(s.completed_iterations), s.information_sets,
                    s.joint_deals, s.max_gap_bb, s.sum_gap_bb);
      std::fflush(stdout);
    }
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// RFC 0037 action-abstraction ablation
// ---------------------------------------------------------------------------
//
// Trains a candidate at cap c and evaluates it by an information-set-
// consistent pooled best response WALKING A RICHER cap-r REFERENCE TREE (the
// same GameDef and streets(1) geometry; r >= c). Candidate policy rows are
// re-keyed onto reference nodes by exact action-path correspondence (cap
// suppression only removes deeper aggressive edges, so every candidate node
// has one matching reference node); reference-only (beyond-cap) action nodes
// receive a pinned fill:
//   * passive arm: uniform over legal check/call, else fold (the candidate
//     never invents aggression past its cap);
//   * uniform arm: uniform over the whole legal menu (sensitivity bound).
// E_r(c) = reference-tree NashConv of the projected policy. The identity
// control c == r must reproduce the in-model NashConv I(c) within tolerance;
// a mismatch invalidates the cell.

// Pair every candidate-tree node with the reference node reached by the same
// concrete action/chance path from the root. Returns -1 for unpaired nodes;
// the inverse marks reference nodes the candidate cap can reach.
void pair_nodes_by_path(const AbstractTree& cand, const AbstractTree& ref, std::size_t ci,
                        std::size_t ri, std::vector<long long>& cand_to_ref) {
  cand_to_ref[ci] = static_cast<long long>(ri);
  const TreeNode& cn = cand.nodes()[ci];
  const TreeNode& rn = ref.nodes()[ri];
  if (cn.is_action()) {
    for (std::size_t k = 0; k < cn.children.size(); ++k) {
      const bs::poker::Action wanted = cn.actions[k];
      for (std::size_t j = 0; j < rn.children.size(); ++j) {
        if (ref.nodes()[rn.children[j]].incoming_action == wanted) {
          pair_nodes_by_path(cand, ref, cn.children[k], rn.children[j], cand_to_ref);
          break;
        }
      }
    }
  } else if (cn.is_chance() || cn.is_flop_deal()) {
    for (const std::size_t cc : cn.children) {
      const int wanted_card = cand.nodes()[cc].incoming_card;
      for (const std::size_t rc : rn.children) {
        if (ref.nodes()[rc].incoming_card == wanted_card) {
          pair_nodes_by_path(cand, ref, cc, rc, cand_to_ref);
          break;
        }
      }
    }
  }
}

// Fill probabilities for a beyond-cap reference node. Passive arm: uniform
// over check/call, else fold; every aggressive entry zero. Uniform arm:
// equal over the full menu.
NSeatPolicyRow fill_row(const std::vector<bs::poker::Action>& menu, bool passive_only) {
  NSeatPolicyRow row;
  row.actions = menu;
  row.probabilities.assign(menu.size(), 0.0);
  if (!passive_only) {
    const double p = 1.0 / static_cast<double>(menu.size());
    for (double& q : row.probabilities)
      q = p;
    return row;
  }
  std::vector<std::size_t> passive;
  for (std::size_t i = 0; i < menu.size(); ++i)
    if (menu[i].type == bs::poker::ActionType::Check || menu[i].type == bs::poker::ActionType::Call)
      passive.push_back(i);
  if (passive.empty())
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == bs::poker::ActionType::Fold)
        passive.push_back(i);
  if (!passive.empty()) {
    const double p = 1.0 / static_cast<double>(passive.size());
    for (const std::size_t i : passive)
      row.probabilities[i] = p;
  }
  return row;
}

// Project the trained candidate policy onto the reference tree. Beyond-cap
// action nodes receive the pinned fill for every postflop tier bucket (the
// pooled walker prunes an unrowed opponent node, so the fill must be present
// per bucket).
NSeatPolicy project_policy(const AbstractTree& cand, const AbstractTree& ref,
                           const NSeatPolicy& trained, const std::vector<long long>& cand_to_ref,
                           bool passive_fill) {
  std::map<NSeatInformationKey, NSeatPolicyRow> rows;
  // Re-key reachable candidate rows onto the matching reference node.
  for (const auto& [key, candidate_row] : trained.rows()) {
    if (key.node_index >= cand_to_ref.size() || cand_to_ref[key.node_index] < 0)
      continue;
    const auto ref_index = static_cast<std::size_t>(cand_to_ref[key.node_index]);
    const auto& menu = ref.nodes()[ref_index].actions;
    NSeatPolicyRow projected;
    projected.actions = menu;
    projected.probabilities.assign(menu.size(), 0.0);  // suppressed edges keep zero
    projected.visits = candidate_row.visits;
    for (std::size_t a = 0; a < candidate_row.actions.size(); ++a) {
      for (std::size_t j = 0; j < menu.size(); ++j) {
        if (menu[j] == candidate_row.actions[a]) {
          projected.probabilities[j] = candidate_row.probabilities[a];
          break;
        }
      }
    }
    rows.emplace(NSeatInformationKey{key.own_card_bucket, ref_index}, std::move(projected));
  }
  // Fill every unpaired reference action node (the beyond-cap region).
  std::vector<bool> ref_paired(ref.size(), false);
  for (const long long mapped : cand_to_ref)
    if (mapped >= 0)
      ref_paired[static_cast<std::size_t>(mapped)] = true;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    if (ref_paired[i] || !ref.nodes()[i].is_action())
      continue;
    NSeatPolicyRow row = fill_row(ref.nodes()[i].actions, passive_fill);
    for (std::uint32_t bucket = 1; bucket <= 9; ++bucket)
      rows.emplace(NSeatInformationKey{bucket, static_cast<std::uint64_t>(i)}, row);
  }
  return NSeatPolicy::from_parts(ref.def(), ref.action_id(), std::move(rows));
}

struct AblationOutcome {
  bool declined = false;
  std::string reason;
  std::size_t cand_nodes = 0;
  std::size_t ref_nodes = 0;
  std::size_t cand_infosets = 0;
  double inmodel_nashconv_bb = 0.0;
  double gap_passive_bb = 0.0;
  double gap_uniform_bb = 0.0;
};

// One RFC 0037 cell: candidate cap `cand_cap`, reference cap `ref_cap`,
// turn-rooted streets(1) production identity geometry.
AblationOutcome run_rfc0037_cell(std::size_t seats, int stack_bb, std::uint8_t cand_cap,
                                 std::uint8_t ref_cap, const std::vector<std::array<int, 2>>& range,
                                 std::uint64_t iterations, std::uint64_t seed,
                                 const EquityFrontierEvaluator* frontier) {
  AblationOutcome out;
  GameDef def = make_subgame_def_np(seats, stack_bb, /*river_rooted=*/false);

  std::unique_ptr<AbstractTree> cand_tree;
  std::unique_ptr<AbstractTree> ref_tree;
  try {
    cand_tree = std::make_unique<AbstractTree>(def, ActionAbstraction::identity(cand_cap),
                                               TreeLimits{}, DepthLimit::streets(1));
    ref_tree = std::make_unique<AbstractTree>(def, ActionAbstraction::identity(ref_cap),
                                              TreeLimits{}, DepthLimit::streets(1));
  } catch (const std::exception& e) {
    out.declined = true;
    out.reason = std::string("build: ") + e.what();
    return out;
  }
  out.cand_nodes = cand_tree->size();
  out.ref_nodes = ref_tree->size();

  std::vector<std::vector<WeightedHand>> ranges(def.player_count);
  for (std::size_t p = 0; p < def.player_count; ++p)
    for (const auto& h : range)
      if (!blocks_board(h, def.board, def.board_size))
        ranges[p].push_back({h, 1.0});

  NSeatTrainerLimits trainer_limits;
  trainer_limits.wall = std::chrono::seconds(600);
  NSeatTrainingResult trained;
  try {
    trained = train_nseat(*cand_tree, ranges, iterations, seed, trainer_limits, frontier, nullptr);
  } catch (const std::exception& e) {
    out.declined = true;
    out.reason = std::string("train: ") + e.what();
    return out;
  }
  out.cand_infosets = trained.information_sets;

  std::vector<long long> cand_to_ref(cand_tree->size(), -1);
  pair_nodes_by_path(*cand_tree, *ref_tree, 0, 0, cand_to_ref);

  const double bb = static_cast<double>(def.big_blind);
  const NashConvResult inmodel =
      compute_nash_conv(*cand_tree, &trained.policy, range, def, frontier);
  out.inmodel_nashconv_bb = inmodel.nash_conv / bb;

  for (const bool passive_fill : {true, false}) {
    NSeatPolicy projected =
        project_policy(*cand_tree, *ref_tree, trained.policy, cand_to_ref, passive_fill);
    const NashConvResult translated =
        compute_nash_conv(*ref_tree, &projected, range, def, frontier);
    const double gap = translated.nash_conv / bb;
    if (passive_fill)
      out.gap_passive_bb = gap;
    else
      out.gap_uniform_bb = gap;
  }
  return out;
}

// --rfc0037-ablation seats stack-bb cand-cap ref-cap iterations seed
// Prints one CSV row. The committed checker
// (tools/artifact-audit/rfc0037-ablation-check.py) aggregates repeated seeds.
int run_rfc0037_ablation(int argc, char** argv) {
  if (argc < 8) {
    std::fprintf(stderr,
                 "usage: %s --rfc0037-ablation <seats> <stack-bb> <cand-cap> <ref-cap> "
                 "<iterations> <seed>\n",
                 argv[0]);
    return 2;
  }
  const std::size_t seats = static_cast<std::size_t>(std::atoll(argv[2]));
  const int stack_bb = std::atoi(argv[3]);
  const auto cand_cap = static_cast<std::uint8_t>(std::atoi(argv[4]));
  const auto ref_cap = static_cast<std::uint8_t>(std::atoi(argv[5]));
  const std::uint64_t iterations = std::stoull(argv[6]);
  const std::uint64_t seed = std::stoull(argv[7]);
  const auto range = seats >= 6 ? make_rfc0036_six_range() : make_representative_range();
  const EquityFrontierEvaluator frontier;

  const AblationOutcome r =
      run_rfc0037_cell(seats, stack_bb, cand_cap, ref_cap, range, iterations, seed, &frontier);
  std::printf("rfc0037-ablation,%zu,%d,%u,%u,%llu,%llu,", seats, stack_bb,
              static_cast<unsigned>(cand_cap), static_cast<unsigned>(ref_cap),
              static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(seed));
  if (r.declined) {
    std::printf("INFEASIBLE,%zu,%zu,,,,%s\n", r.cand_nodes, r.ref_nodes, r.reason.c_str());
  } else {
    std::printf("FITS,%zu,%zu,%zu,%.8f,%.8f,%.8f,\n", r.cand_nodes, r.ref_nodes, r.cand_infosets,
                r.inmodel_nashconv_bb, r.gap_passive_bb, r.gap_uniform_bb);
  }
  std::fflush(stdout);
  return 0;
}

// ---------------------------------------------------------------------------
// RFC 0036 M2: river-betting model error
// ---------------------------------------------------------------------------
//
// The M1 in-model NashConv cannot price the error the real river betting
// continuation introduces because the walker and the depth-limited leaf
// share the same all-in-equity model. M2 trains the FULL turn->river
// reference, freezes the cap-1 depth-limited policy's TURN rows, fills the
// reference tree's RIVER nodes two ways (reference equilibrium fill, and
// uniform fill), and evaluates the composed policy in the full reference
// tree. The gap over the reference baseline is the river-model error.
//
// Beyond-cap turn nodes (the cap-1 candidate cannot reach them) get the
// pinned passive fill in both arms, identical to the RFC 0037 ablation, so
// the two arms differ ONLY at river nodes (the model-error isolation).

struct M2Sample {
  bool declined = false;
  std::string reason;
  double reference_nashconv_bb = 0.0;
  double reference_fill_gap_bb = 0.0;
  double uniform_fill_gap_bb = 0.0;
};

M2Sample measure_rfc0036_m2(std::size_t seats, int stack_bb,
                            const std::vector<std::array<int, 2>>& range, std::uint64_t iterations,
                            std::uint64_t seed) {
  M2Sample sample;
  GameDef def = make_subgame_def_np(seats, stack_bb, /*river_rooted=*/false);

  std::unique_ptr<AbstractTree> ref_tree;
  std::unique_ptr<AbstractTree> dl_tree;
  try {
    // Full turn->river reference AND the depth-limited candidate use the
    // SAME production cap-1 menu. M2 isolates the error of replacing the
    // river betting street with the equity frontier, orthogonal to the raise
    // cap (the uncapped production full tree exceeds 1 GiB at every seat
    // count, so no non-proxy cap-0 reference exists; cap 1 is the RFC 0036
    // production shape either way).
    const std::uint8_t ref_cap = 1;
    ref_tree = std::make_unique<AbstractTree>(def, ActionAbstraction::identity(ref_cap),
                                              TreeLimits{}, DepthLimit::none());
    // The bounded production candidate: cap-1 streets(1) with the equity frontier.
    dl_tree = std::make_unique<AbstractTree>(def, ActionAbstraction::identity(/*cap=*/1),
                                             TreeLimits{}, DepthLimit::streets(1));
  } catch (const std::exception& e) {
    sample.declined = true;
    sample.reason = std::string("build: ") + e.what();
    return sample;
  }

  std::vector<std::vector<WeightedHand>> ranges(def.player_count);
  for (std::size_t p = 0; p < def.player_count; ++p)
    for (const auto& h : range)
      if (!blocks_board(h, def.board, def.board_size))
        ranges[p].push_back({h, 1.0});

  NSeatTrainerLimits ref_limits;
  ref_limits.wall = std::chrono::seconds(900);
  NSeatTrainerLimits dl_limits;
  dl_limits.wall = std::chrono::seconds(900);
  const EquityFrontierEvaluator frontier;
  NSeatTrainingResult ref_trained;
  NSeatTrainingResult dl_trained;
  try {
    ref_trained = train_nseat(*ref_tree, ranges, iterations, seed, ref_limits, nullptr, nullptr);
    dl_trained = train_nseat(*dl_tree, ranges, iterations, seed, dl_limits, &frontier, nullptr);
  } catch (const std::exception& e) {
    sample.declined = true;
    sample.reason = std::string("train: ") + e.what();
    return sample;
  }

  // Pair depth-limited candidate nodes onto full-tree nodes by action path.
  // Only TURN action nodes above the streets(1) frontier pair; the
  // candidate's frontier leaves stop recursion (they become the reference's
  // river chance + betting subtree, which is filled separately).
  std::vector<long long> dl_to_ref(dl_tree->size(), -1);
  pair_nodes_by_path(*dl_tree, *ref_tree, 0, 0, dl_to_ref);

  const double bb = static_cast<double>(def.big_blind);
  const NashConvResult ref_nc =
      compute_nash_conv(*ref_tree, &ref_trained.policy, range, def, nullptr);
  sample.reference_nashconv_bb = ref_nc.nash_conv / bb;

  // Build each composed policy on the reference tree's node indices.
  auto compose = [&](bool reference_river_fill) {
    std::map<NSeatInformationKey, NSeatPolicyRow> rows;
    std::vector<bool> turn_paired(ref_tree->size(), false);
    for (const long long mapped : dl_to_ref)
      if (mapped >= 0)
        turn_paired[static_cast<std::size_t>(mapped)] = true;

    for (std::size_t i = 0; i < ref_tree->size(); ++i) {
      const TreeNode& node = ref_tree->nodes()[i];
      if (!node.is_action())
        continue;
      for (std::uint32_t bucket = 1; bucket <= 9; ++bucket) {
        const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(i)};
        NSeatPolicyRow row;
        row.actions = node.actions;

        if (node.street == Street::Turn) {
          if (turn_paired[i]) {
            // Project the candidate's frozen turn row onto this reference node.
            bool found = false;
            for (const auto& [ck, crow] : dl_trained.policy.rows()) {
              if (ck.own_card_bucket != bucket || ck.node_index >= dl_to_ref.size() ||
                  dl_to_ref[ck.node_index] != static_cast<long long>(i))
                continue;
              row.probabilities.assign(node.actions.size(), 0.0);
              for (std::size_t a = 0; a < crow.actions.size(); ++a)
                for (std::size_t j = 0; j < node.actions.size(); ++j)
                  if (node.actions[j] == crow.actions[a])
                    row.probabilities[j] = crow.probabilities[a];
              found = true;
              break;
            }
            if (!found)
              row = fill_row(node.actions, /*passive=*/true);  // beyond-cap turn node
          } else {
            row = fill_row(node.actions, /*passive=*/true);  // beyond-cap turn node
          }
        } else {
          // River node: reference equilibrium fill or the uniform sensitivity arm.
          if (reference_river_fill) {
            const auto it = ref_trained.policy.rows().find(key);
            if (it != ref_trained.policy.rows().end() && it->second.actions == node.actions) {
              row.probabilities = it->second.probabilities;
            } else {
              row = fill_row(node.actions, /*passive=*/false);  // uniform fallback
            }
          } else {
            row = fill_row(node.actions, /*passive=*/false);  // uniform river
          }
        }
        rows.emplace(key, std::move(row));
      }
    }
    return NSeatPolicy::from_parts(ref_tree->def(), ref_tree->action_id(), std::move(rows));
  };

  for (const bool ref_fill : {true, false}) {
    NSeatPolicy composed = compose(ref_fill);
    const NashConvResult nc = compute_nash_conv(*ref_tree, &composed, range, def, nullptr);
    const double gap = nc.nash_conv / bb;
    if (ref_fill)
      sample.reference_fill_gap_bb = gap;
    else
      sample.uniform_fill_gap_bb = gap;
  }
  return sample;
}

int run_rfc0036_m2(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s --rfc0036-m2 <seats> <iterations> <seed>\n", argv[0]);
    return 2;
  }
  const std::size_t seats = static_cast<std::size_t>(std::atoll(argv[2]));
  const std::uint64_t iterations = std::stoull(argv[3]);
  const std::uint64_t seed = std::stoull(argv[4]);
  const auto range = seats >= 6 ? make_rfc0036_six_range() : make_representative_range();
  const M2Sample s = measure_rfc0036_m2(seats, 100, range, iterations, seed);
  std::printf("rfc0036-m2,%zu,100,%llu,%llu,", seats, static_cast<unsigned long long>(iterations),
              static_cast<unsigned long long>(seed));
  if (s.declined)
    std::printf("INFEASIBLE,,,,%s\n", s.reason.c_str());
  else
    std::printf("FITS,%.8f,%.8f,%.8f,\n", s.reference_nashconv_bb, s.reference_fill_gap_bb,
                s.uniform_fill_gap_bb);
  std::fflush(stdout);
  return 0;
}

int main(int argc, char** argv) {
  // T3 feasibility scan: --t3 [iterations] [seed]
  // Runs 2..6 seats x 10/25/100 BB, turn-rooted, with a 30 s solve budget and
  // no NashConv (resource/deadline measurement, not quality certification).
  //
  // T3 depth-limited scan: --t3-dl [iterations] [seed]
  // Same fixtures but with DepthLimit::streets(1) and EquityFrontierEvaluator.
  // Measures the tree-size reduction and solve feasibility that depth-limited
  // resolving provides for 5-6 seat games where the full tree exceeds 1 GiB.
  //
  // T3 depth-limited + raise-cap scan: --t3-dl-cap [iterations] [seed]
  // Same as --t3-dl but with a raise-count cap (1 raise per street) to
  // further reduce the branching factor for 6-seat games.
  bool t3_scan = false;
  bool t3_dl_scan = false;
  bool t3_dl_cap_scan = false;
  bool t3_dl_cap_nc_scan = false;
  bool rfc0036_m1 = false;
  bool rfc0036_m1_envelope = false;
  int arg_index = 1;
  bool rfc0037_ablation = argc > 1 && std::string(argv[1]) == "--rfc0037-ablation";
  if (rfc0037_ablation)
    return run_rfc0037_ablation(argc, argv);
  bool rfc0036_m2 = argc > 1 && std::string(argv[1]) == "--rfc0036-m2";
  if (rfc0036_m2)
    return run_rfc0036_m2(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "--t3") {
    t3_scan = true;
    arg_index = 2;
  } else if (argc > 1 && std::string(argv[1]) == "--t3-dl") {
    t3_dl_scan = true;
    arg_index = 2;
  } else if (argc > 1 && std::string(argv[1]) == "--t3-dl-cap") {
    t3_dl_cap_scan = true;
    arg_index = 2;
  } else if (argc > 1 && std::string(argv[1]) == "--t3-dl-cap-nc") {
    t3_dl_cap_nc_scan = true;
    arg_index = 2;
  } else if (argc > 1 && std::string(argv[1]) == "--rfc0036-m1") {
    rfc0036_m1 = true;
    arg_index = 2;
  } else if (argc > 1 && std::string(argv[1]) == "--rfc0036-m1-envelope") {
    rfc0036_m1_envelope = true;
    arg_index = 2;
  }
  const std::uint64_t iterations = argc > arg_index
                                       ? std::stoull(argv[arg_index])
                                       : (t3_scan ? 100000 : (rfc0036_m1 ? 10000 : 1000));
  const std::uint64_t seed = argc > arg_index + 1 ? std::stoull(argv[arg_index + 1]) : 42;

  if (rfc0036_m1_envelope)
    return run_rfc0036_m1_envelope(seed);
  if (rfc0036_m1)
    return run_rfc0036_m1_sweep(iterations, seed);

  const auto range = make_representative_range();
  const auto schedule = make_coarse_schedule();

  if (t3_scan) {
    std::printf("T3: six-seat normal-stack feasibility scan\n");
    std::printf("  2..6 seats, 10/25/100 BB, turn-rooted, 30 s solve budget, no NashConv\n");
    std::printf("  (resource/deadline measurement; not quality certification)\n");
    int failures = 0;
    for (int stack_bb : {10, 25, 100}) {
      for (std::size_t seats = 2; seats <= 6; ++seats) {
        failures += run_fixture(
            std::to_string(seats) + "-seat turn-rooted " + std::to_string(stack_bb) + " BB",
            make_subgame_def_np(seats, stack_bb, false), range, schedule, iterations, seed,
            /*solve_wall_seconds=*/30,
            /*skip_nash_conv=*/true);
      }
    }
    if (failures == 0)
      std::printf("\nT3 scan completed.\n");
    else
      std::printf("\n%d fixture(s) failed.\n", failures);
    return failures;
  }

  if (t3_dl_scan) {
    std::printf("T3-DL: depth-limited six-seat feasibility scan\n");
    std::printf("  2..6 seats, 10/25/100 BB, turn-rooted, max_streets=1\n");
    std::printf("  EquityFrontierEvaluator, 30 s solve budget, no NashConv\n");
    std::printf("  (resource/deadline measurement; not quality certification)\n");
    const EquityFrontierEvaluator frontier;
    int failures = 0;
    for (int stack_bb : {10, 25, 100}) {
      for (std::size_t seats = 2; seats <= 6; ++seats) {
        failures += run_fixture(
            std::to_string(seats) + "-seat DL turn-rooted " + std::to_string(stack_bb) + " BB",
            make_subgame_def_np(seats, stack_bb, false), range, schedule, iterations, seed,
            /*solve_wall_seconds=*/30,
            /*skip_nash_conv=*/true,
            /*depth_limit=*/DepthLimit::streets(1),
            /*frontier=*/&frontier);
      }
    }
    if (failures == 0)
      std::printf("\nT3-DL scan completed.\n");
    else
      std::printf("\n%d fixture(s) failed.\n", failures);
    return failures;
  }

  if (t3_dl_cap_scan) {
    std::printf("T3-DL-CAP: depth-limited + raise-capped six-seat feasibility scan\n");
    std::printf("  2..6 seats, 10/25/100 BB, turn-rooted, max_streets=1, max_raises=1\n");
    std::printf("  EquityFrontierEvaluator, 30 s solve budget, no NashConv\n");
    std::printf("  (resource/deadline measurement; not quality certification)\n");
    const EquityFrontierEvaluator frontier;
    auto capped_schedule = schedule;
    ActionAbstraction action =
        ActionAbstraction::declared(capped_schedule, CoverSeeds::DeclaredOnly,
                                    /*max_raises_per_street=*/1);
    int failures = 0;
    for (int stack_bb : {10, 25, 100}) {
      for (std::size_t seats = 2; seats <= 6; ++seats) {
        failures += run_fixture(
            std::to_string(seats) + "-seat DL-CAP turn-rooted " + std::to_string(stack_bb) + " BB",
            make_subgame_def_np(seats, stack_bb, false), range, schedule, iterations, seed,
            /*solve_wall_seconds=*/30,
            /*skip_nash_conv=*/true,
            /*depth_limit=*/DepthLimit::streets(1),
            /*frontier=*/&frontier,
            /*action_override=*/&action);
      }
    }
    if (failures == 0)
      std::printf("\nT3-DL-CAP scan completed.\n");
    else
      std::printf("\n%d fixture(s) failed.\n", failures);
    return failures;
  }

  if (t3_dl_cap_nc_scan) {
    std::printf("T3-DL-CAP-NC: depth-limited + raise-capped quality scan\n");
    std::printf("  2..6 seats, 10/25/100 BB, turn-rooted, max_streets=1, max_raises=1\n");
    std::printf("  EquityFrontierEvaluator, 60 s solve budget, NashConv (pooled BR)\n");
    const EquityFrontierEvaluator frontier;
    auto capped_schedule = schedule;
    ActionAbstraction action =
        ActionAbstraction::declared(capped_schedule, CoverSeeds::DeclaredOnly,
                                    /*max_raises_per_street=*/1);
    int failures = 0;
    for (int stack_bb : {10, 25, 100}) {
      for (std::size_t seats = 2; seats <= 6; ++seats) {
        failures += run_fixture(std::to_string(seats) + "-seat DL-CAP-NC turn-rooted " +
                                    std::to_string(stack_bb) + " BB",
                                make_subgame_def_np(seats, stack_bb, false), range, schedule,
                                iterations, seed,
                                /*solve_wall_seconds=*/60,
                                /*skip_nash_conv=*/false,
                                /*depth_limit=*/DepthLimit::streets(1),
                                /*frontier=*/&frontier,
                                /*action_override=*/&action);
      }
    }
    if (failures == 0)
      std::printf("\nT3-DL-CAP-NC scan completed.\n");
    else
      std::printf("\n%d fixture(s) failed.\n", failures);
    return failures;
  }

  std::printf("RFC 0011 Stage 3: Subgame Resolver NashConv Measurement\n");
  std::printf("  2-seat, 3-seat, and 4-seat, turn-rooted and river-rooted subgames\n");

  int failures = 0;
  failures += run_fixture("2-seat turn-rooted", make_subgame_def_2p_turn(), range, schedule,
                          iterations, seed);
  failures += run_fixture("3-seat turn-rooted", make_subgame_def_3p_turn(), range, schedule,
                          iterations, seed);
  failures += run_fixture("4-seat turn-rooted", make_subgame_def_4p_turn(), range, schedule,
                          iterations, seed);
  failures += run_fixture("2-seat river-rooted", make_subgame_def_2p_river(), range, schedule,
                          iterations, seed);
  failures += run_fixture("3-seat river-rooted", make_subgame_def_3p_river(), range, schedule,
                          iterations, seed);
  failures += run_fixture("4-seat river-rooted", make_subgame_def_4p_river(), range, schedule,
                          iterations, seed);
  failures += run_fixture("3-seat turn-rooted, 1 folded", make_subgame_def_3p_turn_folded(), range,
                          schedule, iterations, seed);
  failures += run_fixture("3-seat river-rooted, 1 folded", make_subgame_def_3p_river_folded(),
                          range, schedule, iterations, seed);

  if (failures == 0)
    std::printf("\nAll fixtures completed.\n");
  else
    std::printf("\n%d fixture(s) failed.\n", failures);
  return failures;
}
