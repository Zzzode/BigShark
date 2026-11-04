#include "frontier_enumerator.hpp"

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <vector>

namespace bs::rfc0047 {

using bs::poker::GameState;
using bs::tree::AbstractTree;
using bs::tree::TreeNode;

namespace {

// Carry the GameState down with the DFS (same pattern as the tree builder), so
// the state at each chance child is exact and needs no parent-pointer replay.
void dfs(const AbstractTree& tree, const GameState& state, std::size_t n,
         std::vector<FrontierLeaf>& out) {
  const TreeNode& node = tree.node(n);
  if (node.is_chance()) {
    for (std::size_t child : node.children) {
      const TreeNode& cn = tree.node(child);
      const GameState after = state.after_card(cn.incoming_card);
      FrontierLeaf leaf;
      leaf.turn_chance_node = n;
      leaf.child_node = child;
      leaf.river_card = cn.incoming_card;
      for (int i = 0; i < 5; ++i)
        leaf.board5[i] = after.board()[i];
      // The runtime (not after.def(), which stays the root identity by design)
      // carries the line-specific pot and per-seat gross contributions/stacks;
      // rebuild a five-card river-rooted GameDef from it.
      bs::poker::GameDef& d = leaf.river_def;
      d = bs::poker::GameDef{};
      d.player_count = 2;
      d.button = state.def().button;
      d.big_blind = state.def().big_blind;
      d.ante = 0;
      d.preflop = false;
      d.terminal = bs::poker::TerminalDepth::River;
      d.pot = static_cast<int>(after.pot());
      d.board_size = 5;
      d.board = {leaf.board5[0], leaf.board5[1], leaf.board5[2], leaf.board5[3], leaf.board5[4]};
      for (std::size_t seat = 0; seat < 2; ++seat) {
        d.stacks[seat] = after.players()[seat].stack;
        d.contributions[seat] = after.players()[seat].contributed;
      }
      leaf.frontier_key = static_cast<std::uint32_t>(out.size());
      out.push_back(std::move(leaf));
    }
    return;
  }
  if (node.is_terminal())
    return;
  for (std::size_t a = 0; a < node.children.size(); ++a) {
    const GameState after = state.after_action(node.actor, node.actions[a]);
    dfs(tree, after, node.children[a], out);
  }
}

}  // namespace

std::vector<FrontierLeaf> enumerate_frontiers(const StratumSpec& spec, std::uint8_t cap) {
  const bs::poker::GameDef root_def = make_stratum_def(spec, cap);
  const AbstractTree tree(root_def, bs::abstraction::ActionAbstraction::identity(cap));
  std::vector<FrontierLeaf> leaves;
  const GameState root(root_def);
  dfs(tree, root, tree.root_index(), leaves);
  return leaves;
}

}  // namespace bs::rfc0047
