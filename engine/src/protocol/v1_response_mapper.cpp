#include <algorithm>
#include <array>
#include <bs/prng.hpp>
#include <bs/river_gto.hpp>
#include <bs/v1_protocol.hpp>
#include <cmath>
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

// Maps the poker-domain action kind to the wire enum.
pv::ActionType wireActionType(bs::poker::ActionType type) {
  using bs::poker::ActionType;
  switch (type) {
    case ActionType::Fold:
      return pv::ACTION_TYPE_FOLD;
    case ActionType::Check:
      return pv::ACTION_TYPE_CHECK;
    case ActionType::Call:
      return pv::ACTION_TYPE_CALL;
    case ActionType::Bet:
      return pv::ACTION_TYPE_BET;
    case ActionType::Raise:
      return pv::ACTION_TYPE_RAISE;
  }
  return pv::ACTION_TYPE_UNSPECIFIED;
}

// Fills the heuristic solver metadata exactly as the minor-0 path does; the
// heuristic keeps its REAL source on both minors.
void fillHeuristicMetadata(pv::SolverMetadata* metadata, const bs::Decision& decision) {
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
}

// Pinned protocol sampler: one domain-separated SplitMix64 draw, top 53 bits
// mapped to [0,1), first prefix-CDF bucket STRICTLY greater than the draw.
// Strict inequality makes the tie rule explicit (a bucket ending exactly on
// the draw goes to the next bucket) and zero-probability buckets, which do not
// grow the prefix, can never be selected.
//
// The CDF is self-consistent with the row as validated: the uniform point is
// scaled onto [0, recordedSum) using the LAST prefix sum the row actually
// accumulates, so a row summing to 1 +/- 1e-12 (accepted by
// blueprintRowIsLegal) always lands inside a bucket instead of running past
// the final prefix and becoming an internal error. For exactly normalized
// distributions recordedSum is 1, so every pinned seed->bucket vector is
// unchanged. Returns n only when the row carries no positive probability.
std::size_t sampleBucket(const double* probabilities, std::size_t size, std::uint64_t seed) {
  double recordedSum = 0.0;
  for (std::size_t i = 0; i < size; ++i)
    recordedSum += probabilities[i];
  if (!(recordedSum > 0.0) || !std::isfinite(recordedSum))
    return size;

  bs::SplitMix64 rng = bs::SplitMix64::protocolSampler(seed);
  const std::uint64_t draw = rng.next_u64();
  const double unit = static_cast<double>(draw >> 11) * 0x1.0p-53;  // [0,1)
  // Map onto [0, recordedSum); when recordedSum rounds to exactly 1 the
  // multiplication by 1 is exact and the pinned distribution is preserved.
  const double point = unit * recordedSum;

  double prefix = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    prefix += probabilities[i];
    if (prefix > point)
      return i;
  }
  // Defensive clamp for floating-point end effects at the final positive
  // bucket (a zero-probability tail is never selected by this branch because
  // its prefix does not grow).
  for (std::size_t i = size; i-- > 0;) {
    if (probabilities[i] > 0.0)
      return i;
  }
  return size;
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
  fillHeuristicMetadata(metadata, decision);

  return response;
}

pv::DecisionResponse mapHeuristicExpandedResponse(const pv::DecisionRequest& request,
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
  pv::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  pv::ActionPolicy* policy = expanded->add_actions();
  policy->set_type(type);
  if (hasTarget)
    policy->set_target_total(target);
  policy->set_all_in(false);
  policy->set_probability(1.0);

  // The minor-1 heuristic fallback keeps the same deterministic sampled
  // action convention as minor 0.
  if (request.options().include_sampled_action()) {
    pv::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    if (hasTarget)
      selected->set_target_total(target);
    selected->set_all_in(false);
  }

  fillHeuristicMetadata(expanded->mutable_solver(), decision);
  return response;
}

namespace {

// Locates the request legal entry of the given kind, returning nullptr when
// the kind is not offered.
const pv::LegalAction* findLegal(const pv::HandState& state, pv::ActionType type) {
  for (const pv::LegalAction& legal : state.legal_actions())
    if (legal.type() == type)
      return &legal;
  return nullptr;
}

}  // namespace

bool blueprintRowIsLegal(const pv::DecisionRequest& request, const V1BlueprintRow& row) {
  if (row.size < 1 || row.size > 32 || row.actions == nullptr || row.probabilities == nullptr)
    return false;
  double sum = 0.0;
  for (std::size_t i = 0; i < row.size; ++i) {
    const double probability = row.probabilities[i];
    if (!std::isfinite(probability) || probability < 0.0)
      return false;
    sum += probability;
    const pv::ActionType type = wireActionType(row.actions[i].type);
    const bool aggressive = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
    const std::uint64_t target = row.actions[i].target_total;
    if (!actionIsLegal(request.state(), type, target, aggressive))
      return false;
  }
  return std::isfinite(sum) && std::abs(sum - 1.0) <= 1e-12;
}

pv::DecisionResponse mapBlueprintExpandedResponse(const pv::DecisionRequest& request,
                                                  const V1BlueprintRow& row) {
  if (!blueprintRowIsLegal(request, row))
    throw MappingError(pv::ERROR_CODE_UNSUPPORTED_FEATURE,
                       "blueprint action is outside the requested legal action window", false);
  if (row.artifact_sha256.size() != 64)
    throw MappingError(pv::ERROR_CODE_INTERNAL, "resident answer lacks an artifact digest",
                       /*retryable=*/true);

  pv::DecisionResponse response;
  pv::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  const pv::PlayerState* hero = nullptr;
  for (const pv::PlayerState& player : request.state().players())
    if (player.player_id() == request.state().hero_player_id())
      hero = &player;
  if (hero == nullptr)
    throw MappingError(pv::ERROR_CODE_INTERNAL, "hero vanished while mapping blueprint",
                       /*retryable=*/true);
  const std::uint64_t heroCapacity = hero->stack() + hero->street_committed();

  for (std::size_t i = 0; i < row.size; ++i) {
    const bs::poker::Action& action = row.actions[i];
    const pv::ActionType type = wireActionType(action.type);
    pv::ActionPolicy* policy = expanded->add_actions();
    policy->set_type(type);
    const bool aggressive = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
    bool allIn = false;
    if (aggressive) {
      policy->set_target_total(action.target_total);
      const pv::LegalAction* legal = findLegal(request.state(), type);
      allIn = legal != nullptr && action.target_total == legal->max_target_total() &&
              action.target_total == heroCapacity;
    }
    policy->set_all_in(allIn);
    policy->set_probability(row.probabilities[i]);
  }

  // Deterministic sampled action: one domain-separated draw over the row.
  if (request.options().include_sampled_action()) {
    const std::size_t chosen = sampleBucket(row.probabilities, row.size, request.options().seed());
    if (chosen >= row.size || row.probabilities[chosen] <= 0.0)
      throw MappingError(pv::ERROR_CODE_INTERNAL, "blueprint sampler selected no live action",
                         /*retryable=*/true);
    const bs::poker::Action& action = row.actions[chosen];
    const pv::ActionType type = wireActionType(action.type);
    // Membership by kind AND exact target against BOTH the row (the sampled
    // bucket itself) and the request legal set.
    const bool aggressive = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
    if (!actionIsLegal(request.state(), type, action.target_total, aggressive))
      throw MappingError(pv::ERROR_CODE_INTERNAL,
                         "sampled blueprint action is outside the requested legal window",
                         /*retryable=*/true);
    pv::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    if (aggressive)
      selected->set_target_total(action.target_total);
    const pv::LegalAction* legal = findLegal(request.state(), type);
    selected->set_all_in(aggressive && legal != nullptr &&
                         action.target_total == legal->max_target_total() &&
                         action.target_total == heroCapacity);
  }

  pv::SolverMetadata* metadata = expanded->mutable_solver();
  metadata->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
  metadata->set_solve_time_us(0);
  metadata->set_cache_hit(true);
  metadata->set_reason_code("blueprint");
  metadata->set_artifact_sha256(std::string(row.artifact_sha256));
  // Artifact v1 never writes certification bounds; a complete validated
  // blueprint is therefore "uncertified". modeled_exact_bound is unreachable
  // until certification exists, and "baseline" is reserved for the future
  // resolving-deadline fallback.
  metadata->set_guarantee("uncertified");
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

pv::GetCapabilitiesResponse buildCapabilities(unsigned negotiated_minor,
                                              bool blueprint_advertised) {
  pv::GetCapabilitiesResponse capabilities;
  // The minor-0 advertisement is the frozen Stage-7 output: only minor 0 and
  // no new enum values. A minor-1 query re-advertises 0 plus 1.
  capabilities.add_supported_protocol_minors(0);
  if (negotiated_minor >= 1)
    capabilities.add_supported_protocol_minors(1);
  capabilities.set_engine_build_version(negotiated_minor >= 1 ? "bigshark-engine-v1.1.0"
                                                              : "bigshark-engine-v1.0.0");
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
  // Negotiated minor 1 advertises BLUEPRINT only when at least one resident
  // root was advertised at startup. RESOLVING and any certification feature
  // are deliberately never advertised.
  if (negotiated_minor >= 1 && blueprint_advertised)
    capabilities.add_solver_modes(pv::SOLVER_MODE_BLUEPRINT);
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
