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
// at that flop (it cannot be dealt). The conditioning is exact.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>    // NSeatPolicy
#include <bs/range.hpp>            // comboIndex
#include <map>
#include <vector>

namespace bs::solver {

// One flop's continuation ranges: the per-seat combo distribution at the
// frontier, estimated from the policy's reach. The ranges are normalized to
// sum to one per seat; only combos with positive reach are present.
//
// The combo index is bs::comboIndex(c0, c1) — the same index the belief model
// and artifact reader use.
struct PolicyReachRangePair {
  std::array<int, 3> flop{};
  std::array<std::map<int, double>, 2> ranges;  // [seat][combo_index] = probability
};

// Compute continuation ranges for every flop reached with positive
// probability.
//
// `policy` is the sealed average policy from a preflop flop-terminal training
// run. `tree` must be the tree the policy was trained on (same GameDef and
// action abstraction). `ranges` are the per-seat declared ranges (prior
// weights); they must match the ranges the policy was trained on.
//
// The export walks the preflop tree for each in-range joint deal, following
// the average policy, and accumulates the reach at each flop chance node. A
// flop reachable through multiple preflop lines is accumulated (summed), not
// duplicated. Card removal is exact: a combo that blocks the flop receives
// zero reach at that flop.
//
// Returns one entry per flop with positive total reach, sorted by flop cards
// (ascending). Each entry's ranges are normalized to sum to one per seat.
//
// Throws std::invalid_argument if the policy and tree are mismatched or the
// ranges are malformed.
std::vector<PolicyReachRangePair> export_continuation_ranges(
    const NSeatPolicy& policy, const tree::AbstractTree& tree,
    const std::vector<std::vector<WeightedHand>>& ranges);

}  // namespace bs::solver
