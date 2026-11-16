// RFC 0050 Stage 1 D303 thread-scaling probe (manual, NOT a CTest).
// Measures the exact cap-0 anchor's solve throughput across the frozen
// thread list {1,2,4,8,16,28} on a hard-coded ten-iteration run, with the
// total wall held near a quarter hour. Per point it reports construction
// wall (from the zero-iteration allocation receipt), solve-only
// per-iteration wall, speedup over the single-thread point, the Amdahl
// inverse serial fraction, the reach-bound e_embed and pure-menu term,
// and a hash of the average strategy. The kernel contract is bitwise
// identity across thread counts, so every NashConv, e_embed, and hash
// must equal the t=1 point; the single-thread ten-iteration NashConv is
// additionally pinned to the sealed calibration value. No argv; the
// thread list, iteration count, and the 10x/3x verdict thresholds are
// hard-coded and amendment-controlled.
//
// Usage: bigshark-rfc0050-thread-scaling
#include <sys/resource.h>

#include <array>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "policy_hash.hpp"
#include "reach_bound.hpp"
#include "rfc0050_anchors.hpp"
#include "rfc0050_stage1.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;
using bs::tree::TreeLimits;

namespace {

constexpr std::array<std::size_t, 6> kThreads{1, 2, 4, 8, 16, 28};
constexpr std::uint64_t kScalingIterations = 10;
// Sealed Stage 1 calibration NashConv at iteration 10 on this exact anchor
// (stage1-calibration.csv), pinned within half its last printed digit.
constexpr double kSealedNcAt10 = 4.89227959;
constexpr double kSealedPinTolerance = 5e-9;

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rfc0050 scaling: rank-0 anchor missing");
}

long peak_rss_kb() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return -1;
  return ru.ru_maxrss;
}

const char* verdict_token(ScalingVerdict v) {
  switch (v) {
    case ScalingVerdict::LongWallAmendmentCheap:
      return "long_wall_amendment_cheap";
    case ScalingVerdict::PlateauRiskDominates:
      return "plateau_risk_dominates";
    case ScalingVerdict::ComputeIncompressible:
      return "compute_incompressible";
  }
  return "unknown";
}

struct Point {
  std::size_t threads = 0;
  double construction_ms = 0.0;
  double solve_wall_ms = 0.0;
  double ms_per_iteration = 0.0;
  double nash_conv_bb = 0.0;
  double e_embed_bb = 0.0;
  double pure_menu_bb = 0.0;
  std::uint64_t sigma_hash = 0;
};

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const TreeLimits limits = offline_census_limits();
  Rfc0050Stage1Domain domain = build_stage1_domain(anchor.representative.river_def, limits);

  // Terminal split from an independent tree walk (no kernel change).
  std::size_t showdown = 0;
  std::size_t fold = 0;
  std::size_t action = 0;
  for (const auto& node : domain.bounded_tree.nodes()) {
    if (node.kind == bs::tree::NodeKind::TerminalShowdown)
      ++showdown;
    else if (node.kind == bs::tree::NodeKind::TerminalFold)
      ++fold;
    else if (node.is_action())
      ++action;
  }
  std::printf("# terminal split: action_nodes=%zu showdown=%zu fold=%zu total_nodes=%zu\n", action,
              showdown, fold, domain.bounded_tree.size());

  std::vector<Point> points;
  points.reserve(kThreads.size());
  std::printf(
      "threads,construction_ms,solve_wall_ms,iterations,ms_per_iteration,speedup_vs_t1,"
      "amdahl_serial_fraction,amdahl_parallel_fraction,nash_conv_bb,e_embed_bb,pure_menu_bb,"
      "sigma_hash,peak_rss_kb\n");

  for (std::size_t threads : kThreads) {
    RiverSolveConfig base;
    base.def = anchor.representative.river_def;
    base.threads = threads;
    base.linear_averaging = true;
    base.rm_plus = true;
    base.dcfr = false;
    base.action_abstraction = domain.menu;
    base.tree_limits = limits;

    // Construction-only wall from the zero-iteration allocation receipt.
    RiverSolveConfig alloc_cfg = base;
    alloc_cfg.iterations = 0;
    const RiverAllocationReceipt alloc = probe_river_allocation(alloc_cfg);
    if (!alloc.allocated) {
      std::fprintf(stderr, "allocation failed at threads %zu: %s\n", threads,
                   alloc.failure.c_str());
      return 4;
    }

    RiverSolveConfig cfg = base;
    cfg.iterations = kScalingIterations;
    const std::vector<std::uint64_t> cps{kScalingIterations};
    Point p;
    p.threads = threads;
    p.construction_ms = alloc.construction_wall_ms;
    cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
      RiverSolveResult snap;
      snap.avg_sigma = cp.avg_sigma;
      snap.nash_conv_bb = cp.nash_conv_bb;
      const ReachBoundResult rb =
          reach_bound_from_snapshot(cfg.def, snap, domain.reference_tree, domain.bounded_tree,
                                    domain.subset, CapExtensionCompletion::Uniform);
      p.nash_conv_bb = rb.bounded_nash_conv_bb;
      p.e_embed_bb = rb.exact_exploitability_bb;
      p.pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
      p.sigma_hash = hash_average_snapshot(cp.avg_sigma);
    };
    const auto t0 = std::chrono::steady_clock::now();
    const RiverSolveResult solved = solve_river_subgame(cfg, cps);
    p.solve_wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (solved.stopped_early || solved.iterations_completed != kScalingIterations) {
      std::fprintf(stderr, "scaling solve ended early at threads %zu\n", threads);
      return 3;
    }
    if (!pure_menu_reads_zero(p.pure_menu_bb)) {
      std::fprintf(stderr, "structural failure: nonzero pure-menu %.10g at threads %zu\n",
                   p.pure_menu_bb, threads);
      return 5;
    }
    p.ms_per_iteration = p.solve_wall_ms / static_cast<double>(kScalingIterations);
    points.push_back(p);
  }

  const double t1_ms_per_iter = points.front().ms_per_iteration;
  for (const Point& p : points) {
    const double speedup = t1_ms_per_iter / p.ms_per_iteration;
    const AmdahlBreakdown amdahl = amdahl_from_speedup(speedup, p.threads);
    if (p.threads == 1) {
      // Amdahl fractions are undefined on the single-worker baseline; leave
      // both fraction columns blank so no fake zero is printed.
      std::printf("%zu,%.3f,%.3f,%llu,%.3f,%.4f,,,,%.8f,%.8f,%.8f,%llu,%ld\n", p.threads,
                  p.construction_ms, p.solve_wall_ms,
                  static_cast<unsigned long long>(kScalingIterations), p.ms_per_iteration, speedup,
                  p.nash_conv_bb, p.e_embed_bb, p.pure_menu_bb,
                  static_cast<unsigned long long>(p.sigma_hash), peak_rss_kb());
      continue;
    }
    std::printf("%zu,%.3f,%.3f,%llu,%.3f,%.4f,%.6f,%.6f,%.8f,%.8f,%.8f,%llu,%ld\n", p.threads,
                p.construction_ms, p.solve_wall_ms,
                static_cast<unsigned long long>(kScalingIterations), p.ms_per_iteration, speedup,
                amdahl.serial_fraction, amdahl.parallel_fraction, p.nash_conv_bb, p.e_embed_bb,
                p.pure_menu_bb, static_cast<unsigned long long>(p.sigma_hash), peak_rss_kb());
  }

  // Determinism gate: every thread count must reproduce the t=1 numeric
  // result and strategy hash exactly, and t=1 must match the sealed
  // calibration NashConv at iteration 10.
  const Point& t1 = points.front();
  if (std::fabs(t1.nash_conv_bb - kSealedNcAt10) > kSealedPinTolerance) {
    std::fprintf(stderr, "probe invalid: t=1 NC %.8f != sealed %.8f\n", t1.nash_conv_bb,
                 kSealedNcAt10);
    return 6;
  }
  for (const Point& p : points) {
    if (p.nash_conv_bb != t1.nash_conv_bb || p.e_embed_bb != t1.e_embed_bb ||
        p.sigma_hash != t1.sigma_hash) {
      std::fprintf(stderr, "determinism failure at threads %zu\n", p.threads);
      return 7;
    }
    if (!pure_menu_reads_zero(p.pure_menu_bb)) {
      std::fprintf(stderr, "pure-menu zero failed at threads %zu\n", p.threads);
      return 5;
    }
  }

  const double speedup_28 = t1_ms_per_iter / points.back().ms_per_iteration;
  const AmdahlBreakdown amdahl_28 = amdahl_from_speedup(speedup_28, 28);
  const ScalingVerdict verdict = classify_scaling(speedup_28);
  std::printf(
      "# verdict speedup_28=%.4f amdahl_serial=%.6f amdahl_parallel=%.6f threshold_cheap=%.1f "
      "threshold_incompressible=%.1f -> %s\n",
      speedup_28, amdahl_28.serial_fraction, amdahl_28.parallel_fraction, kRfc0050CheapSpeedup,
      kRfc0050IncompressibleSpeedup, verdict_token(verdict));
  std::printf(
      "# determinism: all %zu thread points bitwise-identical to t=1; t=1 matches sealed "
      "NC %.8f\n",
      kThreads.size(), kSealedNcAt10);
  return 0;
}
