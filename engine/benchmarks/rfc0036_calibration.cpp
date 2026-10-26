// RFC 0036 stage 0 calibration spike.
//
// Measures the production bounded-resolver shapes BEFORE any host routing is
// allowed: the PRODUCTION identity menu with one raise per street and the
// PRODUCTION uniform full-range priors (every board-unblocked combo, weight
// 1.0, the same ranges the live resolver builds in uniform_prior_ranges).
//
// Shapes:
//   G1  river-rooted, River-terminal, chance-free trees with identity(cap=1),
//       seats 2..6. Seats 2..4 are additionally built with identity(cap=0)
//       to cross-check RFC 0011's measured ~2,829-node / 66 ms four-seat
//       uncapped point.
//   G2  street-start turn-rooted, River-terminal, identity(cap=1) with
//       DepthLimit::streets(1) and the EquityFrontierEvaluator, seats 2..6.
//
// Per shape it records the node/byte table (action, chance, and terminal
// leaf counts, depth, accounted bytes), the build-time-per-node coefficient
// (median of repeated builds), the realized information-set count against
// the action-nodes x distinct-buckets upper bound, and the per-iteration
// training cost at several iteration counts (one sampled joint deal per
// traverser sweep per iteration, so cost follows the tree walk, not the
// joint-deal population). A 25 BB vs 100 BB spot check pins the cap-1
// stack independence of the node table.
//
// This is a manual offline measurement tool, NOT a CTest.
//
// Usage: bigshark-rfc0036-calibration [max-iterations] [build-repeats]
//        bigshark-rfc0036-calibration --flop-spike [max-iterations]
//                                     [build-repeats]
//
// The --flop-spike mode is RFC 0037 Stage 0a. It builds the two
// flop-rooted heads-up shapes behind independent spike gates
// (2 live, shallow 10 BB and 2 live, standard 100 BB; production
// identity menu, cap 0, streets(1), equity frontier), records the
// ResolveLimits/envelope verdict per cell, and additionally builds the
// 3-live shallow flop tree as a diagnostic. Measured outcome: shallow
// PASSED and activated (cap 1 -> 0), standard and the 3-live diagnostic
// exceeded the 1 GiB byte cap (standard stays cap 1; the diagnostic
// corroborates cap 1 for every 3+ seat shape). The default campaign is
// unchanged.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::card_bucket;
using bs::gto::EquityFrontierEvaluator;

namespace {

int card_id(const char* name) {
  return bs::cardId(std::string(name));
}

// Production limped root: every seat posts `contribution` chips preflop and
// reaches the observed street. Button is the last seat, so seat 0 acts first
// postflop (multiway convention). Board carries the observed street cards.
// The default is a 100 BB buy-in (200 chips at big blind 2) limped for the
// big blind; the RFC 0011 cross-check reproduces that fixture's 10 BB,
// 1-chip-per-seat root exactly. Every shape is street start.
GameDef make_root(std::size_t seats, int stack_bb, std::span<const int> board,
                  TerminalDepth terminal, Chips contribution = 2, Chips big_blind = 2) {
  GameDef def{};
  def.player_count = seats;
  def.button = static_cast<int>(seats - 1);
  def.big_blind = big_blind;
  def.ante = 0;
  const Chips stack = static_cast<Chips>(big_blind * stack_bb);
  for (std::size_t i = 0; i < seats; ++i) {
    def.stacks[i] = stack - contribution;  // starting_stack - contribution
    def.contributions[i] = contribution;
  }
  def.pot = static_cast<Chips>(contribution * static_cast<Chips>(seats));
  for (std::size_t i = 0; i < board.size(); ++i)
    def.board[i] = board[i];
  def.board_size = static_cast<std::uint8_t>(board.size());
  def.preflop = false;
  def.terminal = terminal;
  return def;
}

// Production uniform priors: every non-blocked 2-card combo at weight 1.0,
// identical ranges for every seat (SubgameResolver::uniform_prior_ranges).
std::vector<std::vector<WeightedHand>> uniform_priors(std::span<const int> board,
                                                      std::size_t seats) {
  std::vector<WeightedHand> range;
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      const std::array<int, 2> cards{c0, c1};
      bool blocked = false;
      for (int b : board)
        if (b == c0 || b == c1) {
          blocked = true;
          break;
        }
      if (!blocked)
        range.push_back({cards, 1.0});
    }
  }
  return std::vector<std::vector<WeightedHand>>(seats, range);
}

struct NodeStats {
  std::size_t nodes = 0;
  std::size_t action = 0;
  std::size_t chance = 0;
  std::size_t fold_leaves = 0;
  std::size_t showdown_leaves = 0;
  std::size_t frontier_leaves = 0;
  std::size_t depth = 0;
  std::size_t bytes = 0;
};

NodeStats node_stats(const AbstractTree& tree) {
  NodeStats s;
  s.nodes = tree.size();
  s.depth = tree.depth();
  s.bytes = tree.accounted_bytes();
  for (const TreeNode& node : tree.nodes()) {
    switch (node.kind) {
      case NodeKind::Action:
        ++s.action;
        break;
      case NodeKind::Chance:
      case NodeKind::FlopDeal:
        ++s.chance;
        break;
      case NodeKind::TerminalFold:
        ++s.fold_leaves;
        break;
      case NodeKind::TerminalShowdown:
        ++s.showdown_leaves;
        break;
      case NodeKind::TerminalFrontier:
        ++s.frontier_leaves;
        break;
    }
  }
  return s;
}

// Distinct N-seat card-bucket values over the production range on this
// board. The serve gate's information-set upper bound is action nodes times
// this count (the N-seat key space is {bucket, node}).
std::size_t distinct_buckets(std::span<const WeightedHand> range, std::span<const int> board) {
  std::set<std::uint32_t> buckets;
  const std::vector<int> board_vec(board.begin(), board.end());
  for (const WeightedHand& hand : range)
    buckets.insert(static_cast<std::uint32_t>(card_bucket(kNSeatCardKind, hand.cards, board_vec)));
  return buckets.size();
}

double median_ms(std::vector<double> samples) {
  if (samples.empty())
    return 0.0;
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

struct ShapeResult {
  std::string label;
  std::size_t seats = 0;
  bool declined_bytes = false;  // tree build exceeded a TreeLimits bound
  std::string decline_reason;
  NodeStats stats;
  double build_ms = 0.0;
  std::size_t buckets = 0;
  std::size_t bound_infosets = 0;
  std::size_t realized_infosets = 0;
  std::uint64_t trained_iterations = 0;
  double train_ms = 0.0;
  double ms_per_iteration = 0.0;
  double visits_per_infoset = 0.0;
};

ShapeResult measure_shape(const std::string& label, const GameDef& def,
                          const ActionAbstraction& action, DepthLimit depth,
                          const std::vector<std::vector<WeightedHand>>& ranges,
                          std::span<const int> board, std::size_t build_repeats,
                          std::uint64_t train_iterations,
                          const bs::gto::FrontierEvaluator* frontier,
                          std::size_t builder_max_nodes = 2'000'000) {
  ShapeResult out;
  out.label = label;
  out.seats = def.player_count;
  out.buckets = distinct_buckets(ranges[0], board);

  std::vector<double> build_samples;
  std::unique_ptr<AbstractTree> trained_tree;
  try {
    for (std::size_t rep = 0; rep < build_repeats; ++rep) {
      TreeLimits limits;  // generous defaults (1 GiB); node cap by argument
      limits.max_nodes = builder_max_nodes;
      const auto t0 = std::chrono::steady_clock::now();
      auto tree = std::make_unique<AbstractTree>(def, action, limits, depth);
      const auto t1 = std::chrono::steady_clock::now();
      build_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      trained_tree = std::move(tree);
    }
  } catch (const tree_resource_exhausted& e) {
    // A shape that cannot be built under a TreeLimits bound is a
    // pre-build decline result, which is exactly what the gate screens for.
    out.declined_bytes = true;
    out.decline_reason = e.what();
    return out;
  }
  out.build_ms = median_ms(std::move(build_samples));
  out.stats = node_stats(*trained_tree);
  out.bound_infosets = out.stats.action * out.buckets;

  NSeatTrainerLimits limits;
  limits.wall = std::chrono::seconds(300);
  const auto t0 = std::chrono::steady_clock::now();
  NSeatTrainingResult result =
      train_nseat(*trained_tree, ranges, train_iterations, /*seed=*/42, limits, frontier, nullptr);
  const auto t1 = std::chrono::steady_clock::now();
  out.train_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  out.trained_iterations = result.completed_iterations;
  out.realized_infosets = result.information_sets;
  if (result.completed_iterations > 0)
    out.ms_per_iteration = out.train_ms / static_cast<double>(result.completed_iterations);
  if (result.information_sets > 0)
    out.visits_per_infoset = static_cast<double>(result.completed_iterations * def.player_count) /
                             static_cast<double>(result.information_sets);
  return out;
}

void print_table(const std::vector<ShapeResult>& rows) {
  std::printf(
      "| shape | seats | result | nodes | action | chance | fold | showdown | "
      "frontier | depth | bytes KiB | build ms | buckets | infosets | bound | "
      "ms/iter | visits/infoset |\n");
  std::printf(
      "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
      "--- | --- | --- | --- | --- | --- | --- |\n");
  for (const ShapeResult& r : rows) {
    if (r.declined_bytes) {
      std::printf(
          "| %s | %zu | DECLINED >1GiB | - | - | - | - | - | - | - | - | - | "
          "- | - | - | - | - |\n",
          r.label.c_str(), r.seats);
      continue;
    }
    std::printf(
        "| %s | %zu | built | %zu | %zu | %zu | %zu | %zu | %zu | %zu | %.1f | %.3f | "
        "%zu | %zu | %zu | %.4f | %.2f |\n",
        r.label.c_str(), r.seats, r.stats.nodes, r.stats.action, r.stats.chance,
        r.stats.fold_leaves, r.stats.showdown_leaves, r.stats.frontier_leaves, r.stats.depth,
        r.stats.bytes / 1024.0, r.build_ms, r.buckets, r.realized_infosets, r.bound_infosets,
        r.ms_per_iteration, r.visits_per_infoset);
  }
  std::fflush(stdout);
}

}  // namespace

// RFC 0037 Stage 0a. The production resolver's ResolveLimits are
// 5,000,000 nodes / 1,000,000 information sets / 1 GiB, and the
// decision envelope is 1,600 ms. A cell activates only when its build
// clears every bound and the envelope projects a positive iteration
// count; each cell is independent.
int run_flop_spike(std::uint64_t max_iterations, std::size_t build_repeats) {
  constexpr std::size_t kResolverMaxNodes = 5'000'000;
  constexpr std::size_t kResolverMaxInfosets = 1'000'000;
  constexpr double kEnvelopeMs = 1600.0;

  const std::array<int, 3> flop_board_arr{card_id("2c"), card_id("3d"), card_id("7h")};
  const std::span<const int> flop_board(flop_board_arr);
  const ActionAbstraction uncapped = ActionAbstraction::identity(/*max_raises=*/0);
  const EquityFrontierEvaluator frontier;

  struct SpikeCell {
    const char* label;
    std::size_t seats;
    int stack_bb;
    bool heads_up_activation;  // false = diagnostic only
  };
  const SpikeCell cells[] = {
      {"flop cap0 2p 10bb", 2, 10, true},
      {"flop cap0 2p 100bb", 2, 100, true},
      {"flop cap0 3p 10bb diagnostic", 3, 10, false},
  };

  std::printf(
      "# RFC 0037 Stage 0a heads-up flop spike "
      "(max_iterations=%llu, build_repeats=%zu)\n",
      static_cast<unsigned long long>(max_iterations), build_repeats);
  std::printf(
      "Production identity rfc0007 menu, cap 0, streets(1),\n"
      "flop-rooted, equity frontier, uniform full-range priors,\n"
      "BB=2, limped contribution 2, seed 42.\n\n");

  std::vector<ShapeResult> rows;
  for (const SpikeCell& cell : cells) {
    GameDef def = make_root(cell.seats, cell.stack_bb, flop_board, TerminalDepth::River,
                            /*contribution=*/2, /*big_blind=*/2);
    auto ranges = uniform_priors(flop_board, cell.seats);
    rows.push_back(measure_shape(cell.label, def, uncapped, DepthLimit::streets(1), ranges,
                                 flop_board, build_repeats, max_iterations, &frontier,
                                 kResolverMaxNodes));
  }
  print_table(rows);

  std::printf("\n## Verdict per cell (ResolveLimits %.0f ms envelope)\n\n", kEnvelopeMs);
  std::printf(
      "| cell | role | nodes <= %zu | infosets <= %zu | bytes <= 1 GiB | "
      "projected iters | verdict |\n",
      kResolverMaxNodes, kResolverMaxInfosets);
  std::printf("| --- | --- | --- | --- | --- | --- | --- |\n");
  for (const ShapeResult& r : rows) {
    const bool heads_up = r.seats == 2;
    if (r.declined_bytes) {
      std::printf("| %s | %s | FAIL | - | DECLINED: %s | - | DROP (%s) |\n", r.label.c_str(),
                  heads_up ? "activation" : "diagnostic", r.decline_reason.c_str(),
                  r.decline_reason.c_str());
      continue;
    }
    const bool nodes_ok = r.stats.nodes <= kResolverMaxNodes;
    const bool infosets_ok = r.realized_infosets <= kResolverMaxInfosets;
    const bool bytes_ok = r.stats.bytes <= (std::size_t{1} << 30);
    const double projected_iters =
        r.ms_per_iteration > 0.0 ? (kEnvelopeMs - r.build_ms) / r.ms_per_iteration : -1.0;
    const bool envelope_ok = projected_iters > 0.0;
    const bool pass = nodes_ok && infosets_ok && bytes_ok && envelope_ok;
    std::printf("| %s | %s | %s | %s | %s | %.0f | %s |\n", r.label.c_str(),
                heads_up ? "activation" : "diagnostic", nodes_ok ? "ok" : "FAIL",
                infosets_ok ? "ok" : "FAIL", bytes_ok ? "ok" : "FAIL", projected_iters,
                heads_up ? (pass ? "ACTIVATE" : "DROP")
                : pass   ? "feasible (no activation)"
                         : "infeasible");
  }
  std::printf("\n# end flop spike\n");
  return 0;
}

int main(int argc, char** argv) {
  const bool flop_spike = argc > 1 && std::string(argv[1]) == "--flop-spike";
  const std::uint64_t max_iterations =
      std::stoull(argc > 2 ? argv[2] : (argc > 1 && !flop_spike ? argv[1] : "1000"));
  const std::size_t build_repeats = static_cast<std::size_t>(
      std::stoull(argc > 3 ? argv[3] : (argc > 2 && !flop_spike ? argv[2] : "5")));
  if (flop_spike)
    return run_flop_spike(max_iterations, build_repeats);

  const int turn_card = card_id("Js");
  const int river_card = card_id("9c");
  const std::array<int, 4> turn_board_arr{card_id("2c"), card_id("3d"), card_id("7h"), turn_card};
  const std::array<int, 5> river_board_arr{card_id("2c"), card_id("3d"), card_id("7h"), turn_card,
                                           river_card};
  const std::span<const int> turn_board(turn_board_arr);
  const std::span<const int> river_board(river_board_arr);

  const ActionAbstraction capped = ActionAbstraction::identity(/*max_raises=*/1);
  const ActionAbstraction uncapped = ActionAbstraction::identity(/*max_raises=*/0);
  const EquityFrontierEvaluator frontier;

  std::vector<ShapeResult> rows;

  std::printf("# RFC 0036 stage 0 calibration (max_iterations=%llu, build_repeats=%zu)\n",
              static_cast<unsigned long long>(max_iterations), build_repeats);
  std::printf(
      "Identity menu, uniform full-range priors, seed 42. Root stacks and\n"
      "contributions are named per section (production limped root is\n"
      "100 BB / 2 chips per seat).\n\n");

  std::printf("## G1 river-rooted cap-1 chance-free trees\n\n");
  for (std::size_t seats = 2; seats <= 6; ++seats) {
    GameDef def = make_root(seats, 100, river_board, TerminalDepth::River);
    auto ranges = uniform_priors(river_board, seats);
    rows.push_back(measure_shape("river cap1", def, capped, DepthLimit::none(), ranges, river_board,
                                 build_repeats, max_iterations, nullptr));
  }
  print_table(rows);
  rows.clear();

  std::printf("\n## RFC 0011 cross-check: river-rooted UNCAPPED trees, seats 2..4, 100 BB\n\n");
  for (std::size_t seats = 2; seats <= 4; ++seats) {
    GameDef def = make_root(seats, 100, river_board, TerminalDepth::River);
    auto ranges = uniform_priors(river_board, seats);
    rows.push_back(measure_shape("river cap0 100bb", def, uncapped, DepthLimit::none(), ranges,
                                 river_board, build_repeats, max_iterations, nullptr));
  }
  print_table(rows);
  rows.clear();

  std::printf(
      "\n## RFC 0011 point: UNCAPPED river trees at its 10 BB fixture "
      "(20 chips, 1 chip per seat), seats 2..4\n\n");
  for (std::size_t seats = 2; seats <= 4; ++seats) {
    GameDef def = make_root(seats, 10, river_board, TerminalDepth::River,
                            /*contribution=*/1, /*big_blind=*/2);
    auto ranges = uniform_priors(river_board, seats);
    rows.push_back(measure_shape("river cap0 10bb", def, uncapped, DepthLimit::none(), ranges,
                                 river_board, build_repeats, max_iterations, nullptr));
  }
  print_table(rows);
  rows.clear();

  std::printf("\n## G2 street-start turn trees, cap-1, streets(1)\n\n");
  for (std::size_t seats = 2; seats <= 6; ++seats) {
    GameDef def = make_root(seats, 100, turn_board, TerminalDepth::River);
    auto ranges = uniform_priors(turn_board, seats);
    rows.push_back(measure_shape("turn cap1 s1", def, capped, DepthLimit::streets(1), ranges,
                                 turn_board, build_repeats, max_iterations, &frontier));
  }
  print_table(rows);
  rows.clear();

  std::printf(
      "\n## RFC 0011 exact point: DECLARED-COARSE menu (one 1/2-pot bet "
      "and raise per street), 10 BB fixture, seats 2..4\n\n");
  {
    using bs::abstraction::CoverSeeds;
    using bs::abstraction::SizeSchedule;
    using bs::abstraction::StreetSizes;
    SizeSchedule coarse_schedule{};
    StreetSizes postflop_sizes;
    postflop_sizes.bets = {{1, 2}};
    postflop_sizes.raises = {{1, 2}};
    coarse_schedule[static_cast<std::size_t>(Street::Flop)] = postflop_sizes;
    coarse_schedule[static_cast<std::size_t>(Street::Turn)] = postflop_sizes;
    coarse_schedule[static_cast<std::size_t>(Street::River)] = postflop_sizes;
    const ActionAbstraction coarse =
        ActionAbstraction::declared(coarse_schedule, CoverSeeds::DeclaredOnly);
    for (std::size_t seats = 2; seats <= 4; ++seats) {
      GameDef def = make_root(seats, 10, river_board, TerminalDepth::River,
                              /*contribution=*/1, /*big_blind=*/2);
      auto ranges = uniform_priors(river_board, seats);
      rows.push_back(measure_shape("river coarse", def, coarse, DepthLimit::none(), ranges,
                                   river_board, build_repeats, max_iterations, nullptr));
    }
  }
  print_table(rows);
  rows.clear();

  std::printf("\n## Stack-independence spot check (six-seat river cap-1, node counts)\n\n");
  std::printf("| stack BB | nodes | action | bytes KiB |\n| --- | --- | --- | --- |\n");
  for (int stack_bb : {25, 100}) {
    GameDef def = make_root(6, stack_bb, river_board, TerminalDepth::River);
    try {
      TreeLimits limits;
      auto tree = std::make_unique<AbstractTree>(def, capped, limits, DepthLimit::none());
      const NodeStats stats = node_stats(*tree);
      std::printf("| %d | %zu | %zu | %.1f |\n", stack_bb, stats.nodes, stats.action,
                  stats.bytes / 1024.0);
    } catch (const tree_resource_exhausted&) {
      std::printf("| %d | DECLINED >1GiB | - | - |\n", stack_bb);
    }
  }
  std::fflush(stdout);

  std::printf("\n# end calibration\n");
  return 0;
}
