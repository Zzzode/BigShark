// fuzz_v1_proto.cpp — fuzz target for RFC 0002 untrusted-input paths.
//
// Feeds arbitrary bytes to the frame decoder, Envelope parser, semantic
// validator, and request mapper. Both framing-level byte streams and
// mutations of serialized structured decision envelopes are exercised (see
// fuzz_seeds.hpp). The solver itself is intentionally not invoked: its time
// budget belongs to validated inputs, not the fuzz corpus.
//
// Build:
//   cmake --preset asan -DBIGSHARK_ENABLE_FUZZ=ON
//   cmake --build --preset asan --target fuzz_v1_proto
// Run (Apple Clang standalone driver is the default; Homebrew LLVM with
// -DBIGSHARK_FUZZ_LIBFUZZER=ON enables coverage-guided libFuzzer). The
// standalone driver mutates the built-in structured seeds in
// fuzz_seeds.hpp; corpus files passed as arguments are replayed directly:
//   ./build/asan/proto/fuzz_v1_proto
//   ./build/asan/proto/fuzz_v1_proto ./a.bin ./b.bin
#include <bs/v1_protocol.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "../src/protocol/v1_mappers.hpp"
#include "fuzz_seeds.hpp"

namespace {

namespace pv = bs::v1::pv;

bool requestIsValid(const pv::DecisionRequest& request, unsigned minor) {
  bs::v1::ValidationReport report;
  return bs::v1::validateDecisionRequest(request, report, minor) == pv::ERROR_CODE_UNSPECIFIED;
}

// RFC 0008 stage 5 symmetric presence oracles: the per-minor guarantee
// vocabularies are disjoint on every successful strategy response.
void checkGuaranteeVocabulary(unsigned minor, const pv::SolverMetadata& solver) {
  if (minor == 2) {
    if (!solver.has_guarantee_level() || solver.has_guarantee())
      __builtin_trap();
  } else if (solver.has_guarantee_level()) {
    __builtin_trap();
  }
}

void exerciseEnvelopeBytes(const std::string& frame) {
  // Every envelope the engine answers must parse back, and a Strategy
  // (including a strategic fold) is only legal when the request itself passed
  // validation. Rejections must be EngineErrors, never folds.
  const bs::v1::EnvelopeResult result = bs::v1::handleEnvelope(frame);
  if (result.outcome != bs::v1::EnvelopeOutcome::Respond)
    return;

  pv::Envelope response;
  if (!response.ParseFromString(result.response))
    __builtin_trap();
  if (response.payload_case() != pv::Envelope::kDecisionResponse)
    return;
  const pv::DecisionResponse& decision = response.decision_response();
  if (decision.has_strategy() || decision.has_expanded_strategy()) {
    pv::Envelope requestEnvelope;
    if (!requestEnvelope.ParseFromString(frame))
      __builtin_trap();
    if (requestEnvelope.payload_case() != pv::Envelope::kDecisionRequest)
      __builtin_trap();
    const unsigned requestMinor =
        requestEnvelope.protocol_minor() <= 2 ? requestEnvelope.protocol_minor() : 0;
    if (!requestIsValid(requestEnvelope.decision_request(), requestMinor))
      __builtin_trap();
    // Minor 0 may only return the v1.0 Strategy; the expanded oneof and the
    // new enums are minor-1+.
    if (requestEnvelope.protocol_minor() == 0 && decision.has_expanded_strategy())
      __builtin_trap();
    // Minor 2 always returns expanded_strategy, never the 5-capped Strategy.
    if (requestEnvelope.protocol_minor() == 2 && decision.has_strategy())
      __builtin_trap();
    if (decision.has_strategy())
      checkGuaranteeVocabulary(requestMinor, decision.strategy().solver());
    if (decision.has_expanded_strategy()) {
      const pv::ExpandedStrategy& expanded = decision.expanded_strategy();
      if (expanded.actions_size() < 1 || expanded.actions_size() > 32 || !expanded.has_solver())
        __builtin_trap();
      checkGuaranteeVocabulary(requestMinor, expanded.solver());
      // A minor-2 engine answer is never labeled operational_fallback: a
      // validated request always has a real source.
      if (requestEnvelope.protocol_minor() == 2 &&
          expanded.solver().guarantee_level() == "operational_fallback")
        __builtin_trap();
      double sum = 0.0;
      for (const pv::ActionPolicy& policy : expanded.actions()) {
        if (!std::isfinite(policy.probability()) || policy.probability() < 0.0)
          __builtin_trap();
        sum += policy.probability();
      }
      if (!std::isfinite(sum) || std::abs(sum - 1.0) > 1e-9)
        __builtin_trap();
    }
  } else if (!decision.has_error()) {
    __builtin_trap();
  }
  // The error/strategy response must always fit the frame limit.
  if (result.response.size() > bs::v1::kMaxFrameBytes)
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string input(reinterpret_cast<const char*>(data), size);

  // Path 1: drive the incremental frame reader, including corrupt prefixes.
  try {
    std::istringstream stream(input);
    bs::v1::FrameReader reader(stream);
    std::string frame;
    int frames = 0;
    while (reader.read(frame) == bs::v1::FrameReader::Status::Complete) {
      exerciseEnvelopeBytes(frame);
      if (++frames > 16)
        break;  // defensive bound on frames synthesized from one input
    }
  } catch (...) {
    __builtin_trap();
  }

  // Path 2: treat the whole input as one Envelope directly.
  try {
    pv::Envelope envelope;
    if (envelope.ParseFromString(input)) {
      if (envelope.has_decision_request()) {
        bs::Ctx context;
        bs::v1::ValidationReport report;
        const unsigned minor = envelope.protocol_minor() <= 2 ? envelope.protocol_minor() : 0;
        (void)bs::v1::validateAndMap(envelope.decision_request(), report, context, minor);
      }
      exerciseEnvelopeBytes(input);
    }
  } catch (...) {
    __builtin_trap();
  }

  return 0;
}

namespace bs::v1::fuzz {

std::vector<std::string> structuredFuzzSeeds() {
  return structuredSeeds();
}

}  // namespace bs::v1::fuzz
