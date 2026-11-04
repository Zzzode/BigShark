// RFC 0047 S2 post-chance frontier enumeration.
//
// Walks the frozen turn-rooted cap-1 tree and, for EVERY river chance child
// (one per turn betting line x river card), yields the concrete FIVE-card
// board and the river-rooted GameDef at that child (its pot/contributions/
// stacks are the ones carried by the replayed GameState). These are the
// distinct frontier subgames the offline oracle builder must solve; menus are
// card-independent, so the (line, card) action subtree shape repeats across
// the 48 cards but its pot/contributions differ across turn betting lines.
#pragma once

#include <bs/game_definition.hpp>
#include <cstddef>
#include <vector>

#include "strata.hpp"

namespace bs::rfc0047 {

struct FrontierLeaf {
  std::size_t turn_chance_node = 0;  // turn-rooted chance node index
  std::size_t child_node = 0;        // post-chance river subtree root
  int river_card = -1;
  std::array<int, 5> board5{};
  bs::poker::GameDef river_def;    // five-card, line-specific root def
  std::uint32_t frontier_key = 0;  // dense index across all enumerated leaves
};

// Enumerate every post-chance river frontier of the turn-rooted stratum tree
// (cap from the stratum). Deterministic order (tree DFS, menu/card order).
std::vector<FrontierLeaf> enumerate_frontiers(const StratumSpec& spec, std::uint8_t cap);

}  // namespace bs::rfc0047
