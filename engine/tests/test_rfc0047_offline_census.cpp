// RFC 0049 Stage 1 Gate 1 native test: the offline wide-menu tree census.
// Exercises real AbstractTree builds under the bench-only offline envelope
// with no source-text matching: frozen envelope constants, count correctness
// against an independent tree walk, deterministic reruns, and typed
// census_rejected handling when a tree cannot fit.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cstdio>
#include <stdexcept>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/turn_solver.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;
using bs::tree::TreeLimits;

namespace {

bs::poker::GameDef shallow_river() {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {8, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

// Independent reference walk: count action nodes and the widest menu by
// iterating the materialized tree directly rather than calling the census
// arithmetic under test.
std::pair<std::size_t, std::size_t> independent_walk(const AbstractTree& tree) {
  std::size_t action_nodes = 0;
  std::size_t max_menu = 0;
  for (const auto& node : tree.nodes()) {
    if (!node.is_action())
      continue;
    ++action_nodes;
    max_menu = std::max(max_menu, node.actions.size());
  }
  return {action_nodes, max_menu};
}

bool test_envelope_constants_frozen() {
  CHECK(kOfflineCensusMaxNodes == 25'000'000);
  CHECK(kOfflineCensusMaxDepth == 256);
  CHECK(kOfflineCensusMaxBytes == (std::size_t{1} << 33));
  CHECK(kOfflinePackMaxMenuActions == 32);
  const TreeLimits limits = offline_census_limits();
  CHECK(limits.max_nodes == kOfflineCensusMaxNodes);
  CHECK(limits.max_depth == kOfflineCensusMaxDepth);
  CHECK(limits.max_bytes == kOfflineCensusMaxBytes);
  return true;
}

bool test_census_counts_match_independent_walk() {
  const auto def = shallow_river();
  const auto menu = make_grid_abstraction(SizingGrid::Coarse, 1);
  const OfflineCensusRow row = offline_census(def, menu, "shallow-coarse-cap1", "control", 1);
  CHECK(row.fits);
  CHECK(row.total_nodes > 0);
  CHECK(row.action_nodes > 0);
  CHECK(row.max_menu_width > 0);
  CHECK(row.max_menu_width <= kOfflinePackMaxMenuActions);
  CHECK(row.menu_within_pack_limit);
  CHECK(row.depth > 0);
  CHECK(row.projected_infosets == row.action_nodes * kRiverC);

  const AbstractTree tree(def, menu, offline_census_limits());
  const auto [walk_actions, walk_menu] = independent_walk(tree);
  CHECK(row.action_nodes == walk_actions);
  CHECK(row.max_menu_width == walk_menu);
  CHECK(row.total_nodes == tree.size());
  CHECK(row.depth == tree.depth());
  return true;
}

bool test_census_deterministic() {
  const auto def = shallow_river();
  const auto menu = make_grid_abstraction(SizingGrid::Wide, 1);
  const OfflineCensusRow a = offline_census(def, menu, "a", "sizing_grid", 1);
  const OfflineCensusRow b = offline_census(def, menu, "b", "sizing_grid", 1);
  CHECK(a.fits && b.fits);
  CHECK(a.total_nodes == b.total_nodes);
  CHECK(a.action_nodes == b.action_nodes);
  CHECK(a.max_menu_width == b.max_menu_width);
  CHECK(a.projected_infosets == b.projected_infosets);
  CHECK(a.accounted_bytes == b.accounted_bytes);
  return true;
}

bool test_tiny_limits_typed_rejection() {
  const auto def = shallow_river();
  const auto menu = make_grid_abstraction(SizingGrid::Coarse, 1);
  // A ten-node envelope cannot hold any real river tree; the census must
  // convert tree_resource_exhaustion into a typed rejected row, not throw.
  const TreeLimits tiny{10, 4, 4096};
  const OfflineCensusRow row = offline_census(def, menu, tiny, "tiny", "control", 1);
  CHECK(!row.fits);
  CHECK(row.action_nodes == 0);
  CHECK(row.total_nodes == 0);
  return true;
}

bool test_w1_control_census_builds() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  CHECK(!leaves.empty());
  const auto menu = make_grid_abstraction(SizingGrid::Coarse, 1);
  const OfflineCensusRow row =
      offline_census(leaves.front().river_def, menu, "w1-coarse-cap1", "control", 1);
  CHECK(row.fits);
  CHECK(row.action_nodes > 0);
  CHECK(row.projected_infosets == row.action_nodes * kRiverC);
  CHECK(row.menu_within_pack_limit);
  std::printf("D0049-G1 w1 control census: nodes=%zu action=%zu max_menu=%zu infosets=%zu\n",
              row.total_nodes, row.action_nodes, row.max_menu_width, row.projected_infosets);
  return true;
}

}  // namespace

int main() {
  if (!test_envelope_constants_frozen() || !test_census_counts_match_independent_walk() ||
      !test_census_deterministic() || !test_tiny_limits_typed_rejection() ||
      !test_w1_control_census_builds())
    return 1;
  std::printf("rfc0047_offline_census: all checks passed\n");
  return 0;
}
