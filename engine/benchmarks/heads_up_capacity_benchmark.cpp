// RFC 0004 Stage 3 manual heads-up blueprint CAPACITY benchmark.
//
// Same schema_version 2 CSV and gates as the default CTest blueprint runner,
// but both sampled cases are spent at the pinned 1,000,000-iteration final
// checkpoint (RFC 0004 lines 311-315 and 324-326). This runner is deliberately
// NOT registered as a CTest: it takes several minutes and is launched manually
// with `cmake --build --preset release --target benchmark-heads-up-capacity`
// when release-scale capacity evidence is being recorded. The default CTest
// runner keeps the free-river case at a bounded 100,000-iteration checkpoint.
//
// Every case runs seeds 1, 17, and 43; the final exploitability must be no
// worse than the first checkpoint, at most the pinned gate (0.002 fixed,
// 0.02 sampled), and an independent repeat must match every published policy
// row within 1e-12. Elapsed time is recorded but never gated.
#include "heads_up_benchmark_support.hpp"

int main() {
  using namespace bs_heads_up_benchmark;
  print_header();
  const Case cases[] = {
      fixed_sampled_case({10000, 1000000}),
      free_river_sampled_case({10000, 1000000}),
  };
  bool passed = true;
  for (const Case& benchmark_case : cases)
    passed = run_case(benchmark_case) && passed;
  return passed ? 0 : 1;
}
