// RFC 0011 Stage 3: NashConv measurement for the turn-rooted subgame resolver.
//
// Builds a representative 2-seat turn-rooted River-terminal subgame, solves
// with CFR (train_nseat), and computes an exact NashConv by enumerating every
// card-compatible joint deal and every legal river card. Chance nodes are
// enumerated exactly (the river chance is bounded at 44 cards for 2 seats),
// so the only approximation is the CFR policy itself. Reports tree size,
// solve time, coverage, and exploitability at 100, 1K, and 10K iterations,
// plus a uniform-policy baseline.
//
// The fixture is a heads-up flop check-through on 2c 3d 7h that reaches the
// turn Js, with 10 BB behind and a 1 BB pot. The action abstraction is a
// declared-only schedule with one 1/2-pot bet and one 1/2-pot raise per
// street, which keeps the tree small enough for exact enumeration while
// still exercising the full turn → river → showdown structure.
//
// Usage: bigshark-subgame-nash-conv [iterations] [seed]
//
// This is a manual offline measurement tool, NOT a CTest.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/prng.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
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

// Legal runout cards: {0..51} minus the board and every seat's hole cards.
std::vector<int> legal_runout_cards(const GameState& state,
                                    const std::vector<std::array<int, 2>>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const auto& h : holes)
    used[h[0]] = used[h[1]] = true;
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
// Representative fixture
// ---------------------------------------------------------------------------

// 2-seat, 10 BB behind, 1 BB pot, flop check-through to the turn Js.
GameDef make_subgame_def() {
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
// NashConv walker
// ---------------------------------------------------------------------------

struct NashConvWalker {
  const AbstractTree& tree;
  const NSeatPolicy* policy = nullptr;  // null = uniform baseline
  std::size_t traverser = 0;
  bool best_response = false;
  std::size_t fallbacks = 0;

  std::vector<double> policy_probs(std::size_t actor, std::size_t node_index,
                                   const std::vector<Action>& menu,
                                   const std::array<int, 2>& hole_cards,
                                   const std::vector<int>& board) {
    if (!policy) {
      return std::vector<double>(menu.size(), 1.0 / static_cast<double>(menu.size()));
    }
    const auto bucket = static_cast<std::uint32_t>(card_bucket(kNSeatCardKind, hole_cards, board));
    const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node_index)};
    const auto it = policy->rows().find(key);
    if (it != policy->rows().end()) {
      const auto& row = it->second;
      if (row.actions != menu)
        throw std::runtime_error("subgame-nash-conv: row menu drifted from tree node");
      return row.probabilities;
    }
    ++fallbacks;
    // Match the trainer's own uniform fallback for unvisited rows.
    return std::vector<double>(menu.size(), 1.0 / static_cast<double>(menu.size()));
  }

  double walk(const GameState& state, std::size_t node_index,
              const std::vector<std::array<int, 2>>& holes) {
    const auto& node = tree.node(node_index);

    if (node.kind == NodeKind::TerminalFold)
      return static_cast<double>(tree.terminal(node.terminal).chip_utility[traverser]);

    if (node.kind == NodeKind::TerminalShowdown) {
      const auto& payload = tree.terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = holes[payload.live_order[i]];
      return static_cast<double>(state.settle_showdown(live).chip_utility[traverser]);
    }

    if (node.is_chance()) {
      const auto legal = legal_runout_cards(state, holes);
      double total = 0.0;
      for (int c : legal) {
        const std::size_t ordinal = board_only_ordinal(state, c);
        if (ordinal >= node.children.size())
          throw std::runtime_error("subgame-nash-conv: chance ordinal out of range");
        total += walk(state.after_card(c), node.children[ordinal], holes);
      }
      return total / static_cast<double>(legal.size());
    }

    if (!node.is_action())
      throw std::runtime_error("subgame-nash-conv: unexpected node kind");

    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;
    const auto board = std::vector<int>(state.board().begin(), state.board().end());
    const auto probs = policy_probs(actor, node_index, menu, holes[actor], board);

    if (best_response && actor == traverser) {
      double best = -std::numeric_limits<double>::infinity();
      for (std::size_t a = 0; a < menu.size(); ++a) {
        best = std::max(best, walk(state.after_action(actor, menu[a]), node.children[a], holes));
      }
      return best;
    }

    double total = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a) {
      total += probs[a] * walk(state.after_action(actor, menu[a]), node.children[a], holes);
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// NashConv computation over all card-compatible joint deals
// ---------------------------------------------------------------------------

struct NashConvResult {
  double nash_conv = 0.0;
  double exploitability = 0.0;
  double exploitability_bb = 0.0;
  std::size_t joint_deals = 0;
  std::size_t fallbacks = 0;
  std::array<double, 2> strategy_value{};
  std::array<double, 2> br_value{};
};

NashConvResult compute_nash_conv(const AbstractTree& tree, const NSeatPolicy* policy,
                                 const std::vector<std::array<int, 2>>& range, const GameDef& def) {
  const GameState root(def);
  const double bb = static_cast<double>(def.big_blind);

  std::array<double, 2> strategy_sum{0.0, 0.0};
  std::array<double, 2> br_sum{0.0, 0.0};
  std::size_t joint_deals = 0;
  std::size_t total_fallbacks = 0;

  for (const auto& hero_hand : range) {
    for (const auto& opp_hand : range) {
      if (hands_collide(hero_hand, opp_hand))
        continue;
      const std::vector<std::array<int, 2>> holes = {hero_hand, opp_hand};
      ++joint_deals;

      for (std::size_t p = 0; p < 2; ++p) {
        NashConvWalker walker{tree, policy, p, /*best_response=*/false};
        const double sv = walker.walk(root, tree.root_index(), holes);
        NashConvWalker br_walker{tree, policy, p, /*best_response=*/true};
        const double bv = br_walker.walk(root, tree.root_index(), holes);
        strategy_sum[p] += sv;
        br_sum[p] += bv;
        total_fallbacks += walker.fallbacks + br_walker.fallbacks;
      }
    }
  }

  NashConvResult result;
  result.joint_deals = joint_deals;
  result.fallbacks = total_fallbacks;
  for (std::size_t p = 0; p < 2; ++p) {
    result.strategy_value[p] = strategy_sum[p] / static_cast<double>(joint_deals);
    result.br_value[p] = br_sum[p] / static_cast<double>(joint_deals);
  }
  result.nash_conv = (result.br_value[0] - result.strategy_value[0]) +
                     (result.br_value[1] - result.strategy_value[1]);
  result.exploitability = result.nash_conv / 2.0;
  result.exploitability_bb = result.exploitability / bb;
  return result;
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
// Main
// ---------------------------------------------------------------------------

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t iterations = argc > 1 ? std::stoull(argv[1]) : 1000;
  const std::uint64_t seed = argc > 2 ? std::stoull(argv[2]) : 42;

  const GameDef def = make_subgame_def();
  const auto range = make_representative_range();
  const auto schedule = make_coarse_schedule();

  std::printf("RFC 0011 Stage 3: Subgame Resolver NashConv Measurement\n");
  std::printf("  fixture:      2-seat, flop check-through 2c 3d 7h -> turn Js\n");
  std::printf("  stacks:       %.0f BB each (%.0f chips)\n",
              def.stacks[0] / static_cast<double>(def.big_blind),
              static_cast<double>(def.stacks[0]));
  std::printf("  pot:          %.1f BB (%lld chips)\n",
              def.pot / static_cast<double>(def.big_blind), static_cast<long long>(def.pot));
  std::printf("  abstraction:  declared-only, 1/2-pot bet + 1/2-pot raise\n");
  std::printf("  range:        %zu hands per seat\n", range.size());
  std::printf("  iterations:   %llu\n", static_cast<unsigned long long>(iterations));
  std::printf("  seed:         %llu\n", static_cast<unsigned long long>(seed));
  std::fflush(stdout);

  // Build the tree.
  ActionAbstraction action = ActionAbstraction::declared(schedule, CoverSeeds::DeclaredOnly);
  TreeLimits tree_limits;
  AbstractTree tree(def, action, tree_limits);

  std::printf("\nTree:\n");
  std::printf("  nodes:        %zu\n", tree.size());
  std::printf("  depth:        %zu\n", tree.depth());
  std::printf("  accounted:    %.1f KiB\n", tree.accounted_bytes() / 1024.0);

  std::size_t action_nodes = 0, chance_nodes = 0, fold_leaves = 0, showdown_leaves = 0;
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
      default:
        break;
    }
  }
  std::printf("  action:       %zu\n", action_nodes);
  std::printf("  chance:       %zu\n", chance_nodes);
  std::printf("  fold leaves:  %zu\n", fold_leaves);
  std::printf("  showdown:     %zu\n", showdown_leaves);
  std::fflush(stdout);

  // Build ranges.
  std::vector<std::vector<WeightedHand>> ranges(2);
  for (std::size_t p = 0; p < 2; ++p) {
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
  trainer_limits.wall = std::chrono::seconds(60);

  const auto solve_start = std::chrono::steady_clock::now();
  NSeatTrainingResult train_result;
  try {
    train_result = train_nseat(tree, ranges, iterations, seed, trainer_limits, nullptr, nullptr);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "train_nseat failed: %s\n", e.what());
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
  std::printf("\nNashConv (exact enumeration):\n");
  std::fflush(stdout);
  const auto cfr_result = compute_nash_conv(tree, &train_result.policy, range, def);
  std::printf("  joint deals:  %zu\n", cfr_result.joint_deals);
  std::printf("  fallbacks:    %zu\n", cfr_result.fallbacks);
  std::printf("  player 0:     strategy=%+.2f  br=%+.2f  gap=%+.2f\n", cfr_result.strategy_value[0],
              cfr_result.br_value[0], cfr_result.br_value[0] - cfr_result.strategy_value[0]);
  std::printf("  player 1:     strategy=%+.2f  br=%+.2f  gap=%+.2f\n", cfr_result.strategy_value[1],
              cfr_result.br_value[1], cfr_result.br_value[1] - cfr_result.strategy_value[1]);
  std::printf("  CFR NashConv:       %.2f chips\n", cfr_result.nash_conv);
  std::printf("  CFR Exploitability: %.2f chips (%.3f BB)\n", cfr_result.exploitability,
              cfr_result.exploitability_bb);

  // NashConv: uniform baseline.
  const auto uniform_result = compute_nash_conv(tree, nullptr, range, def);
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
