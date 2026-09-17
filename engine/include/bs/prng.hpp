// prng.hpp — deterministic SplitMix64 used by the protocol decision sampler.
//
// RFC 0005 Stage 6 already pins SplitMix64 as the trainer's sampled-traversal
// PRNG (file-local in heads_up_solver.cpp). The protocol sampler needs its own
// independent stream, so the state is initialized with a fixed domain
// separation constant XORed into the request seed. A blueprint lookup never
// consumes entropy: the distribution is identical for every seed, and exactly
// one draw selects the sampled bucket.
#pragma once

#include <cstdint>

namespace bs {

class SplitMix64 {
 public:
  SplitMix64() = default;
  explicit constexpr SplitMix64(std::uint64_t seed) : state_(seed) {}

  // Distinct, non-zero domain constant for the v1 protocol sampler stream. It
  // must never equal a trainer stream state initialization (the trainer seeds
  // its state with the raw seed).
  static constexpr std::uint64_t kProtocolSamplerDomain = 0x425356312d73616dULL;  // "BSV1-sam"

  static SplitMix64 protocolSampler(std::uint64_t seed) {
    return SplitMix64(seed ^ kProtocolSamplerDomain);
  }

  std::uint64_t next_u64() {
    state_ += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

 private:
  std::uint64_t state_ = 0;
};

}  // namespace bs
