// RFC 0047 S2 manual probe: deterministic full-deal river NashConv convergence
// curve for ONE stratum (sparse logarithmic checkpoints), with iteration
// throughput. Manual binary, NOT a CTest gate row.
//
//   bigshark-rfc0047-river-curve <stratum> <iters>
#include <chrono>
#include <cstdio>
#include <vector>

#include "river_solver.hpp"
#include "strata.hpp"
using namespace bs::rfc0047;
int main(int argc, char** argv) {
  const char* nm = argc > 1 ? argv[1] : "w1";
  uint64_t it = argc > 2 ? std::stoull(argv[2]) : 800;
  const StratumSpec* s = find_stratum(nm);
  RiverSolveConfig c;
  c.def = make_river_root_def(*s, 1);
  c.iterations = it;
  c.linear_averaging = true;
  // sparse logarithmic checkpoints
  std::vector<uint64_t> cp;
  for (uint64_t x = 10; x <= it; x = (x * 3 + 1) / 2) {
    if (x > it)
      break;
    cp.push_back(x);
  }
  auto t0 = std::chrono::steady_clock::now();
  auto r = solve_river_subgame(c, cp);
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "%s it=%llu nodes? action=%zu wall=%.2fs iter/s=%.0f\n", nm,
               (unsigned long long)it, r.action_nodes, sec, it / sec);
  for (size_t k = 0; k < r.history_nash_conv_bb.size(); ++k)
    std::fprintf(stderr, "  cp=%llu nc_bb=%.6f\n", (unsigned long long)cp[k],
                 r.history_nash_conv_bb[k]);
  std::fflush(stderr);
  printf("done %s\n", nm);
  return 0;
}
