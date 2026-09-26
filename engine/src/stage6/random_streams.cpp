#include <array>
#include <bs/stage6/random_streams.hpp>

namespace bs::stage6 {

namespace {

// SplitMix64 finalizer used as the one-way mix primitive.
std::uint64_t mix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

constexpr std::array<std::uint64_t, 10> kDomains = {
    0x533650316a6f696eULL,  // S P 1 join
    0x533650316f707031ULL,  // S P 1 opp1
    0x53365031626f6172ULL,  // S P 1 boar
    0x533650326a6f696eULL,  // S P 2 join
    0x533650326f707031ULL,  // S P 2 opp1
    0x53365032626f6172ULL,  // S P 2 boar
    0x533674726a6f696eULL,  // S   t r join
    0x53367472626f6172ULL,  // S   t r boar
    0x533674726f707031ULL,  // S   t r opp1
    0x5336747272756e31ULL,  // S   t r run1
};

}  // namespace

std::uint64_t stream_domain_constant(StreamPurpose purpose) {
  const auto index = static_cast<std::size_t>(purpose);
  return kDomains.at(index);
}

bs::SplitMix64 derive_stream(std::uint64_t master_seed, StreamPurpose purpose, std::uint64_t key1,
                             std::uint64_t key2) {
  // The two keys play DISTINCT roles (e.g. iteration vs traverser seat). A bare
  // XOR of their mixes is commutative and would make (key1=a,key2=b)
  // bit-identical to (key1=b,key2=a), reusing one sweep's streams for another.
  // Order-sensitize by chaining key1 through an extra mix round.
  const std::uint64_t state =
      mix64(master_seed ^ stream_domain_constant(purpose)) ^ mix64(mix64(key1)) ^ mix64(key2);
  return bs::SplitMix64(state);
}

double unit_draw(bs::SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
}

std::uint64_t opponent_decision_seed(std::uint64_t master_seed, StreamPurpose purpose,
                                     std::uint64_t hand_id, std::uint64_t public_history_hash,
                                     std::size_t seat) {
  bs::SplitMix64 stream =
      derive_stream(master_seed, purpose, hand_id,
                    mix64(public_history_hash ^ (static_cast<std::uint64_t>(seat) + 1)));
  return stream.next_u64();
}

}  // namespace bs::stage6
