// RFC 0047 Stage-1 per-river structural budget.
//
// Builds the frozen turn-rooted cap-1 public tree and counts, WITHOUT solving.
//
// Two river subtree GRANULARITIES are distinguished (the RFC 0047
// post-chance frontier):
//
//  * ONLINE reached subtree. Play follows ONE turn betting line and ONE river
//    card, so continual re-solving roots at a single chance child. Such a
//    subtree has between 0 and `reached_action_max` river action nodes (0 means
//    the line ends in a direct showdown). This is the G1 online footprint.
//
//  * OFFLINE per-card grouping. Aggregating every turn line for a FIXED river
//    card (sum over all chance nodes' child for that card) gives
//    `per_card_action_nodes` river action nodes, uniform across the 48 cards
//    because menus are card-independent. The offline builder groups work this
//    way; the resulting number (82 limped / 108 raised) is never resident
//    online.
//
// Concrete infosets use dense pairs: turn C(48,2)=1,128 live combos; a
// five-card river board leaves C(47,2)=1,081.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "strata.hpp"

namespace bs::rfc0047 {

struct RiverCounts {
  std::size_t chance_nodes = 0;     // turn lines that deal a river
  std::size_t frontier_leaves = 0;  // chance_nodes x 48
  // Per single chance child (one turn line, one card): river action nodes.
  std::uint32_t reached_action_min = 0;
  std::uint32_t reached_action_mean_x1000 = 0;
  std::uint32_t reached_action_max = 0;
  // Aggregated across turn lines for each actual river card (48 entries).
  std::vector<std::uint32_t> per_card_action_nodes;
  std::uint32_t per_card_min = 0;
  std::uint32_t per_card_max = 0;
  bool per_card_uniform = false;
};

struct TreeBudget {
  std::string stratum;
  int pot = 0;
  std::size_t total_nodes = 0;
  std::size_t turn_action_nodes = 0;
  std::size_t river_action_nodes = 0;  // summed over all children
  RiverCounts river;
  std::uint32_t turn_combos = 0;   // 1,128
  std::uint32_t river_combos = 0;  // 1,081
  // Depth-limited turn solve infosets incl. one -x gadget row per combo.
  std::uint64_t turn_infosets = 0;
  // Online worst-case reached river re-solve (single line+card), incl. gadget.
  std::uint64_t reached_river_infosets_max = 0;
  // Offline grouping for one card across all turn lines, incl. gadget.
  std::uint64_t per_card_infosets = 0;
  // Conservative online sum if both tables were cached resident together.
  std::uint64_t conservative_sum = 0;
};

TreeBudget measure_tree_budget(const StratumSpec& spec, std::uint8_t cap);

std::string infoset_budget_csv_header();
std::string to_csv(const TreeBudget& b);

}  // namespace bs::rfc0047
