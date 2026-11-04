#include "tree_budget.hpp"

#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <map>
#include <numeric>
#include <span>

namespace bs::rfc0047 {

using bs::tree::AbstractTree;
using bs::tree::TreeNode;

namespace {

// Count action nodes of a given street in the subtree rooted at `root`.
std::size_t count_action_in_subtree(const AbstractTree& tree, std::size_t root,
                                    bs::poker::Street street) {
  std::size_t count = 0;
  std::vector<std::size_t> stack{root};
  while (!stack.empty()) {
    const std::size_t idx = stack.back();
    stack.pop_back();
    const TreeNode& node = tree.node(idx);
    if (node.is_action() && node.street == street)
      ++count;
    for (std::size_t child : node.children)
      stack.push_back(child);
  }
  return count;
}

}  // namespace

TreeBudget measure_tree_budget(const StratumSpec& spec, std::uint8_t cap) {
  const auto def = make_stratum_def(spec, cap);
  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(cap));

  TreeBudget b;
  b.stratum = spec.name;
  b.pot = spec.pot;
  b.total_nodes = tree.size();
  const std::array<int, 4> board4 = turn_board_ids(spec);

  for (const TreeNode& node : tree.nodes()) {
    if (!node.is_action())
      continue;
    if (node.street == bs::poker::Street::Turn)
      ++b.turn_action_nodes;
    else if (node.street == bs::poker::Street::River)
      ++b.river_action_nodes;
  }

  // Aggregate per ACTUAL river card (incoming_card) across every turn line's
  // chance node, and record per single-child (reached) counts.
  std::map<int, std::uint32_t> per_card;
  std::vector<std::uint32_t> reached;
  for (const TreeNode& node : tree.nodes()) {
    if (!node.is_chance())
      continue;
    ++b.river.chance_nodes;
    for (std::size_t child : node.children) {
      const TreeNode& cn = tree.node(child);
      const auto n = static_cast<std::uint32_t>(
          count_action_in_subtree(tree, child, bs::poker::Street::River));
      reached.push_back(n);
      if (cn.incoming_card != bs::tree::kNoCard)
        per_card[cn.incoming_card] += n;
    }
  }
  b.river.frontier_leaves = b.river.chance_nodes * 48;

  b.river.reached_action_min = *std::min_element(reached.begin(), reached.end());
  b.river.reached_action_max = *std::max_element(reached.begin(), reached.end());
  const std::uint64_t reached_sum =
      std::accumulate(reached.begin(), reached.end(), std::uint64_t{0});
  b.river.reached_action_mean_x1000 =
      static_cast<std::uint32_t>(reached_sum * 1000ull / reached.size());
  for (const auto& [card, n] : per_card)
    b.river.per_card_action_nodes.push_back(n);
  b.river.per_card_min =
      *std::min_element(b.river.per_card_action_nodes.begin(), b.river.per_card_action_nodes.end());
  b.river.per_card_max =
      *std::max_element(b.river.per_card_action_nodes.begin(), b.river.per_card_action_nodes.end());
  b.river.per_card_uniform = b.river.per_card_min == b.river.per_card_max;

  b.turn_combos = live_combo_count(board4);  // 1,128
  std::array<int, 5> board5{board4[0], board4[1], board4[2], board4[3],
                            bs::cardId(std::string(spec.river))};
  std::span<const int> b5(board5.data(), 5);
  b.river_combos = live_combo_count(b5);  // 1,081

  b.turn_infosets = b.turn_action_nodes * b.turn_combos + b.turn_combos;
  b.reached_river_infosets_max =
      static_cast<std::uint64_t>(b.river.reached_action_max) * b.river_combos + b.river_combos;
  b.per_card_infosets =
      static_cast<std::uint64_t>(b.river.per_card_max) * b.river_combos + b.river_combos;
  b.conservative_sum = b.turn_infosets + b.reached_river_infosets_max;
  return b;
}

std::string infoset_budget_csv_header() {
  return "stratum,pot,total_nodes,turn_action_nodes,chance_nodes,frontier_leaves,"
         "river_action_nodes_total,reached_min,reached_mean_x1000,reached_max,"
         "per_card_actions_min,per_card_actions_max,per_card_uniform,"
         "turn_combos,river_combos,turn_infosets,reached_river_infosets_max,"
         "per_card_infosets_offline,conservative_online_sum,under_1m";
}

std::string to_csv(const TreeBudget& b) {
  const std::uint64_t envelope = 1'000'000;
  const bool under = b.turn_infosets < envelope && b.reached_river_infosets_max < envelope &&
                     b.conservative_sum < envelope;
  const double reached_mean = b.river.reached_action_mean_x1000 / 1000.0;
  std::string s = b.stratum;
  auto append = [&](const std::string& v) { s += "," + v; };
  append(std::to_string(b.pot));
  append(std::to_string(b.total_nodes));
  append(std::to_string(b.turn_action_nodes));
  append(std::to_string(b.river.chance_nodes));
  append(std::to_string(b.river.frontier_leaves));
  append(std::to_string(b.river_action_nodes));
  append(std::to_string(b.river.reached_action_min));
  append(std::to_string(reached_mean));
  append(std::to_string(b.river.reached_action_max));
  append(std::to_string(b.river.per_card_min));
  append(std::to_string(b.river.per_card_max));
  append(b.river.per_card_uniform ? "true" : "false");
  append(std::to_string(b.turn_combos));
  append(std::to_string(b.river_combos));
  append(std::to_string(b.turn_infosets));
  append(std::to_string(b.reached_river_infosets_max));
  append(std::to_string(b.per_card_infosets));
  append(std::to_string(b.conservative_sum));
  append(under ? "true" : "false");
  return s;
}

}  // namespace bs::rfc0047
