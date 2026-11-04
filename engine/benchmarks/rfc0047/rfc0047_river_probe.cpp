// RFC 0047 S2 probe (manual): deterministic full-deal river solver NashConv
// convergence law. Reports exploitability (summed BB) vs iteration checkpoints
// for each frozen stratum's standalone river subgame. NOT a CTest gate row.
//
//   bigshark-rfc0047-river-probe [iters]   (default 2000)
#include <cstdio>
#include <string>
#include <vector>

#include "river_solver.hpp"
#include "strata.hpp"

using namespace bs::rfc0047;

int main(int argc, char** argv) {
  std::uint64_t iters = 2000;
  if (argc > 1)
    iters = std::stoull(argv[1]);
  const std::vector<std::uint64_t> cp{1, 2, 5, 10, 25, 50, 100, 200, 500, 1000, 2000,
                                      5000, 10000, 20000, 50000, 100000};
  std::printf("stratum,pot,action_nodes,iterations,nash_conv_bb,nash_conv_pot\n");
  for (const StratumSpec& s : kStrata) {
    RiverSolveConfig cfg;
    cfg.def = make_river_root_def(s, 1);
    cfg.iterations = iters;
    cfg.linear_averaging = true;
    std::vector<std::uint64_t> cps;
    for (auto c : cp)
      if (c <= iters)
        cps.push_back(c);
    const RiverSolveResult r = solve_river_subgame(cfg, cps);
    std::printf("%s,%d,%zu,%llu,%.6f,%.6f\n", s.name, s.pot, r.action_nodes,
                static_cast<unsigned long long>(r.iterations), r.nash_conv_bb,
                r.nash_conv_pot);
    if (!r.history_nash_conv_bb.empty()) {
      std::fprintf(stderr, "[%s] checkpoints(bb):", s.name);
      std::size_t k = 0;
      for (auto c : cp) {
        if (c > iters)
          break;
        std::fprintf(stderr, " %llu=%.5f", static_cast<unsigned long long>(c),
                     r.history_nash_conv_bb[k++]);
      }
      std::fprintf(stderr, "\n");
    }
  }
  return 0;
}
