// RFC 0050 Stage 1 calibration probe (manual, NOT a CTest). The D296 hard
// precondition before the frozen 20,000-iteration convergence run: the
// exact cap-0 anchor has 1,600 action nodes against the RFC 0049 cap-1
// control's six, so bounded-menu throughputs cannot be extrapolated. This
// probe runs a hard-coded short 30-iteration solve on the measured-smallest
// decision-bearing anchor (srp pot 32, contributions 16/16), records the
// per-rung wall to derive a steady-state per-iteration cost, times one
// reach-bound scoring separately, validates the pure-menu zero identity
// early, and prints the budget extrapolation against the frozen 1,800 s
// wall and nine-rung ladder. No argv; the calibration counts are frozen
// here and cannot alter the gate probe's constants.
//
// Usage: bigshark-rfc0050-stage1-calibration
#include <sys/resource.h>

#include <bs/abstraction.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "reach_bound.hpp"
#include "rfc0050_anchors.hpp"
#include "rfc0050_stage1.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;

namespace {

constexpr std::array<std::uint64_t, 4> kCalibrationRungs{5, 10, 20, 30};

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rfc0050 calibration: rank-0 anchor missing");
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const bs::tree::TreeLimits limits = offline_census_limits();
  Rfc0050Stage1Domain domain = build_stage1_domain(anchor.representative.river_def, limits);

  RiverSolveConfig cfg;
  cfg.def = anchor.representative.river_def;
  cfg.iterations = kCalibrationRungs.back();
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = domain.menu;
  cfg.tree_limits = limits;
  const std::vector<std::uint64_t> checkpoints(kCalibrationRungs.begin(), kCalibrationRungs.end());

  std::printf(
      "record_type,family,pot,contribution0,contribution1,iteration,wall_ms,incremental_ms,"
      "solve_ms_per_iteration,bounded_nc_bb,pure_menu_bb,scoring_ms,cumulative_scoring_ms,"
      "peak_rss_kb\n");

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t last_iteration = 0;
  double last_wall_ms = 0.0;
  double scoring_ms = 0.0;
  double cumulative_scoring_ms = 0.0;  // scorings of rungs before the current one
  double final_pure_menu_bb = 0.0;
  double final_bounded_nc_bb = 0.0;
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double incremental_ms = wall_ms - last_wall_ms;
    // Solve-only steady-state throughput: wall at sink entry contains
    // construction plus solve time plus every PREVIOUS rung's scoring, so
    // subtract the prior cumulative scoring before dividing. The current
    // rung's scoring is measured below and added only afterwards.
    const double solve_ms_per_iteration =
        (wall_ms - cumulative_scoring_ms) / static_cast<double>(cp.iteration);
    RiverSolveResult snap;
    snap.avg_sigma = cp.avg_sigma;
    snap.nash_conv_bb = cp.nash_conv_bb;
    const auto score_t0 = std::chrono::steady_clock::now();
    const ReachBoundResult rb =
        reach_bound_from_snapshot(cfg.def, snap, domain.reference_tree, domain.bounded_tree,
                                  domain.subset, CapExtensionCompletion::Uniform);
    scoring_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - score_t0)
            .count();
    const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
    final_pure_menu_bb = pure_menu_bb;
    final_bounded_nc_bb = rb.bounded_nash_conv_bb;
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::printf("rung,%s,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.8f,%.8f,%.3f,%.3f,%ld\n",
                anchor.family.c_str(), static_cast<unsigned long long>(anchor.key.pot),
                static_cast<unsigned long long>(anchor.key.contribution0),
                static_cast<unsigned long long>(anchor.key.contribution1),
                static_cast<unsigned long long>(cp.iteration), wall_ms, incremental_ms,
                solve_ms_per_iteration, rb.bounded_nash_conv_bb, pure_menu_bb, scoring_ms,
                cumulative_scoring_ms + scoring_ms, ru.ru_maxrss);
    std::fflush(stdout);
    cumulative_scoring_ms += scoring_ms;
    last_wall_ms = wall_ms;
    last_iteration = cp.iteration;
  };

  const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
  if (solved.stopped_early || solved.iterations_completed != kCalibrationRungs.back()) {
    std::fprintf(stderr, "calibration solve ended early: %llu / %llu\n",
                 static_cast<unsigned long long>(solved.iterations_completed),
                 static_cast<unsigned long long>(kCalibrationRungs.back()));
    return 3;
  }
  if (!pure_menu_reads_zero(final_pure_menu_bb)) {
    std::fprintf(stderr,
                 "structural failure: exact-menu pure-menu term %.10g BB outside zero tolerance "
                 "%.0e at calibration rung %llu\n",
                 final_pure_menu_bb, kRfc0050PureMenuZeroBb,
                 static_cast<unsigned long long>(last_iteration));
    return 5;
  }

  // Solve-only steady-state per-iteration cost: total wall minus every
  // scoring, divided by completed iterations. Construction is embedded in
  // the first interval and becomes negligible over the full ladder; the
  // gate receipt replaces this estimate regardless.
  const double solve_ms_per_iteration =
      (last_wall_ms - cumulative_scoring_ms) / static_cast<double>(last_iteration);
  const double mean_scoring_ms =
      cumulative_scoring_ms / static_cast<double>(kCalibrationRungs.size());
  const Rfc0050Calibration estimate =
      extrapolate_stage1_calibration(solve_ms_per_iteration, mean_scoring_ms);
  struct rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf("summary,%s,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.8f,%.8f,%.3f,%.3f,%ld\n",
              anchor.family.c_str(), static_cast<unsigned long long>(anchor.key.pot),
              static_cast<unsigned long long>(anchor.key.contribution0),
              static_cast<unsigned long long>(anchor.key.contribution1),
              static_cast<unsigned long long>(last_iteration), last_wall_ms, 0.0,
              solve_ms_per_iteration, final_bounded_nc_bb, final_pure_menu_bb, mean_scoring_ms,
              cumulative_scoring_ms, ru.ru_maxrss);
  std::printf(
      "estimate,projected_20k_solve_s=%.1f,projected_full_ladder_s=%.1f,"
      "iterations_inside_1800s_wall=%.0f,last_rung_inside_wall=%llu,"
      "frozen_wall_s=%.0f,pure_menu_zero_tolerance_bb=%.0e\n",
      estimate.projected_20k_wall_s, estimate.projected_full_ladder_wall_s,
      estimate.iterations_inside_wall,
      static_cast<unsigned long long>(estimate.last_rung_inside_wall),
      kRfc0050Stage1WallMs / 1000.0, kRfc0050PureMenuZeroBb);
  return 0;
}
