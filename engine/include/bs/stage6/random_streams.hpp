// stage6/random_streams.hpp — the single reproducibility root for stage-6
// training and measurement.
//
// Every random value in step 5 (deviation estimation), step 7 (MCCFR) and
// step 8 (the confirmatory table) is a deterministic function of one master
// seed and a (purpose, key1, key2) triple. There is no mutable per-hand RNG
// threaded through paired rollouts: a draw is a pure lookup, so paired legs
// share a value whenever their public coordinate is identical and stay
// independent after they diverge.
//
// All randomness is SplitMix64 (bs/prng.hpp). The unit-draw convention is the
// existing top-53-bits / 2^53 one used by the simulator and estimator.
#pragma once

#include <bs/prng.hpp>
#include <cstddef>
#include <cstdint>

namespace bs::stage6 {

enum class StreamPurpose : std::uint64_t {
  Phase1LearnJointDeal,
  Phase1OpponentAction,
  Phase1BoardRunout,
  Phase2ConfirmJointDeal,
  Phase2OpponentAction,
  Phase2BoardRunout,
  TrainJointDeal,
  TrainRootBoard,
  TrainOpponentAction,
  TrainBoardRunout,
};

// Pairwise-distinct, nonzero, frozen domain tags. They are ASCII markers for
// auditability; changing one is a declared stream-identity change.
std::uint64_t stream_domain_constant(StreamPurpose purpose);

// One canonical derivation: mix64 over the master seed and role-tagged domain,
// then the two keys. Deterministic and order sensitive in every argument.
bs::SplitMix64 derive_stream(std::uint64_t master_seed, StreamPurpose purpose, std::uint64_t key1,
                             std::uint64_t key2);

// Top-53-bits / 2^53 uniform unit draw, matching simulator/estimator convention.
double unit_draw(bs::SplitMix64& rng);

// Convenience: the keyed opponent-action unit draw at a public node.
std::uint64_t opponent_decision_seed(std::uint64_t master_seed, StreamPurpose purpose,
                                     std::uint64_t hand_id, std::uint64_t public_history_hash,
                                     std::size_t seat);

}  // namespace bs::stage6
