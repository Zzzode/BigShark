// RFC 0048 Path B (D192): benchmark-only reach-weighted certified bound.
//
// This component is a parallel benchmark instrument next to the Stage-2b
// union_embedding component: it reuses the Stage-2b SubsetMap and river
// solver entry without modifying them and stays inside bigshark_rfc0047_bench
// behind the configure-time link guard. No production surface exists here.
//
// The math is frozen in the RFC 0048 Path B Addendum:
//   Lemma 1  sum_i (BR-EV)_root = sum_n sum_i pi_s(n,i) delta_s(n,i),
//            an exact telescoping identity (not an approximation);
//   Lemma 2  B(n) = R(n)*S(n) structurally dominates delta, giving the
//            selective-widening sufficient certificate C >= e(sigma);
//   Lemma 3  foreign reach, node deletion, and root-level weighting are not
//            conservative and fail closed here.
// Reach is always the evaluated profile's OWN per-combo forward reach,
// validated by an independent recomputation under the evaluator's exact
// snapshot operation order. Every output stays candidate evidence and can
// never be recorded as an M2 or frozen G5 pass.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cap_embedding.hpp"  // CapExtensionCompletion
#include "river_solver.hpp"
#include "union_embedding.hpp"  // candidate-domain label constants
#include "union_menu.hpp"

namespace bs::rfc0047 {

// Exact node-level accounting for one mapped action node of one seat.
struct NodeGapRecord {
  std::size_t reference_node = 0;
  std::size_t bounded_node = 0;
  std::size_t seat = 0;
  // Full forward reach pi_s(n,i): the product of the seat's OWN action
  // masses on the root-to-n path. This is the outer telescope weight; the
  // opponent's path masses live inside the continuation values, not here.
  std::vector<double> own_reach;
  // Opponent forward reach at the same node (dense, blocker-blind;
  // blockers enter only through the showdown matrix); its sum is S(n).
  std::vector<double> opponent_reach;
  // BR-walk continuation values, row-major [combo][reference action].
  std::vector<double> continuation;
  // Full-reference-menu gap delta(n,i) = max_a C(a) - sigma_b . C_b.
  std::vector<double> delta;
  // Bounded-menu-only gap: the laundering control, identically zero only
  // when no extension action carries option value.
  std::vector<double> bounded_gap;
  // Parallel to the reference node's menu: bounded action index or -1.
  std::vector<int> edge_map;
  bool widened = false;               // edge_map contains no -1 entry
  double payoff_range_r = 0.0;        // R(n), chips, signed-payoff width
  double opponent_reach_sum_s = 0.0;  // S(n) over all 1,081 reaches
  double structural_bound_b = 0.0;    // R(n) * S(n)
};

struct ReachBoundResult {
  const char* reference_label = kUnionReferenceLabel;
  const char* m2_marking = kUnionM2Marking;
  std::uint64_t bounded_abstraction_digest = 0;
  std::uint64_t reference_abstraction_digest = 0;
  std::uint64_t bounded_shape_digest = 0;
  std::uint64_t reference_shape_digest = 0;
  std::array<std::vector<NodeGapRecord>, 2> records;
  std::size_t mapped_action_nodes = 0;
  std::size_t widened_nodes = 0;
  double bounded_nash_conv_bb = 0.0;
  // Exact embedded exploitability reconstructed from the node records
  // (Lemma 1); must equal the Stage-2b union measure to FP order.
  double exact_exploitability_bb = 0.0;
  // Structural selective-widening certificate C (Lemma 2); >= exact.
  double structural_certificate_bb = 0.0;
  // Raw per-seat chip sums for the telescope checks.
  std::array<double, 2> raw_exact_chips{0.0, 0.0};
  // Raw-chip to BB divisor (1081 * 990 * big_blind) used by every
  // certificate variant so normalization stays identical across entries.
  double bb_normalization = 0.0;
};

// Solve the bounded policy and produce the full reach-bound accounting
// against the explicit reference abstraction. Throws std::invalid_argument
// on every Path B precondition failure.
ReachBoundResult measure_reach_bound(const bs::poker::GameDef& river_def,
                                     const bs::abstraction::ActionAbstraction& bounded_action,
                                     const bs::abstraction::ActionAbstraction& reference_action,
                                     std::uint64_t iterations, std::size_t threads,
                                     CapExtensionCompletion completion);

// Same accounting from an externally prepared immutable snapshot (tests and
// future composition; the snapshot is threaded into BOTH passes).
ReachBoundResult reach_bound_from_snapshot(const bs::poker::GameDef& river_def,
                                           const RiverSolveResult& solved,
                                           const bs::tree::AbstractTree& reference,
                                           const bs::tree::AbstractTree& bounded,
                                           const SubsetMap& subset,
                                           CapExtensionCompletion completion);

// Independent top-down forward-reach recomputation for one seat, indexed by
// REFERENCE tree node (empty at non-mapped/non-own nodes). Uses the
// evaluator's snapshot update order so a correct table matches bitwise.
std::vector<std::vector<double>> recompute_forward_reach(const bs::tree::AbstractTree& reference,
                                                         const bs::tree::AbstractTree& bounded,
                                                         const SubsetMap& subset,
                                                         const RiverSolveResult& solved,
                                                         std::size_t seat);

// Validate a supplied per-node reach table for one seat against the
// independent recomputation. Exact bitwise match by design; throws
// invalid_argument on any mismatch, wrong size, negative, or non-finite
// entry (foreign/tampered reach cannot enter a certificate).
void validate_supplied_reach(const bs::tree::AbstractTree& reference,
                             const bs::tree::AbstractTree& bounded, const SubsetMap& subset,
                             const RiverSolveResult& solved, std::size_t seat,
                             const std::vector<std::vector<double>>& supplied);

// Verify a custom nodewise scalar bound table against measured deltas.
// For every positive-own-reach combo of every record it requires
// bound[record_index] >= delta; throws invalid_argument otherwise (the
// RED naive-bound falsifier uses this entry point).
void verify_custom_bounds(const ReachBoundResult& result, const std::vector<double>& node_bound);

// Compute a selective-widening certificate with an explicit widened-node
// claim (reference-node indices). Throws invalid_argument if a claimed
// node is not a mapped action node or still carries an extension (-1)
// edge: widening cannot be claimed where an action is missing.
double selective_certificate(const ReachBoundResult& result,
                             const std::vector<std::size_t>& widened_reference_nodes);

// The Lemma-1 exact diagnostic: node-level delta at EVERY mapped node,
// independent of any widening claim. Must equal exact_exploitability_bb
// and exists as a named entry so the tight bound is never confused with
// the structural sufficient certificate.
double exact_diagnostic_certificate(const ReachBoundResult& result);

// Root aggregation stays the frozen unweighted MAX. Any attempt to combine
// per-root exploitabilities with convex weights is rejected here; the
// function exists so the refusal is a tested behavioral contract rather
// than an unwritten convention.
double aggregate_roots_max(const std::vector<double>& per_root_exploitability_bb);
void refuse_root_weights(const std::vector<double>& per_root_exploitability_bb,
                         const std::vector<double>& root_weights);

// The INVALID draft bound from Path B review round 1: max absolute
// descendant terminal payoff per record node, missing the opponent-reach
// mass and the payoff-range factor. Retained solely so the native RED
// fixture can prove verify_custom_bounds rejects it against measured gaps.
std::vector<double> naive_max_abs_payoff_bounds(const bs::tree::AbstractTree& reference,
                                                const ReachBoundResult& result);

// D195: one exact restricted-gap record per bounded own-seat action
// node. The walk runs on the BOUNDED tree alone (no extension nodes, so
// no completion-arm parameter); indices are bounded-tree node indices.
struct RestrictedNodeGap {
  std::size_t bounded_node = 0;
  std::size_t seat = 0;
  // Full forward own reach on the bounded tree (the telescope weight).
  std::vector<double> own_reach;
  // Exact bounded-game local gap g_g(n,i) = max_{a in A_b} C_g(a)
  // - sigma.C_g, arm-free by construction.
  std::vector<double> restricted_gap;
};

struct RestrictedGapResult {
  std::array<std::vector<RestrictedNodeGap>, 2> records;
  double bounded_nash_conv_bb = 0.0;
  // Telescope sum of reach*g_g per seat in raw chips.
  std::array<double, 2> raw_chips{0.0, 0.0};
  double bb_normalization = 0.0;
  double nash_conv_bb = 0.0;
};

// Self-consistent restricted BR walk over the bounded tree: at every own
// node it maximizes only over the bounded menu and never enters a
// reference extension. Paired back to the reference via SubsetMap only
// for later node-level menu_gap reconciliation. Throws invalid_argument
// on domain failures.
RestrictedGapResult measure_restricted_gaps(
    const bs::poker::GameDef& river_def, const bs::abstraction::ActionAbstraction& bounded_action,
    std::uint64_t iterations, std::size_t threads);

RestrictedGapResult restricted_gaps_from_snapshot(const bs::poker::GameDef& river_def,
                                                  const RiverSolveResult& solved,
                                                  const bs::tree::AbstractTree& bounded);

// Static extension-free predicate for seat s at a mapped reference node:
// true only when no reachable same-seat mapped action node in its subtree
// carries a -1 edge. DFS over mapped edges only; fail-closed if a mapped
// edge reaches an unmapped child (SubsetMap invariant violation).
bool subtree_is_extension_free(const bs::tree::AbstractTree& reference, const SubsetMap& subset,
                               std::size_t reference_node, std::size_t seat);

}  // namespace bs::rfc0047
