// RFC 0048 D202: benchmark-only per-node heterogeneous (mixed) menu mechanism.
//
// An edge mask over an immutable cap-0 reference tree defines a restricted
// extensive-form game without constructing a second AbstractTree: nodes,
// edges, and terminal payloads stay untouched, and only the candidate
// policy's per-node allowed action set varies. A self-contained CFR+ solver
// (MixedMenuGame) runs over compact allowed-slot storage and re-expands its
// average policy into reference-width rows with exact zero mass on
// suppressed edges, so the existing union/reach-bound evaluators consume
// the policy through a mask-derived SubsetMap with no production edit.
//
// Benchmark-only under the rfc0047 link guard; no production resolver,
// ActionAbstraction public API, schema, protocol, or flag is changed.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "river_solver.hpp"
#include "union_menu.hpp"

namespace bs::rfc0047 {

// One allowed-edge row per reference tree node. Rows are empty exactly at
// non-action nodes; an action row always spans the node's full reference
// menu (entries are moot but present at mask-unreachable nodes). The
// structure has no combo dimension: a mask is a function of public node
// identity alone, and tree nodes are the engine infoset units.
struct MixedMenuMask {
  std::vector<std::vector<bool>> allowed;
};

// Validate raw mask well-formedness against the reference tree:
//   - one row per tree node, empty exactly at non-action nodes;
//   - every action row width equals the node's reference menu width;
//   - every mask-reachable action node keeps at least one allowed edge;
//   - suppressed edges are aggressive only: legality-derived passive edges
//     (fold/check/call) can never be removed.
// Throws std::invalid_argument on the first violation.
void validate_mask(const bs::tree::AbstractTree& reference, const MixedMenuMask& mask);

// Single-tree subset-map validator, the analog of the D184
// map_bounded_subset invariants when no second tree exists:
//   - table sizes match the reference and the root is mapped;
//   - node map is the identity bijection on the mapped node set;
//   - mapped action rows are in-range self indices with at least one entry
//     and -1 only on aggressive edges;
//   - allowed-edge child closedness: an allowed edge reaches a mapped child,
//     a suppressed edge reaches a kNoNode child, and the mapped set equals
//     the root DFS closure over allowed edges.
// Throws std::invalid_argument on the first violation.
void validate_mask_subset_map(const bs::tree::AbstractTree& reference, const SubsetMap& map);

// Derive the embedding SubsetMap from a validated mask. Mapped nodes are
// exactly the mask-reachable DFS closure; edge_map[n][a] is a when allowed,
// else -1, and every suppressed-edge subtree is kNoNode so the D192/D195
// telescope weights stay exact. The returned map passes
// validate_mask_subset_map.
SubsetMap mask_to_subset_map(const bs::tree::AbstractTree& reference, const MixedMenuMask& mask);

// The single pre-selected D202 candidate: the root plays the full reference
// cap-0 menu; nodes present in the coarse subset keep their coarse allowed
// sets; reference-only subtrees made reachable through a widened root edge
// inherit the full reference menu. The coarse->reference subset map supplied
// by the D184 mapper is the baseline, so no action matching is duplicated.
MixedMenuMask make_root_only_widening_mask(const bs::tree::AbstractTree& reference,
                                           const bs::tree::AbstractTree& coarse);

// RFC 0048 D208: sparse multi-layer widening under a deterministic public
// selection rule. Up to widened_budget coarse-mapped action nodes that still
// carry a suppressed edge are widened to the full reference menu, ordered by
// public betting depth with a DFS-index intra-depth tie-break (the root is
// always first); every other coarse-mapped node keeps its coarse edges, and a
// reference-only node reached through an opened edge gets the sparse minimal
// legal menu (all passive edges plus the single first aggressive edge when one
// exists). The result is a legal mask consumable by mask_to_subset_map and
// solve_mixed_menu unchanged.
//
// Fails closed with std::invalid_argument when widened_budget is non-positive,
// when the root itself has no suppressed edge (the root-first precondition is
// unmeetable), or the derived mask fails validate_mask. An over-large budget
// clamps to the number of eligible suppressed nodes rather than padding.
MixedMenuMask make_sparse_multilayer_mask(const bs::tree::AbstractTree& reference,
                                          const bs::tree::AbstractTree& coarse,
                                          std::size_t widened_budget);

struct MixedMenuSolveResult {
  // Standard river result fields; avg_sigma rows are full REFERENCE-width
  // with exact zero bits on suppressed edges (empty at non-action and
  // mask-unreachable nodes). nash_conv_* is the masked game's own
  // bounded NashConv from the allowed-edge best-response walk.
  RiverSolveResult river;
  // Full forward reach per seat (own action masses included), reference-node
  // indexed with empty rows off the mask-reachable closure, for bitwise
  // external validation against recompute_forward_reach.
  std::array<std::vector<std::vector<double>>, 2> forward_reach;
  // Per-seat telescope sum of forward reach times the allowed-only local
  // best-response gap. Equals the root BR-EV total within chip tolerance and
  // pins that the internal bounded NashConv walk is self-consistent.
  std::array<double, 2> telescope_raw_chips{0.0, 0.0};
  // Storage accounting for the resource gate: compact allowed slots versus
  // the full reference edge total the masked storage can never exceed.
  std::size_t compact_slot_total = 0;
  std::size_t reference_edge_total = 0;
};

// Seedless, deterministic CFR+ solve of the restricted game the mask
// defines. RM+ regret clipping, the public 1/|allowed(n)| fallback, and
// linear average weighting match the river kernel; every policy operation
// ranges over allowed slots only and traversal never descends a suppressed
// edge. Throws std::invalid_argument on a malformed def or mask.
MixedMenuSolveResult solve_mixed_menu(const bs::poker::GameDef& def,
                                      const bs::abstraction::ActionAbstraction& reference_action,
                                      const MixedMenuMask& mask, std::uint64_t iterations,
                                      std::size_t threads);

}  // namespace bs::rfc0047
