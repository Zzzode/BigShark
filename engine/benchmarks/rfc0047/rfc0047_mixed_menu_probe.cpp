// RFC 0048 D202 per-node mixed-menu anchor probe (manual, NOT a CTest).
//
// One single-instance process runs the ONE pre-selected root-only widening
// candidate on the w1 reached-river fixture for the frozen 300 iterations,
// then evaluates the immutable re-expanded snapshot independently in the
// cap-0 reference domain under BOTH completion arms (no re-solve inside
// evaluation, no second 300-iteration solve; the coarse baseline is the
// bitwise-reproducible D184/D195 same-source evidence, not recomputed).
// Output is exploratory mechanism evidence: a non-cap-1 candidate menu can
// never be reported as an M2/G5 pass, and no prior certificate transfers.
//
// Usage: bigshark-rfc0047-mixed-menu-probe ITERS [THREADS]
//            [uniform|br_favorable|both]
#include <sys/resource.h>
#include <unistd.h>

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "frontier_enumerator.hpp"
#include "mixed_menu.hpp"
#include "patha_grid_budget.hpp"
#include "policy_hash.hpp"
#include "reach_bound.hpp"
#include "strata.hpp"
#include "union_menu.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;

namespace {

struct ResourceSample {
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
  long rss_peak_kb = 0;
  long rss_current_kb = 0;
};

double timeval_ms(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
}

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

ResourceSample sample_resource(const std::chrono::steady_clock::time_point& t0, const rusage& ru0) {
  rusage ru1{};
  getrusage(RUSAGE_SELF, &ru1);
  const auto t1 = std::chrono::steady_clock::now();
  return {.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count(),
          .cpu_ms = (timeval_ms(ru1.ru_utime) - timeval_ms(ru0.ru_utime)) +
                    (timeval_ms(ru1.ru_stime) - timeval_ms(ru0.ru_stime)),
          .rss_peak_kb = ru1.ru_maxrss,
          .rss_current_kb = current_rss_kb()};
}

const char* arm_name(CapExtensionCompletion arm) {
  return arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable";
}

const char* kHeader =
    "reference_label,m2_marking,candidate,completion,policy_hash,"
    "reference_abstraction_digest,mapped_action_nodes,bounded_nash_conv_bb,"
    "exact_exploitability_bb,pure_menu_term_bb,compact_slot_total,reference_edge_total,"
    "u1_coarse_infosets,u1_reference_infosets,u1_reference_under_1m,u2_artifact_rows,"
    "u2_artifact_bytes,u3_coarse_infosets,u3_reference_infosets,reference_under_node_limit,"
    "reference_under_byte_limit,solve_wall_ms,solve_cpu_ms,eval_wall_ms,eval_cpu_ms,"
    "rss_peak_kb,rss_current_kb\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s ITERS [THREADS] [uniform|br_favorable|both]\n", argv[0]);
    return 2;
  }
  const std::uint64_t iters = std::stoull(argv[1]);
  const std::size_t threads = argc >= 3 ? std::stoull(argv[2]) : 1;
  const std::string arm_select = argc >= 4 ? argv[3] : "both";
  const std::vector<CapExtensionCompletion> arms =
      arm_select == "uniform" ? std::vector{CapExtensionCompletion::Uniform}
      : arm_select == "br_favorable"
          ? std::vector{CapExtensionCompletion::BrFavorable}
          : std::vector{CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable};

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;
  const ActionAbstraction coarse = make_grid_abstraction(SizingGrid::Coarse, 1);
  const ActionAbstraction reference = ActionAbstraction::identity(0);
  const AbstractTree ref_tree(def, reference);
  const AbstractTree coarse_tree(def, coarse);
  const MixedMenuMask mask = make_root_only_widening_mask(ref_tree, coarse_tree);
  const SubsetMap subset = mask_to_subset_map(ref_tree, mask);
  const GridBudget ub = measure_grid_budget(def, "reached_river", coarse, "coarse", 1, "coarse");
  const GridBudget ur = measure_grid_budget(def, "reached_river", reference, "cap0", 0, "coarse");

  // The single authorized 300-iteration solve of the mixed candidate.
  rusage solve_ru{};
  getrusage(RUSAGE_SELF, &solve_ru);
  const auto solve_t0 = std::chrono::steady_clock::now();
  const MixedMenuSolveResult mixed = solve_mixed_menu(def, reference, mask, iters, threads);
  const ResourceSample solve_rs = sample_resource(solve_t0, solve_ru);

  const std::uint64_t hash = hash_average_snapshot(mixed.river.avg_sigma);
  std::fprintf(stderr,
               "mixed root-only candidate: hash=%llu bounded_nc=%.8f compact_slots=%zu/%zu\n",
               static_cast<unsigned long long>(hash), mixed.river.nash_conv_bb,
               mixed.compact_slot_total, mixed.reference_edge_total);

  std::printf("%s", kHeader);
  for (CapExtensionCompletion arm : arms) {
    rusage eval_ru{};
    getrusage(RUSAGE_SELF, &eval_ru);
    const auto eval_t0 = std::chrono::steady_clock::now();
    // Single-tree wiring: reference tree as both arguments, identity node
    // map, reference-width re-expanded rows, mask edge map (review L2).
    const ReachBoundResult rb =
        reach_bound_from_snapshot(def, mixed.river, ref_tree, ref_tree, subset, arm);
    const ResourceSample eval_rs = sample_resource(eval_t0, eval_ru);
    const double pure_menu = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
    std::printf(
        "%s,%s,root_only_widened,%s,%llu,%llu,%zu,%.8f,%.8f,%.8f,%zu,%zu,%llu,%llu,%d,%llu,%llu,"
        "%llu,%llu,%d,%d,%.3f,%.3f,%.3f,%.3f,%ld,%ld\n",
        rb.reference_label, rb.m2_marking, arm_name(arm), static_cast<unsigned long long>(hash),
        static_cast<unsigned long long>(rb.reference_abstraction_digest), rb.mapped_action_nodes,
        rb.bounded_nash_conv_bb, rb.exact_exploitability_bb, pure_menu, mixed.compact_slot_total,
        mixed.reference_edge_total, static_cast<unsigned long long>(ub.u1_online_resident_infosets),
        static_cast<unsigned long long>(ur.u1_online_resident_infosets),
        ur.u1_under_1m_infoset ? 1 : 0, static_cast<unsigned long long>(ur.u2_artifact_rows),
        static_cast<unsigned long long>(ur.u2_artifact_bytes),
        static_cast<unsigned long long>(ub.u3_build_grand_total_infosets),
        static_cast<unsigned long long>(ur.u3_build_grand_total_infosets),
        ur.under_tree_node_limit ? 1 : 0, ur.under_byte_limit ? 1 : 0, solve_rs.wall_ms,
        solve_rs.cpu_ms, eval_rs.wall_ms, eval_rs.cpu_ms, eval_rs.rss_peak_kb,
        eval_rs.rss_current_kb);
  }
  return 0;
}
