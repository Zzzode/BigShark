// RFC 0048 D249 manual single-instance fine-grid anchor probe (NOT a CTest).
// One process performs two 300-iteration river solves on the Fine sizing
// grid at the frozen w1 reached-river root: one uniform-average candidate
// and one in-process linear-average control, each with fresh state. Every
// snapshot is evaluated independently in its empirically determined
// reference domain (cap0 subset when every Fine action maps, otherwise the
// declared union reference). A steady-clock 120 s per-solve tripwire emits
// a typed wall_budget_exceeded row and exits non-zero; the OS timeout
// launcher is the outer hard stop. Trees use the enforced TreeLimits; RSS
// is a measured tripwire, never an enforced allocation cap. No sweep,
// second root, retry, or budget increase exists in this binary.
//
// Usage: bigshark-rfc0047-fine-grid-probe ITERS [THREADS]
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
#include "patha_grid_budget.hpp"
#include "policy_hash.hpp"
#include "reach_bound.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "union_menu.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;
using bs::tree::TreeLimits;

namespace {

constexpr double kWallBudgetMs = 120'000.0;
constexpr double kGateBb = 0.002;

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

const char* kHeader =
    "reference_label,m2_marking,grid,reference_kind,cap0_direct_map,averaging_mode,"
    "policy_hash,bounded_abstraction_digest,reference_abstraction_digest,mapped_action_nodes,"
    "bounded_nash_conv_bb,exact_exploitability_bb,pure_menu_term_bb,candidate_gate_pass,"
    "wall_ms,cpu_ms,rss_peak_kb,rss_current_kb,wall_budget_exceeded\n";

struct Domain {
  std::unique_ptr<ActionAbstraction> reference;
  std::string reference_kind;
  std::string cap0_direct_map;
  std::unique_ptr<AbstractTree> ref_tree;
  std::unique_ptr<AbstractTree> bounded_tree;
  SubsetMap subset;
};

Domain build_domain(const bs::poker::GameDef& def, const ActionAbstraction& fine,
                    const TreeLimits& limits) {
  Domain d;
  d.bounded_tree = std::make_unique<AbstractTree>(def, fine, limits);
  const AbstractTree cap0(def, ActionAbstraction::identity(0), limits);
  try {
    d.subset = map_bounded_subset(cap0, *d.bounded_tree);
    d.reference = std::make_unique<ActionAbstraction>(ActionAbstraction::identity(0));
    d.reference_kind = "cap0";
    d.cap0_direct_map = "mapped";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
  } catch (const std::invalid_argument&) {
    d.reference = std::make_unique<ActionAbstraction>(
        make_union_abstraction(ActionAbstraction::identity(0), fine));
    d.reference_kind = "union";
    d.cap0_direct_map = "rejected";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
    d.subset = map_bounded_subset(*d.ref_tree, *d.bounded_tree);
  }
  return d;
}

void print_row(const bs::poker::GameDef& def, const Domain& d, const char* mode,
               const RiverSolveResult& solved, const ResourceSample& rs, bool wall_exceeded) {
  const ReachBoundResult rb = reach_bound_from_snapshot(def, solved, *d.ref_tree, *d.bounded_tree,
                                                        d.subset, CapExtensionCompletion::Uniform);
  const double pure_menu = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
  const bool pass = (!wall_exceeded) && rb.exact_exploitability_bb <= kGateBb;
  std::printf(
      "%s,%s,fine,%s,%s,%s,%llu,%llu,%llu,%zu,%.8f,%.8f,%.8f,%d,%.3f,%.3f,%ld,%ld,%d\n",
      rb.reference_label, rb.m2_marking, d.reference_kind.c_str(), d.cap0_direct_map.c_str(), mode,
      static_cast<unsigned long long>(hash_average_snapshot(solved.avg_sigma)),
      static_cast<unsigned long long>(rb.bounded_abstraction_digest),
      static_cast<unsigned long long>(rb.reference_abstraction_digest), rb.mapped_action_nodes,
      rb.bounded_nash_conv_bb, rb.exact_exploitability_bb, pure_menu, pass ? 1 : 0, rs.wall_ms,
      rs.cpu_ms, rs.rss_peak_kb, rs.rss_current_kb, wall_exceeded ? 1 : 0);
}

struct TimedSolve {
  RiverSolveResult solved;
  ResourceSample sample;
  bool wall_exceeded = false;
};

TimedSolve timed_solve(const RiverSolveConfig& base, bool linear, const ActionAbstraction& fine) {
  RiverSolveConfig cfg = base;
  cfg.linear_averaging = linear;
  cfg.action_abstraction = fine;
  rusage ru_start{};
  getrusage(RUSAGE_SELF, &ru_start);
  const auto t0 = std::chrono::steady_clock::now();
  TimedSolve out;
  out.solved = solve_river_subgame(cfg, {});
  out.sample = sample_resource(t0, ru_start);
  if (out.sample.wall_ms > kWallBudgetMs) {
    out.wall_exceeded = true;
    std::fprintf(stderr, "wall_budget_exceeded: %.3f ms > %.0f ms\n", out.sample.wall_ms,
                 kWallBudgetMs);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s ITERS [THREADS]\n", argv[0]);
    return 2;
  }
  const std::uint64_t iters = std::stoull(argv[1]);
  const std::size_t threads = argc >= 3 ? std::stoull(argv[2]) : 1;

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;

  const ActionAbstraction fine = make_grid_abstraction(SizingGrid::Fine, 1);
  const TreeLimits enforced{2'000'000, 256, std::size_t{1} << 30};
  const Domain domain = build_domain(def, fine, enforced);

  RiverSolveConfig base;
  base.def = def;
  base.iterations = iters;
  base.threads = threads;

  std::printf("%s", kHeader);
  bool any_wall_exceeded = false;
  // Linear control first, then the uniform candidate: two fresh solves in one
  // single-instance process, matching the D198 in-process convention.
  for (const bool is_uniform : {false, true}) {
    const TimedSolve run = timed_solve(base, !is_uniform, fine);
    any_wall_exceeded = any_wall_exceeded || run.wall_exceeded;
    print_row(def, domain, is_uniform ? "uniform_average_candidate" : "linear_average_control",
              run.solved, run.sample, run.wall_exceeded);
  }
  return any_wall_exceeded ? 4 : 0;
}
