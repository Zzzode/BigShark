// RFC 0050 Stage 1 manual single-instance exact-menu convergence probe
// (NOT a CTest). Solves the exact cap-0 menu on the hard-coded
// measured-smallest decision-bearing anchor through the seedless river
// kernel, scores every rung of the frozen nine-rung ladder through the
// reach-bound instrument inside the 1,800 s in-process wall, and requires
// the pure-menu term to read zero at every rung. The bounded tree and the
// reference tree are the same cap-0 tree, so e_embed equals the bounded
// NashConv and the gate reduces to pure convergence. A nonzero pure-menu
// reading is a structural contradiction that aborts with a distinct exit
// code before any verdict is emitted; the four convergence verdicts come
// from the frozen RFC 0049 classifier. No argv; the anchor, ladder, wall,
// and 0.002 BB gate are hard-coded and amendment-controlled.
//
// Usage: bigshark-rfc0050-exact-convergence-probe
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "offline_convergence.hpp"
#include "reach_bound.hpp"
#include "rfc0050_anchors.hpp"
#include "rfc0050_stage1.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;

namespace {

long current_rss_kb() {
  std::ifstream status("/proc/self/statm");
  long resident_pages = 0;
  long total_pages = 0;
  status >> total_pages >> resident_pages;
  if (resident_pages <= 0)
    return -1;
  const long page_kb = sysconf(_SC_PAGESIZE) / 1024L;
  return resident_pages * (page_kb > 0 ? page_kb : 4L);
}

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rfc0050 stage1: rank-0 anchor missing");
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const bs::tree::TreeLimits limits = offline_census_limits();
  Rfc0050Stage1Domain domain = build_stage1_domain(anchor.representative.river_def, limits);

  RiverSolveConfig cfg;
  cfg.def = anchor.representative.river_def;
  cfg.iterations = kRfc0050Stage1Rungs.back();
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = domain.menu;
  cfg.tree_limits = limits;
  const std::vector<std::uint64_t> checkpoints(kRfc0050Stage1Rungs.begin(),
                                               kRfc0050Stage1Rungs.end());

  std::vector<OfflineRungRow> rows;
  const auto t0 = std::chrono::steady_clock::now();
  cfg.stop_predicate = [](std::uint64_t, double elapsed_ms) {
    return elapsed_ms >= kRfc0050Stage1WallMs;
  };
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
    RiverSolveResult snap;
    snap.avg_sigma = cp.avg_sigma;
    snap.nash_conv_bb = cp.nash_conv_bb;
    const ReachBoundResult rb =
        reach_bound_from_snapshot(cfg.def, snap, domain.reference_tree, domain.bounded_tree,
                                  domain.subset, CapExtensionCompletion::Uniform);
    const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
    // D296 boundary 1: the structural term must be exactly zero on the
    // exact menu. A reading outside the pre-registered FP tolerance means
    // the menu was not actually exact; abort for investigation instead of
    // emitting a convergence verdict.
    if (!pure_menu_reads_zero(pure_menu_bb)) {
      std::fprintf(stderr,
                   "structural failure: pure-menu term %.10g BB outside zero tolerance %.0e at "
                   "rung %llu\n",
                   pure_menu_bb, kRfc0050PureMenuZeroBb,
                   static_cast<unsigned long long>(cp.iteration));
      std::_Exit(5);
    }
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    OfflineRungRow row;
    row.iteration = cp.iteration;
    row.bounded_nash_conv_bb = rb.bounded_nash_conv_bb;
    row.exact_exploitability_bb = rb.exact_exploitability_bb;
    row.pure_menu_term_bb = pure_menu_bb;
    row.e_embed_bb = rb.exact_exploitability_bb;  // == bounded NC with zero menu term
    row.wall_ms = wall_ms;
    if (!std::isfinite(row.e_embed_bb) || !std::isfinite(row.bounded_nash_conv_bb)) {
      std::fprintf(stderr, "non-finite reach-bound result at rung %llu\n",
                   static_cast<unsigned long long>(cp.iteration));
      std::_Exit(3);
    }
    rows.push_back(row);
    std::printf("rung,exact-cap0,%s,%llu,%llu,%llu,,%llu,,,%.8f,%.8f,%.8f,%.8f,%.3f,%ld\n",
                anchor.family.c_str(), static_cast<unsigned long long>(anchor.key.pot),
                static_cast<unsigned long long>(anchor.key.contribution0),
                static_cast<unsigned long long>(anchor.key.contribution1),
                static_cast<unsigned long long>(cp.iteration), row.bounded_nash_conv_bb,
                row.exact_exploitability_bb, row.pure_menu_term_bb, row.e_embed_bb, wall_ms,
                current_rss_kb());
    std::fflush(stdout);
  };

  std::printf(
      "record_type,label,family,pot,contribution0,contribution1,verdict,iteration,"
      "requested_iterations,rung_rows,bounded_nc_bb,exact_e_embed_bb,pure_menu_bb,e_embed_bb,"
      "wall_ms,rss_current_kb\n");
  std::fflush(stdout);
  const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
  const double total_wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const OfflineConvergenceOutcome outcome =
      classify_convergence(std::move(rows), solved.stopped_early);
  struct rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf("summary,exact-cap0,%s,%llu,%llu,%llu,%s,%llu,%llu,%zu,,,,,%.3f,%ld\n",
              anchor.family.c_str(), static_cast<unsigned long long>(anchor.key.pot),
              static_cast<unsigned long long>(anchor.key.contribution0),
              static_cast<unsigned long long>(anchor.key.contribution1),
              verdict_token(outcome.verdict),
              static_cast<unsigned long long>(solved.iterations_completed),
              static_cast<unsigned long long>(solved.iterations), outcome.rows.size(),
              total_wall_ms, ru.ru_maxrss);
  std::fflush(stdout);
  return 0;
}
