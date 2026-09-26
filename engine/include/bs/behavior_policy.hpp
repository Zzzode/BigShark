// behavior_policy.hpp — RFC 0008 stage 6 offline seat-indexed behavior policy.
//
// Stage 6 measures whole-hand, multi-seat profiles. The deployed heuristic is
// reachable only through the string-context evaluatePolicy(Ctx) surface and
// the heads-up policy types are two-seat only, so measurement needs one
// seat-agnostic interface over the unified game:
//
//     state + acting seat + that seat's two hole cards
//       -> probability distribution over CONCRETE legal actions
//
// Design constraints:
//  * The interface lives in its own offline target, `bigshark_behavior`,
//    which links only `bigshark_poker`. No Ctx/policy vocabulary may leak into
//    L0/L1 (that would invert the existing policy -> poker edge), and nothing
//    here is linked by the decision service or either protocol.
//  * A policy returns concrete poker::Action values (bets/raises carry their
//    target total). It never returns an abstract menu index; exact<->coarse
//    translation belongs to the candidate implementation, not the interface.
//  * The distribution is over the policy's own finite support (a deterministic
//    policy returns one action with probability one); the CALLER is
//    responsible for action legality against GameState::legal() and for
//    sampling. A policy that emits an illegal action is a harness error, never
//    something the caller silently clamps.
//  * Policies are deterministic for fixed inputs; stochastic measurement
//    policies sample via caller-owned RNG streams, never ambient randomness.
#pragma once

#include <array>
#include <bs/game_definition.hpp>  // poker::GameState
#include <bs/heads_up.hpp>         // poker::Action / ActionType
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs::stage6 {

// One concrete action and the probability the policy assigns to it.
struct PolicyAction {
  poker::Action action{};
  double probability = 0.0;
};

// A seat's two hole cards as engine card ids (0..51).
using HoleCards = std::array<int, 2>;

// One observed public action with the seat that took it. Chance cards are not
// actions; the board comes from GameState.
struct LoggedAction {
  std::size_t seat = 0;
  poker::Action action{};
};

// The public action history of one simulated hand, split by street. The
// unified GameState is historyless by design, so policies that condition on
// observation (the pinned baseline) reconstruct their inputs from this log
// via the eval-target adapter; policies that do not need history ignore it.
struct HandLog {
  std::vector<LoggedAction> preflop;
  std::vector<LoggedAction> flop;
  std::vector<LoggedAction> turn;
  std::vector<LoggedAction> river;
};

// Per-decision context the simulator supplies: the public action history and
// the deterministic seed for this one decision. The seed is nonzero and
// deterministically derived per decision so every figure is reproducible.
struct PolicyContext {
  const HandLog* hand_log = nullptr;
  std::uint64_t decision_seed = 0;
};

// Seat-indexed behavior over the unified 2..10-seat game. Implementations:
//  * the pinned baseline adapter (drives evaluatePolicy through a
//    GameState->Ctx adapter; lives in bigshark_stage6_eval),
//  * UniformBehaviorPolicy (the declared fixed uniform reference; here),
//  * the composed candidate (pinned charts preflop, translated trained policy
//    postflop; lives in bigshark_stage6_eval).
class BehaviorPolicy {
 public:
  virtual ~BehaviorPolicy() = default;

  // Probability distribution over concrete actions at `state` for `seat`
  // holding `hole`, with the public hand log and decision seed in `context`.
  // Contract:
  //  * probabilities are finite, non-negative, and sum to 1;
  //  * every returned action is legal for `seat` at `state` (the caller still
  //    re-checks via GameState::legal() and treats a violation as a typed
  //    harness failure rather than clamping);
  //  * bets and raises carry a target total inside the legal aggressive
  //    interval;
  //  * the result is deterministic for fixed inputs.
  virtual std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                                 HoleCards hole,
                                                 const PolicyContext& context) const = 0;
};

// Builds the declared finite behavior menu at an action state: the legal
// passive actions in fixed order (fold, check, call) followed by aggressive
// targets (min-raise, each declared pot fraction of pot-after-call snapped to
// the inclusive [minimum, cap] interval, and the cap), deduplicated while
// preserving order. Pure function of the legal set and pot; the fraction
// schedule is the published identity of every menu-based stage-6 policy
// (uniform reference and R8 deviator), declared here so they cannot drift.
std::vector<poker::Action> declared_behavior_menu(const poker::GameState& state, std::size_t seat);

// Content hash naming the published deviation-menu schedule: fold/check/call
// in fixed order, the minimum, the five declared pot fractions, and the cap.
// Frozen artifacts record this so a schedule change is visible as an identity
// change; it hashes the exact numerator/denominator pairs, not snapped totals.
std::uint64_t declared_menu_identity_hash() noexcept;

// The fixed uniform-random reference opponent (RFC 0008 stage 6 "additional
// reference opponents"). Uniform over declared_behavior_menu: every entry
// gets equal mass. The legal raise interval is a continuum, so "uniform" is
// meaningful only over this declared finite menu, which the reference names.
class UniformBehaviorPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;
};

// Selects one action from a distribution with one unit draw in [0, 1). The
// draw is supplied by the caller (SplitMix64 top 53 bits convention), so this
// stays free of PRNG ownership and every selection is reproducible.
poker::Action sample_distribution(const std::vector<PolicyAction>& distribution, double unit_draw);

}  // namespace bs::stage6
