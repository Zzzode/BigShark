// RFC 0004 Stage 3 heads-up blueprint quality benchmark (default CTest gate).
//
// Versioned independently from the legacy multi-street CSV (schema_version 2):
// it exercises the new heads-up game facade with both full and external-
// sampling traversal. Gates are the RFC 0004 release thresholds:
//   fixed small games ........ normalized NashConv <= 0.002 per root pot
//   sampled-chance small games normalized NashConv <= 0.02 per root pot
// measured within the pinned one-million-iteration compute budget. The
// sampled fixed case is spent at the full 1,000,000 checkpoint here; the free-
// river case is spent at a bounded 100,000-iteration checkpoint that already
// passes the 0.02 gate inside that budget (the full 1,000,000 free-river
// checkpoint runs only in the manual benchmark-heads-up-capacity runner, so
// the default CTest suite stays bounded). Gates run for seeds 1, 17, and 43,
// the final value must be no worse than the first checkpoint, and same-build
// repeats must match every published policy row within 1e-12. Elapsed time is
// recorded but never gated.
#include "heads_up_benchmark_support.hpp"

int main() {
  using namespace bs_heads_up_benchmark;
  print_header();
  const Case cases[] = {
      fixed_full_case(),
      fixed_sampled_case({10000, 100000, 1000000}),
      free_river_sampled_case({10000, 50000, 100000}),
  };
  bool passed = true;
  for (const Case& benchmark_case : cases)
    passed = run_case(benchmark_case) && passed;
  return passed ? 0 : 1;
}
