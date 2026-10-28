// RFC 0007: continuation-range export for preflop flop-terminal policies.
//
// A preflop policy trained on a flop-terminal game brings a RANGE to each
// flop, not a single hand: the distribution over each seat's combos at the
// frontier, conditioned on the policy's action distribution. This component
// computes those ranges from the sealed average policy's reach along the
// preflop line. They are the input to the offline blueprint evaluation that
// produces the DeclaredFrontierTable (option A).
//
// NAMING: the result is a PolicyReachRangePair — a policy-reach estimate at a
// declared profile, with NO equilibrium claim. It is not a certified
// equilibrium range and must not be read as one.
//
// MULTI-LINE ACCUMULATION: a flop reachable through several preflop action
// lines (limped pot, raised pot, etc.) is accumulated across every line that
// reaches it. The export sums the reach over all such lines; a flop appears
// exactly once in the output, never once per line.
//
// CARD REMOVAL: a combo that shares a card with the flop receives zero reach
// at that flop (it cannot be dealt). Joint deals where two seats share a card
// are impossible and excluded. The conditioning is exact.
//
// SEATS: the export serves any player count the tree supports (2..10). The
// walker follows the policy's per-actor rows at every action node, so every
// seat's hole cards condition the reach through the tree.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>    // NSeatPolicy
#include <bs/range.hpp>            // comboIndex
#include <map>
#include <optional>
#include <vector>

namespace bs::solver {

// One flop's continuation ranges: the per-seat combo distribution at the
// frontier, estimated from the policy's reach. The ranges are normalized to
// sum to one per seat; only combos with positive reach are present. The
// outer vector is sized to the game's player count (one map per seat).
//
// The combo index is bs::comboIndex(c0, c1) — the same index the belief model
// and artifact reader use.
struct PolicyReachRangePair {
  std::array<int, 3> flop{};
  std::vector<std::map<int, double>> ranges;  // [seat][combo_index] = probability
};

// Compute continuation ranges for every flop reached with positive
// probability.
//
// `policy` is the sealed average policy from a preflop flop-terminal training
// run. `tree` must be the tree the policy was trained on (same GameDef and
// action abstraction). `ranges` are the per-seat declared ranges (prior
// weights); they must match the ranges the policy was trained on, with one
// entry per seat.
//
// The export walks the preflop tree for each in-range joint deal, following
// the average policy, and accumulates the reach at each flop chance node. A
// flop reachable through multiple preflop lines is accumulated (summed), not
// duplicated. Card removal is exact in two dimensions: (1) joint deals where
// two seats share a card are impossible and excluded, and (2) a combo that
// blocks the flop receives zero reach at that flop.
//
// The tree walk is memoized by the per-seat Preflop169 bucket tuple, so the
// number of walks is at most the number of distinct bucket tuples across the
// in-range combos, not the number of joint deals.
//
// Returns one entry per flop with positive total reach, sorted by flop cards
// (ascending). Each entry's ranges are normalized to sum to one per seat.
//
// Throws std::invalid_argument if the policy and tree are mismatched or the
// ranges are malformed.
std::vector<PolicyReachRangePair> export_continuation_ranges(
    const NSeatPolicy& policy, const tree::AbstractTree& tree,
    const std::vector<std::vector<WeightedHand>>& ranges);

// RFC 0039: exact conditioned combo ranges at ONE turn depth-limit river
// boundary leaf.
//
// Given a sealed average policy on a TURN-ROOTED streets(1) tree, the
// per-seat declared root ranges, and a TerminalFrontier node index, compute
// the combo distribution conditioned on the public action line and river
// card that identify the leaf. The walk multiplies EVERY acting seat's
// average-policy probability along the deterministic action path to the
// leaf (missing rows fall back to uniform, matching the trainer), requires
// the leaf's river card to be card-disjoint from the joint deal, blocks
// every five-card-board combo, and independently normalizes each seat's
// marginal to sum to one (the same per-seat product-conditional input
// semantics train_nseat's JointDealDealer consumes; never joint-normalized).
//
// Folded seats retain their action-conditioned folding range: the trainer
// still needs the seat's combos for card blockage. The river chance
// probability is NOT part of the conditioning weight (the public river
// card is observed, so its probability cancels in the conditional).
//
// Returns std::nullopt when any seat has zero conditioned mass at the leaf
// (the caller maps that to the nested-model INVALID status). Throws
// std::invalid_argument on a malformed tree/policy/range combination.
struct LeafRanges {
  std::size_t frontier_node = 0;
  std::array<int, 5> board{};
  std::vector<std::vector<WeightedHand>> ranges;  // one normalized vector per seat
};
std::optional<LeafRanges> export_leaf_continuation_ranges(
    const NSeatPolicy& policy, const tree::AbstractTree& turn_tree,
    const std::vector<std::vector<WeightedHand>>& root_ranges, std::size_t frontier_node_index);

}  // namespace bs::solver
