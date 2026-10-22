// envelope.cpp — RFC 0022 envelope dispatch and decision routing.
//
// The handler accepts only protocol_version == 3. It parses the envelope,
// validates the request ID, and dispatches capabilities or decision requests.
// Decision requests are validated, reconstructed from the TransitionEvent
// path, and routed by solver_mode to the blueprint, resolving, or heuristic
// path.
#include <bs/guarantee.hpp>
#include <bs/protocol.hpp>
#include <bs/service.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>

#include "mappers.hpp"

namespace bs {
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

// Pass by value and Swap into the envelope: the RepeatedField copy
// constructor triggers an ASan CHECK failure in protobuf's container
// annotations (AnnotateSize). With C++17 guaranteed copy elision, prvalues
// are constructed directly in the parameter; std::move call sites use the
// noexcept move constructor. Swap moves internal pointers without copying.
EnvelopeResult respondWithDecision(const std::string& requestId, pv2::DecisionResponse response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv2::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(requestId);
  envelope.mutable_decision_response()->Swap(&response);
  result.response = envelope.SerializeAsString();
  return result;
}

// Stamp the receipt-to-action wall time on a successful decision response.
// The measurement covers validation, state reconstruction, blueprint lookup,
// and solve — the full receipt-to-action budget. Error responses carry no
// metadata and are left untouched.
void stampSolveTime(pv2::DecisionResponse& response, std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  switch (response.result_case()) {
    case pv2::DecisionResponse::kStrategy:
      response.mutable_strategy()->mutable_solver()->set_solve_time_us(us);
      break;
    case pv2::DecisionResponse::kExpandedStrategy:
      response.mutable_expanded_strategy()->mutable_solver()->set_solve_time_us(us);
      break;
    default:
      break;
  }
}

EnvelopeResult respondWithCapabilities(const std::string& requestId,
                                       pv2::GetCapabilitiesResponse response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv2::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(requestId);
  envelope.mutable_get_capabilities_response()->Swap(&response);
  result.response = envelope.SerializeAsString();
  return result;
}

struct BlueprintLookup {
  bool hit = false;
  BlueprintMiss miss = BlueprintMiss::None;
  BlueprintRow row;
};

BlueprintLookup lookupBlueprint(const HostServices& services, const ReconstructedState& recon) {
  if (!services.blueprintAdvertised())
    return {.hit = false, .miss = BlueprintMiss::RootNotSupported, .row = {}};
  const BlueprintResult result =
      services.blueprintHeroDecision(*recon.state, recon.history, recon.hero_cards, {});
  return {.hit = result.hit, .miss = result.miss, .row = result.row};
}

ResolveResult lookupResolving(const HostServices& services, const ReconstructedState& recon,
                              std::uint32_t deadline_ms) {
  if (!services.resolvingAdvertised())
    return {};
  return services.resolvingDecision(*recon.state, recon.history, recon.hero_cards, {}, deadline_ms);
}

// Constructs the operational fallback decision: check if legal, otherwise
// call if legal, otherwise fold. This is the labeled fallback served when
// both the blueprint and the resolver miss.
bs::Decision operationalFallbackDecision(const pv2::DecisionRequest& request) {
  bs::Decision decision;
  // RFC 0032 stage 3: a postflop snapshot wider than a flop is a turn- or
  // river-rooted decision. The resident library stores a three-card board, so a
  // record cannot yet answer it; the fallback is still legal, but the log must
  // say the root was unserved rather than presenting it as an ordinary
  // mechanical decision. Preflop and flop keep the original token byte for
  // byte, so existing counts and replay fixtures are unaffected.
  decision.reason =
      request.state().board_size() >= 4 ? "turn_root_unserved" : "operational-fallback";
  bool canCheck = false;
  bool canCall = false;
  for (const pv2::LegalAction& legal : request.state().legal_actions()) {
    if (legal.type() == pv2::ACTION_TYPE_CHECK)
      canCheck = true;
    else if (legal.type() == pv2::ACTION_TYPE_CALL)
      canCall = true;
  }
  if (canCheck)
    decision.action = "check";
  else if (canCall)
    decision.action = "call";
  else
    decision.action = "fold";
  return decision;
}

pv2::ErrorCode mapMissToError(BlueprintMiss miss) {
  switch (miss) {
    case BlueprintMiss::None:
      return pv2::ERROR_CODE_UNSPECIFIED;
    case BlueprintMiss::MissingHistory:
      return pv2::ERROR_CODE_HISTORY_INCOMPLETE;
    case BlueprintMiss::OffTree:
    case BlueprintMiss::OffTreeAmount:
    case BlueprintMiss::ZeroProbabilityObservedAction:
      return pv2::ERROR_CODE_OFF_TREE_AMOUNT;
    case BlueprintMiss::RootIdentityMismatch:
    case BlueprintMiss::RootNotSupported:
    case BlueprintMiss::SeatCountNotSupported:
      return pv2::ERROR_CODE_INCOMPATIBLE_ROOT;
    default:
      return pv2::ERROR_CODE_UNSUPPORTED_FEATURE;
  }
}

pv2::ErrorCode mapReplayMissToError(ReplayMiss miss) {
  switch (miss) {
    case ReplayMiss::None:
      return pv2::ERROR_CODE_UNSPECIFIED;
    case ReplayMiss::HistoryIncomplete:
      return pv2::ERROR_CODE_HISTORY_INCOMPLETE;
    case ReplayMiss::ReplayMismatch:
      return pv2::ERROR_CODE_REPLAY_MISMATCH;
    case ReplayMiss::IncompatibleRoot:
      return pv2::ERROR_CODE_INCOMPATIBLE_ROOT;
    case ReplayMiss::OffTreeAmount:
      return pv2::ERROR_CODE_OFF_TREE_AMOUNT;
    case ReplayMiss::RulesUnconfirmed:
      return pv2::ERROR_CODE_RULES_UNCONFIRMED;
    case ReplayMiss::UnsupportedFeature:
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
  return bs::handleEnvelope(frame, noResidentServices());
}

EnvelopeResult handleEnvelope(const std::string& frame, const HostServices& services) {
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
    // T6: receipt-to-action timing. Covers validation, reconstruction,
    // blueprint lookup, and solve.
    const auto decisionStart = std::chrono::steady_clock::now();

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
    ReplayMiss replayMiss = ReplayMiss::None;
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
        pv2::DecisionResponse response = mapBlueprintResponse(request, lookup.row);
        stampSolveTime(response, decisionStart);
        return respondWithDecision(requestId, std::move(response));
      }

      case pv2::SOLVER_MODE_RESOLVING: {
        const ResolveResult resolve =
            lookupResolving(services, recon, request.options().solve_time_budget_ms());
        if (resolve.outcome == ResolveOutcome::Unsupported) {
          const pv2::ErrorCode code = mapMissToError(resolve.miss);
          return respondWithDecision(requestId,
                                     errorResponse(code, messageFor(code), retryableFor(code)));
        }
        pv2::DecisionResponse response = mapResolvedResponse(request, resolve.row, resolve.outcome);
        stampSolveTime(response, decisionStart);
        return respondWithDecision(requestId, std::move(response));
      }

      case pv2::SOLVER_MODE_AUTOMATIC: {
        // Blueprint first; on miss, the terminal-only resolver; if the
        // resolver also misses, the labeled operational fallback.
        const BlueprintLookup lookup = lookupBlueprint(services, recon);
        if (lookup.hit) {
          pv2::DecisionResponse response = mapBlueprintResponse(request, lookup.row);
          stampSolveTime(response, decisionStart);
          return respondWithDecision(requestId, std::move(response));
        }
        const ResolveResult resolve =
            lookupResolving(services, recon, request.options().solve_time_budget_ms());
        if (resolve.outcome != ResolveOutcome::Unsupported) {
          pv2::DecisionResponse response =
              mapResolvedResponse(request, resolve.row, resolve.outcome);
          stampSolveTime(response, decisionStart);
          return respondWithDecision(requestId, std::move(response));
        }
        pv2::DecisionResponse response =
            mapOperationalFallbackResponse(request, operationalFallbackDecision(request));
        stampSolveTime(response, decisionStart);
        return respondWithDecision(requestId, std::move(response));
      }

      case pv2::SOLVER_MODE_HEURISTIC: {
        // Map the request to a heuristic context and decide.
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
        pv2::DecisionResponse response = mapHeuristicResponse(request, answer);
        stampSolveTime(response, decisionStart);
        return respondWithDecision(requestId, std::move(response));
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
  // Field 1 (protocol_version) is a uint32 varint at wire tag 0x08.
  // We decode just this one field without parsing the payload so the
  // host can route by version.
  if (frame.empty() || static_cast<std::uint8_t>(frame[0]) != 0x08)
    return std::nullopt;
  const auto decoded = decodeVarint32(frame, 1);
  if (!decoded)
    return std::nullopt;
  return decoded->first;
}

}  // namespace bs
