// Shared harness for the RFC 0004 heads-up blueprint quality benchmarks
// (schema_version 2 CSV). The default CTest runner and the manual capacity
// runner exercise the same gates over different checkpoint budgets.
#pragma once

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up_solver.hpp>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bs_heads_up_benchmark {

using bs::solver::HeadsUpGame;
using bs::solver::HeadsUpPolicy;
using bs::solver::HeadsUpTrainer;
using bs::solver::TrainingLimits;
using bs::solver::TrainingResult;

inline int card(std::string_view name) {
  return bs::cardId(std::string(name));
}

inline TrainingLimits limits() {
  TrainingLimits value;
  value.max_nodes = 500000000;
  value.max_information_sets = 20000000;
  value.time = std::chrono::minutes{30};
  return value;
}

// Two combos per side with asymmetric weights, the small one-chip profile.
inline HeadsUpGame weighted_game(std::optional<int> turn, std::optional<int> river) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
  game.fixed_runout = {turn, river};
  return game;
}

struct Case {
  std::string_view name;
  HeadsUpGame game;
  std::vector<std::uint64_t> seeds;
  std::vector<std::uint64_t> checkpoints;
  double maximum_final_exploitability;
  bool sampled;
};

inline Case fixed_full_case() {
  return {"heads-up-fixed-full",
          weighted_game(card("Js"), card("9c")),
          {1},
          {1000, 8192},
          0.002,
          false};
}

inline Case fixed_sampled_case(std::vector<std::uint64_t> checkpoints) {
  return {"heads-up-fixed-sampled",
          weighted_game(card("Js"), card("9c")),
          {1, 17, 43},
          std::move(checkpoints),
          0.002,
          true};
}

inline Case free_river_sampled_case(std::vector<std::uint64_t> checkpoints) {
  // Fixed turn, free river: public chance below every decision.
  return {"heads-up-free-river-sampled",
          weighted_game(card("Js"), std::nullopt),
          {1, 17, 43},
          std::move(checkpoints),
          0.02,
          true};
}

struct Measurement {
  bool finite = false;
  std::uint64_t iterations = 0;
  std::uint64_t prng_state = 0;
  std::size_t information_sets = 0;
  double exploitability_pot = 1;
  double elapsed_ms = 0;
  HeadsUpPolicy policy;
};

inline Measurement measure(const HeadsUpTrainer& trainer, const Case& benchmark_case,
                           std::uint64_t seed, std::uint64_t iterations) {
  const auto started = std::chrono::steady_clock::now();
  TrainingResult result = benchmark_case.sampled ? trainer.train_sampled(iterations, seed, limits())
                                                 : trainer.train(iterations, limits());
  const double exploitability = trainer.evaluate(result.policy, limits()).exploitability_pot;
  const auto finished = std::chrono::steady_clock::now();
  Measurement measurement;
  measurement.finite = result.status == bs::solver::TrainingStatus::Complete &&
                       std::isfinite(exploitability) && exploitability >= 0;
  measurement.iterations = result.completed_iterations;
  measurement.prng_state = result.prng_state;
  measurement.information_sets = result.information_sets;
  measurement.exploitability_pot = exploitability;
  measurement.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
  measurement.policy = std::move(result.policy);
  return measurement;
}

// Maximum absolute difference over every published row probability, in key
// order; a row-count or key mismatch reports infinity so the gate fails.
inline double policy_distance(const HeadsUpPolicy& a, const HeadsUpPolicy& b) {
  if (a.rows().size() != b.rows().size())
    return std::numeric_limits<double>::infinity();
  double distance = 0;
  auto j = b.rows().begin();
  for (auto i = a.rows().begin(); i != a.rows().end(); ++i, ++j) {
    if (i->first != j->first || i->second.probabilities.size() != j->second.probabilities.size())
      return std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < i->second.probabilities.size(); ++k)
      distance =
          std::max(distance, std::abs(i->second.probabilities[k] - j->second.probabilities[k]));
  }
  return distance;
}

inline bool run_case(const Case& benchmark_case) {
  const HeadsUpTrainer trainer(benchmark_case.game);
  bool passed = true;
  for (std::uint64_t seed : benchmark_case.seeds) {
    std::vector<Measurement> measurements;
    measurements.reserve(benchmark_case.checkpoints.size());
    double first = 0;
    for (std::size_t i = 0; i < benchmark_case.checkpoints.size(); ++i) {
      const auto iterations = benchmark_case.checkpoints[i];
      Measurement measurement = measure(trainer, benchmark_case, seed, iterations);
      measurements.push_back(measurement);
      if (i == 0)
        first = measurement.exploitability_pot;
      const bool final_checkpoint = i + 1 == benchmark_case.checkpoints.size();
      std::cout << "2," << benchmark_case.name << ',' << seed << ',' << iterations << ','
                << measurement.information_sets << ',' << measurement.exploitability_pot << ','
                << benchmark_case.maximum_final_exploitability << ',' << measurement.elapsed_ms
                << ',';
      if (final_checkpoint) {
        const Measurement repeated = measure(trainer, benchmark_case, seed, iterations);
        const double repeat_delta =
            std::max(std::abs(repeated.exploitability_pot - measurement.exploitability_pot),
                     std::abs(static_cast<double>(
                         static_cast<std::int64_t>(repeated.prng_state - measurement.prng_state))));
        const double policy_delta = policy_distance(measurement.policy, repeated.policy);
        const bool deterministic = repeat_delta <= 1e-12 && policy_delta <= 1e-12 &&
                                   repeated.information_sets == measurement.information_sets;
        // RFC 0004: the final value must IMPROVE on the first checkpoint.
        const bool converged =
            measurement.exploitability_pot <= benchmark_case.maximum_final_exploitability &&
            measurement.exploitability_pot < first;
        const bool case_passed =
            measurement.finite && repeated.finite && deterministic && converged;
        passed = passed && case_passed;
        std::cout << std::max(repeat_delta, policy_delta) << ',' << (case_passed ? "PASS" : "FAIL");
      } else {
        std::cout << ",MEASURED";
      }
      std::cout << '\n';
    }
  }
  return passed;
}

inline void print_header() {
  std::cout << std::setprecision(12);
  std::cout << "schema_version,case,seed,iterations,information_sets,exploitability_pot,"
               "max_final_exploitability,elapsed_ms,repeat_delta,status\n";
}

}  // namespace bs_heads_up_benchmark
