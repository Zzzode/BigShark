#include <algorithm>
#include <bs/guarantee.hpp>
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

// Street dispatch: preflop requests go through the RFC 0007 preflop
// reconstructor; postflop requests use the existing postflop path.
bool reconstructRequest(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                        V1BlueprintMiss& miss, bool for_resolve) {
  if (request.state().street() == pv::STREET_PREFLOP)
    return reconstructPreflop(request, out, miss);
  return for_resolve ? reconstructPostflopForResolve(request, out, miss)
                     : reconstructPostflop(request, out, miss);
}

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
    case pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST:
      return "achieved guarantee level is below the requested minimum";
    case pv::ERROR_CODE_INTERNAL:
      return "internal engine error";
    default:
      return "invalid request";
  }
}

// Shared decision-error envelope for a resident coverage miss. RFC 0008 stage
// 5 (R16): the echo minor is the negotiated minor; minor 1 passes literal 1
// (byte-identical behavior), minor 2 passes 2. Code and detail are unchanged
// across minors.
EnvelopeResult blueprintMissResponse(const std::string& requestId, const std::string& detail,
                                     std::uint32_t negotiated_minor) {
  std::string message = "blueprint coverage unavailable";
  if (!detail.empty()) {
    message += ": ";
    message += detail;
  }
  return respondWithDecision(
      requestId, errorResponse(pv::ERROR_CODE_UNSUPPORTED_FEATURE, message, /*retryable=*/false),
      negotiated_minor);
}

// RFC 0008 stage 5 (R9): protobuf-free lookup core. Returns the reconstructed
// blueprint row plus the verbatim miss detail WITHOUT serializing it; each
// per-minor dispatcher applies its own vocabulary mapper. Exactly one host
// service invocation happens per decision; the returned row is reused by the
// floor check and the serializer, never re-looked-up.
struct BlueprintLookup {
  bool hit = false;
  std::string miss_detail;
  V1BlueprintRow row;
};

BlueprintLookup lookupBlueprint(const V1HostServices& services,
                                const pv::DecisionRequest& request) {
  BlueprintLookup result;
  if (!services.blueprintAdvertised()) {
    result.miss_detail = "no resident blueprint root advertised";
    return result;
  }
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss reconstruct_miss = V1BlueprintMiss::None;
  if (!reconstructRequest(request, reconstructed, reconstruct_miss, /*for_resolve=*/false)) {
    result.miss_detail = to_string(reconstruct_miss);
    return result;
  }
  const V1BlueprintResult answer = services.blueprintHeroDecision(
      *reconstructed.state, reconstructed.history, reconstructed.hero_cards, std::string_view{});
  if (!answer.hit) {
    result.miss_detail = to_string(answer.miss);
    return result;
  }
  // An abstract row action outside the client legal window is a coverage
  // miss, never a clamp.
  if (!blueprintRowIsLegal(request, answer.row)) {
    result.miss_detail = "off-tree-amount";
    return result;
  }
  result.hit = true;
  result.row = answer.row;
  return result;
}

// Protobuf-free RFC 0005 Stage 9 terminal-only resolve lookup. Returns the
// outcome class and selected row; the DeadlineExceeded early return stays
// BEFORE the row legality check and the off-tree outcome overwrite is
// preserved verbatim from the former shared helper.
struct ResolvingLookup {
  bool answered = false;
  V1ResolveOutcome outcome = V1ResolveOutcome::Unsupported;
  std::string miss_detail;
  V1BlueprintRow row;
};

ResolvingLookup lookupResolving(const V1HostServices& services, const pv::DecisionRequest& request,
                                std::uint32_t deadline_ms) {
  ResolvingLookup result;
  if (!services.resolvingAdvertised()) {
    result.miss_detail = "no resolving root advertised";
    return result;
  }
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss reconstruct_miss = V1BlueprintMiss::None;
  if (!reconstructRequest(request, reconstructed, reconstruct_miss, /*for_resolve=*/true)) {
    result.miss_detail = to_string(reconstruct_miss);
    return result;
  }
  const V1ResolveResult resolved =
      services.resolvingDecision(*reconstructed.state, reconstructed.history,
                                 reconstructed.hero_cards, std::string_view{}, deadline_ms);
  result.outcome = resolved.outcome;
  if (resolved.outcome == V1ResolveOutcome::Unsupported) {
    result.miss_detail = to_string(resolved.miss == V1BlueprintMiss::None ? V1BlueprintMiss::OffTree
                                                                          : resolved.miss);
    return result;
  }
  if (resolved.outcome == V1ResolveOutcome::DeadlineExceeded)
    return result;
  if (resolved.row.size == 0 || !blueprintRowIsLegal(request, resolved.row)) {
    result.miss_detail = "off-tree-amount";
    result.outcome = V1ResolveOutcome::Unsupported;
    return result;
  }
  result.answered = true;
  result.row = resolved.row;
  return result;
}

// Maps the request floor enum to the domain guarantee. The validator already
// rejected an absent or unknown value at minor 2.
bs::Guarantee requestedFloor(const pv::DecisionOptions& options) {
  switch (options.minimum_guarantee()) {
    case pv::GUARANTEE_LEVEL_OPERATIONAL_FALLBACK:
      return bs::Guarantee::OperationalFallback;
    case pv::GUARANTEE_LEVEL_APPROXIMATE:
      return bs::Guarantee::Approximate;
    case pv::GUARANTEE_LEVEL_ABSTRACT_SOLVED:
      return bs::Guarantee::AbstractSolved;
    case pv::GUARANTEE_LEVEL_EXACT_SOLVED:
      return bs::Guarantee::ExactSolved;
    case pv::GUARANTEE_LEVEL_CERTIFIED_BOUND:
      return bs::Guarantee::CertifiedBound;
    default:
      return bs::Guarantee::CertifiedBound;  // unreachable after validation
  }
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
    const BlueprintLookup lookup = lookupBlueprint(services, request);
    if (!lookup.hit)
      return blueprintMissResponse(requestId, lookup.miss_detail, 1);
    return respondWithDecision(requestId, mapBlueprintExpandedResponse(request, lookup.row), 1);
  }

  // RFC 0005 Stage 9: forced resolving on minor 1 only. AUTOMATIC never
  // resolves. A certified candidate is source RESOLVING/modeled_exact_bound; a
  // deadline with a complete validated baseline is source BLUEPRINT/baseline;
  // otherwise the forced request fails DEADLINE_EXCEEDED, and any unsupported
  // node/coverage/digest case fails UNSUPPORTED_FEATURE.
  if (mode == pv::SOLVER_MODE_RESOLVING) {
    const std::uint32_t deadline_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(request.options().solve_time_budget_ms(), 120000));
    const ResolvingLookup lookup = lookupResolving(services, request, deadline_ms);
    if (lookup.answered) {
      if (lookup.outcome == V1ResolveOutcome::Certified)
        return respondWithDecision(
            requestId,
            mapResolvedExpandedResponse(request, lookup.row, pv::SOLVER_SOURCE_RESOLVING,
                                        "modeled_exact_bound"),
            1);
      return respondWithDecision(
          requestId,
          mapResolvedExpandedResponse(request, lookup.row, pv::SOLVER_SOURCE_BLUEPRINT, "baseline"),
          1);
    }
    if (lookup.outcome == V1ResolveOutcome::DeadlineExceeded)
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_DEADLINE_EXCEEDED,
                                               messageFor(pv::ERROR_CODE_DEADLINE_EXCEEDED),
                                               retryableFor(pv::ERROR_CODE_DEADLINE_EXCEEDED)),
                                 1);
    return blueprintMissResponse(requestId, lookup.miss_detail, 1);
  }

  // AUTOMATIC may use a resident hit; an explicit HEURISTIC request always
  // runs the heuristic. Any automatic-mode miss falls through to the existing
  // heuristic engine, which reports its real source.
  if (mode == pv::SOLVER_MODE_AUTOMATIC) {
    const BlueprintLookup lookup = lookupBlueprint(services, request);
    if (lookup.hit)
      return respondWithDecision(requestId, mapBlueprintExpandedResponse(request, lookup.row), 1);
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

// RFC 0009 W3: the operational fallback for a minor-2 AUTOMATIC blueprint
// miss. Check when legal, else call when legal, else fold - chosen from the
// supplied legal set only. This is the single definition of the v1 fallback
// semantics; the chart+heuristic cascade is reachable only via an explicit
// HEURISTIC request, never as an AUTOMATIC fallthrough.
bs::Decision operationalFallbackDecision(const bs::Ctx& context) {
  bs::Decision decision;
  if (context.legal.has("check")) {
    decision.action = "check";
  } else if (context.legal.has("call")) {
    decision.action = "call";
    decision.amount = context.legal.call;
  } else {
    decision.action = "fold";
  }
  decision.reason = "operational-fallback";
  return decision;
}

// Negotiated minor 2 decision dispatch (RFC 0008 stage 5). Same mode routing
// as minor 1; differences are: every success is an expanded_strategy carrying
// field 11 (never field 10), the heuristic wire source is the source the
// policy declared at the routing branch, and a present minimum_guarantee is
// compared against the achieved level only AFTER validation, lookup, and the
// mapper's completeness preconditions established a complete answer.
EnvelopeResult handleMinor2Decision(const std::string& requestId,
                                    const pv::DecisionRequest& request,
                                    const V1HostServices& services) {
  ValidationReport report;
  bs::Ctx context;
  const pv::ErrorCode validationCode = validateAndMap(request, report, context, 2);
  if (validationCode != pv::ERROR_CODE_UNSPECIFIED) {
    return respondWithDecision(
        requestId,
        errorResponse(validationCode, messageFor(validationCode), retryableFor(validationCode),
                      std::move(report.violations)),
        2);
  }

  const pv::SolverMode mode = request.options().solver_mode();

  // The one complete answer plus the level it achieved. Coverage/deadline and
  // completeness errors take precedence over the floor (R9/R15): code 9 exists
  // only when a complete, serializable, weaker answer is real.
  pv::DecisionResponse complete;
  bs::Guarantee achieved = bs::Guarantee::OperationalFallback;

  if (mode == pv::SOLVER_MODE_BLUEPRINT) {
    const BlueprintLookup lookup = lookupBlueprint(services, request);
    if (!lookup.hit)
      return blueprintMissResponse(requestId, lookup.miss_detail, 2);
    verifyStorageRowComplete(request, lookup.row);
    achieved = bs::Guarantee::Approximate;
    complete = mapGuaranteedBlueprintResponse(request, lookup.row);
  } else if (mode == pv::SOLVER_MODE_RESOLVING) {
    const std::uint32_t deadline_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(request.options().solve_time_budget_ms(), 120000));
    const ResolvingLookup lookup = lookupResolving(services, request, deadline_ms);
    if (lookup.answered) {
      verifyStorageRowComplete(request, lookup.row);
      if (lookup.outcome == V1ResolveOutcome::Certified) {
        achieved = bs::Guarantee::CertifiedBound;
        complete = mapGuaranteedCertifiedResponse(request, lookup.row);
      } else {
        // DeadlineBlueprint: a complete validated baseline row tagged with
        // the BLUEPRINT source, exactly like minor 1's baseline.
        achieved = bs::Guarantee::Approximate;
        complete = mapGuaranteedDeadlineResponse(request, lookup.row);
      }
    } else if (lookup.outcome == V1ResolveOutcome::DeadlineExceeded) {
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_DEADLINE_EXCEEDED,
                                               messageFor(pv::ERROR_CODE_DEADLINE_EXCEEDED),
                                               retryableFor(pv::ERROR_CODE_DEADLINE_EXCEEDED)),
                                 2);
    } else {
      return blueprintMissResponse(requestId, lookup.miss_detail, 2);
    }
  } else if (mode == pv::SOLVER_MODE_HEURISTIC) {
    // RFC 0009 W3: an explicit HEURISTIC request still runs the sourced
    // chart+heuristic cascade. AUTOMATIC no longer reaches it (below).
    const bs::SourcedDecision answer = bs::decideSourced(context);
    if (answer.decision.action.empty()) {
      return respondWithDecision(
          requestId,
          errorResponse(pv::ERROR_CODE_NO_DECISION, messageFor(pv::ERROR_CODE_NO_DECISION),
                        retryableFor(pv::ERROR_CODE_NO_DECISION)),
          2);
    }
    verifyHeuristicAnswerComplete(request, answer);
    achieved = guaranteeFor(answer.source);
    complete = mapGuaranteedHeuristicResponse(request, answer);
  } else {
    // AUTOMATIC: a resident blueprint hit serves; a miss tries terminal-only
    // resolving (RFC 0009 W4d) before the declared operational fallback. The
    // resolver is best-effort — any miss (not advertised, spot not eligible,
    // deadline exceeded without a baseline) ends at the operational fallback.
    // RFC 0009 W3 demotes the chart+heuristic cascade from an AUTOMATIC
    // fallthrough to an explicit HEURISTIC-only request, so a miss never
    // silently runs the parallel strategy.
    const BlueprintLookup lookup = lookupBlueprint(services, request);
    if (lookup.hit) {
      verifyStorageRowComplete(request, lookup.row);
      achieved = bs::Guarantee::Approximate;
      complete = mapGuaranteedBlueprintResponse(request, lookup.row);
    } else {
      const std::uint32_t deadline_ms = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(request.options().solve_time_budget_ms(), 120000));
      const ResolvingLookup resolved = lookupResolving(services, request, deadline_ms);
      if (resolved.answered) {
        verifyStorageRowComplete(request, resolved.row);
        if (resolved.outcome == V1ResolveOutcome::Certified) {
          achieved = bs::Guarantee::CertifiedBound;
          complete = mapGuaranteedCertifiedResponse(request, resolved.row);
        } else {
          // DeadlineBlueprint: a complete validated baseline row tagged with
          // the BLUEPRINT source, exactly like the forced path.
          achieved = bs::Guarantee::Approximate;
          complete = mapGuaranteedDeadlineResponse(request, resolved.row);
        }
      } else {
        achieved = bs::Guarantee::OperationalFallback;
        complete = mapOperationalFallbackResponse(request, operationalFallbackDecision(context));
      }
    }
  }

  // Response-side floor enforcement that never alters the action: a weaker
  // complete answer is refused (code 9, no strategy payload) instead of being
  // served silently. Static text and non-retryable for an unchanged state.
  if (request.options().has_minimum_guarantee()) {
    const bs::Guarantee floor = requestedFloor(request.options());
    if (!guaranteeMeets(achieved, floor)) {
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
                                               messageFor(pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST),
                                               /*retryable=*/false),
                                 2);
    }
  }
  return respondWithDecision(requestId, complete, 2);
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
    case V1BlueprintMiss::SeatCountNotSupported:
      return "seat-count-not-supported";
  }
  return "unknown";
}

namespace {

class NoResidentServices final : public V1HostServices {
 public:
  bool blueprintAdvertised() const noexcept override { return false; }

  V1BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                          std::span<const bs::poker::PublicAction>,
                                          const std::array<int, 2>&,
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
                               envelope.protocol_minor() <= 2 ? envelope.protocol_minor() : 0);
  }
  if (!isValidUtf8(requestId)) {
    return respondWithDecision(
        "", errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "request_id must be valid UTF-8", false),
        envelope.protocol_minor() <= 2 ? envelope.protocol_minor() : 0);
  }

  const std::uint32_t minor = envelope.protocol_minor();
  const std::uint32_t echo_minor = minor <= 2 ? minor : 0;
  if (minor > 2) {
    return respondWithDecision(
        requestId,
        errorResponse(pv::ERROR_CODE_UNSUPPORTED_PROTOCOL,
                      "only protocol minors 0, 1 and 2 are supported", false),
        echo_minor);
  }

  switch (envelope.payload_case()) {
    case pv::Envelope::kGetCapabilitiesRequest:
      if (minor == 0)
        return respondWithCapabilities(requestId, buildCapabilities(), 0);
      return respondWithCapabilities(
          requestId,
          buildCapabilities(minor, services.blueprintAdvertised(), services.resolvingAdvertised()),
          minor);

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

    return minor == 2 ? handleMinor2Decision(requestId, request, services)
                      : handleMinor1Decision(requestId, request, services);
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
