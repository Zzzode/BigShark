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
#include <stdexcept>
#include <string>
#include <vector>

#include "reach_bound.hpp"
#include "river_solver.hpp"
#include "union_menu.hpp"

namespace bs::rfc0047 {

// Deployment-observable public betting state reconstructed from the root
// GameDef and the public parent-chain action history (D217). On a
// river-rooted tree the root pot is dead money and the river betting street
// starts at zero, so street_committed is the fresh per-street cumulative
// investment rather than the gross contribution already sitting in the pot.
struct PublicState {
  std::uint64_t pot = 0;  // live pot including the root dead money
  std::array<std::uint64_t, 2> street_committed{0, 0};
};

// Reconstruct pot and per-seat street commitment at every node by replaying
// the incoming actions along each parent chain. Single river street only; the
// mover is the parent node's actor and short all-in calls are clamped to the
// remaining stack. The root must be a street-start root (every
// def.street_committed zero); a mid-street root fails loud with
// std::invalid_argument. NDEBUG-independent: throws std::logic_error if the
// invariant pot == root_pot + street_committed[0] + street_committed[1] ever
// breaks. Fold rows keep the pre-refund gross pot: the production
// unmatched-bet refund is a terminal settlement, and choosers consume
// action-node states only, where no refund has occurred.
std::vector<PublicState> replay_public_states(const bs::tree::AbstractTree& reference);

// Pick the legal aggressive edge whose additional street payment
// (target_total - current street commitment) is nearest ideal_additional; an
// exact equidistant pair resolves to the smaller target (lower sorted index).
// Returns -1 when the node has no aggressive action. Bench-visible so the
// tie-break is unit-testable with synthetic inputs.
long nearest_aggressive_choice(const bs::tree::TreeNode& node, std::uint64_t ideal_additional,
                               std::uint64_t street_base);

// The D208/D225 sparse-minimal chooser: the sorted index of the first legal
// aggressive edge, or -1 when the node has no aggressive action. Bench-visible
// so closure tests can pin the exact edge rather than re-implement the rule.
long first_aggressive_choice(const bs::tree::TreeNode& node);

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

// RFC 0048 D217: single-factor pot-fraction arm. The widened set W, budget,
// ordering, coarse retention, and closure are identical to make_sparse_multilayer_mask;
// the only difference is the aggressive choice at reference-only restricted
// nodes, which is the existing legal reference action nearest a canonical
// pot-sized target (fraction 1.0) instead of the sorted first aggressive edge.
// Pot and call due are reconstructed from public history; no action amount is
// synthesized and every passive legality edge is retained. Same preconditions
// and fail-closed behavior as the D208 builder.
MixedMenuMask make_pot_fraction_mask(const bs::tree::AbstractTree& reference,
                                     const bs::tree::AbstractTree& coarse,
                                     std::size_t widened_budget);

// RFC 0048 D225: counterexample-driven single-edge witness augmentation.
// The selector turns SEALED per-node best-response evidence (the
// ReachBoundResult of a frozen baseline snapshot) into exactly one public
// (node, edge) augmentation over the immutable cap-0 reference. It never
// solves or walks a BR tree itself: every numeric input comes from the
// sealed records, while the reference tree and baseline mask are read
// in-memory for legality classification. Bench-visible, not production.

// Distinct from std::invalid_argument so an exhausted cumulative budget is
// not misclassified as malformed sealed evidence (mirrors the
// tree_resource_exhausted typed-error convention).
class witness_budget_exhausted : public std::runtime_error {
 public:
  explicit witness_budget_exhausted(const std::string& what) : std::runtime_error(what) {}
};

// Cumulative monotone-only budget for real solves. Selection and mask
// augmentation never charge a token; a charge with no remaining token
// throws witness_budget_exhausted BEFORE any solver call. There is no
// reset: baseline, selection, validation, and a retry share one ledger.
class RefinementBudget {
 public:
  explicit RefinementBudget(std::size_t solve_tokens) : remaining_(solve_tokens) {}
  void charge();
  std::size_t remaining() const noexcept { return remaining_; }

 private:
  std::size_t remaining_;
};

enum class WitnessStatus { kSelected, kNoCandidate };

struct WitnessSelection {
  WitnessStatus status = WitnessStatus::kNoCandidate;
  // Valid only when status == kSelected: the reference node and the single
  // suppressed aggressive reference edge chosen.
  std::size_t node = 0;
  std::size_t edge = 0;
  double node_score = 0.0;  // canonical reach-weighted node option value
  double edge_score = 0.0;  // canonical reach-weighted chosen-edge score
  // Evidence provenance copied verbatim from the sealed ReachBoundResult so
  // a selection can never be silently paired with a different abstraction.
  std::uint64_t bounded_abstraction_digest = 0;
  std::uint64_t reference_abstraction_digest = 0;
  std::uint64_t bounded_shape_digest = 0;
  std::uint64_t reference_shape_digest = 0;
};

// Deterministically select at most one witness (node, edge) from the sealed
// evidence. Throws std::invalid_argument on malformed records, evidence/mask
// disagreement, a zero-bounded-edge row, or a non-finite scoring term;
// returns kNoCandidate when no eligible node or edge clears the fixed floor.
// Repeated calls and combo-row-permuted evidence copies return the bitwise
// identical result (canonical sorted reduction).
WitnessSelection select_witness_augmentation(const bs::tree::AbstractTree& reference,
                                             const MixedMenuMask& baseline,
                                             const ReachBoundResult& evidence);

// Apply a selection to the baseline mask: kNoCandidate returns an untouched
// copy; kSelected flips the one scored suppressed aggressive edge and fills
// every newly reachable all-false row with the D208 sparse minimal legal
// menu, then validates. Throws std::invalid_argument on an illegal witness.
MixedMenuMask apply_witness_augmentation(const bs::tree::AbstractTree& reference,
                                         const MixedMenuMask& baseline,
                                         const WitnessSelection& selection);

// RFC 0048 D227: deterministic two-public-node combination over the SAME
// sealed baseline mask and SAME sealed evidence as the D225 selector. The
// "two" is a fixed constant; there is no k-node parameterization. Both
// candidates come from the frozen evidence, and a post-flip mask is never
// treated as certified by the old evidence. Bench-visible, not production.
enum class CombinationStatus {
  kNoCandidate,     // fewer than two usable edges (count/scores explain why)
  kSingleEligible,  // exactly one eligible node; the single-edge case stays D225
  kContained,       // second edge already legal after the first closure
  kCombined         // two distinct nodes, one suppressed aggressive edge each
};

struct CombinationSelection {
  CombinationStatus status = CombinationStatus::kNoCandidate;
  // First witness (reproduces the D225 selection when kCombined).
  std::size_t node1 = 0;
  std::size_t edge1 = 0;
  double node1_score = 0.0;
  double edge1_score = 0.0;
  // Second witness on a distinct reference node; valid only when kCombined.
  std::size_t node2 = 0;
  std::size_t edge2 = 0;
  double node2_score = 0.0;
  double edge2_score = 0.0;
  // Observability (pre-registration P3b): the eligible-node count is present
  // on every path so no-candidate reasons stay distinguishable.
  std::size_t eligible_count = 0;
  // Evidence provenance copied verbatim from the sealed ReachBoundResult.
  std::uint64_t bounded_abstraction_digest = 0;
  std::uint64_t reference_abstraction_digest = 0;
  std::uint64_t bounded_shape_digest = 0;
  std::uint64_t reference_shape_digest = 0;
};

// Select at most two distinct-node witnesses from the sealed evidence.
// Throws std::invalid_argument on malformed records or evidence/mask
// disagreement; returns a typed non-combined status (never a partial result)
// when fewer than two usable edges exist. Repeated and combo-row-permuted
// calls return the bitwise identical result.
CombinationSelection select_combination_augmentation(const bs::tree::AbstractTree& reference,
                                                     const MixedMenuMask& baseline,
                                                     const ReachBoundResult& evidence);

// Apply a combination: non-kCombined statuses return an untouched baseline
// copy; kCombined flips both edges through the shared closure and validates
// the result. Throws std::invalid_argument on an illegal coordinate while the
// caller's baseline stays bitwise identical (all edits are on copies).
MixedMenuMask apply_combination_augmentation(const bs::tree::AbstractTree& reference,
                                             const MixedMenuMask& baseline,
                                             const CombinationSelection& selection);

// RFC 0048 D233: comparable counterfactual attribution and candidate admission.
// Pure in-memory, zero-solve: the entry point accepts no RiverSolveResult,
// RefinementBudget, iteration count, or thread count, so a CFR/BR computation
// is not expressible through this API. Bench-visible, not production.

// A bounded NashConv value of a re-solved policy. Deliberately has no
// arithmetic or comparison operators: a difference between two of these mixes
// a menu change with policy re-convergence and must never be attributed to an
// edge or ordering. Only the raw value is extractable, for reporting.
class PolicyNashConvValue {
 public:
  PolicyNashConvValue() = default;
  explicit PolicyNashConvValue(double value) : value_(value) {}
  double raw() const noexcept { return value_; }

 private:
  double value_ = 0.0;
};

// A scored mask coordinate independent of the single-edge selector types, so
// the admission API can describe a one- or two-coordinate candidate without
// being forced into a fixed selector output shape (review round-3 note 2).
struct MaskCoordinate {
  std::size_t node = 0;
  std::size_t edge = 0;
};

enum class AttributionStatus { kComparable, kIncomparableSource, kInadmissibleCandidate };

// Exhaustive reason enum; consumers switch under -Werror=switch.
enum class AttributionReason {
  kNone,
  kDigestMismatch,
  kMaskWidthMismatch,
  kGameDefMismatch,
  kNormalizationMismatch,
  kCompletionArmMismatch,
  kTermMaskBindingMismatch,
  kLabelMismatch,
  kMaskContentInvalid,
  kNotStrictSuperset,
  kSingleEdgeCountViolation,
  kCombinedNodeCountViolation,
  kDuplicateNode,
  kMissingOrNonFiniteTerm,
  kNegativeMenuTerm,
  kSelectionNotActive,
  kCoordinateUnreachable
};

// Field projection of one sealed candidate. Constructible without any solver
// or tree access; the constructor only copies and subtracts, so NaN and
// negative terms remain representable and are rejected later by the admission
// gate rather than here.
class SealedCandidateTerms {
 public:
  SealedCandidateTerms() = default;
  SealedCandidateTerms(const bs::poker::GameDef& def, std::array<std::uint64_t, 4> tree_digests,
                       const char* reference_label, const char* m2_marking, std::uint64_t mask_hash,
                       std::uint64_t policy_hash, double bb_normalization,
                       bs::rfc0047::CapExtensionCompletion completion_arm,
                       double exact_exploitability_bb, double bounded_nash_conv_bb);

  std::uint64_t game_def_digest() const noexcept { return game_def_digest_; }
  const std::array<std::uint64_t, 4>& tree_digests() const noexcept { return tree_digests_; }
  std::uint64_t mask_hash() const noexcept { return mask_hash_; }
  std::uint64_t policy_hash() const noexcept { return policy_hash_; }
  double bb_normalization() const noexcept { return bb_normalization_; }
  bs::rfc0047::CapExtensionCompletion completion_arm() const noexcept { return completion_arm_; }
  double exact_exploitability_bb() const noexcept { return exact_bb_; }
  double bounded_nash_conv_bb_raw() const noexcept { return bounded_bb_; }
  double pure_menu_term_bb() const noexcept { return exact_bb_ - bounded_bb_; }
  PolicyNashConvValue bounded_nash_conv() const noexcept {
    return PolicyNashConvValue(bounded_bb_);
  }
  const std::string& reference_label() const noexcept { return reference_label_; }
  const std::string& m2_marking() const noexcept { return m2_marking_; }

 private:
  std::uint64_t game_def_digest_ = 0;
  std::array<std::uint64_t, 4> tree_digests_{};
  std::string reference_label_;
  std::string m2_marking_;
  std::uint64_t mask_hash_ = 0;
  std::uint64_t policy_hash_ = 0;
  double bb_normalization_ = 0.0;
  bs::rfc0047::CapExtensionCompletion completion_arm_ =
      bs::rfc0047::CapExtensionCompletion::Uniform;
  double exact_bb_ = 0.0;
  double bounded_bb_ = 0.0;
};

// One candidate slot: its mask, the scored coordinates it claims on
// baseline-reachable rows, and its sealed terms.
struct CandidateBundleEntry {
  MixedMenuMask mask;
  std::vector<MaskCoordinate> scored;
  SealedCandidateTerms terms;
};

struct AttributionReport {
  AttributionStatus status = AttributionStatus::kIncomparableSource;
  AttributionReason reason = AttributionReason::kNone;
  // Populated only when status == kComparable.
  double pure_menu_baseline = 0.0;
  double pure_menu_single = 0.0;
  double pure_menu_combined = 0.0;
  // Sign-unconstrained observed changes, descriptive only, never edge effects.
  double observed_menu_omission_baseline_to_single = 0.0;
  double observed_menu_omission_single_to_combined = 0.0;
  // List-only, non-subtractable policy NC values.
  std::array<PolicyNashConvValue, 3> bounded_nash_conv{};
};

// FNV-1a over a GameDef using exactly the same_game_def identity field set.
std::uint64_t hash_game_def(const bs::poker::GameDef& def);

// FNV-1a over a mask with the frozen serialization: node index bytes, then per
// row its width and one 0/1 byte per cell; independent of vector<bool> packing.
std::uint64_t hash_mask(const bs::tree::AbstractTree& reference, const MixedMenuMask& mask);

// Admit or reject the three-candidate bundle (baseline, single, combined).
// Read-only over every entry: a rejected or successful call leaves all masks
// bitwise unchanged, and the result is a value report. Throws only on
// allocation failure; all domain failures are typed in the report.
AttributionReport attribute_candidate_bundle(const bs::tree::AbstractTree& reference,
                                             const bs::poker::GameDef& root_def,
                                             const CandidateBundleEntry& baseline,
                                             const CandidateBundleEntry& single,
                                             const CandidateBundleEntry& combined);

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
