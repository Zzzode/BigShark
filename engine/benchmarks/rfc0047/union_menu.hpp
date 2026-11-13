// RFC 0048 Path A Stage 2b (D184): benchmark-only UNION-MENU reference
// construction and generalized subset embedding map. This is a distinct
// candidate reference domain from the frozen cap_embedding instrument: it
// makes non-prefix sizing grids (interleaved subsets, reference-foreign
// targets) legally measurable without modifying the frozen G5 cap-0
// component, whose formula and verdicts stay byte-stable.
//
// Union algebra (RFC 0048 Stage-2b addendum subsection B): under
// CoverSeeds::MinAndCap the builder's per-node target set is
// sort(unique({minimum, cap} U clamp(F))), and clamping is a fixed function
// at a node, so merging the two schedules' fraction sets yields exactly the
// set union of the clamped target menus. The union abstraction is always
// declared(..., MinAndCap, 0): uncapped, with its own declared-family digest
// so a union artifact can never be mistaken for an identity(0) artifact.
//
// Subset map (subsection C): unlike the frozen prefix mapper, every mass and
// policy decision goes through an explicit per-node edge_map keyed by exact
// Action equality. An ordered prefix degenerates to the identity edge map
// [0..m-1, -1..]; interleaved and true-union subsets are first-class cases.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs::rfc0047 {

// Merge two MinAndCap action abstractions into the uncapped union
// abstraction declared(union_schedule, MinAndCap, 0). Throws
// std::invalid_argument when either input uses DeclaredOnly (the union
// algebra relies on the shared minimum/cap seeds) or when the merged menu
// cannot be declared (malformed fraction or >32 actions at a node).
bs::abstraction::ActionAbstraction make_union_abstraction(
    const bs::abstraction::ActionAbstraction& first,
    const bs::abstraction::ActionAbstraction& second);

// Generalized bounded -> reference subset embedding.
//
// node_map is parallel to the REFERENCE tree: paired bounded-tree node
// index, or kNoSubsetMap for a node inside a reference-only extension
// subtree. edge_map is populated only at paired action nodes, parallel to
// the reference node's menu: the bounded action index equal to each
// reference action, or -1 when the reference edge is extension-only.
//
// Fails closed with std::invalid_argument when:
//   - the trees are not built from the identical GameDef
//     (bs::poker::same_game_def, checked before any DFS);
//   - paired nodes diverge in kind;
//   - a bounded action has no equal reference action (not a subset);
//   - a reference-only edge is passive (passive actions are legality-derived
//     and identical at equal states, so this proves state divergence);
//   - a bounded menu is wider than the reference menu at a paired node.
struct SubsetMap {
  static constexpr std::size_t kNoNode = static_cast<std::size_t>(-1);
  std::vector<std::size_t> node_map;
  // Indexed [reference_node][reference_action]; empty at non-action nodes.
  std::vector<std::vector<int>> edge_map;
};

SubsetMap map_bounded_subset(const bs::tree::AbstractTree& reference,
                             const bs::tree::AbstractTree& bounded);

}  // namespace bs::rfc0047
