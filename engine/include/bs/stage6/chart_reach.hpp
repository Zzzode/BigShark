// stage6/chart_reach.hpp — chart-reach-restricted preflop hole ranges.
//
// The geometry enumerators are card-free: they describe which flop chip
// geometries the PINNED preflop chart can reach without dealing a card.
// Measurement additionally needs, for every live seat of a reached geometry,
// the set of concrete hole-card combos the seat can hold CONDITIONED ON ITS OWN
// CHART ACTIONS on the reaching line. This component replays one reaching
// preflop HandLog through the exact pinned chart path (adapt_to_ctx ->
// evaluatePolicySourced -> map_deployed_decision, the same 1326-holding sweep
// geometry_enumerator.cpp uses) and intersects, per seat, the holdings whose
// chart action equals the logged action at every one of that seat's decisions.
//
// A seat's range is conditioned ONLY on its own actions; inter-seat card
// compatibility is the joint dealer's job
// (bs::solver::sample_scalable_joint_deal).
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>  // HandLog
#include <bs/multiway_sampler.hpp>
#include <bs/stage6/geometry.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::stage6 {

// Typed refusal for an unreachable chart-reach condition: a live seat's
// conditioned range came back empty (under chart-only reach this happens for
// all-in-at-flop signatures, which never train or measure), or two bucket
// members with identical live sets produced different surviving card sets
// (the fail-closed bucket-consistency invariant).
class stage6_chart_reach_error : public std::runtime_error {
 public:
  explicit stage6_chart_reach_error(const std::string& what) : std::runtime_error(what) {}
};

// One flop geometry together with the exact preflop log of the chart-only walk
// that reached it. The geometry is unique per enumeration; when more than one
// walk reached it the lexicographically smallest canonical line is retained.
struct ReachableGeometry {
  GeometrySignature sig;
  HandLog preflop_log;
};

// Chart-ONLY walk (deviator seat = player_count, so no deviation grid), one
// entry per unique signature, sorted by sig.to_string(). When two walks reach
// the same signature the lexicographically smallest canonical line
// ("s<seat>:<int(action.type)>=<target_total> " joined) is retained.
std::vector<ReachableGeometry> enumerate_chart_flop_reach(std::size_t player_count,
                                                          poker::Chips big_blind);

// RAW chart-only walk hits BEFORE per-signature dedupe: one entry per walk
// that closed preflop, so a signature reached by several distinct lines
// appears several times (each with its own log). Exists for the audit pinning
// that every ACTIONABLE signature is reached by a single line (a future chart
// that produced two diverging ranges for one actionable signature would make
// the lexicographic-line tie-break lossy). Not used by production training.
std::vector<ReachableGeometry> enumerate_chart_flop_reach_hits(std::size_t player_count,
                                                               poker::Chips big_blind);

// Per-live-seat chart-reach-restricted hole ranges for one reached geometry.
// Returns exactly one vector per entry of rec.sig.live (ascending full-table
// seat order, which is the reduced-game seat order); folded seats get no
// vector. A combo is kept for seat s iff the pinned chart action equals the
// logged action at EVERY acting decision of s on the line; every kept combo
// has weight 1.0.
//
// Throws stage6_chart_reach_error when a live seat's conditioned range is
// empty. Throws std::invalid_argument when the log cannot be replayed against
// a fresh pinned fixture (actor mismatch, non-action/non-preflop state, the
// log does not close into the flop deal) or when a logged action is outside
// the chart support for all 1326 holdings at its state.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_ranges(
    const ReachableGeometry& rec, poker::Chips big_blind);

// One bucket member's per-live-seat surviving card sets. `live` is the
// member's full-table live list (ascending); `card_sets` holds one sorted
// combo set per entry of `live`, in the same reduced-seat order. `label` is
// the member signature's canonical string, used verbatim in error messages.
struct ChartReachMemberSets {
  std::string label;
  std::vector<std::size_t> live;
  std::vector<std::vector<std::array<int, 2>>> card_sets;
};

// Indicator-unions one bucket's per-member ranges: reduced seat i unions the
// i-th surviving set of every member, combo-deduplicated, every weight exactly
// 1.0. Fail-closed invariant: any two members whose live lists are identical
// MUST carry identical per-seat card sets, otherwise stage6_chart_reach_error
// names the bucket key and both member labels (silently mixing them would
// average incompatible chart-conditioned ranges). This is the production
// union step used by chart_reach_bucket_ranges; it is declared publicly so the
// invariant can be tested with fabricated member sets.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_union_member_ranges(
    const GeometryBucketKey& key, const std::vector<ChartReachMemberSets>& members);

// Builds one indicator-unioned range per reduced seat for a whole geometry
// bucket. `reach` is indexed by sig.to_string(); EVERY bucket.members entry
// must be present (a missing signature is a harness error, because under
// chart-reach the bucket is built from the chart-only matrix) and a missing
// member throws std::runtime_error naming it. Per-member ranges go through
// chart_reach_ranges; members with identical live lists must agree, enforced
// by chart_reach_union_member_ranges. Returns bucket.key.live_count vectors.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_bucket_ranges(
    const GeometryBucket& bucket, const std::vector<ReachableGeometry>& reach,
    poker::Chips big_blind);

}  // namespace bs::stage6
