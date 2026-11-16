// RFC 0048 D253 manual single-instance convergence-law certification probe
// (NOT a CTest). One process performs ONE 300-iteration RM+ linear Coarse
// cap-1 w1 river solve — the best in-domain policy — and records the kernel
// NashConv curve at the fixed seven-point checkpoint ladder
// {30,60,100,150,200,250,300}. Only the final T=300 snapshot exists, so only
// that endpoint receives an independent reach-bound walk; interior points
// inherit the bounded-NC identity via the D195 Coarse domain theorem and the
// T13 restricted-walk reconciliation. The probe then fits log10(NC) =
// a + b*log10(T) by ordinary least squares, reports R^2 and the
// pre-registered decision/falsifier rule, and extrapolates the iterations
// T* and wall time t* needed to reach the 0.002 BB gate against the G1
// envelope (p95 10 s, max 12 s). All seven checkpoints are printed
// unconditionally; a non-finite value exits non-zero; local NC increases are
// shown as observed. A steady-clock 120 s solve tripwire exits 4; the OS
// timeout launcher is the outer hard stop. TreeLimits (1 GiB byte cap) apply
// to every tree; RSS is sampled only. No solver, menu, threshold, sweep,
// retry, or budget change exists in this binary.
//
// Usage: bigshark-rfc0047-convergence-cert-probe ITERS [THREADS]
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "frontier_enumerator.hpp"
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

constexpr double kWallBudgetMs = 120'000.0;
constexpr double kGateBb = 0.002;
constexpr double kG1P95Ms = 10'000.0;
constexpr double kG1MaxMs = 12'000.0;
const std::uint64_t kLadder[] = {30, 60, 100, 150, 200, 250, 300};

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

struct OlsFit {
  double intercept = 0.0;
  double slope = 0.0;
  double r2 = 0.0;
};

// Ordinary least squares on (x, y) with the coefficient of determination.
OlsFit ols_log_fit(const std::vector<double>& x, const std::vector<double>& y) {
  const std::size_t n = x.size();
  double mx = 0.0, my = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    mx += x[i];
    my += y[i];
  }
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);
  double sxx = 0.0, sxy = 0.0, syy = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double dx = x[i] - mx;
    const double dy = y[i] - my;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  OlsFit f;
  f.slope = sxy / sxx;
  f.intercept = my - f.slope * mx;
  const double ssr = syy - sxy * sxy / sxx;  // residual sum of squares
  f.r2 = syy > 0.0 ? 1.0 - ssr / syy : 1.0;
  return f;
}

struct Domain {
  std::unique_ptr<ActionAbstraction> reference;
  std::string reference_kind;
  std::string cap0_direct_map;
  std::unique_ptr<AbstractTree> ref_tree;
  std::unique_ptr<AbstractTree> bounded_tree;
  SubsetMap subset;
};

Domain build_domain(const bs::poker::GameDef& def, const ActionAbstraction& coarse,
                    const TreeLimits& limits) {
  Domain d;
  d.bounded_tree = std::make_unique<AbstractTree>(def, coarse, limits);
  const AbstractTree cap0(def, ActionAbstraction::identity(0), limits);
  try {
    d.subset = map_bounded_subset(cap0, *d.bounded_tree);
    d.reference = std::make_unique<ActionAbstraction>(ActionAbstraction::identity(0));
    d.reference_kind = "cap0";
    d.cap0_direct_map = "mapped";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
  } catch (const std::invalid_argument&) {
    d.reference = std::make_unique<ActionAbstraction>(
        make_union_abstraction(ActionAbstraction::identity(0), coarse));
    d.reference_kind = "union";
    d.cap0_direct_map = "rejected";
    d.ref_tree = std::make_unique<AbstractTree>(def, *d.reference, limits);
    d.subset = map_bounded_subset(*d.ref_tree, *d.bounded_tree);
  }
  return d;
}

const char* kHeader =
    "record_type,iterations,nash_conv_bb,log10_t,log10_nc,fit_intercept,fit_slope,fit_r2,"
    "t_star_iters,t_star_wall_s,g1_p95_feasible_iters,g1_max_feasible_iters,overshoot_vs_g1_max,"
    "bounded_nc_reconciled_bb,pure_menu_bb,reference_kind,cap0_direct_map,verdict,"
    "solve_wall_ms,total_wall_ms,cpu_ms,rss_peak_kb,rss_current_kb,wall_budget_exceeded\n";

constexpr std::size_t kColumns = 24;

std::string join_row(const std::array<std::string, kColumns>& fields) {
  std::string out;
  for (std::size_t i = 0; i < kColumns; ++i) {
    if (i)
      out.push_back(',');
    out += fields[i];
  }
  out.push_back('\n');
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
  if (iters != kLadder[6]) {
    std::fprintf(stderr, "D253 pre-registration fixes ITERS to %llu\n",
                 static_cast<unsigned long long>(kLadder[6]));
    return 2;
  }

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;

  const ActionAbstraction coarse = make_grid_abstraction(SizingGrid::Coarse, 1);
  const TreeLimits enforced{2'000'000, 256, std::size_t{1} << 30};
  const Domain domain = build_domain(def, coarse, enforced);

  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = iters;
  cfg.threads = threads;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = coarse;
  const std::vector<std::uint64_t> checkpoints(std::begin(kLadder), std::end(kLadder));

  rusage ru_start{};
  getrusage(RUSAGE_SELF, &ru_start);
  const auto t_solve0 = std::chrono::steady_clock::now();
  const RiverSolveResult solved = solve_river_subgame(cfg, checkpoints);
  const auto t_solve1 = std::chrono::steady_clock::now();
  const double solve_wall_ms =
      std::chrono::duration<double, std::milli>(t_solve1 - t_solve0).count();
  const bool wall_exceeded = solve_wall_ms > kWallBudgetMs;
  if (wall_exceeded)
    std::fprintf(stderr, "wall_budget_exceeded: %.3f ms > %.0f ms\n", solve_wall_ms, kWallBudgetMs);

  if (solved.history_nash_conv_bb.size() != checkpoints.size()) {
    std::fprintf(stderr, "checkpoint count mismatch: %zu != %zu\n",
                 solved.history_nash_conv_bb.size(), checkpoints.size());
    return 3;
  }
  std::vector<double> xs;
  std::vector<double> ys;
  for (std::size_t i = 0; i < checkpoints.size(); ++i) {
    const double nc = solved.history_nash_conv_bb[i];
    if (!std::isfinite(nc) || nc <= 0.0) {
      std::fprintf(stderr, "non-finite or non-positive checkpoint NC at t=%llu: %g\n",
                   static_cast<unsigned long long>(checkpoints[i]), nc);
      return 3;
    }
    xs.push_back(std::log10(static_cast<double>(checkpoints[i])));
    ys.push_back(std::log10(nc));
  }
  const OlsFit fit = ols_log_fit(xs, ys);
  // T*: log10(M) = a + b*log10(T*)  =>  log10(T*) = (log10(M) - a)/b.
  const double log_tstar = (std::log10(kGateBb) - fit.intercept) / fit.slope;
  const double t_star_iters = std::pow(10.0, log_tstar);
  const double per_iter_ms = solve_wall_ms / static_cast<double>(iters);
  const double t_star_wall_s = t_star_iters * per_iter_ms / 1000.0;
  const double g1_p95_iters = std::floor(kG1P95Ms / per_iter_ms);
  const double g1_max_iters = std::floor(kG1MaxMs / per_iter_ms);
  const double overshoot = t_star_wall_s / (kG1MaxMs / 1000.0);

  // Endpoint-only independent reach-bound reconciliation.
  const ReachBoundResult rb =
      reach_bound_from_snapshot(def, solved, *domain.ref_tree, *domain.bounded_tree, domain.subset,
                                CapExtensionCompletion::Uniform);
  const double pure_menu = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
  if (!std::isfinite(rb.bounded_nash_conv_bb) || !std::isfinite(rb.exact_exploitability_bb)) {
    std::fprintf(stderr, "non-finite reach-bound result\n");
    return 3;
  }

  // Pre-registered decision rule (slope band and R^2 are conventional,
  // not calibrated; inconclusive never upgrades the closure).
  const bool r2_pass = fit.r2 >= 0.90;
  const bool band_pass = fit.slope >= -0.60 && fit.slope <= -0.45;
  const bool decreasing = fit.slope < 0.0;
  const bool falsifier = fit.slope < -0.60 && t_star_wall_s <= (kG1MaxMs / 1000.0);
  const char* verdict;
  if (falsifier)
    verdict = "falsifier_reopen";
  else if (decreasing && r2_pass && band_pass && t_star_wall_s > (kG1MaxMs / 1000.0))
    verdict = "certified_outside_g1";
  else
    verdict = "inconclusive";

  const ResourceSample total = sample_resource(t_solve0, ru_start);
  auto fnum = [](double v, const char* fmt) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return std::string(buf);
  };
  auto fint = [](long long v) { return std::to_string(v); };

  std::printf("%s", kHeader);
  for (std::size_t i = 0; i < checkpoints.size(); ++i) {
    std::array<std::string, kColumns> row{};
    row[0] = "checkpoint";
    row[1] = fint(static_cast<long long>(checkpoints[i]));
    row[2] = fnum(solved.history_nash_conv_bb[i], "%.8f");
    row[3] = fnum(xs[i], "%.6f");
    row[4] = fnum(ys[i], "%.6f");
    // Columns 5-17 (fit through verdict) stay empty on checkpoint rows.
    row[18] = fnum(solve_wall_ms, "%.3f");
    row[19] = fnum(total.wall_ms, "%.3f");
    row[20] = fnum(total.cpu_ms, "%.3f");
    row[21] = fint(total.rss_peak_kb);
    row[22] = fint(total.rss_current_kb);
    row[23] = fint(wall_exceeded ? 1 : 0);
    std::printf("%s", join_row(row).c_str());
  }
  {
    std::array<std::string, kColumns> row{};
    row[0] = "summary";
    row[1] = fint(static_cast<long long>(iters));
    row[2] = fnum(solved.nash_conv_bb, "%.8f");
    row[3] = "";
    row[4] = "";
    row[5] = fnum(fit.intercept, "%.8f");
    row[6] = fnum(fit.slope, "%.8f");
    row[7] = fnum(fit.r2, "%.8f");
    row[8] = fnum(t_star_iters, "%.3f");
    row[9] = fnum(t_star_wall_s, "%.3f");
    row[10] = fint(static_cast<long long>(g1_p95_iters));
    row[11] = fint(static_cast<long long>(g1_max_iters));
    row[12] = fnum(overshoot, "%.3f");
    row[13] = fnum(rb.bounded_nash_conv_bb, "%.8f");
    row[14] = fnum(pure_menu, "%.8f");
    row[15] = domain.reference_kind;
    row[16] = domain.cap0_direct_map;
    row[17] = verdict;
    row[18] = fnum(solve_wall_ms, "%.3f");
    row[19] = fnum(total.wall_ms, "%.3f");
    row[20] = fnum(total.cpu_ms, "%.3f");
    row[21] = fint(total.rss_peak_kb);
    row[22] = fint(total.rss_current_kb);
    row[23] = fint(wall_exceeded ? 1 : 0);
    std::printf("%s", join_row(row).c_str());
  }
  return wall_exceeded ? 4 : 0;
}
