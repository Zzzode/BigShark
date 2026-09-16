#include <bs/service.hpp>
#include <bs/v1_protocol.hpp>
#include <exception>
#include <new>
#include <string>

#include "v1_mappers.hpp"

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

constexpr std::size_t kMaxRequestIdBytes = 128;

EnvelopeResult respondWithDecision(const std::string& requestId,
                                   const pv::DecisionResponse& response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv::Envelope envelope;
  envelope.set_protocol_minor(0);
  envelope.set_request_id(requestId);
  *envelope.mutable_decision_response() = response;
  result.response = envelope.SerializeAsString();
  return result;
}

EnvelopeResult respondWithCapabilities(const std::string& requestId,
                                       const pv::GetCapabilitiesResponse& response) {
  EnvelopeResult result;
  result.outcome = EnvelopeOutcome::Respond;
  pv::Envelope envelope;
  envelope.set_protocol_minor(0);
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

}  // namespace

EnvelopeResult handleEnvelope(const std::string& frame) {
  pv::Envelope envelope;
  if (!envelope.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
    return respondWithDecision(
        "", errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "malformed envelope", false));
  }

  const std::string requestId = envelope.request_id();
  if (requestId.empty() || requestId.size() > kMaxRequestIdBytes) {
    return respondWithDecision("", errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                                 "request_id must be 1..128 characters", false));
  }
  if (!isValidUtf8(requestId)) {
    return respondWithDecision(
        "", errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "request_id must be valid UTF-8", false));
  }
  if (envelope.protocol_minor() != 0) {
    return respondWithDecision(requestId,
                               errorResponse(pv::ERROR_CODE_UNSUPPORTED_PROTOCOL,
                                             "only protocol minor 0 is supported", false));
  }

  switch (envelope.payload_case()) {
    case pv::Envelope::kGetCapabilitiesRequest:
      return respondWithCapabilities(requestId, buildCapabilities());

    case pv::Envelope::kGetCapabilitiesResponse:
      return respondWithDecision(
          requestId, errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                   "capability responses are not accepted by the host", false));

    case pv::Envelope::kDecisionResponse:
      return respondWithDecision(
          requestId, errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                   "decision responses are not accepted by the host", false));

    case pv::Envelope::kDecisionRequest:
      break;

    case pv::Envelope::PAYLOAD_NOT_SET:
      return respondWithDecision(requestId,
                                 errorResponse(pv::ERROR_CODE_INVALID_REQUEST,
                                               "envelope requires exactly one payload", false));
  }

  const pv::DecisionRequest& request = envelope.decision_request();
  try {
    ValidationReport report;
    bs::Ctx context;
    const pv::ErrorCode validationCode = validateAndMap(request, report, context);
    if (validationCode != pv::ERROR_CODE_UNSPECIFIED) {
      return respondWithDecision(
          requestId, errorResponse(validationCode, messageFor(validationCode),
                                   retryableFor(validationCode), std::move(report.violations)));
    }
    const bs::Decision decision = bs::decide(context);
    if (decision.action.empty()) {
      return respondWithDecision(
          requestId,
          errorResponse(pv::ERROR_CODE_NO_DECISION, messageFor(pv::ERROR_CODE_NO_DECISION),
                        retryableFor(pv::ERROR_CODE_NO_DECISION)));
    }
    pv::DecisionResponse response = mapDecisionResponse(request, decision);
    return respondWithDecision(requestId, response);
  } catch (const MappingError& error) {
    return respondWithDecision(requestId, errorResponse(error.code(), error.what(),
                                                        error.retryable(), error.violations()));
  } catch (const std::bad_alloc&) {
    return respondWithDecision(requestId,
                               errorResponse(pv::ERROR_CODE_RESOURCE_EXHAUSTED,
                                             messageFor(pv::ERROR_CODE_RESOURCE_EXHAUSTED), true));
  } catch (const std::exception& error) {
    return respondWithDecision(requestId,
                               errorResponse(pv::ERROR_CODE_INTERNAL, error.what(), true));
  }
}

}  // namespace bs::v1
