// RFC 0047 S1 manual binary: G1 per-river infoset budget for the six frozen
// strata (no solve). NOT a CTest, not a gate row by itself; emits CSV consumed
// by tools/rfc/rfc0047-gates.ts.
#include <cstdio>

#include "strata.hpp"
#include "tree_budget.hpp"

using namespace bs::rfc0047;

int main() {
  std::printf("%s\n", infoset_budget_csv_header().c_str());
  for (const StratumSpec& s : kStrata)
    std::printf("%s\n", to_csv(measure_tree_budget(s, 1)).c_str());
  return 0;
}
