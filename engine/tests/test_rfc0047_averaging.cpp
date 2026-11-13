// RFC 0048 D198 native test: the existing linear_averaging solver switch
// produces distinct, legal, independently measurable policies. Real
// seedless solves on the enumerable shallow river root; no text matching.
// The production default (linear=true) is never changed.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/union_embedding.hpp"
#include "../benchmarks/rfc0047/union_menu.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kTinyIters = 60;
constexpr double kDistinctL1 = 1e-12;

bs::poker::GameDef shallow_root() {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {8, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

RiverSolveResult solve_mode(const bs::poker::GameDef& def, const ActionAbstraction& bounded,
                            bool linear) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kTinyIters;
  cfg.linear_averaging = linear;
  cfg.threads = 1;
  cfg.action_abstraction = bounded;
  return solve_river_subgame(cfg, {});
}

// A1: both snapshots are finite non-negative normalized distributions,
// and the tot==0 uniform-fallback trigger sets match between modes.
bool test_a1_legality_and_fallback_parity() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult lin = solve_mode(def, bounded, true);
  const RiverSolveResult uni = solve_mode(def, bounded, false);
  CHECK(lin.avg_sigma.size() == uni.avg_sigma.size());
  for (std::size_t n = 0; n < lin.avg_sigma.size(); ++n) {
    CHECK(lin.avg_sigma[n].empty() == uni.avg_sigma[n].empty());
    if (lin.avg_sigma[n].empty())
      continue;
    const std::size_t width = lin.avg_sigma[n].size();
    CHECK(uni.avg_sigma[n].size() == width);
    double sum_lin = 0.0;
    double sum_uni = 0.0;
    const std::size_t menu = width / kC;
    for (std::uint32_t i = 0; i < kC; ++i) {
      double row_lin = 0.0;
      double row_uni = 0.0;
      for (std::size_t a = 0; a < menu; ++a) {
        const double x = lin.avg_sigma[n][i * menu + a];
        const double y = uni.avg_sigma[n][i * menu + a];
        CHECK(std::isfinite(x) && x >= 0.0);
        CHECK(std::isfinite(y) && y >= 0.0);
        row_lin += x;
        row_uni += y;
      }
      CHECK(std::fabs(row_lin - 1.0) <= 1e-12);
      CHECK(std::fabs(row_uni - 1.0) <= 1e-12);
      sum_lin += row_lin;
      sum_uni += row_uni;
    }
    // Fallback parity: mode-independent regret/reach implies identical
    // uniform-fallback rows, detectable as exactly equal 1/menu entries.
    for (std::uint32_t i = 0; i < kC; ++i)
      for (std::size_t a = 0; a < menu; ++a) {
        const bool lin_fallback = lin.avg_sigma[n][i * menu + a] == 1.0 / static_cast<double>(menu);
        const bool uni_fallback = uni.avg_sigma[n][i * menu + a] == 1.0 / static_cast<double>(menu);
        CHECK(lin_fallback == uni_fallback);
      }
    (void)sum_lin;
    (void)sum_uni;
  }
  return true;
}

// A2: deterministic distinctness via L1 + content hash at 60 iterations.
bool test_a2_distinctness_metric() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult lin = solve_mode(def, bounded, true);
  const RiverSolveResult uni = solve_mode(def, bounded, false);
  const std::uint64_t h_lin = hash_average_snapshot(lin.avg_sigma);
  const std::uint64_t h_uni = hash_average_snapshot(uni.avg_sigma);
  const double l1 = max_snapshot_l1(lin.avg_sigma, uni.avg_sigma);
  // The hash function is deterministic for equal content.
  CHECK(hash_average_snapshot(lin.avg_sigma) == h_lin);
  CHECK(std::isfinite(l1) && l1 >= 0.0);
  // On this non-trivial evolving tree the two time weights must diverge.
  if (h_lin == h_uni || l1 < kDistinctL1) {
    std::printf("A2 diagnostic: policies coincide at 60 iters (l1=%.3e); anchor must cancel\n", l1);
    return false;
  }
  return true;
}

// A3: each policy independently reproduces the solver root CFV through
// the external union evaluator, so it is a measurable legal policy.
bool test_a3_external_evaluation() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const AbstractTree ref_tree(def, ActionAbstraction::identity(0));
  const AbstractTree bounded_tree(def, bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  for (const bool linear : {true, false}) {
    const RiverSolveResult solved = solve_mode(def, bounded, linear);
    const UnionRootValues v = root_values_from_snapshot(def, solved, ref_tree, bounded_tree, subset,
                                                        CapExtensionCompletion::Uniform);
    for (std::uint32_t i = 0; i < kC; ++i) {
      CHECK(std::fabs(v.ev_seat0[i] - solved.root_cfv_seat0[i] * 990.0) <= 1e-6);
      CHECK(std::fabs(v.ev_seat1[i] - solved.root_cfv_seat1[i] * 990.0) <= 1e-6);
    }
    // Bitwise reach validation on the same supplied snapshot.
    for (std::size_t seat = 0; seat < 2; ++seat) {
      const auto reach = recompute_forward_reach(ref_tree, bounded_tree, subset, solved, seat);
      validate_supplied_reach(ref_tree, bounded_tree, subset, solved, seat, reach);
    }
  }
  return true;
}

// A4: the default linear path is bitwise stable across in-process repeats.
bool test_a4_default_stability() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult a = solve_mode(def, bounded, true);
  const RiverSolveResult b = solve_mode(def, bounded, true);
  CHECK(hash_average_snapshot(a.avg_sigma) == hash_average_snapshot(b.avg_sigma));
  CHECK(max_snapshot_l1(a.avg_sigma, b.avg_sigma) == 0.0);
  CHECK(a.nash_conv_bb == b.nash_conv_bb);
  return true;
}

// A5: malformed snapshots fail closed through the existing validators.
bool test_a5_fail_closed() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult good = solve_mode(def, bounded, true);
  const AbstractTree ref_tree(def, ActionAbstraction::identity(0));
  const AbstractTree bounded_tree(def, bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);

  RiverSolveResult negative = good;
  for (auto& row : negative.avg_sigma)
    for (double& x : row)
      if (x > 0.0) {
        x = -1.0;
        break;
      }
  bool threw = false;
  try {
    (void)reach_bound_from_snapshot(def, negative, ref_tree, bounded_tree, subset,
                                    CapExtensionCompletion::Uniform);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  RiverSolveResult non_normalized = good;
  for (auto& row : non_normalized.avg_sigma)
    if (!row.empty())
      for (double& x : row)
        x *= 2.0;
  threw = false;
  try {
    (void)reach_bound_from_snapshot(def, non_normalized, ref_tree, bounded_tree, subset,
                                    CapExtensionCompletion::Uniform);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  RiverSolveResult nan_snapshot = good;
  for (auto& row : nan_snapshot.avg_sigma)
    if (!row.empty()) {
      row[0] = std::nan("");
      break;
    }
  threw = false;
  try {
    (void)reach_bound_from_snapshot(def, nan_snapshot, ref_tree, bounded_tree, subset,
                                    CapExtensionCompletion::Uniform);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// A6: reported (non-asserted) tiny-root NashConv signal for both modes.
bool test_a6_nashconv_signal() {
  const auto def = shallow_root();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult lin = solve_mode(def, bounded, true);
  const RiverSolveResult uni = solve_mode(def, bounded, false);
  CHECK(std::isfinite(lin.nash_conv_bb) && std::isfinite(uni.nash_conv_bb));
  CHECK(lin.nash_conv_bb >= 0.0 && uni.nash_conv_bb >= 0.0);
  // Report only; either ordering is a legitimate empirical outcome.
  std::printf("A6 tiny-root NC: linear=%.8f uniform=%.8f signed_diff=%.8f hashes=(%llu,%llu)\n",
              lin.nash_conv_bb, uni.nash_conv_bb, uni.nash_conv_bb - lin.nash_conv_bb,
              static_cast<unsigned long long>(hash_average_snapshot(lin.avg_sigma)),
              static_cast<unsigned long long>(hash_average_snapshot(uni.avg_sigma)));
  return true;
}

}  // namespace

int main() {
  if (!test_a1_legality_and_fallback_parity() || !test_a2_distinctness_metric() ||
      !test_a3_external_evaluation() || !test_a4_default_stability() || !test_a5_fail_closed() ||
      !test_a6_nashconv_signal())
    return 1;
  std::printf("rfc0047_averaging: all checks passed\n");
  return 0;
}
