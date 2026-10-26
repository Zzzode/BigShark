// practice/engine_served_policy.hpp — BehaviorPolicy backed by the engine host.
//
// Injects the engine host's decision pipeline into a PracticeTable bot seat.
// Each bot decision builds an engine DecisionRequest from the current GameState,
// sends it to the shared EngineProcess, and converts the engine DecisionResponse
// into a PolicyAction distribution. One EngineProcess instance is shared
// across all bot seats so every decision comes from the same loaded artifact
// library and resolver state.
#pragma once

#include <bs/behavior_policy.hpp>
#include <bs/practice/engine_process.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace bs::practice {

// A BehaviorPolicy that delegates every decision to the engine host.
// Construct with a shared EngineProcess (already started) and the seat index
// this policy represents.
class EngineServedPolicy final : public stage6::BehaviorPolicy {
 public:
  // `engine` must outlive this policy. `seat` is the hand-local seat index.
  EngineServedPolicy(EngineProcess& engine, std::size_t seat, std::uint32_t solve_budget_ms = 1000);

  std::vector<stage6::PolicyAction> distribution(
      const poker::GameState& state, std::size_t seat, stage6::HoleCards hole,
      const stage6::PolicyContext& context) const override;

  // Statistics for the shutdown report.
  std::size_t decisions_served() const noexcept { return served_; }
  std::size_t fallbacks() const noexcept { return fallbacks_; }

 private:
  EngineProcess& engine_;
  std::size_t seat_;
  std::uint32_t solve_budget_ms_;
  mutable std::size_t served_ = 0;
  mutable std::size_t fallbacks_ = 0;
  mutable std::uint64_t hand_counter_ = 0;
};

}  // namespace bs::practice
