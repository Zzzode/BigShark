// stage6/baseline_policy.hpp — the pinned deployed heuristic as a BehaviorPolicy.
//
// This is the RFC 0008 stage 6 PINNED BASELINE OPPONENT, not a freshly authored
// strategy: distribution() builds the deployed Ctx through the GameState
// adapter and calls the exact evaluatePolicySourced entry point production
// uses, with the river solver disabled. It is deterministic (the adapter
// passes an explicit nonzero seed), so the result is one action with
// probability one. The byte-identical corpus gate (R4) proves this adapter
// path decides the same action and amount as the deployed parseRequest path.
//
// Lives in bigshark_stage6_eval: it is the one place allowed to link both the
// behavior interface and bigshark_policy.
#pragma once

#include <bs/behavior_policy.hpp>
#include <bs/decision.hpp>
#include <bs/stage6/adapter.hpp>
#include <cstdint>

namespace bs::stage6 {

class BaselineBehaviorPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;
};

// Maps a deployed Decision (string action + amount) to a concrete legal
// poker::Action for `state`. Used by both the behavior wrapper and the
// byte-identical corpus gate. Throws std::runtime_error when the deployed
// decision is not legal under GameState::legal(); the caller treats that as a
// typed harness failure, never a clamp.
poker::Action map_deployed_decision(const poker::GameState& state, const Decision& decision);

}  // namespace bs::stage6
