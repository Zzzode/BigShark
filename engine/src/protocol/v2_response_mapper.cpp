// v2_response_mapper.cpp — RFC 0022 v2 native -> wire response mapping.
//
// Maps native decision and lookup results into v2 wire responses: heuristic
// decisions, blueprint rows, resolved rows, and operational fallbacks. Builds
// the v2 capability advertisement with CapabilityTuple messages.
//
// v2-specific differences from v1:
// - All responses use ExpandedStrategy (not Strategy)
// - SolverMetadata carries utility_profile (field 11, always CASH_CHIP)
// - guarantee_level is field 10 (string token from guaranteeToken)
// - Hero lookup by seat (not player_id)
// - LegalAction uses min_target_total/max_target_total
#include <algorithm>
#include <array>
#include <bs/guarantee.hpp>
#include <bs/prng.hpp>
#include <bs/v2_protocol.hpp>
#include <cmath>
#include <string>

#include "v2_mappers.hpp"

namespace bs::v2 {
namespace {

namespace pv2 = ::bigshark::engine::v2;

// Maps the poker-domain action kind to the wire enum.
pv2::ActionType wireActionType(bs::poker::ActionType type) {
  using bs::poker::ActionType;
  switch (type) {
    case ActionType::Fold:
      return pv2::ACTION_TYPE_FOLD;
    case ActionType::Check:
      return pv2::ACTION_TYPE_CHECK;
    case ActionType::Call:
      return pv2::ACTION_TYPE_CALL;
    case ActionType::Bet:
      return pv2::ACTION_TYPE_BET;
    case ActionType::Raise:
      return pv2::ACTION_TYPE_RAISE;
  }
  return pv2::ACTION_TYPE_UNSPECIFIED;
}

// Maps bs::DecisionSource to the v2 wire SolverSource.
pv2::SolverSource toWireSource(bs::DecisionSource source) {
  switch (source) {
    case bs::DecisionSource::PreflopChart:
      return pv2::SOLVER_SOURCE_PREFLOP_CHART;
    case bs::DecisionSource::PostflopHeuristic:
      return pv2::SOLVER_SOURCE_POSTFLOP_HEURISTIC;
    case bs::DecisionSource::RiverLp:
      return pv2::SOLVER_SOURCE_RIVER_LP;
    case bs::DecisionSource::RiverDcfr:
      return pv2::SOLVER_SOURCE_RIVER_DCFR;
    case bs::DecisionSource::MultistreetCfr:
      return pv2::SOLVER_SOURCE_MULTISTREET_CFR;
  }
  return pv2::SOLVER_SOURCE_UNSPECIFIED;
}

const char* sourceCode(pv2::SolverSource source) {
  switch (source) {
    case pv2::SOLVER_SOURCE_PREFLOP_CHART:
      return "preflop-chart";
    case pv2::SOLVER_SOURCE_RIVER_LP:
      return "river-lp";
    case pv2::SOLVER_SOURCE_RIVER_DCFR:
      return "river-dcfr";
    case pv2::SOLVER_SOURCE_MULTISTREET_CFR:
      return "multistreet-cfr";
    case pv2::SOLVER_SOURCE_BLUEPRINT:
      return "blueprint";
    case pv2::SOLVER_SOURCE_RESOLVING:
      return "resolving";
    default:
      return "postflop-heuristic";
  }
}

// Verifies the engine action against the platform-provided legal set by kind
// and, for bets/raises, by exact inclusive target total.
bool actionIsLegal(const pv2::HandState& state, pv2::ActionType type, std::uint64_t target,
                   bool hasTarget) {
  for (const pv2::LegalAction& legal : state.legal_actions()) {
    if (legal.type() != type)
      continue;
    if (type != pv2::ACTION_TYPE_BET && type != pv2::ACTION_TYPE_RAISE)
      return !hasTarget;
    if (!hasTarget || !legal.has_min_target_total() || !legal.has_max_target_total())
      return false;
    return target >= legal.min_target_total() && target <= legal.max_target_total();
  }
  return false;
}

// Locates the request legal entry of the given kind, returning nullptr when
// the kind is not offered.
const pv2::LegalAction* findLegal(const pv2::HandState& state, pv2::ActionType type) {
  for (const pv2::LegalAction& legal : state.legal_actions())
    if (legal.type() == type)
      return &legal;
  return nullptr;
}

// Normalizes the engine's Bet/Raise distinction to the platform's aggressive
// vocabulary on preflop. River uses RAISE for all preflop aggression; the
// engine types it as Bet when due == 0 (e.g. BB facing a limp). Returns the
// wire type unchanged on postflop streets or when no aggressive legal action
// is present.
pv2::ActionType normalizeAggressiveType(const pv2::HandState& state, pv2::ActionType type) {
  if (state.street() != pv2::STREET_PREFLOP)
    return type;
  if (type != pv2::ACTION_TYPE_BET && type != pv2::ACTION_TYPE_RAISE)
    return type;
  for (const pv2::LegalAction& legal : state.legal_actions()) {
    if (legal.type() == pv2::ACTION_TYPE_BET || legal.type() == pv2::ACTION_TYPE_RAISE)
      return legal.type();
  }
  return type;
}

// Pinned protocol sampler: one domain-separated SplitMix64 draw, top 53 bits
// mapped to [0,1), first prefix-CDF bucket STRICTLY greater than the draw.
// Uses the v2 domain constant "BSV2-sam" so v1 pinned sampler bytes are
// untouched (prng.hpp's protocolSampler carries the v1 "BSV1-sam" domain).
std::size_t sampleBucket(const double* probabilities, std::size_t size, std::uint64_t seed) {
  double recordedSum = 0.0;
  for (std::size_t i = 0; i < size; ++i)
    recordedSum += probabilities[i];
  if (!(recordedSum > 0.0) || !std::isfinite(recordedSum))
    return size;

  constexpr std::uint64_t kV2SamplerDomain = 0x425356322d73616dULL;  // "BSV2-sam"
  bs::SplitMix64 rng(seed ^ kV2SamplerDomain);
  const std::uint64_t draw = rng.next_u64();
  const double unit = static_cast<double>(draw >> 11) * 0x1.0p-53;  // [0,1)
  const double point = unit * recordedSum;

  double prefix = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    prefix += probabilities[i];
    if (prefix > point)
      return i;
  }
  // Defensive clamp for floating-point end effects.
  for (std::size_t i = size; i-- > 0;) {
    if (probabilities[i] > 0.0)
      return i;
  }
  return size;
}

// Finds the hero player by seat, returning nullptr when not found.
const pv2::PlayerState* findHero(const pv2::HandState& state) {
  for (const pv2::PlayerState& player : state.players())
    if (player.seat() == state.hero_seat())
      return &player;
  return nullptr;
}

// Fills the heuristic solver metadata with v2 fields.
void fillHeuristicMetadata(pv2::SolverMetadata* metadata, const bs::Decision& decision,
                           pv2::SolverSource source) {
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
  metadata->set_utility_profile(pv2::UTILITY_PROFILE_CASH_CHIP);
}

// Core row serializer shared by blueprint and resolved responses.
// Every row action becomes one ActionPolicy; the sampled action is selected
// by the pinned SplitMix64 convention when requested.
pv2::DecisionResponse mapExpandedResponseImpl(const pv2::DecisionRequest& request,
                                              const V1BlueprintRow& row, pv2::SolverSource source,
                                              bs::Guarantee level) {
  if (!blueprintRowIsLegal(request, row))
    throw MappingError(pv2::ERROR_CODE_UNSUPPORTED_FEATURE,
                       "blueprint action is outside the requested legal action window", false);
  if (row.artifact_sha256.size() != 64)
    throw MappingError(pv2::ERROR_CODE_INTERNAL, "resident answer lacks an artifact digest",
                       /*retryable=*/true);

  pv2::DecisionResponse response;
  pv2::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  const pv2::PlayerState* hero = findHero(request.state());
  if (hero == nullptr)
    throw MappingError(pv2::ERROR_CODE_INTERNAL, "hero vanished while mapping blueprint",
                       /*retryable=*/true);
  const std::uint64_t heroCapacity = hero->stack() + hero->street_committed();

  for (std::size_t i = 0; i < row.size; ++i) {
    const bs::poker::Action& action = row.actions[i];
    const pv2::ActionType type =
        normalizeAggressiveType(request.state(), wireActionType(action.type));
    pv2::ActionPolicy* policy = expanded->add_actions();
    policy->set_type(type);
    const bool aggressive = type == pv2::ACTION_TYPE_BET || type == pv2::ACTION_TYPE_RAISE;
    bool allIn = false;
    if (aggressive) {
      policy->set_target_total(action.target_total);
      const pv2::LegalAction* legal = findLegal(request.state(), type);
      allIn = legal != nullptr && legal->has_max_target_total() &&
              action.target_total == legal->max_target_total() &&
              action.target_total == heroCapacity;
    }
    policy->set_all_in(allIn);
    policy->set_probability(row.probabilities[i]);
  }

  // Deterministic sampled action: one domain-separated draw over the row.
  if (request.options().include_sampled_action()) {
    const std::size_t chosen = sampleBucket(row.probabilities, row.size, request.options().seed());
    if (chosen >= row.size || row.probabilities[chosen] <= 0.0)
      throw MappingError(pv2::ERROR_CODE_INTERNAL, "blueprint sampler selected no live action",
                         /*retryable=*/true);
    const bs::poker::Action& action = row.actions[chosen];
    const pv2::ActionType type =
        normalizeAggressiveType(request.state(), wireActionType(action.type));
    pv2::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    const bool aggressive = type == pv2::ACTION_TYPE_BET || type == pv2::ACTION_TYPE_RAISE;
    if (aggressive)
      selected->set_target_total(action.target_total);
    const pv2::LegalAction* legal = findLegal(request.state(), type);
    selected->set_all_in(aggressive && legal != nullptr && legal->has_max_target_total() &&
                         action.target_total == legal->max_target_total() &&
                         action.target_total == heroCapacity);
  }

  pv2::SolverMetadata* metadata = expanded->mutable_solver();
  metadata->set_source(source);
  metadata->set_solve_time_us(0);
  metadata->set_cache_hit(source == pv2::SOLVER_SOURCE_BLUEPRINT);
  metadata->set_reason_code(sourceCode(source));
  metadata->set_artifact_sha256(std::string(row.artifact_sha256));
  metadata->set_guarantee_level(guaranteeToken(level));
  metadata->set_utility_profile(pv2::UTILITY_PROFILE_CASH_CHIP);
  if (!row.diagnostic.empty())
    metadata->set_diagnostic_reason(std::string(row.diagnostic));
  return response;
}

}  // namespace

pv2::DecisionResponse mapHeuristicResponse(const pv2::DecisionRequest& request,
                                           const bs::SourcedDecision& answer) {
  const bs::Decision& decision = answer.decision;
  const pv2::SolverSource source = toWireSource(answer.source);

  // Map the string action name to the wire enum.
  pv2::ActionType type = pv2::ACTION_TYPE_UNSPECIFIED;
  if (decision.action == "fold")
    type = pv2::ACTION_TYPE_FOLD;
  else if (decision.action == "check")
    type = pv2::ACTION_TYPE_CHECK;
  else if (decision.action == "call")
    type = pv2::ACTION_TYPE_CALL;
  else if (decision.action == "bet")
    type = pv2::ACTION_TYPE_BET;
  else if (decision.action == "raise")
    type = pv2::ACTION_TYPE_RAISE;

  if (type == pv2::ACTION_TYPE_UNSPECIFIED || decision.amount < 0)
    throw MappingError(pv2::ERROR_CODE_INTERNAL, "engine produced an unrecognized action",
                       /*retryable=*/true);

  const bool hasTarget = type == pv2::ACTION_TYPE_BET || type == pv2::ACTION_TYPE_RAISE;
  const std::uint64_t target = static_cast<std::uint64_t>(decision.amount);
  if (!actionIsLegal(request.state(), type, target, hasTarget))
    throw MappingError(pv2::ERROR_CODE_INTERNAL,
                       "engine action is not a member of the requested legal actions",
                       /*retryable=*/true);

  pv2::DecisionResponse response;
  pv2::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  pv2::ActionPolicy* policy = expanded->add_actions();
  policy->set_type(type);
  if (hasTarget)
    policy->set_target_total(target);
  policy->set_all_in(false);
  policy->set_probability(1.0);

  if (request.options().include_sampled_action()) {
    pv2::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    if (hasTarget)
      selected->set_target_total(target);
    selected->set_all_in(false);
  }

  pv2::SolverMetadata* metadata = expanded->mutable_solver();
  fillHeuristicMetadata(metadata, decision, source);
  metadata->set_guarantee_level(guaranteeToken(guaranteeFor(answer.source)));
  return response;
}

pv2::DecisionResponse mapBlueprintResponse(const pv2::DecisionRequest& request,
                                           const V1BlueprintRow& row) {
  return mapExpandedResponseImpl(request, row, pv2::SOLVER_SOURCE_BLUEPRINT,
                                 bs::Guarantee::Approximate);
}

pv2::DecisionResponse mapResolvedResponse(const pv2::DecisionRequest& request,
                                          const V1BlueprintRow& row, V1ResolveOutcome outcome) {
  // A certified resolve earns certified_bound; deadline baselines earn
  // approximate. The source is RESOLVING for certified, BLUEPRINT for
  // deadline baselines.
  const bool certified = outcome == V1ResolveOutcome::Certified;
  return mapExpandedResponseImpl(
      request, row, certified ? pv2::SOLVER_SOURCE_RESOLVING : pv2::SOLVER_SOURCE_BLUEPRINT,
      certified ? bs::Guarantee::CertifiedBound : bs::Guarantee::Approximate);
}

pv2::DecisionResponse mapOperationalFallbackResponse(const pv2::DecisionRequest& request,
                                                     const bs::Decision& decision) {
  pv2::ActionType type = pv2::ACTION_TYPE_UNSPECIFIED;
  if (decision.action == "fold")
    type = pv2::ACTION_TYPE_FOLD;
  else if (decision.action == "check")
    type = pv2::ACTION_TYPE_CHECK;
  else if (decision.action == "call")
    type = pv2::ACTION_TYPE_CALL;

  if (type == pv2::ACTION_TYPE_UNSPECIFIED)
    throw MappingError(pv2::ERROR_CODE_INTERNAL,
                       "operational fallback produced a bet/raise or unrecognized action",
                       /*retryable=*/true);

  if (!actionIsLegal(request.state(), type, 0, /*hasTarget=*/false))
    throw MappingError(pv2::ERROR_CODE_INTERNAL,
                       "operational fallback action is not a member of the requested legal actions",
                       /*retryable=*/true);

  pv2::DecisionResponse response;
  pv2::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  pv2::ActionPolicy* policy = expanded->add_actions();
  policy->set_type(type);
  policy->set_all_in(false);
  policy->set_probability(1.0);

  if (request.options().include_sampled_action()) {
    pv2::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    selected->set_all_in(false);
  }

  pv2::SolverMetadata* metadata = expanded->mutable_solver();
  metadata->set_source(pv2::SOLVER_SOURCE_UNSPECIFIED);
  metadata->set_solve_time_us(0);
  metadata->set_cache_hit(false);
  metadata->set_reason_code("operational-fallback");
  metadata->set_diagnostic_reason("operational-fallback");
  metadata->set_guarantee_level(guaranteeToken(bs::Guarantee::OperationalFallback));
  metadata->set_utility_profile(pv2::UTILITY_PROFILE_CASH_CHIP);
  return response;
}

bool blueprintRowIsLegal(const pv2::DecisionRequest& request, const V1BlueprintRow& row) {
  if (row.size < 1 || row.size > 32 || row.actions == nullptr || row.probabilities == nullptr)
    return false;
  double sum = 0.0;
  for (std::size_t i = 0; i < row.size; ++i) {
    const double probability = row.probabilities[i];
    if (!std::isfinite(probability) || probability < 0.0)
      return false;
    sum += probability;
    const pv2::ActionType type =
        normalizeAggressiveType(request.state(), wireActionType(row.actions[i].type));
    const bool aggressive = type == pv2::ACTION_TYPE_BET || type == pv2::ACTION_TYPE_RAISE;
    const std::uint64_t target = row.actions[i].target_total;
    if (!actionIsLegal(request.state(), type, target, aggressive))
      return false;
  }
  return std::isfinite(sum) && std::abs(sum - 1.0) <= 1e-12;
}

pv2::DecisionResponse errorResponse(pv2::ErrorCode code, const std::string& message, bool retryable,
                                    std::vector<pv2::FieldViolation> violations) {
  pv2::DecisionResponse response;
  pv2::EngineError* error = response.mutable_error();
  error->set_code(code);
  std::string safe = message;
  if (safe.empty())
    safe = "engine request failed";
  if (safe.size() > 512)
    safe.resize(512);
  error->set_message(safe);
  error->set_retryable(retryable);
  // Bound the violation list so a malformed request cannot produce an
  // oversized error envelope. The final entry marks truncation.
  if (violations.size() > static_cast<std::size_t>(kMaxFieldViolations)) {
    violations.resize(static_cast<std::size_t>(kMaxFieldViolations) - 1);
    pv2::FieldViolation marker;
    marker.set_field_path("");
    marker.set_description("additional field violations were omitted");
    violations.push_back(std::move(marker));
  }
  for (const pv2::FieldViolation& violation : violations)
    *error->add_violations() = violation;
  return response;
}

pv2::GetCapabilitiesResponse buildCapabilities(bool blueprint_advertised,
                                               bool resolving_advertised) {
  pv2::GetCapabilitiesResponse capabilities;
  capabilities.add_supported_protocol_versions(3);
  capabilities.set_engine_build_version("bigshark-engine-v2.0.0");
  capabilities.set_maximum_request_bytes(bs::v1::kMaxFrameBytes);
  capabilities.set_maximum_solve_time_ms(120000);

  // Helper to emit one hu-r1 capability tuple for a given solver mode.
  const auto emitHuR1 = [&](pv2::SolverMode mode, pv2::GuaranteeLevel guarantee) {
    pv2::CapabilityTuple* tuple = capabilities.add_capabilities();
    tuple->set_min_dealt_players(2);
    tuple->set_max_dealt_players(2);
    tuple->set_min_live_players(2);
    tuple->set_max_live_players(2);
    tuple->add_supported_streets(pv2::STREET_PREFLOP);
    tuple->add_supported_streets(pv2::STREET_FLOP);
    tuple->add_supported_streets(pv2::STREET_TURN);
    tuple->add_supported_streets(pv2::STREET_RIVER);
    tuple->set_max_stack_bb(100);
    tuple->set_side_pots(false);
    pv2::RuleProfile* profile = tuple->mutable_rule_profile();
    profile->set_settlement(pv2::SETTLEMENT_RULE_CLOCKWISE_LEFT_OF_BUTTON);
    profile->set_reopening(pv2::REOPENING_RULE_NEVER_CUMULATIVE);
    profile->set_runout(pv2::RUNOUT_RULE_SINGLE);
    profile->set_dealt_seat_count(2);
    tuple->set_utility_profile(pv2::UTILITY_PROFILE_CASH_CHIP);
    tuple->set_required_coverage(pv2::OBSERVATION_COVERAGE_FULL);
    tuple->set_solver_mode(mode);
    tuple->set_max_guarantee(guarantee);
    tuple->set_quality_scope("heads-up-r1");
  };

  // Always available: AUTOMATIC and HEURISTIC.
  emitHuR1(pv2::SOLVER_MODE_AUTOMATIC, pv2::GUARANTEE_LEVEL_APPROXIMATE);
  emitHuR1(pv2::SOLVER_MODE_HEURISTIC, pv2::GUARANTEE_LEVEL_APPROXIMATE);
  // Conditionally advertised: BLUEPRINT and RESOLVING.
  if (blueprint_advertised)
    emitHuR1(pv2::SOLVER_MODE_BLUEPRINT, pv2::GUARANTEE_LEVEL_APPROXIMATE);
  if (resolving_advertised)
    emitHuR1(pv2::SOLVER_MODE_RESOLVING, pv2::GUARANTEE_LEVEL_CERTIFIED_BOUND);

  return capabilities;
}

}  // namespace bs::v2
