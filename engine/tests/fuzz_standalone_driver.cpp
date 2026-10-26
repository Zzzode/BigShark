// fuzz_standalone_driver.cpp — standalone main for the protocol fuzz target.
//
// RFC 0034: this TU is added to fuzz_proto only when BIGSHARK_FUZZ_LIBFUZZER
// is OFF, so libFuzzer's own main is never duplicated. It (1) replays the
// fixed negative corpus, which the harness rejects as a strategy source,
// (2) pins the legal seed reaching the decision arm, and (3) runs the
// deterministic bounded mutation campaign. Coverage-guided fuzzing links
// fuzz_harness.cpp directly under -fsanitize=fuzzer.
#include <algorithm>
#include <bs/protocol.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fuzz_seeds.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

namespace pe = ::bigshark::engine;

std::vector<std::string> framingGarbage() {
  return {
      std::string(),
      std::string("\x00", 1),
      std::string("\x80", 1),
      std::string("\xff\xff\xff\xff\xff", 5),
      std::string("\x01\x00", 2),
      std::string("\x80\x80\x40", 3),
  };
}

void feed(const std::string& input) {
  LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

void mutate(std::string& value, std::mt19937_64& rng) {
  const int operation = static_cast<int>(rng() % 6);
  if (value.empty() || operation == 0) {
    value.push_back(static_cast<char>(rng() & 0xff));
    return;
  }
  const std::size_t index = rng() % value.size();
  switch (operation) {
    case 1:
      value[index] = static_cast<char>(rng() & 0xff);
      break;
    case 2:
      value.insert(value.begin() + static_cast<std::string::difference_type>(index),
                   static_cast<char>(rng() & 0xff));
      break;
    case 3:
      value.erase(value.begin() + static_cast<std::string::difference_type>(index));
      break;
    case 4:
      value[index] = static_cast<char>(
          std::vector<unsigned char>{0x00, 0x7f, 0x80, 0x81, 0xff, 0x40}[rng() % 6]);
      break;
    default: {
      const std::size_t run = std::min<std::size_t>(64, rng() % 8 + 1);
      for (std::size_t i = 0; i < run && index + i < value.size(); ++i)
        value[index + i] = static_cast<char>(rng() & 0xff);
      break;
    }
  }
  if (value.size() > 1'048'600)
    value.resize(1'048'600);
}

// The legal HEURISTIC seed must produce a decision strategy (it is the seed
// the mutation campaign starts from, so this proves it reaches the arm).
void pin_legal_seed_reaches_decision() {
  pe::Envelope envelope;
  for (const std::string& seed : bs::fuzz::structuredFuzzSeeds()) {
    if (!envelope.ParseFromString(seed) || !envelope.has_decision_request() ||
        envelope.protocol_version() != 3)
      continue;
    if (envelope.request_id() != "valid")
      continue;
    const bs::EnvelopeResult result = bs::handleEnvelope(seed);
    const bool reached_decision = result.outcome == bs::EnvelopeOutcome::Respond &&
                                  envelope.ParseFromString(result.response) &&
                                  envelope.has_decision_response() &&
                                  (envelope.decision_response().has_strategy() ||
                                   envelope.decision_response().has_expanded_strategy());
    if (!reached_decision) {
      std::fprintf(stderr, "legal fuzz seed did not reach the decision arm\n");
      std::abort();
    }
    return;
  }
  std::fprintf(stderr, "legal fuzz seed not found\n");
  std::abort();
}

}  // namespace

int main(int argc, char** argv) {
  int runs = 100'000;
  if (const char* configured = std::getenv("BS_FUZZ_MAX_RUNS"))
    runs = std::max(0, std::atoi(configured));

  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::ifstream file(argv[i], std::ios::binary);
      std::string input((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      feed(input);
    }
    std::printf("replayed %d corpus file(s) cleanly\n", argc - 1);
    return 0;
  }

  // Negative corpus: framing garbage plus structured negatives. The harness
  // aborts if any of them produces a strategy.
  for (const std::string& input : framingGarbage())
    feed(input);
  for (const std::string& input : bs::fuzz::negativeFuzzSeeds())
    feed(input);

  pin_legal_seed_reaches_decision();

  std::mt19937_64 rng(0xC0FFEEULL);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const std::vector<std::string> seeds = bs::fuzz::structuredFuzzSeeds();
  std::string current;
  int completed = 0;
  for (int i = 0; i < runs; ++i) {
    if ((i & 0x3ff) == 0 && std::chrono::steady_clock::now() > deadline) {
      completed = i;
      break;
    }
    if (i % 256 == 0)
      current = seeds[rng() % seeds.size()];
    mutate(current, rng);
    feed(current);
    completed = i + 1;
  }
  std::printf("standalone fuzz campaign completed: %d inputs over %zu seeds cleanly\n", completed,
              seeds.size());
  return 0;
}
