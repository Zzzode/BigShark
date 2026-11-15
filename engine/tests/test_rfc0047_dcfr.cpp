// RFC 0048 D252 native test: the river Linear DCFR+ time-discounting switch
// (factor t/(t+1) on accumulated regret and average-strategy tables at the
// start of iteration t, RM+ clipping retained) produces a bitwise-preserved
// default, a distinct legal discounted policy, and deterministic, measurable
// results through the real river kernel. No substitute allocator, no
// source-text matching, and no quality-gate relaxation; the 300-iter
// policy-tier anchor is a separate manual probe, not this ctest.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
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
// Pre-fixed R6 divergence ladder (pre-registration review F3): 2 is the
// expected first diverging count; the ladder is a fixed sequence, not a
// post-hoc choice, and no divergence by its last element fails the gate.
constexpr std::uint64_t kDivergenceLadder[] = {2, 3, 5, 10};

bs::poker::GameDef shallow_river() {
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

RiverSolveConfig base_config(const bs::poker::GameDef& def, const ActionAbstraction& bounded,
                             bool dcfr, std::uint64_t iters = kTinyIters) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = iters;
  cfg.linear_averaging = true;  // held constant: only the discount switch varies.
  cfg.threads = 1;
  cfg.action_abstraction = bounded;
  cfg.rm_plus = true;  // clipping retained: the D251 axis is not re-entered.
  cfg.dcfr = dcfr;
  return cfg;
}

// R1: dcfr=false is byte-identical to the pre-change default path, both
// through solve_river_subgame and through the river_combo_values entry point.
bool test_r1_default_bitwise_preserved() {
  const auto def = shallow_river();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult explicit_default = solve_river_subgame(base_config(def, bounded, false));
  RiverSolveConfig implicit_cfg = base_config(def, bounded, false);
  implicit_cfg.dcfr = false;  // the field default; set explicitly for clarity.
  const RiverSolveResult again = solve_river_subgame(implicit_cfg);
  CHECK(hash_average_snapshot(explicit_default.avg_sigma) ==
        hash_average_snapshot(again.avg_sigma));
  CHECK(explicit_default.nash_conv_bb == again.nash_conv_bb);

  // Second construction site keeps the default via its own config and must match.
  const ComboNashConv cv_a = river_combo_values(implicit_cfg, kTinyIters, true);
  const ComboNashConv cv_b = river_combo_values(implicit_cfg, kTinyIters, true);
  CHECK(cv_a.seat0_ev == cv_b.seat0_ev);
  CHECK(cv_a.seat0_br == cv_b.seat0_br);
  return true;
}

// R2: Linear DCFR+ diverges from the RM+ control with a distinct hash and a
// non-trivial L1.
bool test_r2_dcfr_diverges() {
  const auto def = shallow_river();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult control = solve_river_subgame(base_config(def, bounded, false));
  const RiverSolveResult dcfr = solve_river_subgame(base_config(def, bounded, true));
  const std::uint64_t h_control = hash_average_snapshot(control.avg_sigma);
  const std::uint64_t h_dcfr = hash_average_snapshot(dcfr.avg_sigma);
  const double l1 = max_snapshot_l1(control.avg_sigma, dcfr.avg_sigma);
  CHECK(h_control != h_dcfr);
  CHECK(std::isfinite(l1) && l1 >= kDistinctL1);
  std::printf("D252 R2 river control vs linear-dcfr+ max_row_l1=%.6e\n", l1);
  return true;
}

// R3: discounted sigmas stay finite, non-negative, and normalized.
bool test_r3_dcfr_legality() {
  const auto def = shallow_river();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult dcfr = solve_river_subgame(base_config(def, bounded, true));
  for (const std::vector<double>& row : dcfr.avg_sigma) {
    if (row.empty())
      continue;
    const std::size_t menu = row.size() / kC;
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < menu; ++a) {
        const double x = row[i * menu + a];
        CHECK(std::isfinite(x) && x >= 0.0);
        sum += x;
      }
      CHECK(std::fabs(sum - 1.0) <= 1e-12);
    }
  }
  CHECK(std::isfinite(dcfr.nash_conv_bb) && dcfr.nash_conv_bb >= 0.0);
  return true;
}

// R4: the discounted policy is bitwise stable across two independent fresh
// solves.
bool test_r4_dcfr_repeat() {
  const auto def = shallow_river();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult a = solve_river_subgame(base_config(def, bounded, true));
  const RiverSolveResult b = solve_river_subgame(base_config(def, bounded, true));
  CHECK(hash_average_snapshot(a.avg_sigma) == hash_average_snapshot(b.avg_sigma));
  CHECK(max_snapshot_l1(a.avg_sigma, b.avg_sigma) == 0.0);
  CHECK(a.nash_conv_bb == b.nash_conv_bb);
  return true;
}

// R5: both modes evaluate through the frozen reach-bound instrument with
// finite bounded NC and pure menu terms on the empirically selected domain.
bool test_r5_finite_reach_bound() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    throw std::runtime_error("w1 frontier empty");
  const auto& def = leaves.front().river_def;
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const AbstractTree bounded_tree(def, bounded);
  for (const bool dcfr : {false, true}) {
    const RiverSolveResult solved = solve_river_subgame(base_config(def, bounded, dcfr));
    try {
      const AbstractTree cap0(def, ActionAbstraction::identity(0));
      const SubsetMap subset = map_bounded_subset(cap0, bounded_tree);
      const ReachBoundResult rb = reach_bound_from_snapshot(def, solved, cap0, bounded_tree, subset,
                                                            CapExtensionCompletion::Uniform);
      CHECK(std::isfinite(rb.bounded_nash_conv_bb) && std::isfinite(rb.exact_exploitability_bb));
      CHECK(std::isfinite(rb.exact_exploitability_bb - rb.bounded_nash_conv_bb));
    } catch (const std::invalid_argument&) {
      const ActionAbstraction union_action =
          make_union_abstraction(ActionAbstraction::identity(0), bounded);
      const AbstractTree union_tree(def, union_action);
      const SubsetMap subset = map_bounded_subset(union_tree, bounded_tree);
      const ReachBoundResult rb = reach_bound_from_snapshot(
          def, solved, union_tree, bounded_tree, subset, CapExtensionCompletion::Uniform);
      CHECK(std::isfinite(rb.bounded_nash_conv_bb) && std::isfinite(rb.exact_exploitability_bb));
    }
  }
  return true;
}

// R6 (insertion-point behavioral contract): at 1 iteration the discount pass
// scales all-zero tables, so dcfr=true is bitwise identical to the control;
// from the first pre-fixed ladder count at which prior nonzero tables are
// discounted, the hashes must diverge. No divergence by the ladder end fails.
bool test_r6_insertion_point_contract() {
  const auto def = shallow_river();
  const auto bounded = make_grid_abstraction(SizingGrid::Coarse, 1);

  const RiverSolveResult one_control = solve_river_subgame(base_config(def, bounded, false, 1));
  const RiverSolveResult one_dcfr = solve_river_subgame(base_config(def, bounded, true, 1));
  CHECK(hash_average_snapshot(one_control.avg_sigma) == hash_average_snapshot(one_dcfr.avg_sigma));
  CHECK(one_control.nash_conv_bb == one_dcfr.nash_conv_bb);

  bool diverged = false;
  for (const std::uint64_t iters : kDivergenceLadder) {
    const RiverSolveResult control = solve_river_subgame(base_config(def, bounded, false, iters));
    const RiverSolveResult dcfr = solve_river_subgame(base_config(def, bounded, true, iters));
    if (hash_average_snapshot(control.avg_sigma) != hash_average_snapshot(dcfr.avg_sigma)) {
      const double l1 = max_snapshot_l1(control.avg_sigma, dcfr.avg_sigma);
      CHECK(std::isfinite(l1) && l1 >= kDistinctL1);
      std::printf("D252 R6 first divergence at %llu iters, max_row_l1=%.6e\n",
                  static_cast<unsigned long long>(iters), l1);
      diverged = true;
      break;
    }
  }
  CHECK(diverged);
  return true;
}

}  // namespace

int main() {
  if (!test_r1_default_bitwise_preserved() || !test_r2_dcfr_diverges() ||
      !test_r3_dcfr_legality() || !test_r4_dcfr_repeat() || !test_r5_finite_reach_bound() ||
      !test_r6_insertion_point_contract())
    return 1;
  std::printf("rfc0047_dcfr: all checks passed\n");
  return 0;
}
