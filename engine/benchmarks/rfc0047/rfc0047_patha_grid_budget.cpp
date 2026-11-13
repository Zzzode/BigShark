// RFC 0048 Path A stage-1 (D176) manual feasibility probe, NOT a CTest.
// Solve-free public-tree census over the two widening levers on the w1 turn
// root and one reached-river root:
//   Lever 1 raise cap  identity(k), k = 1..6 then 0 (uncapped)
//   Lever 2 sizing     declared(coarse|identity|wide, MinAndCap, cap=1)
// Each row carries the deterministic GridBudget structural verdict (node/byte/
// infoset envelope) plus this process's build wall/cpu and RSS. A
// tree_resource_exhausted build is emitted as a resource_boundary row and the
// sweep continues; any other exception is a probe fault and aborts. Bounded and
// fail-fast: 20 tree builds, no equilibrium, no full-tree training budget.
//
// Usage: bigshark-rfc0047-patha-grid-budget [STRATUM] [THREADS]
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

#include "frontier_enumerator.hpp"
#include "patha_grid_budget.hpp"
#include "strata.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;

namespace {

double tv_ms(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
}

long current_rss_kb() {
  std::ifstream status("/proc/self/statm");
  long resident = 0;
  long total = 0;
  status >> total >> resident;
  if (resident <= 0)
    return -1;
  const long page_kb = sysconf(_SC_PAGESIZE) / 1024L;
  return resident * (page_kb > 0 ? page_kb : 4L);
}

struct Timed {
  GridBudget budget;
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
  long rss_peak_kb = 0;
  long rss_current_kb = 0;
};

Timed run(const bs::poker::GameDef& def, const std::string& root_kind,
          const ActionAbstraction& action, const std::string& label, std::uint8_t cap,
          const std::string& grid_name) {
  rusage ru0{};
  getrusage(RUSAGE_SELF, &ru0);
  const auto t0 = std::chrono::steady_clock::now();
  Timed r;
  r.budget = measure_grid_budget(def, root_kind, action, label, cap, grid_name);
  const auto t1 = std::chrono::steady_clock::now();
  rusage ru1{};
  getrusage(RUSAGE_SELF, &ru1);
  r.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  r.cpu_ms =
      (tv_ms(ru1.ru_utime) - tv_ms(ru0.ru_utime)) + (tv_ms(ru1.ru_stime) - tv_ms(ru0.ru_stime));
  r.rss_peak_kb = ru1.ru_maxrss;  // process high-water across the whole sweep
  r.rss_current_kb = current_rss_kb();
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const char* stratum_name = argc >= 2 ? argv[1] : "w1";
  const StratumSpec* spec = find_stratum(stratum_name);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", stratum_name);
    return 2;
  }

  // F1: GameDefs are built with the fixed literal cap 1; the real cap travels
  // only on each ActionAbstraction. The turn-rooted def covers the offline
  // trunk; the reached-river def is one real frontier subgame.
  const bs::poker::GameDef turn_def = make_stratum_def(*spec, 1);
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);
  if (leaves.empty()) {
    std::fprintf(stderr, "no frontier leaves for %s\n", spec->name);
    return 2;
  }
  const bs::poker::GameDef river_def = leaves.front().river_def;

  struct Instance {
    const bs::poker::GameDef& def;
    std::string root_kind;
    ActionAbstraction action;
    std::string label;
    std::uint8_t cap;
    std::string grid_name;
  };
  std::vector<Instance> instances;
  // Lever 1: raise cap. identity(0) is the uncapped reference and is expected
  // to hit the 1 GiB boundary on the turn root.
  for (std::uint8_t k = 1; k <= 6; ++k) {
    instances.push_back({turn_def, "turn", ActionAbstraction::identity(k),
                         "turn_identity_cap" + std::to_string(k), k, ""});
    instances.push_back({river_def, "reached_river", ActionAbstraction::identity(k),
                         "river_identity_cap" + std::to_string(k), k, ""});
  }
  instances.push_back(
      {turn_def, "turn", ActionAbstraction::identity(0), "turn_identity_cap0", 0, ""});
  instances.push_back(
      {river_def, "reached_river", ActionAbstraction::identity(0), "river_identity_cap0", 0, ""});
  // Lever 2: sizing grids at the fixed online raise cap 1.
  for (SizingGrid grid : {SizingGrid::Coarse, SizingGrid::Identity, SizingGrid::Wide}) {
    const std::string gname = sizing_grid_name(grid);
    instances.push_back(
        {turn_def, "turn", make_grid_abstraction(grid, 1), "turn_grid_" + gname, 1, gname});
    instances.push_back({river_def, "reached_river", make_grid_abstraction(grid, 1),
                         "river_grid_" + gname, 1, gname});
  }

  std::printf("%s,wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n", grid_budget_csv_header().c_str());
  for (const Instance& in : instances) {
    const Timed r = run(in.def, in.root_kind, in.action, in.label, in.cap, in.grid_name);
    std::printf("%s,%.3f,%.3f,%ld,%ld\n", to_csv(r.budget).c_str(), r.wall_ms, r.cpu_ms,
                r.rss_peak_kb, r.rss_current_kb);
    std::fflush(stdout);  // preserve partial evidence if a later build faults
  }
  return 0;
}
