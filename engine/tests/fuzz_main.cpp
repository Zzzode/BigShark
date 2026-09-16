// fuzz_main.cpp — standalone driver for LLVMFuzzerTestOneInput.
//
// Apple Clang does not ship the libclang_rt.fuzzer runtime, so this driver
// links the same fuzz entry point under AddressSanitizer and
// UndefinedBehaviorSanitizer without libFuzzer. It replays corpus file
// arguments directly and otherwise runs a deterministic bounded mutation
// campaign over both framing seeds and serialized structured decision
// envelopes (fuzz_seeds.hpp). Full coverage-guided fuzzing uses Homebrew LLVM:
//   cmake --preset asan -DBIGSHARK_ENABLE_FUZZ=ON \
//     -DBIGSHARK_FUZZ_LIBFUZZER=ON \
//     -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
#include <algorithm>
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

std::vector<std::string> framingSeeds() {
  return {
      "",
      std::string("\x00", 1),
      std::string("\x80", 1),
      std::string("\xff\xff\xff\xff\xff", 5),
      std::string("\x01\x00", 2),
      std::string("\x80\x80\x40", 3),
  };
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

std::vector<std::string> allSeeds() {
  std::vector<std::string> seeds = framingSeeds();
  for (const std::string& structured : bs::v1::fuzz::structuredFuzzSeeds())
    seeds.push_back(structured);
  return seeds;
}

}  // namespace

int main(int argc, char** argv) {
  int runs = 20'000;
  if (const char* configured = std::getenv("BS_FUZZ_MAX_RUNS"))
    runs = std::max(0, std::atoi(configured));

  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::ifstream file(argv[i], std::ios::binary);
      std::string input((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    }
    std::printf("replayed %d corpus file(s) cleanly\n", argc - 1);
    return 0;
  }

  std::mt19937_64 rng(0xC0FFEEULL);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const std::vector<std::string> seeds = allSeeds();
  std::string current;
  for (int i = 0; i < runs; ++i) {
    if ((i & 0x3ff) == 0 && std::chrono::steady_clock::now() > deadline) {
      runs = i;
      break;
    }
    if (i % 256 == 0)
      current = seeds[rng() % seeds.size()];
    mutate(current, rng);
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(current.data()), current.size());
  }
  std::printf("standalone fuzz campaign completed: %d inputs over %zu seeds cleanly\n", runs,
              seeds.size());
  return 0;
}
