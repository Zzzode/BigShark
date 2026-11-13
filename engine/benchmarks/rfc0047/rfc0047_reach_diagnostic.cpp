// RFC 0048 Path B (D192) reach-bound diagnostic probe (manual, NOT a
// CTest). Single-instance bounded run on ONE w1 reached-river root: the
// exact Lemma-1 node-level allocation of the D184 menu failure, the
// Lemma-2 structural sufficient certificate, the U1/U2/U3 unit taxonomy,
// and explicit wall/CPU/RSS cost. Every row is candidate evidence; the
// frozen G5 cap0 verdict is never overwritten.
//
// Usage: bigshark-rfc0047-reach-diagnostic GRID ITERS [THREADS]
//            [uniform|br_favorable|both]
//        GRID = coarse | wide
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
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

const char* kSummaryHeader =
    "reference_label,m2_marking,grid,reference_kind,cap0_direct_map,completion,"
    "bounded_abstraction_digest,reference_abstraction_digest,mapped_action_nodes,"
    "widened_nodes,bounded_nash_conv_bb,exact_exploitability_bb,"
    "structural_certificate_bb,u1_bounded_infosets,u1_reference_infosets,"
    "u1_reference_under_1m,u2_artifact_rows,u2_artifact_bytes,u3_bounded_infosets,"
    "u3_reference_infosets,reference_under_node_limit,reference_under_byte_limit,"
    "wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n";

void print_summary(const char* grid, const char* reference_kind, const char* cap0_direct_map,
                   CapExtensionCompletion arm, const ReachBoundResult& rb, const GridBudget& ub,
                   const GridBudget& ur, const ResourceSample& rs) {
  std::printf(
      "%s,%s,%s,%s,%s,%s,%llu,%llu,%zu,%zu,%.8f,%.8f,%.8f,%llu,%llu,%d,%llu,%llu,%llu,%llu,%d,%d,"
      "%.3f,%.3f,%ld,%ld\n",
      rb.reference_label, rb.m2_marking, grid, reference_kind, cap0_direct_map, arm_name(arm),
      static_cast<unsigned long long>(rb.bounded_abstraction_digest),
      static_cast<unsigned long long>(rb.reference_abstraction_digest), rb.mapped_action_nodes,
      rb.widened_nodes, rb.bounded_nash_conv_bb, rb.exact_exploitability_bb,
      rb.structural_certificate_bb, static_cast<unsigned long long>(ub.u1_online_resident_infosets),
      static_cast<unsigned long long>(ur.u1_online_resident_infosets),
      ur.u1_under_1m_infoset ? 1 : 0, static_cast<unsigned long long>(ur.u2_artifact_rows),
      static_cast<unsigned long long>(ur.u2_artifact_bytes),
      static_cast<unsigned long long>(ub.u3_build_grand_total_infosets),
      static_cast<unsigned long long>(ur.u3_build_grand_total_infosets),
      ur.under_tree_node_limit ? 1 : 0, ur.under_byte_limit ? 1 : 0, rs.wall_ms, rs.cpu_ms,
      rs.rss_peak_kb, rs.rss_current_kb);
}

// Per-node exact contribution in BB, sorted descending: where the menu
// restriction's exploitability actually concentrates by reach.
struct NodeContribution {
  std::size_t seat;
  std::size_t reference_node;
  bool widened;
  double contribution_bb;
  double mean_own_reach;
};

void print_node_detail(const ReachBoundResult& rb) {
  std::printf("\n# node contributions (exact Lemma-1 allocation, BB, descending)\n");
  std::printf("seat,reference_node,widened,mean_own_reach,contribution_bb\n");
  std::vector<NodeContribution> rows;
  for (std::size_t s = 0; s < 2; ++s) {
    for (const NodeGapRecord& r : rb.records[s]) {
      double raw = 0.0;
      double reach_sum = 0.0;
      for (std::uint32_t i = 0; i < r.own_reach.size(); ++i) {
        raw += r.own_reach[i] * r.delta[i];
        reach_sum += r.own_reach[i];
      }
      rows.push_back({.seat = s,
                      .reference_node = r.reference_node,
                      .widened = r.widened,
                      .contribution_bb = raw / rb.bb_normalization,
                      .mean_own_reach = reach_sum / static_cast<double>(r.own_reach.size())});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const NodeContribution& a, const NodeContribution& b) {
    return a.contribution_bb > b.contribution_bb;
  });
  for (const NodeContribution& x : rows)
    std::printf("%zu,%zu,%d,%.8f,%.8f\n", x.seat, x.reference_node, x.widened ? 1 : 0,
                x.mean_own_reach, x.contribution_bb);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s coarse|wide ITERS [THREADS] [uniform|br_favorable|both]\n",
                 argv[0]);
    return 2;
  }
  const std::string grid_name = argv[1];
  SizingGrid grid;
  if (grid_name == "coarse")
    grid = SizingGrid::Coarse;
  else if (grid_name == "wide")
    grid = SizingGrid::Wide;
  else {
    std::fprintf(stderr, "unknown grid '%s'\n", grid_name.c_str());
    return 2;
  }
  const std::uint64_t iters = std::stoull(argv[2]);
  const std::size_t threads = argc >= 4 ? std::stoull(argv[3]) : 1;
  const std::string arm_select = argc >= 5 ? argv[4] : "both";
  const std::vector<CapExtensionCompletion> arms =
      arm_select == "uniform" ? std::vector{CapExtensionCompletion::Uniform}
      : arm_select == "br_favorable"
          ? std::vector{CapExtensionCompletion::BrFavorable}
          : std::vector{CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable};

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;

  const ActionAbstraction bounded = make_grid_abstraction(grid, 1);
  const ActionAbstraction union_action =
      make_union_abstraction(ActionAbstraction::identity(0), bounded);
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree bounded_tree(def, bounded);
  bool cap0_maps = true;
  try {
    (void)map_bounded_subset(cap0, bounded_tree);
  } catch (const std::invalid_argument&) {
    cap0_maps = false;
  }

  const GridBudget ub = measure_grid_budget(def, "reached_river", bounded, grid_name, 1, grid_name);
  const GridBudget ur_cap0 = measure_grid_budget(
      def, "reached_river", ActionAbstraction::identity(0), "cap0", 0, grid_name);
  const GridBudget ur_union =
      measure_grid_budget(def, "reached_river", union_action, "union", 0, grid_name);
  if (ub.resource_boundary || ur_cap0.resource_boundary || ur_union.resource_boundary) {
    std::fprintf(stderr, "resource boundary encountered\n");
    return 3;
  }

  std::printf("%s", kSummaryHeader);
  for (CapExtensionCompletion arm : arms) {
    if (cap0_maps) {
      rusage ru0{};
      getrusage(RUSAGE_SELF, &ru0);
      const auto t0 = std::chrono::steady_clock::now();
      const ReachBoundResult rb =
          measure_reach_bound(def, bounded, ActionAbstraction::identity(0), iters, threads, arm);
      print_summary(grid_name.c_str(), "cap0_subset", "mapped", arm, rb, ub, ur_cap0,
                    sample_resource(t0, ru0));
      if (arm == CapExtensionCompletion::Uniform)
        print_node_detail(rb);
    }
    rusage ru0{};
    getrusage(RUSAGE_SELF, &ru0);
    const auto t0 = std::chrono::steady_clock::now();
    const ReachBoundResult rb =
        measure_reach_bound(def, bounded, union_action, iters, threads, arm);
    print_summary(grid_name.c_str(), "union", cap0_maps ? "mapped" : "rejected", arm, rb, ub,
                  ur_union, sample_resource(t0, ru0));
    if (!cap0_maps && arm == CapExtensionCompletion::Uniform)
      print_node_detail(rb);
  }
  return 0;
}
