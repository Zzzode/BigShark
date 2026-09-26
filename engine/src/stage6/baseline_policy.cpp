#include <bs/policy.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/chart_preflop.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::stage6 {

poker::Action map_deployed_decision(const poker::GameState& state, const Decision& decision) {
  poker::Action action;
  if (decision.action == "fold")
    action = poker::Action{poker::ActionType::Fold, 0};
  else if (decision.action == "check")
    action = poker::Action{poker::ActionType::Check, 0};
  else if (decision.action == "call")
    // Domain convention: a call carries target 0; after_action derives the
    // amount from the state.
    action = poker::Action{poker::ActionType::Call, 0};
  else if (decision.action == "bet")
    action = poker::Action{poker::ActionType::Bet, static_cast<poker::Chips>(decision.amount)};
  else if (decision.action == "raise")
    action = poker::Action{poker::ActionType::Raise, static_cast<poker::Chips>(decision.amount)};
  else
    throw std::runtime_error("deployed baseline returned an unknown action: " + decision.action);

  const poker::LegalActions legal = state.legal();
  if (!legal.contains(action))
    throw std::runtime_error("deployed baseline decision is not legal in the unified state: " +
                             decision.action + " " + std::to_string(decision.amount));
  return action;
}

std::vector<PolicyAction> BaselineBehaviorPolicy::distribution(const poker::GameState& state,
                                                               std::size_t seat, HoleCards hole,
                                                               const PolicyContext& context) const {
  // One shared pinned-chart implementation; the baseline plays it on every
  // street, the composed candidate only preflop.
  const poker::Action action = pinned_chart_action(state, seat, hole, context);
  return std::vector<PolicyAction>{PolicyAction{action, 1.0}};
}

}  // namespace bs::stage6
