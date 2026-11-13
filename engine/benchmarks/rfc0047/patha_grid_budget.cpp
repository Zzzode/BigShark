#include "patha_grid_budget.hpp"

#include <array>
#include <bs/heads_up.hpp>
#include <cstdio>
#include <memory>
#include <numeric>
#include <span>

#include "cv_oracle.hpp"

namespace bs::rfc0047 {

using bs::abstraction::ActionAbstraction;
using bs::abstraction::CoverSeeds;
using bs::abstraction::Fraction;
using bs::abstraction::SizeSchedule;
using bs::poker::Street;
using bs::tree::AbstractTree;
using bs::tree::tree_resource_exhausted;
using bs::tree::TreeNode;

const char* sizing_grid_name(SizingGrid grid) {
  switch (grid) {
    case SizingGrid::Coarse:
      return "coarse";
    case SizingGrid::Identity:
      return "identity";
    case SizingGrid::Wide:
      return "wide";
  }
  return "unknown";
}

ActionAbstraction make_grid_abstraction(SizingGrid grid, std::uint8_t raise_cap) {
  // Default-construct so every street carries valid fractions first; reduced()
  // validates all four streets eagerly even when the root never reaches some.
  SizeSchedule schedule = bs::abstraction::default_size_schedule();
  if (grid == SizingGrid::Coarse) {
    for (Street st : {Street::Flop, Street::Turn, Street::River}) {
      schedule[static_cast<std::size_t>(st)].bets = {Fraction{1, 2}};
      schedule[static_cast<std::size_t>(st)].raises = {Fraction{1, 1}};
    }
  } else if (grid == SizingGrid::Wide) {
    for (Street st : {Street::Flop, Street::Turn, Street::River}) {
      schedule[static_cast<std::size_t>(st)].bets = {Fraction{1, 4}, Fraction{1, 2}, Fraction{3, 4},
                                                     Fraction{1, 1}, Fraction{3, 2}};
      schedule[static_cast<std::size_t>(st)].raises = {Fraction{1, 2}, Fraction{1, 1},
                                                       Fraction{2, 1}};
    }
  }
  // Identity leaves the default schedule unchanged. MinAndCap keeps the grid
  // points comparable to identity(); the raise cap isolates the sizing lever.
  return ActionAbstraction::declared(schedule, CoverSeeds::MinAndCap, raise_cap);
}

namespace {
// live_combo_count depends only on the board span LENGTH (C(52-n,2)), so a
// representative n-card span suffices; ids are irrelevant to the count.
std::uint32_t combos_for_board_length(const bs::poker::GameDef& def, std::size_t n_cards) {
  std::array<int, 5> board{};
  for (std::size_t i = 0; i < n_cards; ++i)
    board[i] = i < def.board_size ? def.board[i] : 0;
  return live_combo_count(std::span<const int>(board.data(), n_cards));
}

// Count river action nodes in the subtree rooted at `root` (one chance child =
// one reached turn line + river card). Ported from tree_budget.cpp so U1 uses
// the identical reached-subtree convention.
std::size_t count_river_action_in_subtree(const AbstractTree& tree, std::size_t root) {
  std::size_t count = 0;
  std::vector<std::size_t> stack{root};
  while (!stack.empty()) {
    const std::size_t idx = stack.back();
    stack.pop_back();
    const TreeNode& node = tree.node(idx);
    if (node.is_action() && node.street == Street::River)
      ++count;
    for (std::size_t child : node.children)
      stack.push_back(child);
  }
  return count;
}
}  // namespace

GridBudget measure_grid_budget(const bs::poker::GameDef& def, const std::string& root_kind,
                               const ActionAbstraction& action, const std::string& label,
                               std::uint8_t raise_cap, const std::string& grid_name) {
  return measure_grid_budget(def, root_kind, action, label, raise_cap, grid_name,
                             bs::tree::TreeLimits{});
}

GridBudget measure_grid_budget(const bs::poker::GameDef& def, const std::string& root_kind,
                               const ActionAbstraction& action, const std::string& label,
                               std::uint8_t raise_cap, const std::string& grid_name,
                               const bs::tree::TreeLimits& limits) {
  GridBudget b;
  b.label = label;
  b.root_kind = root_kind;
  b.lever = grid_name.empty() ? "raise_cap" : "sizing_grid";
  b.raise_cap = raise_cap;
  b.grid = grid_name;

  std::unique_ptr<AbstractTree> tree;
  try {
    tree = std::make_unique<AbstractTree>(def, action, limits);
  } catch (const tree_resource_exhausted& e) {
    // Boundary is an expected negative feasibility result. Any other exception
    // type is a probe/environment fault and must propagate (review F3).
    b.resource_boundary = true;
    b.boundary_reason = e.what();
    return b;
  }

  b.total_nodes = tree->size();
  b.accounted_bytes = tree->accounted_bytes();
  std::size_t action_width_sum = 0;
  for (const TreeNode& node : tree->nodes()) {
    if (node.is_action()) {
      ++b.action_nodes;
      if (node.street == Street::Turn)
        ++b.turn_action_nodes;
      else if (node.street == Street::River)
        ++b.river_action_nodes;
      b.max_actions_per_node = std::max(b.max_actions_per_node, node.actions.size());
      action_width_sum += node.actions.size();
    } else if (node.is_chance()) {
      ++b.chance_nodes;
    } else if (node.is_terminal()) {
      ++b.terminal_nodes;
    }
  }
  b.mean_actions_per_action_node = b.action_nodes == 0 ? 0.0
                                                       : static_cast<double>(action_width_sum) /
                                                             static_cast<double>(b.action_nodes);

  b.turn_combos = b.turn_action_nodes == 0 ? 0 : combos_for_board_length(def, 4);
  b.river_combos = b.river_action_nodes == 0 ? 0 : combos_for_board_length(def, 5);

  // --- U1: online-resident infosets (turn trunk + ONE reached river subtree).
  if (b.turn_action_nodes != 0)
    b.u1_turn_infosets =
        static_cast<std::uint64_t>(b.turn_action_nodes) * b.turn_combos + b.turn_combos;
  std::size_t reached_river_action_max = 0;
  if (b.chance_nodes == 0) {
    // A river-rooted tree IS the single reached subtree (no chance fanout).
    reached_river_action_max = b.river_action_nodes;
  } else {
    for (const TreeNode& node : tree->nodes()) {
      if (!node.is_chance())
        continue;
      for (std::size_t child : node.children)
        reached_river_action_max =
            std::max(reached_river_action_max, count_river_action_in_subtree(*tree, child));
    }
  }
  if (reached_river_action_max != 0 && b.river_combos != 0)
    b.u1_reached_river_infosets_max =
        static_cast<std::uint64_t>(reached_river_action_max) * b.river_combos + b.river_combos;
  b.u1_online_resident_infosets = b.u1_turn_infosets + b.u1_reached_river_infosets_max;
  b.u1_under_1m_infoset = b.u1_online_resident_infosets < kPathAInfosetLimit;

  // --- U2: offline BSNCV artifact rows (one OracleRow per leaf/seat/combo).
  // The oracle emits a per-combo row for EVERY frontier leaf even when the
  // subtree is terminal-only (all-in), so this uses the true five-card live
  // combo count (1081), independent of whether action nodes exist - do NOT gate
  // it on river_action_nodes like the infoset math above.
  const std::uint32_t u2_river_combos = combos_for_board_length(def, 5);
  if (b.chance_nodes == 0) {
    b.u2_artifact_rows = 2ull * u2_river_combos;  // one reached leaf, two seats
  } else {
    const std::uint64_t frontier_leaves = static_cast<std::uint64_t>(b.chance_nodes) * 48ull;
    b.u2_artifact_rows = frontier_leaves * 2ull * u2_river_combos;
  }
  b.u2_artifact_bytes = b.u2_artifact_rows * sizeof(OracleRow);

  // --- U3: whole-trunk build grand total (all 48 river fanouts; not resident).
  if (b.river_action_nodes != 0)
    b.u3_all_river_infosets =
        static_cast<std::uint64_t>(b.river_action_nodes) * b.river_combos + b.river_combos;
  b.u3_build_grand_total_infosets = b.u1_turn_infosets + b.u3_all_river_infosets;

  // Build envelope (U3) only; the online 1M gate is the U1 flag above.
  b.under_tree_node_limit = b.total_nodes <= kPathANodeLimit;
  b.under_byte_limit = b.accounted_bytes <= kPathAByteLimit;
  return b;
}

std::string grid_budget_csv_header() {
  return "label,root,lever,raise_cap,grid,resource_boundary,boundary_reason,total_nodes,"
         "action_nodes,turn_action_nodes,river_action_nodes,chance_nodes,terminal_nodes,"
         "accounted_bytes,max_actions_per_node,mean_actions_per_node,turn_combos,river_combos,"
         "u1_turn_infosets,u1_reached_river_infosets_max,u1_online_resident_infosets,"
         "u1_under_1m_infoset,u2_artifact_rows,u2_artifact_bytes,u3_all_river_infosets,"
         "u3_build_grand_total_infosets,under_node_limit,under_byte_limit";
}

std::string to_csv(const GridBudget& b) {
  std::string s;
  auto append = [&](const std::string& v) { s += (s.empty() ? "" : ",") + v; };
  append(b.label);
  append(b.root_kind);
  append(b.lever);
  append(std::to_string(b.raise_cap));
  append(b.grid);
  append(b.resource_boundary ? "true" : "false");
  append(b.boundary_reason);
  append(std::to_string(b.total_nodes));
  append(std::to_string(b.action_nodes));
  append(std::to_string(b.turn_action_nodes));
  append(std::to_string(b.river_action_nodes));
  append(std::to_string(b.chance_nodes));
  append(std::to_string(b.terminal_nodes));
  append(std::to_string(b.accounted_bytes));
  append(std::to_string(b.max_actions_per_node));
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", b.mean_actions_per_action_node);
    append(buf);
  }
  append(std::to_string(b.turn_combos));
  append(std::to_string(b.river_combos));
  append(std::to_string(b.u1_turn_infosets));
  append(std::to_string(b.u1_reached_river_infosets_max));
  append(std::to_string(b.u1_online_resident_infosets));
  append(b.u1_under_1m_infoset ? "true" : "false");
  append(std::to_string(b.u2_artifact_rows));
  append(std::to_string(b.u2_artifact_bytes));
  append(std::to_string(b.u3_all_river_infosets));
  append(std::to_string(b.u3_build_grand_total_infosets));
  append(b.under_tree_node_limit ? "true" : "false");
  append(b.under_byte_limit ? "true" : "false");
  return s;
}

}  // namespace bs::rfc0047
