// RFC 0050 D326 decisive convergence measurement (manual, NOT a CTest).
// Extends the cheapest anchor (rank 0, srp pot 32) to iteration 1600 on
// the exact cap-0 menu at 28 threads (the D303 measured 2.26x ceiling;
// numeric results are bitwise identical across thread counts), with a
// diagnostic 3-hour wall and fixed rungs 400/800/1600. The three scored
// points feed a pre-registered OLS slope classification that decides
// whether the early ~-1.0 convergence law persists or decays toward the
// ~-0.52 regime, or plateaus. This is a D326 diagnostic gate, not the
// frozen Stage 1 1,800 s wall; no frozen constant, the nine-rung ladder,
// the 0.002 gate, or the classifier changes. No argv; the anchor, rungs,
// thread count, and wall are hard-coded.
//
// Usage: bigshark-rfc0050-decisive-probe
#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "reach_bound.hpp"
#include "rfc0050_anchors.hpp"
#include "rfc0050_stage1.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;

namespace {

constexpr std::size_t kThreads = 28;  // one socket; D303 measured ceiling 2.2561x
constexpr double kDiagnosticWallMs = 3.0 * 60.0 * 60.0 * 1000.0;  // 3 hours
constexpr std::array<std::uint64_t, 3> kRungs{400, 800, 1600};

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rfc0050 decisive: rank-0 anchor missing");
}

const char* regime_token(DecisiveSlopeRegime r) {
  switch (r) {
    case DecisiveSlopeRegime::SteepLawHolds:
      return "steep_law_holds";
    case DecisiveSlopeRegime::Decaying:
      return "decaying";
    case DecisiveSlopeRegime::DecayedConfirmed:
      return "decayed_confirmed";
    case DecisiveSlopeRegime::Plateau:
      return "plateau";
  }
  return "unknown";
}

struct ScoredRung {
  std::uint64_t iteration = 0;
  double e_embed_bb = 0.0;
  double pure_menu_bb = 0.0;
  bool measured = false;
};

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const bs::tree::TreeLimits limits = offline_census_limits();
  Rfc0050Stage1Domain domain = build_stage1_domain(anchor.representative.river_def, limits);

  RiverSolveConfig cfg;
  cfg.def = anchor.representative.river_def;
  cfg.iterations = kRungs.back();
  cfg.threads = kThreads;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = domain.menu;
  cfg.tree_limits = limits;
  const std::vector<std::uint64_t> checkpoints(kRungs.begin(), kRungs.end());

  std::vector<ScoredRung> scored;
  const auto t0 = std::chrono::steady_clock::now();
  cfg.stop_predicate = [](std::uint64_t, double elapsed_ms) {
    return elapsed_ms >= kDiagnosticWallMs;
  };
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
    RiverSolveResult snap;
    snap.avg_sigma = cp.avg_sigma;
    snap.nash_conv_bb = cp.nash_conv_bb;
    const ReachBoundResult rb =
        reach_bound_from_snapshot(cfg.def, snap, domain.reference_tree, domain.bounded_tree,
                                  domain.subset, CapExtensionCompletion::Uniform);
    const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
    if (!pure_menu_reads_zero(pure_menu_bb) || !std::isfinite(rb.exact_exploitability_bb)) {
      std::fprintf(stderr, "structural/instrumentation failure at rung %llu: pure_menu %.10g\n",
                   static_cast<unsigned long long>(cp.iteration), pure_menu_bb);
      std::_Exit(5);
    }
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    scored.push_back({cp.iteration, rb.exact_exploitability_bb, pure_menu_bb, true});
    std::printf("rung,srp32,%llu,%.8f,%.8f,1,%.3f\n", static_cast<unsigned long long>(cp.iteration),
                rb.exact_exploitability_bb, pure_menu_bb, wall_ms);
    std::fflush(stdout);
  };

  std::printf("record_type,anchor,iteration,e_embed_bb,pure_menu_bb,pure_menu_measured,wall_ms\n");
  std::fflush(stdout);
  const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
  const double wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  struct rusage ru{};
  getrusage(RUSAGE_SELF, &ru);

  if (scored.size() < 3) {
    // The 3-hour wall tripped before all three rungs; the partial curve is
    // still flushed above. Report the budget outcome without inventing a slope.
    std::printf(
        "summary,srp32,completed=%llu,rungs=%zu,stopped_early=%d,wall_ms=%.3f,verdict="
        "budget_unreachable_full_curve\n",
        static_cast<unsigned long long>(solved.iterations_completed), scored.size(),
        solved.stopped_early ? 1 : 0, wall_ms);
    return 4;
  }

  const DecisiveSlopeResult result =
      classify_decisive_slope(scored[0].e_embed_bb, scored[1].e_embed_bb, scored[2].e_embed_bb);
  // Two extrapolations anchored on the measured rung-200 Stage 1 value and the
  // measured three-point slope, presented as scenarios, not certified targets.
  std::printf(
      "summary,srp32,completed=%llu,rungs=%zu,stopped_early=%d,wall_ms=%.3f,slope=%.4f,"
      "regime=%s,plateau=%d,e400=%.8f,e800=%.8f,e1600=%.8f,peak_rss_kb=%ld\n",
      static_cast<unsigned long long>(solved.iterations_completed), scored.size(),
      solved.stopped_early ? 1 : 0, wall_ms, result.slope, regime_token(result.regime),
      result.plateau ? 1 : 0, scored[0].e_embed_bb, scored[1].e_embed_bb, scored[2].e_embed_bb,
      ru.ru_maxrss);
  std::fflush(stdout);
  return 0;
}
