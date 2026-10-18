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
  const double bb = static_cast<double>(def.big_blind);
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
  std::size_t live_seats = 0;
  for (std::size_t p = 0; p < seat_count; ++p)
    if (!def.folded[p])
      ++live_seats;

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
  result.exploitability = result.nash_conv / static_cast<double>(live_seats);
  result.exploitability_bb = result.exploitability / bb;
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

}  // namespace

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
  int arg_index = 1;
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
  }
  const std::uint64_t iterations =
      argc > arg_index ? std::stoull(argv[arg_index]) : (t3_scan ? 100000 : 1000);
  const std::uint64_t seed = argc > arg_index + 1 ? std::stoull(argv[arg_index + 1]) : 42;

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
        ActionAbstraction::declared(capped_schedule, CoverSeeds::DeclaredOnly);
    action.set_max_raises_per_street(1);
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
        ActionAbstraction::declared(capped_schedule, CoverSeeds::DeclaredOnly);
    action.set_max_raises_per_street(1);
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
