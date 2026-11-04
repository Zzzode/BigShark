// RFC 0047 S1 manual binary: G4 canonical board-class census and per-shape
// frontier factor. Emits tidy `metric,value` CSV consumed by the evaluator.
// Enumerating the 134,459 river classes takes a few seconds; this is a census
// tool, not a CTest gate path (the native test pins the constants on the
// guarded enumerator).
#include <cstdio>

#include "board_classes.hpp"
#include "strata.hpp"
#include "tree_budget.hpp"

using namespace bs::rfc0047;

namespace {
std::uint64_t choose(std::uint64_t n, std::uint64_t k) {
  std::uint64_t num = 1;
  std::uint64_t den = 1;
  for (std::uint64_t i = 0; i < k; ++i) {
    num *= n - i;
    den *= i + 1;
  }
  return num / den;
}
}  // namespace

int main() {
  std::printf("metric,value\n");
  std::printf("concrete_flop_boards,%llu\n", static_cast<unsigned long long>(choose(52, 3)));
  std::printf("canonical_flop_classes,%u\n", kCanonicalFlopClasses);
  std::printf("concrete_turn_boards,%llu\n", static_cast<unsigned long long>(choose(52, 4)));
  std::printf("canonical_turn_classes,%u\n",
              static_cast<unsigned>(enumerate_canonical_class_keys(4).size()));
  std::printf("concrete_river_boards,%llu\n", static_cast<unsigned long long>(choose(52, 5)));
  std::printf("canonical_river_classes,%u\n",
              static_cast<unsigned>(enumerate_canonical_class_keys(5).size()));
  const std::uint64_t turn_x48 = choose(52, 4) * 48;
  const std::uint64_t five_x_river = 5 * choose(52, 5);
  std::printf("turn_boards_times_48,%llu\n", static_cast<unsigned long long>(turn_x48));
  std::printf("five_times_river_boards,%llu\n", static_cast<unsigned long long>(five_x_river));
  std::printf("turn_river_identity,%d\n", turn_x48 == five_x_river ? 1 : 0);

  // Per-shape post-chance frontier factor from the built trees.
  for (const StratumSpec& s : kStrata) {
    const TreeBudget b = measure_tree_budget(s, 1);
    std::printf("%s_chance_nodes,%zu\n", s.name, b.river.chance_nodes);
    std::printf("%s_frontier_leaves,%zu\n", s.name, b.river.frontier_leaves);
    std::printf("%s_reached_river_action_nodes_max,%u\n", s.name, b.river.reached_action_max);
    std::printf("%s_per_card_river_action_nodes,%u\n", s.name, b.river.per_card_max);
    // Stored rows per concrete five-card frontier subtree (offline grouping
    // for one card across all turn lines): river action nodes x 1,081 combos
    // x 2 seats + 1,081 responder gadget rows (informational).
    const std::uint64_t rows_per_card = std::uint64_t{b.river.per_card_max} * 1081 * 2 + 1081;
    std::printf("%s_rows_per_card_grouping,%llu\n", s.name,
                static_cast<unsigned long long>(rows_per_card));
  }
  return 0;
}
