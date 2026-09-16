#include <algorithm>
#include <array>
#include <bs/river_gto.hpp>
#include <bs/v1_protocol.hpp>
#include <string>

#include "v1_mappers.hpp"

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

pv::ActionType actionType(const std::string& action) {
  if (action == "fold")
    return pv::ACTION_TYPE_FOLD;
  if (action == "check")
    return pv::ACTION_TYPE_CHECK;
  if (action == "call")
    return pv::ACTION_TYPE_CALL;
  if (action == "bet")
    return pv::ACTION_TYPE_BET;
  if (action == "raise")
    return pv::ACTION_TYPE_RAISE;
  return pv::ACTION_TYPE_UNSPECIFIED;
}

pv::SolverSource solverSource(const std::string& reason) {
  if (reason.rfind("gto-cfr", 0) == 0)
    return pv::SOLVER_SOURCE_RIVER_DCFR;
  if (reason.rfind("gto", 0) == 0)
    return pv::SOLVER_SOURCE_RIVER_LP;
  static const std::array<const char*, 12> kPreflopReasons = {
      "short jam",   "short premium", "RFI",        "3bet value", "3bet bluff",  "flat",
      "call strong", "4bet value",    "4bet bluff", "vs3 call",   "jam premium", "BB option"};
  for (const char* prefix : kPreflopReasons)
    if (reason.rfind(prefix, 0) == 0)
      return pv::SOLVER_SOURCE_PREFLOP_CHART;
  return pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC;
}

const char* sourceCode(pv::SolverSource source) {
  switch (source) {
    case pv::SOLVER_SOURCE_PREFLOP_CHART:
      return "preflop-chart";
    case pv::SOLVER_SOURCE_RIVER_LP:
      return "river-lp";
    case pv::SOLVER_SOURCE_RIVER_DCFR:
      return "river-dcfr";
    case pv::SOLVER_SOURCE_MULTISTREET_CFR:
      return "multistreet-cfr";
    default:
      return "postflop-heuristic";
  }
}

// Verifies the engine action against the platform-provided legal set by kind
// and, for bets/raises, by exact inclusive target total.
bool actionIsLegal(const pv::HandState& state, pv::ActionType type, std::uint64_t target,
                   bool hasTarget) {
  for (const pv::LegalAction& legal : state.legal_actions()) {
    if (legal.type() != type)
      continue;
    if (type != pv::ACTION_TYPE_BET && type != pv::ACTION_TYPE_RAISE)
      return !hasTarget;
    return hasTarget && target >= legal.min_target_total() && target <= legal.max_target_total();
  }
  return false;
}

}  // namespace

MappingError::MappingError(pv::ErrorCode code, std::string message, bool retryable,
                           std::vector<pv::FieldViolation> violations)
    : std::runtime_error(std::move(message)),
      code_(code),
      retryable_(retryable),
      violations_(std::move(violations)) {}

pv::DecisionResponse mapDecisionResponse(const pv::DecisionRequest& request,
                                         const bs::Decision& decision) {
  const pv::ActionType type = actionType(decision.action);
  if (type == pv::ACTION_TYPE_UNSPECIFIED || decision.amount < 0)
    throw MappingError(pv::ERROR_CODE_INTERNAL, "engine produced an unrecognized action",
                       /*retryable=*/true);

  const bool hasTarget = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
  const std::uint64_t target = static_cast<std::uint64_t>(decision.amount);
  if (!actionIsLegal(request.state(), type, target, hasTarget))
    throw MappingError(pv::ERROR_CODE_INTERNAL,
                       "engine action is not a member of the requested legal actions",
                       /*retryable=*/true);

  pv::DecisionResponse response;
  pv::Strategy* strategy = response.mutable_strategy();

  pv::ActionPolicy* policy = strategy->add_actions();
  policy->set_type(type);
  if (hasTarget)
    policy->set_target_total(target);
  policy->set_all_in(false);
  policy->set_probability(1.0);

  // Minor-0 exposes the heuristic degenerate distribution. The sampled action
  // is deterministic (probability-1 policy) and only present when requested.
  if (request.options().include_sampled_action()) {
    pv::SelectedAction* selected = strategy->mutable_selected_action();
    selected->set_type(type);
    if (hasTarget)
      selected->set_target_total(target);
    selected->set_all_in(false);
  }

  pv::SolverMetadata* metadata = strategy->mutable_solver();
  const pv::SolverSource source = solverSource(decision.reason);
  metadata->set_source(source);
  metadata->set_solve_time_us(0);
  if (decision.equity >= 0)
    metadata->set_equity(std::clamp(decision.equity, 0.0, 1.0));
  if (decision.mdf >= 0)
    metadata->set_minimum_defense_frequency(std::clamp(decision.mdf, 0.0, 1.0));
  metadata->set_cache_hit(false);
  metadata->set_reason_code(sourceCode(source));
  std::string diagnostic = decision.reason;
  if (diagnostic.size() > 512)
    diagnostic.resize(512);
  if (!diagnostic.empty())
    metadata->set_diagnostic_reason(diagnostic);

  return response;
}

pv::DecisionResponse errorResponse(pv::ErrorCode code, const std::string& message, bool retryable,
                                   std::vector<pv::FieldViolation> violations) {
  pv::DecisionResponse response;
  pv::EngineError* error = response.mutable_error();
  error->set_code(code);
  std::string safe = message;
  if (safe.empty())
    safe = "engine request failed";
  if (safe.size() > 512)
    safe.resize(512);
  error->set_message(safe);
  error->set_retryable(retryable);
  // Bound the violation list so a request with hundreds of thousands of bad
  // entries cannot produce an error envelope beyond the frame limit and tear
  // down the persistent host. The final entry marks truncation.
  if (violations.size() > static_cast<std::size_t>(kMaxFieldViolations)) {
    violations.resize(static_cast<std::size_t>(kMaxFieldViolations) - 1);
    pv::FieldViolation marker;
    marker.set_field_path("");
    marker.set_description("additional field violations were omitted");
    violations.push_back(std::move(marker));
  }
  for (const pv::FieldViolation& violation : violations)
    *error->add_violations() = violation;
  return response;
}

pv::GetCapabilitiesResponse buildCapabilities() {
  pv::GetCapabilitiesResponse capabilities;
  capabilities.add_supported_protocol_minors(0);
  capabilities.set_engine_build_version("bigshark-engine-v1.0.0");
  capabilities.add_supported_game_variants(pv::GAME_VARIANT_NLHE);
  capabilities.add_supported_betting_structures(pv::BETTING_STRUCTURE_NO_LIMIT);
  capabilities.set_minimum_players(2);
  capabilities.set_maximum_players(10);
  capabilities.add_supported_streets(pv::STREET_PREFLOP);
  capabilities.add_supported_streets(pv::STREET_FLOP);
  capabilities.add_supported_streets(pv::STREET_TURN);
  capabilities.add_supported_streets(pv::STREET_RIVER);
  capabilities.add_supported_actions(pv::ACTION_TYPE_FOLD);
  capabilities.add_supported_actions(pv::ACTION_TYPE_CHECK);
  capabilities.add_supported_actions(pv::ACTION_TYPE_CALL);
  capabilities.add_supported_actions(pv::ACTION_TYPE_BET);
  capabilities.add_supported_actions(pv::ACTION_TYPE_RAISE);
  capabilities.set_amount_semantics(pv::AMOUNT_SEMANTICS_TARGET_TOTAL_INCLUSIVE);
  // Minor 0 accepts only engine-selected modes; forced river backends are
  // rejected by the validator. The exact LP/DCFR *feature* bits below still
  // report which internal backends the heuristic engine may use.
  capabilities.add_solver_modes(pv::SOLVER_MODE_AUTOMATIC);
  capabilities.add_solver_modes(pv::SOLVER_MODE_HEURISTIC);
  capabilities.add_strategy_profiles("tag");
  capabilities.add_strategy_profiles("lag");
  capabilities.add_strategy_profiles("station-hunter");
  capabilities.set_exact_lp(bs::gto::hasExactRiverLp() ? pv::FEATURE_SUPPORT_SUPPORTED
                                                       : pv::FEATURE_SUPPORT_EXPERIMENTAL);
  capabilities.set_dcfr(pv::FEATURE_SUPPORT_SUPPORTED);
  capabilities.set_multistreet(pv::FEATURE_SUPPORT_EXPERIMENTAL);
  capabilities.set_side_pots(pv::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities.set_rake(pv::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities.set_tournament_icm(pv::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities.set_maximum_request_bytes(kMaxFrameBytes);
  capabilities.set_maximum_solve_time_ms(120000);
  return capabilities;
}

}  // namespace bs::v1
