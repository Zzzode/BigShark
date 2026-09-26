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
  else if (decision.action == "bet" || decision.action == "raise") {
    // The deployed chart's aggressive verb follows the live server vocabulary
    // the adapter advertises: postflop "bet" opens the betting (nothing owed)
    // and "raise" re-opens over a wager; preflop the big blind's option after
    // a limp is still "raise" even though the unified rules type it Bet. Map
    // onto the exact ActionType the state requires, and treat any other
    // token/state pairing as a harness mismatch rather than guessing.
    const poker::LegalActions probe = state.legal();
    if (!probe.aggressive)
      throw std::runtime_error(
          "deployed baseline decided to wager into a state with no legal "
          "aggressive action: " +
          decision.action + " " + std::to_string(decision.amount));
    const poker::ActionType state_type = probe.aggressive->type;
    const bool preflop = state.street() == poker::Street::Preflop;
    // Preflop the server advertises exactly one aggressive token ("raise"),
    // including at the big-blind option where the unified state types the
    // same action Bet; a literal "bet" must never arrive preflop. Postflop
    // the token must match the state type exactly (bet opens, raise re-opens).
    const bool token_accepted =
        preflop ? decision.action == "raise"
                : ((decision.action == "bet" && state_type == poker::ActionType::Bet) ||
                   (decision.action == "raise" && state_type == poker::ActionType::Raise));
    if (!token_accepted)
      throw std::runtime_error(
          "deployed baseline aggressive verb does not match the unified "
          "state: " +
          decision.action + " " + std::to_string(decision.amount));
    action = poker::Action{state_type, static_cast<poker::Chips>(decision.amount)};
  } else
    throw std::runtime_error("deployed baseline returned an unknown action: " + decision.action);

  const poker::LegalActions legal = state.legal();
  if (!legal.contains(action))
    throw std::runtime_error("deployed baseline decision is not legal in the unified state: " +
                             decision.action + " " + std::to_string(decision.amount) +
                             " (mapped to target " + std::to_string(action.target_total) + ")");
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
