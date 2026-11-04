// RFC 0047 S2 native test: post-chance frontier enumeration.
#include <array>
#include <cstdio>
#include <set>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

bool check_stratum(const char* name, std::size_t expect_frontiers, std::size_t expect_chance,
                   std::size_t expect_ledgers) {
  const StratumSpec* s = find_stratum(name);
  CHECK(s != nullptr);
  const auto leaves = enumerate_frontiers(*s, 1);
  CHECK(leaves.size() == expect_frontiers);
  std::set<std::size_t> lines;
  std::set<std::array<bs::poker::Chips, 3>> ledgers;
  for (const FrontierLeaf& L : leaves) {
    lines.insert(L.turn_chance_node);
    ledgers.insert({L.river_def.pot, L.river_def.contributions[0], L.river_def.contributions[1]});
    CHECK(L.river_def.board_size == 5);
    CHECK(L.river_def.terminal == bs::poker::TerminalDepth::River);
    CHECK(L.river_card == L.board5[4]);
  }
  CHECK(lines.size() == expect_chance);
  CHECK(leaves.size() == expect_chance * 48);
  CHECK(ledgers.size() == expect_ledgers);
  return true;
}

int main() {
  // Four limped non-w1 shapes share the turn tree; w1 range differs but the
  // public tree is identical.
  static const char* kLimped[] = {"w1", "unpaired-rainbow-ahigh", "paired", "monotone",
                                  "unpaired-twotone"};
  for (const char* n : kLimped)
    if (!check_stratum(n, 432, 9, 5))
      return 1;
  if (!check_stratum("raised-srp", 528, 11, 6))
    return 1;
  std::printf("rfc0047_frontier_enumerator: all checks passed\n");
  return 0;
}
