// RFC 0015: benchmark-private sampled, frozen deviation measurement.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/frontier.hpp>
#include <bs/seat_policy.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace bs::benchmarks::preflop {

// The query sees only the existing information key and the public action menu.
// nullopt retains the historical check/call/fold profile fallback.
using ProfileQuery = std::function<std::optional<solver::SeatPolicyRow>(
    const solver::InformationKey&, std::span<const poker::Action>)>;

struct World {
  std::uint64_t seed = 0;
  std::vector<std::array<int, 2>> holes;
  std::vector<std::array<int, 3>> flops;
};

struct Counts {
  std::size_t profile_queries = 0;
  std::size_t profile_fallbacks = 0;
  std::size_t response_queries = 0;
  std::size_t response_misses = 0;
};

struct FrozenAction {
  std::vector<poker::Action> menu;
  std::size_t chosen = 0;
  bool operator==(const FrozenAction&) const = default;
};

struct LearnedResponse {
  std::size_t seat = 0;
  std::map<solver::InformationKey, FrozenAction> actions;
  std::vector<std::uint64_t> learn_seeds;
  Counts counts;
};

struct Estimate {
  double mean = 0.0;
  // Undefined for fewer than two worlds; represented by NaN rather than zero.
  double standard_error = 0.0;
  double ci95_low = 0.0;
  double ci95_high = 0.0;
};

struct SeatConfirmation {
  double profile_value = 0.0;
  double deviation_value = 0.0;
  Estimate gain;
  Counts counts;
  std::vector<double> paired_gains;
};

struct Measurement {
  std::uint64_t learn_seed = 0;
  std::uint64_t confirm_seed = 0;
  std::size_t worlds_per_phase = 0;
  std::vector<LearnedResponse> learned;
  std::vector<SeatConfirmation> seats;
  Estimate summed_gain;
};

std::vector<World> sample_worlds(std::size_t seats, std::size_t count, std::size_t flops_per_world,
                                 std::uint64_t seed);
LearnedResponse learn_response(const tree::AbstractTree& tree, const ProfileQuery& profile,
                               const gto::FrontierEvaluator& frontier, std::size_t seat,
                               std::span<const World> worlds);
SeatConfirmation confirm_response(const tree::AbstractTree& tree, const ProfileQuery& profile,
                                  const gto::FrontierEvaluator& frontier,
                                  const LearnedResponse& frozen, std::span<const World> worlds);
Estimate estimate(std::span<const double> samples);
Measurement measure(const tree::AbstractTree& tree, const ProfileQuery& profile,
                    const gto::FrontierEvaluator& frontier, std::size_t deals, std::size_t flops,
                    std::uint64_t seed);

}  // namespace bs::benchmarks::preflop
