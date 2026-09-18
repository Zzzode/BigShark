// RFC 0006 experimental multiway joint-deal sampling.
//
// The contract in RFC 0006 is precise about the one mistake this module must
// not make: deals are sampled jointly, proportional to the product of the
// supplied range weights conditioned on mutual card compatibility. Sampling
// each player's hand independently and then renormalizing would silently
// produce a different distribution, so the joint enumeration here is the
// reference that the sampler is checked against.
//
// This header is deliberately small and side-effect free: it owns the joint
// distribution and nothing about training, so the trainer can be validated
// against it rather than the other way around.
#pragma once

#include <array>
#include <bs/prng.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs::solver {

// One hole-card pair offered for one seat, with its declared weight. A weight
// of zero means the combo is not in the range at all.
struct MultiwayWeightedHand {
  std::array<int, 2> cards;
  double weight = 0;
};

// A joint deal: one compatible hole-card pair per seat.
struct MultiwayDeal {
  std::vector<std::array<int, 2>> hands;
  double weight = 0;  // probability; the enumerated set sums to one
};

// The complete joint support for a set of per-seat ranges. Enumerated
// exhaustively, so it is only usable on small validation games; it exists to
// be the reference the sampler matches.
struct JointDealTable {
  std::vector<MultiwayDeal> deals;
  // Sum of the unnormalized products BEFORE normalization, recorded for
  // diagnostics and for rejection-rate reporting.
  double unnormalized_mass = 0;
  // Seat combinations rejected for card conflicts, so a caller can report how
  // selective the support is instead of guessing.
  std::size_t rejected_conflicts = 0;
};

// Enumerates every mutually compatible joint deal with positive joint weight.
// `ranges` holds one entry per seat, each non-empty. `board` cards, when
// supplied, are excluded from every hand. Throws std::invalid_argument when a
// range is empty, a weight is negative or non-finite, a card id is out of
// range, the board carries duplicates, or the total joint mass is zero (no
// compatible combination exists).
JointDealTable enumerate_joint_deals(const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                                     const std::vector<int>& board = {});

// Draws one joint deal from the SAME distribution the enumeration describes,
// by proposing a combination proportional to the per-seat weights and
// rejecting it when the seats conflict. That rejection-conditional draw is the
// joint distribution RFC 0006 requires; it is deliberately not independent
// per-seat sampling with a last-seat renormalization. Returns the index of the
// drawn deal in `table.deals`.
//
// The caller owns the PRNG stream so a run is reproducible from its seed.
// Throws std::invalid_argument when the table is empty or the ranges do not
// match the table's seat count, and std::runtime_error when `max_attempts`
// consecutive rejections occur (impossible while the support is non-empty, but
// bounded rather than looping forever).
std::size_t sample_joint_deal(const JointDealTable& table,
                              const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                              SplitMix64& rng, std::size_t max_attempts = 10000);

}  // namespace bs::solver
