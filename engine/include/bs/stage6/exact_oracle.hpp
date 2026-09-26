// stage6/exact_oracle.hpp — RFC 0008 stage 6 R9 exact small-game oracle.
//
// On a game small enough to enumerate, this computes the EXACT expected
// per-seat utility of a joint behavior profile by full recursion over the real
// L1 GameState tree (not the coarse abstraction):
//
//   * Action node: expectation over the acting seat's policy distribution.
//   * Deal node: uniform average over every board card still available after
//     removing the board AND every seat's two hole cards (runouts are
//     conditioned on the full 2N hole deal, including seats that fold).
//   * Folded / Showdown: the exact GameState settle_* utility N-vector.
//
// The top-level oracle enumerates every mutually compatible joint hole deal
// over small declared per-seat ranges (the RFC 0006 joint table, uniform over
// combos) and averages the per-deal expectations. There is no sampling and no
// tolerance in the tree walk: the number of leaves is exact. Every terminal
// utility vector is zero-sum; the expected vector therefore is too, which the
// tests assert. R12 step 5 requires the Monte Carlo estimator to agree with
// this number on the enumerable game before any 7/10-seat figure is trusted.
//
// Offline-only, in bigshark_stage6_eval; never linked by the service/protocols.
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/stage6/infoset_key.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace bs::stage6 {

// Counters for the exact walk, surfaced for the pinned node/leaf assertions.
struct OracleCounts {
  std::uint64_t action_nodes = 0;
  std::uint64_t chance_nodes = 0;
  std::uint64_t fold_leaves = 0;
  std::uint64_t showdown_leaves = 0;
  std::uint64_t joint_deals = 0;
};

// The exact best response's per-information-set choices, optionally exported
// by exact_best_response_utility so the estimator's exact-freeze validation
// reuses the oracle walk as the single source of pooling truth. One entry per
// traverser node the walk reaches: the argmax deviation action and the full
// declared menu it was chosen from. Keyed exactly as the estimator keys its
// frozen table.
struct ExactBrChoiceSink {
  std::map<InfosetKey, poker::Action> actions;
  std::map<InfosetKey, std::vector<poker::Action>> menus;
};

// Exact expected utility N-vector for ONE fixed joint hole deal, averaging over
// all legal public runouts and policy randomness. `holes` has one pair per
// seat. Every reachable terminal contributes a zero-sum vector; the returned
// expectation is zero-sum to floating precision.
std::array<double, 10> exact_deal_utility(const poker::GameDef& rooted_or_preflop_def,
                                          const std::vector<HoleCards>& holes,
                                          const std::vector<const BehaviorPolicy*>& policies,
                                          OracleCounts* counts = nullptr);

// Exact expected utility N-vector averaged uniformly over every mutually
// compatible joint deal in `ranges` (one range per seat). Only valid on the
// small validation games where the joint support enumerates; the R9 fixtures
// use 2..4 combos per seat. `def` is the game root (the pinned rooted flop for
// the R9 3p fixtures). Policy i is used at seat i for every deal in which seat
// i holds a combo in its range. The result is the exact profile value the
// Monte Carlo estimator must reproduce.
std::array<double, 10> exact_oracle_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, OracleCounts* counts = nullptr);

// OMINSCIENT per-deal deviation utility (a diagnostic upper bound, NOT the
// best response). At each traverser node it picks the best action SEPARATELY
// for every joint hole deal, so the traverser can condition on opponents'
// unrevealed cards. By the max-expectation inequality this is >= the true
// best-response value (strictly when the per-infoset argmax disagrees across
// deals). It is kept only as a named bound; the R8 estimator must NOT be
// validated against it. Averaged uniformly over the joint deals in `ranges`,
// opponents fixed, exact settlement.
std::array<double, 10> exact_omniscient_deviation_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
    OracleCounts* counts = nullptr);

// EXACT best-response utility of one traverser against the N-1 fixed policies
// over the joint hole-deal distribution in `ranges`. This is the R9 reference
// the Monte Carlo two-phase estimator must reproduce. A best response is a
// single action per INFORMATION SET (public history plus the traverser's own
// two cards), so the choice at a traverser node POOLS every joint-deal view
// consistent with that infoset and maximizes the summed expected utility
// max_a sum_z w_z Q(z,a) — it never sees opponent holdings:
// max_a E_z[Q] <= E_z[max_a Q] (the latter is exact_omniscient_deviation).
// Opponent action nodes follow their policy distribution; deal nodes average
// uniformly over runout cards conditioned on each full joint deal; terminals
// settle exactly. The other seats' policies are unchanged (a unilateral
// deviation over the declared menu).
//
// The traverser's value is non-negative against its joint-profile value ONLY
// when the traverser profile's action support is contained in the declared
// deviation menu at every reachable node (e.g. check/call or always-jam
// policies); for an arbitrary profile that emits off-grid totals the
// best-WITHIN-menu value can be lower, which is an honest property of the
// declared-menu NashConv quantity.
std::array<double, 10> exact_best_response_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
    OracleCounts* counts = nullptr, ExactBrChoiceSink* choices = nullptr);

}  // namespace bs::stage6
