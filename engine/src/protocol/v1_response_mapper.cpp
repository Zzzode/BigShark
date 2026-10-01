#include <algorithm>
#include <array>
#include <bs/guarantee.hpp>
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

// Fail-closed reading of a WIRE source (R3/R8): UNSPECIFIED and any future or
// out-of-range value map to operational_fallback rather than being assigned a
// stronger level by default. The host write path never uses this - its own
// levels derive from guaranteeFor() on the declared policy source, or from the
// two-arm resolve-outcome labeling; this is a boundary defense, table-tested
// directly. RESOLVING means certified here because a deadline baseline is
// emitted under the BLUEPRINT source on every minor.
// (Definition follows the anonymous namespace so the symbol is externally
// testable per its v1_mappers.hpp declaration.)
pv::SolverSource toWireSource(bs::DecisionSource source) {
  switch (source) {
    case bs::DecisionSource::PreflopChart:
      return pv::SOLVER_SOURCE_PREFLOP_CHART;
    case bs::DecisionSource::PostflopHeuristic:
      return pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC;
    case bs::DecisionSource::RiverLp:
      return pv::SOLVER_SOURCE_RIVER_LP;
    case bs::DecisionSource::RiverDcfr:
      return pv::SOLVER_SOURCE_RIVER_DCFR;
    case bs::DecisionSource::MultistreetCfr:
      return pv::SOLVER_SOURCE_MULTISTREET_CFR;
  }
  return pv::SOLVER_SOURCE_UNSPECIFIED;
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

// Fills the heuristic solver metadata. Minor 0/1 pass the source INFERRED FROM
// the reason text (frozen behavior, including the four chart folds pinned as
// postflop-heuristic); minor 2 passes the source the policy declared at the
// routing branch. Equity/MDF/diagnostic fields are identical on every minor.
void fillHeuristicMetadata(pv::SolverMetadata* metadata, const bs::Decision& decision,
                           pv::SolverSource source) {
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
  fillHeuristicMetadata(metadata, decision, solverSource(decision.reason));

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

  fillHeuristicMetadata(expanded->mutable_solver(), decision, solverSource(decision.reason));
  return response;
}

// RFC 0008 stage 5, minor 2: the heuristic answer keeps the source the POLICY
// declared at its routing branch (no reason-text inference) and always carries
// field 11, derived from the single normative table. Field 10 is never set.
pv::DecisionResponse mapGuaranteedHeuristicResponse(const pv::DecisionRequest& request,
                                                    const bs::SourcedDecision& answer) {
  const bs::Decision& decision = answer.decision;
  const pv::SolverSource source = toWireSource(answer.source);
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

  if (request.options().include_sampled_action()) {
    pv::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    if (hasTarget)
      selected->set_target_total(target);
    selected->set_all_in(false);
  }

  pv::SolverMetadata* metadata = expanded->mutable_solver();
  fillHeuristicMetadata(metadata, decision, source);
  metadata->set_guarantee_level(guaranteeToken(guaranteeFor(answer.source)));
  return response;
}

// RFC 0009 W3: the minor-2 AUTOMATIC operational fallback. A resident
// blueprint miss no longer falls through to the chart+heuristic cascade; it
// ends at this declared fallback (check, else call, else fold, chosen from the
// supplied legal set only). The source is UNSPECIFIED and the level is
// operational_fallback, so a journal reader can never mistake the fallback for
// a strategy answer. Field 11 only; field 10 is never set. Legal membership is
// validated exactly as on the heuristic path.
pv::DecisionResponse mapOperationalFallbackResponse(const pv::DecisionRequest& request,
                                                    const bs::Decision& decision) {
  const pv::ActionType type = actionType(decision.action);
  if (type == pv::ACTION_TYPE_UNSPECIFIED || type == pv::ACTION_TYPE_BET ||
      type == pv::ACTION_TYPE_RAISE)
    throw MappingError(pv::ERROR_CODE_INTERNAL,
                       "operational fallback produced a bet/raise or unrecognized action",
                       /*retryable=*/true);

  if (!actionIsLegal(request.state(), type, 0, /*hasTarget=*/false))
    throw MappingError(pv::ERROR_CODE_INTERNAL,
                       "operational fallback action is not a member of the requested legal actions",
                       /*retryable=*/true);

  pv::DecisionResponse response;
  pv::ExpandedStrategy* expanded = response.mutable_expanded_strategy();

  pv::ActionPolicy* policy = expanded->add_actions();
  policy->set_type(type);
  policy->set_all_in(false);
  policy->set_probability(1.0);

  if (request.options().include_sampled_action()) {
    pv::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    selected->set_all_in(false);
  }

  pv::SolverMetadata* metadata = expanded->mutable_solver();
  metadata->set_source(pv::SOLVER_SOURCE_UNSPECIFIED);
  metadata->set_solve_time_us(0);
  metadata->set_cache_hit(false);
  metadata->set_reason_code("operational-fallback");
  metadata->set_diagnostic_reason("operational-fallback");
  metadata->set_guarantee_level(guaranteeToken(bs::Guarantee::OperationalFallback));
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

bs::Guarantee failClosedGuaranteeForWireSource(pv::SolverSource source) {
  switch (source) {
    case pv::SOLVER_SOURCE_PREFLOP_CHART:
    case pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC:
    case pv::SOLVER_SOURCE_RIVER_LP:
    case pv::SOLVER_SOURCE_RIVER_DCFR:
    case pv::SOLVER_SOURCE_MULTISTREET_CFR:
    case pv::SOLVER_SOURCE_BLUEPRINT:
      return bs::Guarantee::Approximate;
    case pv::SOLVER_SOURCE_RESOLVING:
      return bs::Guarantee::CertifiedBound;
    default:
      return bs::Guarantee::OperationalFallback;
  }
}

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

// RFC 0008 stage 5 (R9/R15): mapper-independent completeness preconditions for
// a storage row, run by the minor-2 dispatcher BEFORE the guarantee-floor
// comparison and again inside the mapper. An un-mappable row must surface as
// its real error (coverage/INTERNAL), never as code 9. The checks are exactly
// those the mapper enforces when serializing: row legality, a 64-character
// artifact digest, and sampled-action liveness plus legal membership when a
// sample is requested.
void verifyStorageRowComplete(const pv::DecisionRequest& request, const V1BlueprintRow& row) {
  if (!blueprintRowIsLegal(request, row))
    throw MappingError(pv::ERROR_CODE_UNSUPPORTED_FEATURE,
                       "blueprint action is outside the requested legal action window", false);
  if (row.artifact_sha256.size() != 64)
    throw MappingError(pv::ERROR_CODE_INTERNAL, "resident answer lacks an artifact digest",
                       /*retryable=*/true);
  if (request.options().include_sampled_action()) {
    const std::size_t chosen = sampleBucket(row.probabilities, row.size, request.options().seed());
    if (chosen >= row.size || row.probabilities[chosen] <= 0.0)
      throw MappingError(pv::ERROR_CODE_INTERNAL, "blueprint sampler selected no live action",
                         /*retryable=*/true);
    const bs::poker::Action& action = row.actions[chosen];
    const pv::ActionType type = wireActionType(action.type);
    const bool aggressive = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
    if (!actionIsLegal(request.state(), type, action.target_total, aggressive))
      throw MappingError(pv::ERROR_CODE_INTERNAL,
                         "sampled blueprint action is outside the requested legal window",
                         /*retryable=*/true);
  }
}

// Heuristic counterpart of the row verifier (R15): the answer is complete only
// when the mapper could actually serialize it. Run before the floor comparison
// so an engine-produced illegal action stays INTERNAL instead of becoming a
// floor refusal.
void verifyHeuristicAnswerComplete(const pv::DecisionRequest& request,
                                   const bs::SourcedDecision& answer) {
  const bs::Decision& decision = answer.decision;
  const pv::ActionType type = actionType(decision.action);
  if (type == pv::ACTION_TYPE_UNSPECIFIED || decision.amount < 0)
    throw MappingError(pv::ERROR_CODE_INTERNAL, "engine produced an unrecognized action",
                       /*retryable=*/true);
  const bool hasTarget = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
  if (!actionIsLegal(request.state(), type, static_cast<std::uint64_t>(decision.amount), hasTarget))
    throw MappingError(pv::ERROR_CODE_INTERNAL,
                       "engine action is not a member of the requested legal actions",
                       /*retryable=*/true);
}

pv::DecisionResponse mapExpandedResponseImpl(const pv::DecisionRequest& request,
                                             const V1BlueprintRow& row, pv::SolverSource source,
                                             std::string_view minor1_guarantee,
                                             bs::Guarantee minor2_level, bool use_field_11) {
  verifyStorageRowComplete(request, row);

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
    const bs::poker::Action& action = row.actions[chosen];
    const pv::ActionType type = wireActionType(action.type);
    pv::SelectedAction* selected = expanded->mutable_selected_action();
    selected->set_type(type);
    const bool aggressive = type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE;
    if (aggressive)
      selected->set_target_total(action.target_total);
    const pv::LegalAction* legal = findLegal(request.state(), type);
    selected->set_all_in(aggressive && legal != nullptr &&
                         action.target_total == legal->max_target_total() &&
                         action.target_total == heroCapacity);
  }

  pv::SolverMetadata* metadata = expanded->mutable_solver();
  metadata->set_source(source);
  metadata->set_solve_time_us(0);
  metadata->set_cache_hit(source == pv::SOLVER_SOURCE_BLUEPRINT);
  metadata->set_reason_code(source == pv::SOLVER_SOURCE_RESOLVING ? "resolving" : "blueprint");
  metadata->set_artifact_sha256(std::string(row.artifact_sha256));
  // Per-minor disjoint vocabularies, a construction invariant: minor 1 writes
  // field 10 and never field 11; minor 2 writes field 11 (level token sourced
  // ONLY from guaranteeToken) and never field 10.
  if (use_field_11)
    metadata->set_guarantee_level(guaranteeToken(minor2_level));
  else
    metadata->set_guarantee(std::string(minor1_guarantee));
  // A multiway (3-seat) certified row carries a diagnostic token identifying
  // its per-seat unilateral non-regression semantics; a two-seat row leaves
  // the field unset so the frozen two-seat wire bytes are unchanged.
  if (!row.diagnostic.empty())
    metadata->set_diagnostic_reason(std::string(row.diagnostic));
  return response;
}

pv::DecisionResponse mapBlueprintExpandedResponse(const pv::DecisionRequest& request,
                                                  const V1BlueprintRow& row) {
  // Artifact v1 blueprint rows carry no certification bounds by construction;
  // the guarantee is therefore "uncertified".
  return mapExpandedResponseImpl(request, row, pv::SOLVER_SOURCE_BLUEPRINT, "uncertified",
                                 bs::Guarantee::Approximate, /*use_field_11=*/false);
}

// RFC 0008 stage 5 minor-2 storage mappers. The level is a TWO-ARM function of
// the lookup outcome (R8): a certified resolve is certified_bound; blueprint
// hits and deadline baselines are approximate. Every token, including the
// approximate arms (R14), comes from guaranteeToken - no string literal here.
pv::DecisionResponse mapGuaranteedBlueprintResponse(const pv::DecisionRequest& request,
                                                    const V1BlueprintRow& row) {
  return mapExpandedResponseImpl(request, row, pv::SOLVER_SOURCE_BLUEPRINT, std::string_view{},
                                 bs::Guarantee::Approximate,
                                 /*use_field_11=*/true);
}

pv::DecisionResponse mapGuaranteedDeadlineResponse(const pv::DecisionRequest& request,
                                                   const V1BlueprintRow& row) {
  // The deadline baseline is emitted under the BLUEPRINT source on every minor.
  return mapExpandedResponseImpl(request, row, pv::SOLVER_SOURCE_BLUEPRINT, std::string_view{},
                                 bs::Guarantee::Approximate,
                                 /*use_field_11=*/true);
}

pv::DecisionResponse mapGuaranteedCertifiedResponse(const pv::DecisionRequest& request,
                                                    const V1BlueprintRow& row) {
  return mapExpandedResponseImpl(request, row, pv::SOLVER_SOURCE_RESOLVING, std::string_view{},
                                 bs::Guarantee::CertifiedBound,
                                 /*use_field_11=*/true);
}

pv::DecisionResponse mapResolvedExpandedResponse(const pv::DecisionRequest& request,
                                                 const V1BlueprintRow& row, pv::SolverSource source,
                                                 std::string_view guarantee) {
  // modeled_exact_bound may be emitted only with the RESOLVING source; the
  // deadline baseline must be tagged BLUEPRINT/baseline. Enforce the discipline
  // at the boundary so the exact-bound string is unreachable any other way.
  if (guarantee == "modeled_exact_bound") {
    if (source != pv::SOLVER_SOURCE_RESOLVING)
      throw MappingError(pv::ERROR_CODE_INTERNAL,
                         "modeled_exact_bound requires the resolving source", /*retryable=*/true);
  } else if (guarantee == "baseline") {
    if (source != pv::SOLVER_SOURCE_BLUEPRINT)
      throw MappingError(pv::ERROR_CODE_INTERNAL, "baseline requires the blueprint source",
                         /*retryable=*/true);
  } else {
    throw MappingError(pv::ERROR_CODE_INTERNAL, "unsupported resolving guarantee label",
                       /*retryable=*/true);
  }
  return mapExpandedResponseImpl(request, row, source, guarantee, bs::Guarantee::Approximate,
                                 /*use_field_11=*/false);
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

pv::GetCapabilitiesResponse buildCapabilities(unsigned negotiated_minor, bool blueprint_advertised,
                                              bool resolving_advertised) {
  pv::GetCapabilitiesResponse capabilities;
  // The minor-0 advertisement is the frozen Stage-7 output: only minor 0 and
  // no new enum values. A minor-1 query re-advertises 0 plus 1.
  capabilities.add_supported_protocol_minors(0);
  if (negotiated_minor >= 1)
    capabilities.add_supported_protocol_minors(1);
  if (negotiated_minor >= 2)
    capabilities.add_supported_protocol_minors(2);
  capabilities.set_engine_build_version(negotiated_minor >= 2   ? "bigshark-engine-v1.2.0"
                                        : negotiated_minor >= 1 ? "bigshark-engine-v1.1.0"
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
  // root was advertised at startup. SOLVER_MODE_RESOLVING is advertised only
  // when the host also has a live resolver plus an advertised terminal-only
  // root; minor 0 never emits either new enum value.
  if (negotiated_minor >= 1 && blueprint_advertised)
    capabilities.add_solver_modes(pv::SOLVER_MODE_BLUEPRINT);
  if (negotiated_minor >= 1 && resolving_advertised)
    capabilities.add_solver_modes(pv::SOLVER_MODE_RESOLVING);
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
