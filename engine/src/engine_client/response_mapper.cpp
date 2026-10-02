#include "response_mapper.hpp"

#include <bs/prng.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

namespace bs::engine_client {

namespace {

namespace pv = ::bigshark::engine::v1;

// The operational fallback, matching the host's operationalFallbackDecision:
// check if legal, else call if legal, else fold. This is the labeled
// operational fallback the minor-2 AUTOMATIC path serves on a blueprint or
// resolver miss, never the heuristic.
bs::poker::Action operational_fallback(const bs::poker::LegalActions& legal) {
  if (legal.check)
    return {bs::poker::ActionType::Check};
  if (legal.call)
    return {bs::poker::ActionType::Call};
  return {bs::poker::ActionType::Fold};
}

// Replicates the engine's protocol sampler exactly: one domain-separated
// SplitMix64 draw, top 53 bits mapped to [0,1), first prefix-CDF bucket
// strictly greater than the draw, clamped to the last positive bucket.
// Mirrors sampleBucket in engine/src/protocol/v1_response_mapper.cpp.
std::size_t sample_bucket(const std::vector<double>& probabilities, std::uint64_t seed) {
  double recorded_sum = 0.0;
  for (double p : probabilities)
    recorded_sum += p;
  if (!(recorded_sum > 0.0) || !std::isfinite(recorded_sum))
    return probabilities.size();

  bs::SplitMix64 rng = bs::SplitMix64::protocolSampler(seed);
  const std::uint64_t draw = rng.next_u64();
  const double unit = static_cast<double>(draw >> 11) * 0x1.0p-53;
  const double point = unit * recorded_sum;

  double prefix = 0.0;
  for (std::size_t i = 0; i < probabilities.size(); ++i) {
    prefix += probabilities[i];
    if (prefix > point)
      return i;
  }
  for (std::size_t i = probabilities.size(); i-- > 0;) {
    if (probabilities[i] > 0.0)
      return i;
  }
  return probabilities.size();
}

// Maps a protobuf action type + optional target_total to a concrete
// poker::Action. Aggressive actions use the LEGAL aggressive verb (not the
// engine's wire verb), so a preflop RAISE lands on the state's Bet type at
// the big-blind option. Returns true on success; false (with reason set) on
// a missing target_total.
bool map_wire_action(pv::ActionType type, bool has_target, std::uint64_t target,
                     const bs::poker::LegalActions& legal, bs::poker::Action& out,
                     std::string& reason) {
  switch (type) {
    case pv::ACTION_TYPE_FOLD:
      out = {bs::poker::ActionType::Fold};
      return true;
    case pv::ACTION_TYPE_CHECK:
      out = {bs::poker::ActionType::Check};
      return true;
    case pv::ACTION_TYPE_CALL:
      out = {bs::poker::ActionType::Call};
      return true;
    case pv::ACTION_TYPE_BET:
    case pv::ACTION_TYPE_RAISE:
      if (!has_target) {
        reason = "engine aggressive action missing target_total";
        return false;
      }
      if (!legal.aggressive) {
        reason = "engine aggressive action with no legal aggressive interval";
        return false;
      }
      out = {legal.aggressive->type, target};
      return true;
    default:
      reason = "engine returned unknown action type";
      return false;
  }
}

}  // namespace

MappedDecision map_decision_response(const pv::DecisionResponse& response,
                                     const bs::poker::LegalActions& legal,
                                     std::uint64_t decision_seed) {
  MappedDecision result;

  if (response.has_error()) {
    result.fell_back = true;
    result.reason = "engine error " + std::to_string(static_cast<int>(response.error().code())) +
                    ": " + response.error().message();
    result.action = operational_fallback(legal);
    return result;
  }

  // Extract the strategy from either the minor-0 `strategy` field or the
  // minor-1/2 `expanded_strategy` field.
  const pv::Strategy* strategy = nullptr;
  const pv::ExpandedStrategy* expanded = nullptr;
  if (response.has_strategy()) {
    strategy = &response.strategy();
  } else if (response.has_expanded_strategy()) {
    expanded = &response.expanded_strategy();
  } else {
    result.fell_back = true;
    result.reason = "engine response has no strategy or error";
    result.action = operational_fallback(legal);
    return result;
  }

  // Extract the guarantee level from the solver metadata (minor 2 only).
  const pv::SolverMetadata* solver = strategy ? &strategy->solver() : &expanded->solver();
  if (solver->has_guarantee_level())
    result.guarantee_level = solver->guarantee_level();

  // Prefer the selected_action when present; otherwise sample from the
  // distribution using the engine's protocol sampler.
  bs::poker::Action action{};
  std::string map_reason;
  bool mapped = false;

  const bool has_selected =
      strategy ? strategy->has_selected_action() : expanded->has_selected_action();
  if (has_selected) {
    const pv::SelectedAction& sel =
        strategy ? strategy->selected_action() : expanded->selected_action();
    mapped = map_wire_action(sel.type(), sel.has_target_total(), sel.target_total(), legal, action,
                             map_reason);
  } else {
    // Sample from the action distribution.
    const auto& actions = strategy ? strategy->actions() : expanded->actions();
    if (actions.empty()) {
      result.fell_back = true;
      result.reason = "engine strategy has no actions";
      result.action = operational_fallback(legal);
      return result;
    }
    std::vector<double> probs;
    probs.reserve(actions.size());
    for (const auto& a : actions)
      probs.push_back(a.probability());
    const std::size_t chosen = sample_bucket(probs, decision_seed);
    if (chosen >= static_cast<std::size_t>(actions.size())) {
      result.fell_back = true;
      result.reason = "engine sampler selected no live action";
      result.action = operational_fallback(legal);
      return result;
    }
    const pv::ActionPolicy& policy = actions[chosen];
    mapped = map_wire_action(policy.type(), policy.has_target_total(), policy.target_total(), legal,
                             action, map_reason);
  }

  if (!mapped) {
    result.fell_back = true;
    result.reason = map_reason;
    result.action = operational_fallback(legal);
    return result;
  }

  // Defense in depth: the engine must return a legal action. A violation is
  // an engine fault, never something the caller silently clamps.
  if (!legal.contains(action)) {
    result.fell_back = true;
    result.reason = "illegal engine action";
    result.action = operational_fallback(legal);
    return result;
  }

  result.action = action;
  return result;
}

}  // namespace bs::engine_client
