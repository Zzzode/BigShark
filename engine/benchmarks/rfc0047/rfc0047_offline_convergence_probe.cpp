// RFC 0049 Stage 1 Gate 2 manual single-instance offline convergence probe
// (NOT a CTest). One process solves ONE pre-registered w1 candidate through
// the exact-combo river kernel with the frozen rung ladder and an in-process
// 1,800 s wall budget, scoring every completed rung through the reach-bound
// certifier and flushing the row before the next rung. A per-iteration stop
// predicate latches at the wall so an overrun is a typed BUDGET_NO_GO with a
// finite partial curve; the 1,900 s OS timeout launched by the runner is an
// outer-only stop. The candidate index selects from the hard-coded frozen
// list; rungs, budgets, and the 0.002 BB gate cannot be overridden by argv.
//
// Usage: bigshark-rfc0047-offline-convergence-probe CANDIDATE_INDEX
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "frontier_enumerator.hpp"
#include "offline_census.hpp"
#include "offline_convergence.hpp"
#include "patha_grid_budget.hpp"
#include "reach_bound.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "union_menu.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;
using bs::tree::TreeLimits;

namespace {

constexpr double kWallBudgetMs = 1'800'000.0;  // 1,800 s in-process
const std::uint64_t kRungs[] = {100, 200, 400, 800, 1'500, 3'000, 6'000, 12'000, 20'000};

struct Candidate {
  std::string label;
  std::string family;
  int raise_cap;
  ActionAbstraction menu;
};

std::vector<Candidate> frozen_candidates() {
  std::vector<Candidate> out;
  out.push_back(
      {"coarse-cap1-control", "control", 1, make_grid_abstraction(SizingGrid::Coarse, 1)});
  for (int cap = 2; cap <= 6; ++cap) {
    out.push_back({"identity-cap" + std::to_string(cap), "raise_cap", cap,
                   ActionAbstraction::identity(static_cast<std::uint8_t>(cap))});
  }
  out.push_back({"wide-cap1", "sizing_grid", 1, make_grid_abstraction(SizingGrid::Wide, 1)});
  out.push_back({"fine-cap1", "sizing_grid", 1, make_grid_abstraction(SizingGrid::Fine, 1)});
  return out;
}

struct Domain {
  std::unique_ptr<ActionAbstraction> reference;
  std::string reference_kind;
  std::string cap0_direct_map;
  std::unique_ptr<AbstractTree> ref_tree;
  std::unique_ptr<AbstractTree> bounded_tree;
  SubsetMap subset;
};

Domain build_domain(const bs::poker::GameDef& def, const ActionAbstraction& bounded,
                    const TreeLimits& limits) {
  Domain d;
  d.bounded_tree = std::make_unique<AbstractTree>(def, bounded, limits);
  const AbstractTree cap0(def, ActionAbstraction::identity(0), limits);
  try {
    d.subset = map_bounded_subset(cap0, *d.bounded_tree);
    d.reference = std::make_unique<ActionAbstraction>(ActionAbstraction::identity(0));
    d.reference_kind = "cap0";
    d.cap0_direct_map = "mapped";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
  } catch (const std::invalid_argument&) {
    d.reference = std::make_unique<ActionAbstraction>(
        make_union_abstraction(ActionAbstraction::identity(0), bounded));
    d.reference_kind = "union";
    d.cap0_direct_map = "rejected";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
    d.subset = map_bounded_subset(*d.ref_tree, *d.bounded_tree);
  }
  return d;
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

constexpr std::size_t kColumns = 16;

std::string join_row(const std::array<std::string, kColumns>& f) {
  std::string out;
  for (std::size_t i = 0; i < kColumns; ++i) {
    if (i)
      out.push_back(',');
    out += f[i];
  }
  out.push_back('\n');
  return out;
}

std::string fnum(double v, const char* fmt) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), fmt, v);
  return std::string(buf);
}

std::string fint(long long v) {
  return std::to_string(v);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s CANDIDATE_INDEX\n", argv[0]);
    return 2;
  }
  const auto candidates = frozen_candidates();
  std::size_t idx = 0;
  try {
    idx = static_cast<std::size_t>(std::stoull(argv[1]));
  } catch (const std::exception&) {
    std::fprintf(stderr, "candidate index must be an integer in [0,%zu)\n", candidates.size());
    return 2;
  }
  if (idx >= candidates.size()) {
    std::fprintf(stderr, "candidate index %zu outside frozen list [0,%zu)\n", idx,
                 candidates.size());
    return 2;
  }
  const Candidate& cand = candidates[idx];

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;
  const TreeLimits limits = offline_census_limits();
  Domain domain = build_domain(def, cand.menu, limits);

  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kRungs[8];
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = cand.menu;
  cfg.tree_limits = limits;
  const std::vector<std::uint64_t> checkpoints(std::begin(kRungs), std::end(kRungs));

  std::vector<OfflineRungRow> rows;
  rusage ru0{};
  getrusage(RUSAGE_SELF, &ru0);
  const auto t0 = std::chrono::steady_clock::now();
  cfg.stop_predicate = [](std::uint64_t, double elapsed_ms) { return elapsed_ms >= kWallBudgetMs; };
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
    RiverSolveResult snap;
    // reach_bound_from_snapshot reads avg_sigma for the BR walks and copies
    // solved.nash_conv_bb as the bounded NashConv (the D195 Coarse domain
    // identity); both must be populated or the bounded term defaults to zero.
    snap.avg_sigma = cp.avg_sigma;
    snap.nash_conv_bb = cp.nash_conv_bb;
    const ReachBoundResult rb =
        reach_bound_from_snapshot(def, snap, *domain.ref_tree, *domain.bounded_tree, domain.subset,
                                  CapExtensionCompletion::Uniform);
    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    OfflineRungRow row;
    row.iteration = cp.iteration;
    row.bounded_nash_conv_bb = rb.bounded_nash_conv_bb;
    row.exact_exploitability_bb = rb.exact_exploitability_bb;
    row.pure_menu_term_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
    row.e_embed_bb = rb.exact_exploitability_bb;
    row.wall_ms = wall_ms;
    if (!std::isfinite(row.e_embed_bb) || !std::isfinite(row.bounded_nash_conv_bb)) {
      std::fprintf(stderr, "non-finite reach-bound result at rung %llu\n",
                   static_cast<unsigned long long>(cp.iteration));
      std::exit(3);
    }
    rows.push_back(row);
    std::array<std::string, kColumns> r{};
    r[0] = "rung";
    r[1] = cand.label;
    r[2] = cand.family;
    r[3] = fint(cand.raise_cap);
    r[4] = domain.reference_kind;
    r[5] = domain.cap0_direct_map;
    r[6] = "";  // verdict only on the summary row
    r[7] = fint(static_cast<long long>(cp.iteration));
    r[8] = "";  // requested iterations / rung rows only on summary
    r[9] = "";
    r[10] = fnum(row.bounded_nash_conv_bb, "%.8f");
    r[11] = fnum(row.exact_exploitability_bb, "%.8f");
    r[12] = fnum(row.pure_menu_term_bb, "%.8f");
    r[13] = fnum(row.e_embed_bb, "%.8f");
    r[14] = fnum(wall_ms, "%.3f");
    r[15] = fint(current_rss_kb());
    std::printf("%s", join_row(r).c_str());
    std::fflush(stdout);
  };

  std::printf(
      "record_type,label,family,raise_cap,reference_kind,cap0_direct_map,verdict,iteration,"
      "requested_iterations,rung_rows,bounded_nc_bb,exact_e_embed_bb,pure_menu_bb,e_embed_bb,"
      "wall_ms,rss_current_kb\n");
  std::fflush(stdout);
  const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
  const double total_wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const OfflineConvergenceOutcome outcome =
      classify_convergence(std::move(rows), solved.stopped_early);
  rusage ru1{};
  getrusage(RUSAGE_SELF, &ru1);
  const long rss_peak_kb = ru1.ru_maxrss;
  std::array<std::string, kColumns> s{};
  s[0] = "summary";
  s[1] = cand.label;
  s[2] = cand.family;
  s[3] = fint(cand.raise_cap);
  s[4] = domain.reference_kind;
  s[5] = domain.cap0_direct_map;
  s[6] = verdict_token(outcome.verdict);
  s[7] = fint(static_cast<long long>(solved.iterations_completed));
  s[8] = fint(static_cast<long long>(solved.iterations));
  s[9] = fint(static_cast<long long>(outcome.rows.size()));
  // Metric columns are per-rung; the summary carries verdict, counts, wall, RSS.
  s[10] = "";
  s[11] = "";
  s[12] = "";
  s[13] = "";
  s[14] = fnum(total_wall_ms, "%.3f");
  s[15] = fint(rss_peak_kb);
  std::printf("%s", join_row(s).c_str());
  std::fflush(stdout);
  return 0;
}
