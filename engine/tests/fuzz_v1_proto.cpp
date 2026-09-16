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
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "../src/protocol/v1_mappers.hpp"
#include "fuzz_seeds.hpp"

namespace {

namespace pv = bs::v1::pv;

bool requestIsValid(const pv::DecisionRequest& request) {
  bs::v1::ValidationReport report;
  return bs::v1::validateDecisionRequest(request, report) == pv::ERROR_CODE_UNSPECIFIED;
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
  if (decision.has_strategy()) {
    pv::Envelope requestEnvelope;
    if (!requestEnvelope.ParseFromString(frame))
      __builtin_trap();
    if (requestEnvelope.payload_case() != pv::Envelope::kDecisionRequest ||
        !requestIsValid(requestEnvelope.decision_request()))
      __builtin_trap();
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
        (void)bs::v1::validateAndMap(envelope.decision_request(), report, context);
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
