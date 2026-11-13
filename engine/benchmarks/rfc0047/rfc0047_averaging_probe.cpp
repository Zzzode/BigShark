// RFC 0048 D198 averaging-policy re-solve probe (manual, NOT a CTest).
// One single-instance process performs BOTH averaging modes on the exact
// D195 coarse cap-1 w1 fixture (one uniform candidate plus one in-process
// linear control), 300 iterations each, and evaluates every fresh snapshot
// independently in the frozen cap-0 reference domain. The historical
// certificates never transfer; output is exploratory evidence.
//
// Usage: bigshark-rfc0047-averaging-probe ITERS [THREADS]
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

#include "cap_embedding.hpp"
#include "frontier_enumerator.hpp"
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
    "reference_label,m2_marking,averaging_mode,completion,policy_hash,"
    "bounded_abstraction_digest,reference_abstraction_digest,mapped_action_nodes,"
    "bounded_nash_conv_bb,exact_exploitability_bb,pure_menu_term_bb,u1_bounded_infosets,"
    "u1_reference_infosets,u1_reference_under_1m,u2_artifact_rows,u2_artifact_bytes,"
    "u3_bounded_infosets,u3_reference_infosets,reference_under_node_limit,"
    "reference_under_byte_limit,wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n";

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
  const ActionAbstraction bounded = make_grid_abstraction(SizingGrid::Coarse, 1);
  const ActionAbstraction reference = ActionAbstraction::identity(0);
  const AbstractTree ref_tree(def, reference);
  const AbstractTree bounded_tree(def, bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  const GridBudget ub = measure_grid_budget(def, "reached_river", bounded, "coarse", 1, "coarse");
  const GridBudget ur = measure_grid_budget(def, "reached_river", reference, "cap0", 0, "coarse");

  // Both modes solved in this one process: candidate (uniform average) and
  // the in-process linear control. No re-solve happens inside evaluation.
  RiverSolveResult linear;
  RiverSolveResult uniform;
  {
    RiverSolveConfig cfg;
    cfg.def = def;
    cfg.iterations = iters;
    cfg.linear_averaging = true;
    cfg.threads = threads;
    cfg.action_abstraction = bounded;
    linear = solve_river_subgame(cfg, {});
  }
  {
    RiverSolveConfig cfg;
    cfg.def = def;
    cfg.iterations = iters;
    cfg.linear_averaging = false;
    cfg.threads = threads;
    cfg.action_abstraction = bounded;
    uniform = solve_river_subgame(cfg, {});
  }
  const std::uint64_t h_lin = hash_average_snapshot(linear.avg_sigma);
  const std::uint64_t h_uni = hash_average_snapshot(uniform.avg_sigma);
  const double l1 = max_snapshot_l1(linear.avg_sigma, uniform.avg_sigma);
  const bool distinct = (h_lin != h_uni) && (l1 >= 1e-12);
  std::fprintf(stderr, "distinct=%d max_row_l1=%.6e hashes linear=%llu uniform=%llu\n",
               distinct ? 1 : 0, l1, static_cast<unsigned long long>(h_lin),
               static_cast<unsigned long long>(h_uni));

  std::printf("%s", kHeader);
  for (const bool is_uniform : {false, true}) {
    const RiverSolveResult& solved = is_uniform ? uniform : linear;
    const std::uint64_t hash = is_uniform ? h_uni : h_lin;
    const char* mode = is_uniform ? "uniform_average" : "linear_average_control";
    for (CapExtensionCompletion arm : arms) {
      rusage ru0{};
      getrusage(RUSAGE_SELF, &ru0);
      const auto t0 = std::chrono::steady_clock::now();
      const ReachBoundResult rb =
          reach_bound_from_snapshot(def, solved, ref_tree, bounded_tree, subset, arm);
      const ResourceSample rs = sample_resource(t0, ru0);
      const double pure_menu = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
      std::printf(
          "%s,%s,%s,%s,%llu,%llu,%llu,%zu,%.8f,%.8f,%.8f,%llu,%llu,%d,%llu,%llu,%llu,%llu,%d,%d,"
          "%.3f,%.3f,%ld,%ld\n",
          rb.reference_label, rb.m2_marking, mode, arm_name(arm),
          static_cast<unsigned long long>(hash),
          static_cast<unsigned long long>(rb.bounded_abstraction_digest),
          static_cast<unsigned long long>(rb.reference_abstraction_digest), rb.mapped_action_nodes,
          rb.bounded_nash_conv_bb, rb.exact_exploitability_bb, pure_menu,
          static_cast<unsigned long long>(ub.u1_online_resident_infosets),
          static_cast<unsigned long long>(ur.u1_online_resident_infosets),
          ur.u1_under_1m_infoset ? 1 : 0, static_cast<unsigned long long>(ur.u2_artifact_rows),
          static_cast<unsigned long long>(ur.u2_artifact_bytes),
          static_cast<unsigned long long>(ub.u3_build_grand_total_infosets),
          static_cast<unsigned long long>(ur.u3_build_grand_total_infosets),
          ur.under_tree_node_limit ? 1 : 0, ur.under_byte_limit ? 1 : 0, rs.wall_ms, rs.cpu_ms,
          rs.rss_peak_kb, rs.rss_current_kb);
    }
  }
  return 0;
}
