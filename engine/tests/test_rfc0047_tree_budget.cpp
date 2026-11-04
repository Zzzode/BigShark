// RFC 0047 Stage-1 native test: per-river structural infoset budget and dense
// combo indexing. No solve.
#include <array>
#include <bs/eval.hpp>
#include <cstdio>
#include <exception>
#include <span>
#include <string>

#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/tree_budget.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

bool test_dense_pairs() {
  const std::array<int, 4> b4{bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                              bs::cardId("Js")};
  std::span<const int> board4(b4.data(), 4);
  CHECK(live_combo_count(board4) == 1128);
  for (std::uint32_t d = 0; d < 1128; ++d) {
    const auto h = holding_for_dense(board4, d);
    CHECK(dense_pair_index(board4, h) == d);
  }
  const std::array<int, 5> b5{b4[0], b4[1], b4[2], b4[3], bs::cardId("9c")};
  std::span<const int> board5(b5.data(), 5);
  CHECK(live_combo_count(board5) == 1081);
  for (std::uint32_t d = 0; d < 1081; ++d) {
    const auto h = holding_for_dense(board5, d);
    CHECK(dense_pair_index(board5, h) == d);
  }
  bool threw = false;
  try {
    (void)dense_pair_index(board4, std::array<int, 2>{b4[0], static_cast<int>(b4[0] + 1)});
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

bool expect_shape(const char* name, std::size_t turn_nodes, std::size_t chance_nodes,
                  std::uint32_t reached_max, std::uint32_t per_card) {
  const StratumSpec* s = find_stratum(name);
  CHECK(s != nullptr);
  const TreeBudget b = measure_tree_budget(*s, 1);
  CHECK(b.turn_action_nodes == turn_nodes);
  CHECK(b.river.chance_nodes == chance_nodes);
  CHECK(b.river.frontier_leaves == chance_nodes * 48);
  // The cross-line per-card aggregation is uniform and matches total/48.
  CHECK(b.river.per_card_uniform);
  CHECK(b.river.per_card_min == per_card);
  CHECK(b.river.per_card_max == per_card);
  CHECK(b.river.per_card_action_nodes.size() == 48);
  CHECK(b.river_action_nodes == chance_nodes * 48 * per_card / chance_nodes);
  // The online reached subtree is at most reached_max river action nodes.
  CHECK(b.river.reached_action_max == reached_max);
  CHECK(b.river.reached_action_max <= per_card);
  CHECK(b.turn_combos == 1128);
  CHECK(b.river_combos == 1081);
  const std::uint64_t expect_turn = turn_nodes * 1128ull + 1128ull;
  const std::uint64_t expect_reached = std::uint64_t{reached_max} * 1081ull + 1081ull;
  const std::uint64_t expect_per_card = std::uint64_t{per_card} * 1081ull + 1081ull;
  CHECK(b.turn_infosets == expect_turn);
  CHECK(b.reached_river_infosets_max == expect_reached);
  CHECK(b.per_card_infosets == expect_per_card);
  CHECK(b.conservative_sum == expect_turn + expect_reached);
  CHECK(b.turn_infosets < 1'000'000);
  CHECK(b.reached_river_infosets_max < 1'000'000);
  CHECK(b.conservative_sum < 1'000'000);
  return true;
}

bool test_shapes() {
  // Node counts are range-independent, so w1 shares the limped tree shape.
  // Limped: 10 turn action nodes, 9 river-chance lines; each single reached
  // (line,card) subtree has <=12 river action nodes, while a fixed card
  // aggregated over all 9 lines has 82 (3936/48) river action nodes.
  static const char* kLimped[] = {"w1", "unpaired-rainbow-ahigh", "paired", "monotone",
                                  "unpaired-twotone"};
  for (const char* n : kLimped)
    if (!expect_shape(n, 10, 9, 12, 82))
      return false;
  // Raised SRP: 12 turn action nodes, 11 chance lines; reached <=12; per card
  // aggregated = 108 (5184/48).
  if (!expect_shape("raised-srp", 12, 11, 12, 108))
    return false;
  return true;
}

int main() {
  if (!test_dense_pairs() || !test_shapes())
    return 1;
  std::printf("rfc0047_tree_budget: all checks passed\n");
  return 0;
}
