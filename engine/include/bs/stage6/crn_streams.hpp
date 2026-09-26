// stage6/crn_streams.hpp — content-addressed common-random-number streams.
//
// Paired best=true/false rollouts and sibling deviation subtrees must draw the
// SAME joint deal, runout card, and opponent action whenever they are on the
// same public node, and diverge independently once their public paths differ.
// A sequentially-consumed RNG cannot do that (draw order shifts once the legs
// branch), so every random value is instead a PURE FUNCTION of
// (seed, public-node token, purpose, seat, counter). Reordering calls does not
// change a value, and reconvergent nodes re-pair automatically. The streams
// deliberately contain neither the traverser index nor any policy identity, so
// the per-traverser runs and candidate-vs-baseline comparisons share draws on
// their common public spine.
#pragma once

#include <bs/prng.hpp>
#include <bs/stage6/infoset_key.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bs::stage6 {

enum class BrStreamPurpose : std::uint64_t {
  Deal = 1,
  RunoutCard = 2,
  OpponentAction = 3,
  TraverserProfileAction = 4,
  PolicyDecision = 5,
};

// Domain constants, pairwise distinct and nonzero (statically asserted in the
// test). Changing one changes every derived stream.
constexpr std::uint64_t kBrSeedDomain = 0x53362d62722d7365ULL;  // "S6-br-se"

// One SplitMix64 finalizer (the same function inside SplitMix64), exposed so
// stream derivation is a visible composition of mix steps.
constexpr std::uint64_t mix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// A raw 64-bit draw at (seed, public_token, purpose, seat, counter). Pure and
// order-independent. `public_token` is the hash of the current public node
// (0 for the pre-deal root). `seat` is the seat whose stochastic action/card
// this draw belongs to (0..9); traverser-enumerated actions do not draw.
std::uint64_t crn_u64(std::uint64_t seed, std::uint64_t public_token, BrStreamPurpose purpose,
                      std::size_t seat, std::uint64_t counter);

// A unit [0,1) draw: top 53 bits * 2^-53, matching the simulator convention.
double crn_unit(std::uint64_t seed, std::uint64_t public_token, BrStreamPurpose purpose,
                std::size_t seat, std::uint64_t counter);

// The master RNG for one hand's JOINT DEAL. Deterministic in the seed only, so
// best/false legs and every per-traverser run at that seed receive identical
// joint holes regardless of action order.
bs::SplitMix64 crn_deal_rng(std::uint64_t seed);

// Order-dependent digest of a seed list (FNV-1a over the length-framed list),
// frozen into a run's lock so a swapped/edited learn or confirm list changes
// the declared identity.
std::uint64_t seed_list_hash(std::span<const std::uint64_t> seeds) noexcept;

}  // namespace bs::stage6
