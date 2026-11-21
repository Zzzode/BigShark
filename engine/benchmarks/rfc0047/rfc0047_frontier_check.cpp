// RFC 0047 S2 manual check: enumerate post-chance frontiers per stratum and
// report counts, distinct (pot,contribution) ledger groups, and per-leaf
// five-card board validity. NOT a CTest gate row.
#include <array>
#include <cstdio>
#include <set>

#include "frontier_enumerator.hpp"
using namespace bs::rfc0047;
int main() {
  std::printf(
      "stratum,frontiers,chance_lines,distinct_pots,distinct_ledgers,"
      "river_cards_per_line_min,max\n");
  for (const auto& s : kStrata) {
    auto leaves = enumerate_frontiers(s, 1);
    std::set<int> pots;
    std::set<std::array<bs::poker::Chips, 3>> ledgers;
    std::map<std::size_t, std::size_t> per_line;
    for (auto& L : leaves) {
      pots.insert(static_cast<int>(L.river_def.pot));
      ledgers.insert({L.river_def.pot, L.river_def.contributions[0], L.river_def.contributions[1]});
      per_line[L.turn_chance_node]++;
    }
    std::size_t mn = 99, mx = 0;
    for (auto& [k, v] : per_line) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    std::printf("%s,%zu,%zu,%zu,%zu,%zu,%zu\n", s.name, leaves.size(), per_line.size(), pots.size(),
                ledgers.size(), mn, mx);
  }
}
