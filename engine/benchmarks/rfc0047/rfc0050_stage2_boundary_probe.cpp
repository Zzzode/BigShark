// RFC 0050 Stage 2 manual six-anchor boundary probe (NOT a CTest).
// Runs the frozen exact-menu convergence gate on each of the six
// hard-coded decision-bearing anchors in a separate per-anchor process
// phase with its own 1,800 s wall and the same nine-rung ladder, zero
// pure-menu guard, and four-verdict classifier used in Stage 1. The
// output maps whether the Stage 1 result generalizes across the
// measured ledger ordering. No argv; anchors, ladder, wall, and the
// 0.002 BB gate are hard-coded and amendment-controlled.
//
// Usage: bigshark-rfc0050-stage2-boundary
#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

const LedgerAnchor* find_anchor(const std::vector<LedgerAnchor>& anchors,
                                const AnchoredLedger& wanted) {
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return &a;
  return nullptr;
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const bs::tree::TreeLimits limits = offline_census_limits();
  const auto& hard_coded = rfc0050_six_anchors();

  std::printf(
      "anchor_rank,family,pot,contribution0,contribution1,verdict,completed_iterations,"
      "requested_iterations,rung_rows,min_e_embed_bb,min_pure_menu_abs_bb,wall_ms,"
      "peak_rss_kb\n");
  std::fflush(stdout);

  int structural_failures = 0;
  for (std::size_t rank = 0; rank < hard_coded.size(); ++rank) {
    const AnchoredLedger& wanted = hard_coded[rank];
    const LedgerAnchor* anchor = find_anchor(anchors, wanted);
    if (anchor == nullptr) {
      std::fprintf(stderr, "anchor rank %zu missing from census\n", rank);
      return 4;
    }
    Rfc0050Stage1Domain domain = build_stage1_domain(anchor->representative.river_def, limits);

    RiverSolveConfig cfg;
    cfg.def = anchor->representative.river_def;
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
    double max_pure_menu_abs = 0.0;
    double min_e_embed = std::numeric_limits<double>::infinity();
    const auto phase_t0 = std::chrono::steady_clock::now();
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
      max_pure_menu_abs = std::max(max_pure_menu_abs, std::fabs(pure_menu_bb));
      if (!pure_menu_reads_zero(pure_menu_bb)) {
        std::fprintf(stderr, "structural failure: nonzero pure-menu %.10g at rank %zu rung %llu\n",
                     pure_menu_bb, rank, static_cast<unsigned long long>(cp.iteration));
        std::_Exit(5);
      }
      OfflineRungRow row;
      row.iteration = cp.iteration;
      row.bounded_nash_conv_bb = rb.bounded_nash_conv_bb;
      row.exact_exploitability_bb = rb.exact_exploitability_bb;
      row.pure_menu_term_bb = pure_menu_bb;
      row.e_embed_bb = rb.exact_exploitability_bb;
      row.wall_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_t0)
              .count();
      if (!std::isfinite(row.e_embed_bb))
        std::_Exit(3);
      min_e_embed = std::min(min_e_embed, row.e_embed_bb);
      rows.push_back(row);
      std::fflush(stdout);
    };

    const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_t0)
            .count();
    const OfflineConvergenceOutcome outcome =
        classify_convergence(std::move(rows), solved.stopped_early);
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::printf("%zu,%s,%llu,%llu,%llu,%s,%llu,%llu,%zu,%.8f,%.8f,%.3f,%ld\n", rank,
                anchor->family.c_str(), static_cast<unsigned long long>(anchor->key.pot),
                static_cast<unsigned long long>(anchor->key.contribution0),
                static_cast<unsigned long long>(anchor->key.contribution1),
                verdict_token(outcome.verdict),
                static_cast<unsigned long long>(solved.iterations_completed),
                static_cast<unsigned long long>(solved.iterations), outcome.rows.size(),
                min_e_embed, max_pure_menu_abs, wall_ms, ru.ru_maxrss);
    std::fflush(stdout);
    if (max_pure_menu_abs > kRfc0050PureMenuZeroBb)
      ++structural_failures;
  }

  if (structural_failures != 0) {
    std::fprintf(stderr, "stage 2 structural failures: %d\n", structural_failures);
    return 5;
  }
  return 0;
}
