// v2_envelope.cpp — RFC 0022 v2 envelope dispatch and decision routing.
//
// The v2 handler accepts only protocol_version == 3. It parses the envelope,
// validates the request ID, and dispatches capabilities or decision requests.
// Decision requests are validated, reconstructed from the TransitionEvent
// path, and routed by solver_mode to the blueprint, resolving, or heuristic
// path. The v1 handler is frozen: a v2 host routes 0..2 to v1 and 3 to v2.
#include <bs/guarantee.hpp>
#include <bs/service.hpp>
#include <bs/v2_protocol.hpp>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>

#include "v2_mappers.hpp"

namespace bs::v2 {
namespace {

namespace pv2 = ::bigshark::engine::v2;

constexpr std::size_t kMaxRequestIdBytes = 128;

// Decodes a canonical ULEB128 varint (up to 5 bytes for a uint32) from the
// buffer. Returns the decoded value and the number of bytes consumed, or
// nullopt on malformation.
std::optional<std::pair<std::uint32_t, std::size_t>> decodeVarint32(std::string_view data,
                                                                    std::size_t offset) noexcept {
  std::uint32_t result = 0;
  std::size_t shift = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    if (offset + i >= data.size())
      return std::nullopt;
    const std::uint8_t byte = static_cast<std::uint8_t>(data[offset + i]);
    result |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      // Canonical check: the last byte must not have leading zeros that a
      // shorter encoding would have omitted (except for the value 0 itself).
      if (i > 0 && byte == 0)
        return std::nullopt;
      return std::make_pair(result, i + 1);
    }
    shift += 7;
  }
  return std::nullopt;  // Too many bytes for a uint32.
}

const char* messageFor(pv2::ErrorCode code) {
  switch (code) {
    case pv2::ERROR_CODE_INVALID_REQUEST:
      return "invalid request";
    case pv2::ERROR_CODE_UNSUPPORTED_PROTOCOL:
      return "unsupported protocol version";
    case pv2::ERROR_CODE_UNSUPPORTED_GAME:
      return "unsupported game";
    case pv2::ERROR_CODE_UNSUPPORTED_FEATURE:
      return "unsupported feature";
    case pv2::ERROR_CODE_NO_DECISION:
      return "engine produced no decision";
    case pv2::ERROR_CODE_DEADLINE_EXCEEDED:
      return "solve deadline exceeded";
    case pv2::ERROR_CODE_RESOURCE_EXHAUSTED:
      return "resource exhausted";
    case pv2::ERROR_CODE_INTERNAL:
      return "internal error";
    case pv2::ERROR_CODE_GUARANTEE_BELOW_REQUEST:
      return "guarantee below requested minimum";
    case pv2::ERROR_CODE_HISTORY_INCOMPLETE:
      return "action history is incomplete";
    case pv2::ERROR_CODE_REPLAY_MISMATCH:
      return "replayed state does not match declared snapshot";
    case pv2::ERROR_CODE_INCOMPATIBLE_ROOT:
      return "no strategy root matches the game definition";
    case pv2::ERROR_CODE_OFF_TREE_AMOUNT:
      return "legal action amount is outside the policy";
    case pv2::ERROR_CODE_RULES_UNCONFIRMED:
      return "rule profile has not been confirmed";
    case pv2::ERROR_CODE_UNKNOWN_UTILITY:
      return "requested utility profile is not supported";
    default:
      return "unspecified error";
  }
}

bool retryableFor(pv2::ErrorCode code) {
  switch (code) {
    case pv2::ERROR_CODE_RESOURCE_EXHAUSTED:
    case pv2::ERROR_CODE_INTERNAL:
    case pv2::ERROR_CODE_DEADLINE_EXCEEDED:
      return true;
    default:
      return false;
  }
}

EnvelopeResult respondWithDecision(const std::string& requestId,
                                   const pv2::DecisionResponse& response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv2::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(requestId);
  *envelope.mutable_decision_response() = response;
  result.response = envelope.SerializeAsString();
  return result;
}

EnvelopeResult respondWithCapabilities(const std::string& requestId,
                                       const pv2::GetCapabilitiesResponse& response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv2::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(requestId);
  *envelope.mutable_get_capabilities_response() = response;
  result.response = envelope.SerializeAsString();
  return result;
}

struct BlueprintLookup {
  bool hit = false;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  V1BlueprintRow row;
};

BlueprintLookup lookupBlueprint(const V1HostServices& services, const ReconstructedState& recon) {
  if (!services.blueprintAdvertised())
    return {.hit = false, .miss = V1BlueprintMiss::RootNotSupported, .row = {}};
  const V1BlueprintResult result =
      services.blueprintHeroDecision(*recon.state, recon.history, recon.hero_cards, {});
  return {.hit = result.hit, .miss = result.miss, .row = result.row};
}

V1ResolveResult lookupResolving(const V1HostServices& services, const ReconstructedState& recon,
                                std::uint32_t deadline_ms) {
  if (!services.resolvingAdvertised())
    return {};
  return services.resolvingDecision(*recon.state, recon.history, recon.hero_cards, {}, deadline_ms);
}

pv2::ErrorCode mapMissToError(V1BlueprintMiss miss) {
  switch (miss) {
    case V1BlueprintMiss::None:
      return pv2::ERROR_CODE_UNSPECIFIED;
    case V1BlueprintMiss::MissingHistory:
      return pv2::ERROR_CODE_HISTORY_INCOMPLETE;
    case V1BlueprintMiss::OffTree:
    case V1BlueprintMiss::OffTreeAmount:
    case V1BlueprintMiss::ZeroProbabilityObservedAction:
      return pv2::ERROR_CODE_OFF_TREE_AMOUNT;
    case V1BlueprintMiss::RootIdentityMismatch:
    case V1BlueprintMiss::RootNotSupported:
    case V1BlueprintMiss::SeatCountNotSupported:
      return pv2::ERROR_CODE_INCOMPATIBLE_ROOT;
    default:
      return pv2::ERROR_CODE_UNSUPPORTED_FEATURE;
  }
}

pv2::ErrorCode mapReplayMissToError(V2ReplayMiss miss) {
  switch (miss) {
    case V2ReplayMiss::None:
      return pv2::ERROR_CODE_UNSPECIFIED;
    case V2ReplayMiss::HistoryIncomplete:
      return pv2::ERROR_CODE_HISTORY_INCOMPLETE;
    case V2ReplayMiss::ReplayMismatch:
      return pv2::ERROR_CODE_REPLAY_MISMATCH;
    case V2ReplayMiss::IncompatibleRoot:
      return pv2::ERROR_CODE_INCOMPATIBLE_ROOT;
    case V2ReplayMiss::OffTreeAmount:
      return pv2::ERROR_CODE_OFF_TREE_AMOUNT;
    case V2ReplayMiss::RulesUnconfirmed:
      return pv2::ERROR_CODE_RULES_UNCONFIRMED;
    case V2ReplayMiss::UnsupportedFeature:
      return pv2::ERROR_CODE_UNSUPPORTED_FEATURE;
  }
  return pv2::ERROR_CODE_UNSUPPORTED_FEATURE;
}

}  // namespace

MappingError::MappingError(pv2::ErrorCode code, std::string message, bool retryable,
                           std::vector<pv2::FieldViolation> violations)
    : std::runtime_error(std::move(message)),
      code_(code),
      retryable_(retryable),
      violations_(std::move(violations)) {}

void ValidationReport::add(const std::string& fieldPath, const std::string& description) {
  if (violations.size() >= static_cast<std::size_t>(kMaxFieldViolations)) {
    if (violations.size() == static_cast<std::size_t>(kMaxFieldViolations)) {
      pv2::FieldViolation& last = violations.back();
      last.set_field_path("");
      last.set_description("further violations truncated");
    }
    return;
  }
  pv2::FieldViolation& violation = violations.emplace_back();
  violation.set_field_path(fieldPath);
  violation.set_description(description);
}

EnvelopeResult handleEnvelope(const std::string& frame) {
  return bs::v2::handleEnvelope(frame, noResidentServices());
}

EnvelopeResult handleEnvelope(const std::string& frame, const V1HostServices& services) {
  pv2::Envelope envelope;
  if (!envelope.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
    return respondWithDecision(
        "", errorResponse(pv2::ERROR_CODE_INVALID_REQUEST, "malformed envelope", false));
  }

  const std::string requestId = envelope.request_id();
  if (requestId.empty() || requestId.size() > kMaxRequestIdBytes) {
    return respondWithDecision("", errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                                 "request_id must be 1..128 characters", false));
  }
  if (!isValidUtf8(requestId)) {
    return respondWithDecision("", errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                                 "request_id must be valid UTF-8", false));
  }

  if (envelope.protocol_version() != 3) {
    return respondWithDecision(
        requestId, errorResponse(pv2::ERROR_CODE_UNSUPPORTED_PROTOCOL,
                                 "only protocol version 3 is supported by the v2 handler", false));
  }

  switch (envelope.payload_case()) {
    case pv2::Envelope::kGetCapabilitiesRequest:
      return respondWithCapabilities(requestId, buildCapabilities(services.blueprintAdvertised(),
                                                                  services.resolvingAdvertised()));

    case pv2::Envelope::kGetCapabilitiesResponse:
      return respondWithDecision(
          requestId, errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                   "capability responses are not accepted by the host", false));

    case pv2::Envelope::kDecisionResponse:
      return respondWithDecision(
          requestId, errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                   "decision responses are not accepted by the host", false));

    case pv2::Envelope::kDecisionRequest:
      break;

    case pv2::Envelope::PAYLOAD_NOT_SET:
      return respondWithDecision(requestId,
                                 errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                               "envelope requires exactly one payload", false));
  }

  const pv2::DecisionRequest& request = envelope.decision_request();
  try {
    // Phase 1: semantic validation.
    ValidationReport report;
    const pv2::ErrorCode validationCode = validateDecisionRequest(request, report);
    if (validationCode != pv2::ERROR_CODE_UNSPECIFIED) {
      return respondWithDecision(
          requestId, errorResponse(validationCode, messageFor(validationCode),
                                   retryableFor(validationCode), std::move(report.violations)));
    }

    // Phase 2: reconstruct the native state from the TransitionEvent path.
    ReconstructedState recon;
    V2ReplayMiss replayMiss = V2ReplayMiss::None;
    if (!reconstructHandState(request, recon, replayMiss)) {
      const pv2::ErrorCode code = mapReplayMissToError(replayMiss);
      return respondWithDecision(requestId,
                                 errorResponse(code, messageFor(code), retryableFor(code)));
    }

    // Phase 3: route by solver_mode.
    const pv2::SolverMode mode = request.options().solver_mode();
    switch (mode) {
      case pv2::SOLVER_MODE_BLUEPRINT: {
        const BlueprintLookup lookup = lookupBlueprint(services, recon);
        if (!lookup.hit) {
          const pv2::ErrorCode code = mapMissToError(lookup.miss);
          return respondWithDecision(requestId,
                                     errorResponse(code, messageFor(code), retryableFor(code)));
        }
        return respondWithDecision(requestId, mapBlueprintResponse(request, lookup.row));
      }

      case pv2::SOLVER_MODE_RESOLVING: {
        const V1ResolveResult resolve =
            lookupResolving(services, recon, request.options().solve_time_budget_ms());
        if (resolve.outcome == V1ResolveOutcome::Unsupported) {
          const pv2::ErrorCode code = mapMissToError(resolve.miss);
          return respondWithDecision(requestId,
                                     errorResponse(code, messageFor(code), retryableFor(code)));
        }
        return respondWithDecision(requestId,
                                   mapResolvedResponse(request, resolve.row, resolve.outcome));
      }

      case pv2::SOLVER_MODE_AUTOMATIC: {
        // Try blueprint first; on miss, fall through to the heuristic.
        const BlueprintLookup lookup = lookupBlueprint(services, recon);
        if (lookup.hit)
          return respondWithDecision(requestId, mapBlueprintResponse(request, lookup.row));
        // Fall through to heuristic.
        [[fallthrough]];
      }

      case pv2::SOLVER_MODE_HEURISTIC: {
        // Map the v2 request to a heuristic context and decide.
        bs::Ctx context;
        const pv2::ErrorCode mapCode = validateAndMap(request, report, context);
        if (mapCode != pv2::ERROR_CODE_UNSPECIFIED) {
          return respondWithDecision(
              requestId, errorResponse(mapCode, messageFor(mapCode), retryableFor(mapCode),
                                       std::move(report.violations)));
        }
        const bs::SourcedDecision answer = bs::decideSourced(context);
        if (answer.decision.action.empty()) {
          return respondWithDecision(
              requestId,
              errorResponse(pv2::ERROR_CODE_NO_DECISION, messageFor(pv2::ERROR_CODE_NO_DECISION),
                            retryableFor(pv2::ERROR_CODE_NO_DECISION)));
        }
        return respondWithDecision(requestId, mapHeuristicResponse(request, answer));
      }

      default:
        return respondWithDecision(requestId, errorResponse(pv2::ERROR_CODE_INVALID_REQUEST,
                                                            "unsupported solver mode", false));
    }
  } catch (const MappingError& error) {
    return respondWithDecision(requestId, errorResponse(error.code(), error.what(),
                                                        error.retryable(), error.violations()));
  } catch (const std::bad_alloc&) {
    return respondWithDecision(requestId,
                               errorResponse(pv2::ERROR_CODE_RESOURCE_EXHAUSTED,
                                             messageFor(pv2::ERROR_CODE_RESOURCE_EXHAUSTED), true));
  } catch (const std::exception& error) {
    return respondWithDecision(requestId,
                               errorResponse(pv2::ERROR_CODE_INTERNAL, error.what(), true));
  }
}

std::optional<std::uint32_t> peekProtocolVersion(std::string_view frame) noexcept {
  // Field 1 (protocol_version) is a uint32 varint at wire tag 0x08 in both
  // v1 and v2 envelopes. We decode just this one field without parsing the
  // payload so the host can route by version.
  if (frame.empty() || static_cast<std::uint8_t>(frame[0]) != 0x08)
    return std::nullopt;
  const auto decoded = decodeVarint32(frame, 1);
  if (!decoded)
    return std::nullopt;
  return decoded->first;
}

}  // namespace bs::v2
