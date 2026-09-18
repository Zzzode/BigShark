#include <algorithm>
#include <bs/service.hpp>
#include <bs/v1_protocol.hpp>
#include <cstdint>
#include <exception>
#include <new>
#include <string>

#include "v1_mappers.hpp"
#include "v1_resident_mapper.hpp"

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

constexpr std::size_t kMaxRequestIdBytes = 128;

EnvelopeResult respondWithDecision(const std::string& requestId,
                                   const pv::DecisionResponse& response,
                                   std::uint32_t negotiated_minor) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv::Envelope envelope;
  envelope.set_protocol_minor(negotiated_minor);
  envelope.set_request_id(requestId);
  *envelope.mutable_decision_response() = response;
  result.response = envelope.SerializeAsString();
  return result;
}

EnvelopeResult respondWithCapabilities(const std::string& requestId,
                                       const pv::GetCapabilitiesResponse& response,
                                       std::uint32_t negotiated_minor) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv::Envelope envelope;
  envelope.set_protocol_minor(negotiated_minor);
  envelope.set_request_id(requestId);
  *envelope.mutable_get_capabilities_response() = response;
  result.response = envelope.SerializeAsString();
  return result;
}

bool retryableFor(pv::ErrorCode code) {
  switch (code) {
    case pv::ERROR_CODE_NO_DECISION:
    case pv::ERROR_CODE_DEADLINE_EXCEEDED:
    case pv::ERROR_CODE_RESOURCE_EXHAUSTED:
    case pv::ERROR_CODE_INTERNAL:
      return true;
    default:
      return false;
  }
}

const char* messageFor(pv::ErrorCode code) {
  switch (code) {
    case pv::ERROR_CODE_UNSUPPORTED_PROTOCOL:
      return "unsupported protocol minor";
    case pv::ERROR_CODE_UNSUPPORTED_GAME:
      return "unsupported game";
    case pv::ERROR_CODE_UNSUPPORTED_FEATURE:
      return "unsupported feature";
    case pv::ERROR_CODE_NO_DECISION:
      return "no decision available";
    case pv::ERROR_CODE_DEADLINE_EXCEEDED:
      return "decision deadline exceeded";
    case pv::ERROR_CODE_RESOURCE_EXHAUSTED:
      return "engine resources exhausted";
    case pv::ERROR_CODE_INTERNAL:
      return "internal engine error";
    default:
      return "invalid request";
  }
}

// Shared decision-error envelope for the coverage misses unique to minor 1.
EnvelopeResult blueprintMissResponse(const std::string& requestId, const std::string& detail) {
  std::string message = "blueprint coverage unavailable";
  if (!detail.empty()) {
    message += ": ";
    message += detail;
  }
  return respondWithDecision(
      requestId, errorResponse(pv::ERROR_CODE_UNSUPPORTED_FEATURE, message, /*retryable=*/false),
      1);
}

// Runs one resident lookup end to end. Returns true only when the
// reconstructed state has a legal, fully mappable blueprint row.
bool tryBlueprint(const V1HostServices& services, const pv::DecisionRequest& request,
                  pv::DecisionResponse& response, std::string& miss_detail) {
  if (!services.blueprintAdvertised()) {
    miss_detail = "no resident blueprint root advertised";
    return false;
  }
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss reconstruct_miss = V1BlueprintMiss::None;
  if (!reconstructPostflop(request, reconstructed, reconstruct_miss)) {
    miss_detail = to_string(reconstruct_miss);
    return false;
  }
  const V1BlueprintResult answer = services.blueprintHeroDecision(
      *reconstructed.state, reconstructed.hero_cards, std::string_view{});
  if (!answer.hit) {
    miss_detail = to_string(answer.miss);
    return false;
  }
  // An abstract row action outside the client legal window is a coverage
  // miss, never a clamp.
  if (!blueprintRowIsLegal(request, answer.row)) {
    miss_detail = "off-tree-amount";
    return false;
  }
  response = mapBlueprintExpandedResponse(request, answer.row);
  return true;
}

// Runs one RFC 0005 Stage 9 terminal-only resolve end to end. Returns the
// response classification; the caller maps it to an expanded strategy or an
// error. Reconstruction admits a facing-all-in node; the BLUEPRINT gate is
// untouched.
bool tryResolving(const V1HostServices& services, const pv::DecisionRequest& request,
                  const std::uint32_t deadline_ms, pv::DecisionResponse& response,
                  V1ResolveOutcome& outcome, std::string& miss_detail) {
  if (!services.resolvingAdvertised()) {
    miss_detail = "no resolving root advertised";
    outcome = V1ResolveOutcome::Unsupported;
    return false;
  }
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss reconstruct_miss = V1BlueprintMiss::None;
  if (!reconstructPostflopForResolve(request, reconstructed, reconstruct_miss)) {
    miss_detail = to_string(reconstruct_miss);
    outcome = V1ResolveOutcome::Unsupported;
    return false;
  }
  const V1ResolveResult result = services.resolvingDecision(
      *reconstructed.state, reconstructed.hero_cards, std::string_view{}, deadline_ms);
  outcome = result.outcome;
  if (result.outcome == V1ResolveOutcome::Unsupported) {
    miss_detail =
        to_string(result.miss == V1BlueprintMiss::None ? V1BlueprintMiss::OffTree : result.miss);
    return false;
  }
  if (result.outcome == V1ResolveOutcome::DeadlineExceeded)
    return false;
  if (result.row.size == 0 || !blueprintRowIsLegal(request, result.row)) {
    miss_detail = "off-tree-amount";
    outcome = V1ResolveOutcome::Unsupported;
    return false;
  }
  if (result.outcome == V1ResolveOutcome::Certified) {
    response = mapResolvedExpandedResponse(request, result.row, pv::SOLVER_SOURCE_RESOLVING,
                                           "modeled_exact_bound");
  } else {
    response =
        mapResolvedExpandedResponse(request, result.row, pv::SOLVER_SOURCE_BLUEPRINT, "baseline");
  }
  return true;
}

// Negotiated minor 1 decision dispatch.
EnvelopeResult handleMinor1Decision(const std::string& requestId,
                                    const pv::DecisionRequest& request,
                                    const V1HostServices& services) {
  ValidationReport report;
  bs::Ctx context;
  const pv::ErrorCode validationCode = validateAndMap(request, report, context, 1);
  if (validationCode != pv::ERROR_CODE_UNSPECIFIED) {
    return respondWithDecision(
        requestId,
        errorResponse(validationCode, messageFor(validationCode), retryableFor(validationCode),
                      std::move(report.violations)),
        1);
  }

  const pv::SolverMode mode = request.options().solver_mode();
  if (mode == pv::SOLVER_MODE_BLUEPRINT) {
    pv::DecisionResponse response;
    std::string miss_detail;
    if (!tryBlueprint(services, request, response, miss_detail))
      return blueprintMissResponse(requestId, miss_detail);
    return respondWithDecision(requestId, response, 1);
  }

  // RFC 0005 Stage 9: forced resolving on minor 1 only. AUTOMATIC never
  // resolves. A certified candidate is source RESOLVING/modeled_exact_bound; a
  // deadline with a complete validated baseline is source BLUEPRINT/baseline;
  // otherwise the forced request fails DEADLINE_EXCEEDED, and any unsupported
  // node/coverage/digest case fails UNSUPPORTED_FEATURE.
  if (mode == pv::SOLVER_MODE_RESOLVING) {
    pv::DecisionResponse response;
    V1ResolveOutcome outcome = V1ResolveOutcome::Unsupported;
    std::string miss_detail;
    const std::uint32_t deadline_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(request.options().solve_time_budget_ms(), 120000));
    if (tryResolving(services, request, deadline_ms, response, outcome, miss_detail))
      return respondWithDecision(requestId, response, 1);
    if (outcome == V1ResolveOutcome::DeadlineExceeded)
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_DEADLINE_EXCEEDED,
                                               messageFor(pv::ERROR_CODE_DEADLINE_EXCEEDED),
                                               retryableFor(pv::ERROR_CODE_DEADLINE_EXCEEDED)),
                                 1);
    return blueprintMissResponse(requestId, miss_detail);
  }

  // AUTOMATIC may use a resident hit; an explicit HEURISTIC request always
  // runs the heuristic. Any automatic-mode miss falls through to the existing
  // heuristic engine, which reports its real source.
  if (mode == pv::SOLVER_MODE_AUTOMATIC) {
    pv::DecisionResponse response;
    std::string miss_detail;
    if (tryBlueprint(services, request, response, miss_detail))
      return respondWithDecision(requestId, response, 1);
  }

  const bs::Decision decision = bs::decide(context);
  if (decision.action.empty()) {
    return respondWithDecision(
        requestId,
        errorResponse(pv::ERROR_CODE_NO_DECISION, messageFor(pv::ERROR_CODE_NO_DECISION),
                      retryableFor(pv::ERROR_CODE_NO_DECISION)),
        1);
  }
  return respondWithDecision(requestId, mapHeuristicExpandedResponse(request, decision), 1);
}

}  // namespace

const char* to_string(V1BlueprintMiss miss) noexcept {
  switch (miss) {
    case V1BlueprintMiss::None:
      return "none";
    case V1BlueprintMiss::UnsupportedHandState:
      return "unsupported-hand-state";
    case V1BlueprintMiss::RootNotSupported:
      return "root-not-supported";
    case V1BlueprintMiss::RootIdentityMismatch:
      return "root-identity-mismatch";
    case V1BlueprintMiss::OverBudgetNotAdvertised:
      return "over-budget-not-advertised";
    case V1BlueprintMiss::MissingHistory:
      return "missing-history";
    case V1BlueprintMiss::OffTree:
      return "off-tree";
    case V1BlueprintMiss::OffTreeAmount:
      return "off-tree-amount";
    case V1BlueprintMiss::ZeroProbabilityObservedAction:
      return "zero-probability-observed-action";
    case V1BlueprintMiss::EmptyJointRange:
      return "empty-joint-range";
    case V1BlueprintMiss::UntrainedCombo:
      return "untrained-combo";
    case V1BlueprintMiss::ComboBlockedByBoard:
      return "combo-blocked-by-board";
    case V1BlueprintMiss::RunoutDivergence:
      return "runout-divergence";
    case V1BlueprintMiss::OpponentRangeFullyBlocked:
      return "opponent-range-fully-blocked";
    case V1BlueprintMiss::ZeroProbabilityHeroCombination:
      return "zero-probability-hero-combination";
  }
  return "unknown";
}

namespace {

class NoResidentServices final : public V1HostServices {
 public:
  bool blueprintAdvertised() const noexcept override { return false; }

  V1BlueprintResult blueprintHeroDecision(const bs::poker::HeadsUpState&, const std::array<int, 2>&,
                                          std::string_view) const noexcept override {
    V1BlueprintResult result;
    result.hit = false;
    result.miss = V1BlueprintMiss::RootNotSupported;
    return result;
  }
};

}  // namespace

const V1HostServices& noResidentServices() noexcept {
  static const NoResidentServices services;
  return services;
}

EnvelopeResult handleEnvelope(const std::string& frame) {
  return handleEnvelope(frame, noResidentServices());
}

EnvelopeResult handleEnvelope(const std::string& frame, const V1HostServices& services) {
  pv::Envelope envelope;
  if (!envelope.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
    return respondWithDecision(
        "", errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "malformed envelope", false), 0);
  }

  const std::string requestId = envelope.request_id();
  if (requestId.empty() || requestId.size() > kMaxRequestIdBytes) {
    return respondWithDecision("",
                               errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                             "request_id must be 1..128 characters", false),
                               envelope.protocol_minor() <= 1 ? envelope.protocol_minor() : 0);
  }
  if (!isValidUtf8(requestId)) {
    return respondWithDecision(
        "", errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "request_id must be valid UTF-8", false),
        envelope.protocol_minor() <= 1 ? envelope.protocol_minor() : 0);
  }

  const std::uint32_t minor = envelope.protocol_minor();
  const std::uint32_t echo_minor = minor <= 1 ? minor : 0;
  if (minor > 1) {
    return respondWithDecision(requestId,
                               errorResponse(pv::ERROR_CODE_UNSUPPORTED_PROTOCOL,
                                             "only protocol minors 0 and 1 are supported", false),
                               echo_minor);
  }

  switch (envelope.payload_case()) {
    case pv::Envelope::kGetCapabilitiesRequest:
      if (minor == 0)
        return respondWithCapabilities(requestId, buildCapabilities(), 0);
      return respondWithCapabilities(
          requestId,
          buildCapabilities(1, services.blueprintAdvertised(), services.resolvingAdvertised()), 1);

    case pv::Envelope::kGetCapabilitiesResponse:
      return respondWithDecision(
          requestId,
          errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                        "capability responses are not accepted by the host", false),
          echo_minor);

    case pv::Envelope::kDecisionResponse:
      return respondWithDecision(
          requestId,
          errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                        "decision responses are not accepted by the host", false),
          echo_minor);

    case pv::Envelope::kDecisionRequest:
      break;

    case pv::Envelope::PAYLOAD_NOT_SET:
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                               "envelope requires exactly one payload", false),
                                 echo_minor);
  }

  const pv::DecisionRequest& request = envelope.decision_request();
  try {
    // Minor 0 keeps the exact Stage-7 call graph and response bytes: it never
    // consults host services, never touches resident code, and never emits the
    // expanded oneof or the new enums.
    if (minor == 0) {
      ValidationReport report;
      bs::Ctx context;
      const pv::ErrorCode validationCode = validateAndMap(request, report, context);
      if (validationCode != pv::ERROR_CODE_UNSPECIFIED) {
        return respondWithDecision(
            requestId,
            errorResponse(validationCode, messageFor(validationCode), retryableFor(validationCode),
                          std::move(report.violations)),
            0);
      }
      const bs::Decision decision = bs::decide(context);
      if (decision.action.empty()) {
        return respondWithDecision(
            requestId,
            errorResponse(pv::ERROR_CODE_NO_DECISION, messageFor(pv::ERROR_CODE_NO_DECISION),
                          retryableFor(pv::ERROR_CODE_NO_DECISION)),
            0);
      }
      pv::DecisionResponse response = mapDecisionResponse(request, decision);
      return respondWithDecision(requestId, response, 0);
    }

    return handleMinor1Decision(requestId, request, services);
  } catch (const MappingError& error) {
    return respondWithDecision(
        requestId, errorResponse(error.code(), error.what(), error.retryable(), error.violations()),
        echo_minor);
  } catch (const std::bad_alloc&) {
    return respondWithDecision(requestId,
                               errorResponse(pv::ERROR_CODE_RESOURCE_EXHAUSTED,
                                             messageFor(pv::ERROR_CODE_RESOURCE_EXHAUSTED), true),
                               echo_minor);
  } catch (const std::exception& error) {
    return respondWithDecision(
        requestId, errorResponse(pv::ERROR_CODE_INTERNAL, error.what(), true), echo_minor);
  }
}

}  // namespace bs::v1
