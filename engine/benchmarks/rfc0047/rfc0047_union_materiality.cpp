// RFC 0048 Path A Stage 2b (D184) union-menu materiality probe (manual, NOT a
// CTest). Single-instance bounded measurement on ONE w1 reached-river root:
// one sizing grid per process, both completion arms, with mechanical
// candidate-domain labels, abstraction/shape digests, the U1/U2/U3 unit
// taxonomy, and explicit wall/CPU/RSS cost. Every row is supporting evidence
// under a candidate reference domain, never an M2/G5 cap0 verdict.
//
// Usage: bigshark-rfc0047-union-materiality GRID ITERS [THREADS]
//            [uniform|br_favorable|both]
//        GRID = coarse | wide
#include <sys/resource.h>
#include <unistd.h>

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include "cap_embedding.hpp"
#include "frontier_enumerator.hpp"
#include "patha_grid_budget.hpp"
#include "strata.hpp"
#include "union_embedding.hpp"
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
  ResourceSample s;
  s.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  s.cpu_ms = (timeval_ms(ru1.ru_utime) - timeval_ms(ru0.ru_utime)) +
             (timeval_ms(ru1.ru_stime) - timeval_ms(ru0.ru_stime));
  s.rss_peak_kb = ru1.ru_maxrss;
  s.rss_current_kb = current_rss_kb();
  return s;
}

const char* arm_name(CapExtensionCompletion arm) {
  return arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable";
}

const char* kHeader =
    "reference_label,m2_marking,grid,reference_kind,cap0_direct_map,completion,"
    "bounded_abstraction_digest,reference_abstraction_digest,bounded_shape_digest,"
    "reference_shape_digest,reference_action_nodes,bounded_action_nodes,mapped_nodes,"
    "bounded_nash_conv_bb,e_embed_bb,pure_menu_term_bb,candidate_gate_pass,"
    "bounded_total_nodes,reference_total_nodes,bounded_bytes,reference_bytes,"
    "u1_bounded_infosets,u1_reference_infosets,u1_reference_under_1m,"
    "u2_artifact_rows,u2_artifact_bytes,u3_bounded_infosets,u3_reference_infosets,"
    "reference_under_node_limit,reference_under_byte_limit,"
    "wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n";

void print_row(const char* grid, const char* reference_kind, const char* cap0_direct_map,
               CapExtensionCompletion arm, const UnionMateriality& m, const GridBudget& ub,
               const GridBudget& ur, const ResourceSample& rs) {
  std::printf(
      "%s,%s,%s,%s,%s,%s,%llu,%llu,%llu,%llu,%zu,%zu,%zu,%.8f,%.8f,%.8f,%d,"
      "%zu,%zu,%zu,%zu,%llu,%llu,%d,%llu,%llu,%llu,%llu,%d,%d,%.3f,%.3f,%ld,%ld\n",
      m.reference_label, m.m2_marking, grid, reference_kind, cap0_direct_map, arm_name(arm),
      static_cast<unsigned long long>(m.bounded_abstraction_digest),
      static_cast<unsigned long long>(m.reference_abstraction_digest),
      static_cast<unsigned long long>(m.bounded_shape_digest),
      static_cast<unsigned long long>(m.reference_shape_digest), m.reference_action_nodes,
      m.bounded_action_nodes, m.mapped_nodes, m.bounded_nash_conv_bb, m.embedded_exploitability_bb,
      m.pure_menu_term_bb, m.candidate_gate_pass ? 1 : 0, ub.total_nodes, ur.total_nodes,
      ub.accounted_bytes, ur.accounted_bytes,
      static_cast<unsigned long long>(ub.u1_online_resident_infosets),
      static_cast<unsigned long long>(ur.u1_online_resident_infosets),
      ur.u1_under_1m_infoset ? 1 : 0, static_cast<unsigned long long>(ur.u2_artifact_rows),
      static_cast<unsigned long long>(ur.u2_artifact_bytes),
      static_cast<unsigned long long>(ub.u3_build_grand_total_infosets),
      static_cast<unsigned long long>(ur.u3_build_grand_total_infosets),
      ur.under_tree_node_limit ? 1 : 0, ur.under_byte_limit ? 1 : 0, rs.wall_ms, rs.cpu_ms,
      rs.rss_peak_kb, rs.rss_current_kb);
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

  // Structural cap0 compatibility: coarse is an interleaved subset of
  // identity(0); wide carries reference-foreign targets and must reject.
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree bounded_tree(def, bounded);
  bool cap0_maps = true;
  try {
    (void)map_bounded_subset(cap0, bounded_tree);
  } catch (const std::invalid_argument&) {
    cap0_maps = false;
  }

  const GridBudget ub = measure_grid_budget(def, "reached_river", bounded, grid_name, 1, grid_name);
  const GridBudget ur =
      measure_grid_budget(def, "reached_river", union_action, "union", 0, grid_name);
  if (ub.resource_boundary || ur.resource_boundary) {
    std::fprintf(stderr, "resource boundary: bounded=%s reference=%s\n", ub.boundary_reason.c_str(),
                 ur.boundary_reason.c_str());
    return 3;
  }

  std::printf("%s", kHeader);
  for (CapExtensionCompletion arm : arms) {
    if (cap0_maps) {
      rusage ru0{};
      getrusage(RUSAGE_SELF, &ru0);
      const auto t0 = std::chrono::steady_clock::now();
      const UnionMateriality m = measure_subset_materiality(
          def, bounded, ActionAbstraction::identity(0), iters, threads, arm);
      print_row(grid_name.c_str(), "cap0_subset", "mapped", arm, m, ub, ur,
                sample_resource(t0, ru0));
    }
    rusage ru0{};
    getrusage(RUSAGE_SELF, &ru0);
    const auto t0 = std::chrono::steady_clock::now();
    const UnionMateriality m =
        measure_subset_materiality(def, bounded, union_action, iters, threads, arm);
    print_row(grid_name.c_str(), "union", cap0_maps ? "mapped" : "rejected", arm, m, ub, ur,
              sample_resource(t0, ru0));
  }
  return 0;
}
