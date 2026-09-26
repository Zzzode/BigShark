#include <bs/stage6/crn_streams.hpp>

namespace bs::stage6 {

std::uint64_t crn_u64(std::uint64_t seed, std::uint64_t public_token, BrStreamPurpose purpose,
                      std::size_t seat, std::uint64_t counter) {
  std::uint64_t x = mix64(seed ^ kBrSeedDomain);
  x = mix64(x ^ public_token);
  x = mix64(x ^ static_cast<std::uint64_t>(purpose));
  x = mix64(x ^ (static_cast<std::uint64_t>(seat) + 1));
  x = mix64(x ^ (counter + 0x9e3779b97f4a7c15ULL));
  return bs::SplitMix64(x).next_u64();
}

double crn_unit(std::uint64_t seed, std::uint64_t public_token, BrStreamPurpose purpose,
                std::size_t seat, std::uint64_t counter) {
  return static_cast<double>(crn_u64(seed, public_token, purpose, seat, counter) >> 11) * 0x1.0p-53;
}

bs::SplitMix64 crn_deal_rng(std::uint64_t seed) {
  return bs::SplitMix64(mix64(seed ^ kBrSeedDomain));
}

std::uint64_t seed_list_hash(std::span<const std::uint64_t> seeds) noexcept {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  auto eat = [&](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<unsigned char>((value >> shift) & 0xffULL);
      hash *= 0x100000001b3ULL;
    }
  };
  eat(seeds.size());
  for (std::uint64_t seed : seeds)
    eat(seed);
  return hash;
}

}  // namespace bs::stage6
