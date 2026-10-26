// fuzz_harness.cpp — libFuzzer entry point for the framed engine protocol.
//
// RFC 0034: the harness and the standalone driver are separate translation
// units so both link configurations work without a conditional main. This TU
// defines only LLVMFuzzerTestOneInput; fuzz_standalone_driver.cpp adds main
// when BIGSHARK_FUZZ_LIBFUZZER is OFF. Inputs are exercised two ways: fed
// directly to bs::handleEnvelope (the production entry) and fed as
// ULEB128-framed payloads through the same FrameReader the host uses.
//
// The oracle is corpus-scoped. A fixed negative corpus (framing garbage,
// open-enum attacks, a response frame, and wrong transport versions) must
// produce only a clean error response or a closed-stream indication, never a
// strategy decision. The random mutation campaign asserts only no crash, no
// escaping throw, and no hang: a well-formed HEURISTIC request without
// resident services legitimately receives the labeled operational fallback.
#include <bs/protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include "fuzz_seeds.hpp"

namespace {

namespace pe = ::bigshark::engine;

// Every byte string the negative oracle applies to. The framing garbage is
// listed here; the structured negative seeds are collected from fuzz_seeds by
// name so the two never diverge.
const std::unordered_set<std::string>& negative_corpus() {
  static const std::unordered_set<std::string> corpus = [] {
    std::unordered_set<std::string> set{
        std::string(),
        std::string("\x00", 1),
        std::string("\x80", 1),
        std::string("\xff\xff\xff\xff\xff", 5),
        std::string("\x01\x00", 2),
        std::string("\x80\x80\x40", 3),
    };
    for (const std::string& seed : bs::fuzz::negativeFuzzSeeds())
      set.insert(seed);
    return set;
  }();
  return corpus;
}

// True when a parsed response envelope carries a strategy decision, in
// either the compact Strategy arm or the ExpandedStrategy arm.
bool response_is_strategy(const bs::EnvelopeResult& result) {
  if (result.outcome != bs::EnvelopeOutcome::Respond)
    return false;
  pe::Envelope response;
  if (!response.ParseFromString(result.response))
    return false;
  if (!response.has_decision_response())
    return false;
  const pe::DecisionResponse& decision = response.decision_response();
  if (decision.has_strategy())
    return decision.strategy().has_selected_action();
  if (decision.has_expanded_strategy())
    return decision.expanded_strategy().has_selected_action();
  return false;
}

void run_one_payload(std::string_view payload) {
  const bool negative = negative_corpus().find(std::string(payload)) != negative_corpus().end();

  // Direct path: the same handler the host calls.
  const std::string payload_string(payload);
  const bs::EnvelopeResult direct = bs::handleEnvelope(payload_string);
  if (negative && response_is_strategy(direct))
    std::abort();

  // Framed path: wrap the payload in one ULEB128 frame and read it through
  // FrameReader exactly as the host does.
  std::string framed;
  if (bs::writeFrame(framed, std::string(payload))) {
    std::istringstream input(framed);
    bs::FrameReader reader(input);
    std::string decoded;
    const bs::FrameReader::Status status = reader.read(decoded);
    if (status == bs::FrameReader::Status::Complete) {
      if (decoded != payload)
        std::abort();
      const bs::EnvelopeResult framed_result = bs::handleEnvelope(decoded);
      if (negative && response_is_strategy(framed_result))
        std::abort();
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view payload(reinterpret_cast<const char*>(data), size);
  run_one_payload(payload);
  return 0;
}
