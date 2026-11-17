// RFC 0050 Stage 2 D316 deep-anchor fixed-rung rerun (manual, NOT a CTest).
// The fixed-wall boundary run measured per-iteration speed, not a
// comparable boundary: the two deepest anchors (ranks 4 and 5) never
// reached the iteration-100 first rung inside 1,800 s. This probe reruns
// those two anchors at a FIXED rung target — the second rung, iteration
// 200 — with a 3x budget cap (5,400 s) instead of a fixed wall, so each
// anchor either produces comparable rung-100 and rung-200 scoring points
// or is reported as budget-unreachable at that rung. It records the first-
// rung threshold explicitly and a pure_menu_measured flag that is 1 only
// when the scoring sink actually ran, so an unmeasured default zero can
// never be mistaken for a measured zero pure-menu term. No argv; anchor
// keys, rung target, and cap are hard-coded and the nine-rung ladder and
// 0.002 BB gate are untouched.
//
// Usage: bigshark-rfc0050-deep-rerun
#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "offline_convergence.hpp"
#include "reach_bound.hpp"
#include "rfc0050_stage1.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;

namespace {

constexpr double kThreeXWallMs = 3.0 * kRfc0050Stage1WallMs;  // 5,400 s
constexpr std::uint64_t kFirstRung = 100;                     // kRfc0050Stage1Rungs[0]
constexpr std::uint64_t kTargetRung = 200;                    // second rung

struct DeepTarget {
  std::string family;
  bs::poker::Chips pot;
  bs::poker::Chips c0;
  bs::poker::Chips c1;
};

const LedgerAnchor* find_anchor(const std::vector<LedgerAnchor>& anchors,
                                const DeepTarget& wanted) {
  for (const LedgerAnchor& a : anchors)
    if (a.family == wanted.family && a.key.pot == wanted.pot && a.key.contribution0 == wanted.c0 &&
        a.key.contribution1 == wanted.c1)
      return &a;
  return nullptr;
}

}  // namespace

int main() {
  const std::vector<DeepTarget> targets{
      {"srp", 12, 6, 6},     // boundary rank 4
      {"limped", 10, 5, 5},  // boundary rank 5
  };
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const bs::tree::TreeLimits limits = offline_census_limits();

  std::printf(
      "record_type,family,pot,contribution0,contribution1,first_rung_iteration_threshold,"
      "target_rung_iteration,cap_wall_ms,iteration,bounded_nc_bb,e_embed_bb,pure_menu_bb,"
      "pure_menu_measured,wall_ms,peak_rss_kb\n");
  std::fflush(stdout);

  for (const DeepTarget& wanted : targets) {
    const LedgerAnchor* anchor = find_anchor(anchors, wanted);
    if (anchor == nullptr) {
      std::fprintf(stderr, "deep rerun anchor %s %llu missing\n", wanted.family.c_str(),
                   static_cast<unsigned long long>(wanted.pot));
      return 4;
    }
    Rfc0050Stage1Domain domain = build_stage1_domain(anchor->representative.river_def, limits);

    RiverSolveConfig cfg;
    cfg.def = anchor->representative.river_def;
    cfg.iterations = kTargetRung;
    cfg.threads = 1;
    cfg.linear_averaging = true;
    cfg.rm_plus = true;
    cfg.dcfr = false;
    cfg.action_abstraction = domain.menu;
    cfg.tree_limits = limits;
    const std::vector<std::uint64_t> checkpoints{kFirstRung, kTargetRung};

    bool pure_menu_measured = false;
    bool structural_fault = false;
    const auto t0 = std::chrono::steady_clock::now();
    cfg.stop_predicate = [](std::uint64_t, double elapsed_ms) {
      return elapsed_ms >= kThreeXWallMs;
    };
    cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
      RiverSolveResult snap;
      snap.avg_sigma = cp.avg_sigma;
      snap.nash_conv_bb = cp.nash_conv_bb;
      const ReachBoundResult rb =
          reach_bound_from_snapshot(cfg.def, snap, domain.reference_tree, domain.bounded_tree,
                                    domain.subset, CapExtensionCompletion::Uniform);
      const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
      pure_menu_measured = true;  // only a real scoring sets this
      if (!pure_menu_reads_zero(pure_menu_bb) || !std::isfinite(rb.exact_exploitability_bb))
        structural_fault = true;
      const double wall_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      struct rusage ru{};
      getrusage(RUSAGE_SELF, &ru);
      std::printf(
          "rung,%s,%llu,%llu,%llu,%llu,%llu,%.0f,%llu,%.8f,%.8f,%.8f,%d,%.3f,%ld\n",
          wanted.family.c_str(), static_cast<unsigned long long>(wanted.pot),
          static_cast<unsigned long long>(wanted.c0), static_cast<unsigned long long>(wanted.c1),
          static_cast<unsigned long long>(kFirstRung), static_cast<unsigned long long>(kTargetRung),
          kThreeXWallMs, static_cast<unsigned long long>(cp.iteration), rb.bounded_nash_conv_bb,
          rb.exact_exploitability_bb, pure_menu_bb, pure_menu_measured ? 1 : 0, wall_ms,
          ru.ru_maxrss);
      std::fflush(stdout);
    };

    const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const char* reach = solved.stopped_early
                            ? (pure_menu_measured ? "budget_reached_partial_rungs"
                                                  : "budget_unreachable_first_rung")
                            : "target_rung_reached";
    // Summary carries no metric numbers; the measured flag states whether any
    // zero-menu reading is evidence rather than an unmeasured default.
    std::printf(
        "summary,%s,%llu,%llu,%llu,%llu,%llu,%.0f,%llu,,,,%d,%.3f,%ld\n", wanted.family.c_str(),
        static_cast<unsigned long long>(wanted.pot), static_cast<unsigned long long>(wanted.c0),
        static_cast<unsigned long long>(wanted.c1), static_cast<unsigned long long>(kFirstRung),
        static_cast<unsigned long long>(kTargetRung), kThreeXWallMs,
        static_cast<unsigned long long>(solved.iterations_completed), pure_menu_measured ? 1 : 0,
        wall_ms, ru.ru_maxrss);
    std::printf("# reach %s %s(%llu): stopped_early=%d completed=%llu measured=%d fault=%d\n",
                reach, wanted.family.c_str(), static_cast<unsigned long long>(wanted.pot),
                solved.stopped_early ? 1 : 0,
                static_cast<unsigned long long>(solved.iterations_completed),
                pure_menu_measured ? 1 : 0, structural_fault ? 1 : 0);
    std::fflush(stdout);
    if (structural_fault)
      return 5;
  }
  return 0;
}
