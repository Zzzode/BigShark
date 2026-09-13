#include <algorithm>
#include <bs/eval.hpp>
#include <bs/range.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "gto/multistreet_cfr.h"

namespace {

struct WeightedHand {
  std::string_view first;
  std::string_view second;
  float weight;
};

struct BenchmarkCase {
  std::string_view name;
  std::vector<int> flop;
  bs::Range ip_range;
  bs::Range oop_range;
  bs::gto::MultiStreetOptions options;
  std::vector<int> checkpoints;
  double maximum_final_exploitability;
};

struct Measurement {
  bool ok = false;
  int iterations = 0;
  int ip_combos = 0;
  int oop_combos = 0;
  std::size_t information_sets = 0;
  double value_to_ip = 0;
  double exploitability_pot = 1;
  double elapsed_ms = 0;
};

int Card(std::string_view value) {
  return bs::cardId(std::string(value));
}

bs::Range MakeRange(std::initializer_list<WeightedHand> hands) {
  bs::Range range = bs::zeroRange();
  for (const WeightedHand& hand : hands)
    range[bs::comboIndex(Card(hand.first), Card(hand.second))] = hand.weight;
  return range;
}

BenchmarkCase FixedBalancedCase() {
  BenchmarkCase benchmark;
  benchmark.name = "fixed-balanced";
  benchmark.flop = {Card("Kh"), Card("7s"), Card("2d")};
  benchmark.ip_range = MakeRange({
      {"As", "Ad", 1.0F},
      {"2c", "3c", 1.0F},
  });
  benchmark.oop_range = MakeRange({
      {"Ah", "Ac", 1.0F},
      {"9c", "8c", 1.0F},
  });
  benchmark.options.bet_frac = 0.75F;
  benchmark.options.buckets = 0;
  benchmark.options.fixed_turn = Card("Qc");
  benchmark.options.fixed_river = Card("Jd");
  benchmark.options.seed = 1;
  benchmark.checkpoints = {1000, 5000, 20000, 100000};
  benchmark.maximum_final_exploitability = 0.002;
  return benchmark;
}

BenchmarkCase FixedWeightedOverlapCase() {
  BenchmarkCase benchmark;
  benchmark.name = "fixed-weighted-overlap";
  benchmark.flop = {Card("Kh"), Card("7s"), Card("2d")};
  benchmark.ip_range = MakeRange({
      {"As", "Ad", 3.0F},
      {"Ac", "3c", 1.0F},
  });
  benchmark.oop_range = MakeRange({
      {"Ah", "Ac", 5.0F},
      {"9c", "8c", 2.0F},
  });
  benchmark.options.bet_frac = 0.75F;
  benchmark.options.buckets = 0;
  benchmark.options.fixed_turn = Card("Qc");
  benchmark.options.fixed_river = Card("Jd");
  benchmark.options.seed = 1;
  benchmark.checkpoints = {1000, 5000, 20000, 100000};
  benchmark.maximum_final_exploitability = 0.002;
  return benchmark;
}

BenchmarkCase SampledChanceCase() {
  BenchmarkCase benchmark;
  benchmark.name = "sampled-chance-dominant";
  benchmark.flop = {Card("2h"), Card("3h"), Card("4h")};
  benchmark.ip_range = MakeRange({
      {"Ah", "Kh", 1.0F},
  });
  benchmark.oop_range = MakeRange({
      {"9c", "8c", 1.0F},
  });
  benchmark.options.bet_frac = 0.75F;
  benchmark.options.seed = 17;
  benchmark.checkpoints = {1000, 5000, 20000};
  benchmark.maximum_final_exploitability = 0.02;
  return benchmark;
}

Measurement Measure(const BenchmarkCase& benchmark, int iterations) {
  bs::gto::MultiStreetOptions options = benchmark.options;
  options.iterations = iterations;

  const auto started = std::chrono::steady_clock::now();
  bs::gto::MultiStreetSolver solver(benchmark.flop, 100, benchmark.ip_range, benchmark.oop_range,
                                    options);
  const bs::gto::MultiStreetResult result = solver.Solve();
  const auto finished = std::chrono::steady_clock::now();

  Measurement measurement;
  measurement.ok = result.ok;
  measurement.iterations = result.iterations_done;
  measurement.ip_combos = solver.NumCombos(0);
  measurement.oop_combos = solver.NumCombos(1);
  measurement.information_sets = solver.InformationSetCount();
  measurement.value_to_ip = result.value_to_ip;
  measurement.exploitability_pot = result.exploitability_pot;
  measurement.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
  return measurement;
}

bool IsFinite(const Measurement& measurement) {
  return measurement.ok && std::isfinite(measurement.value_to_ip) &&
         std::isfinite(measurement.exploitability_pot) && measurement.exploitability_pot >= 0;
}

double ReproducibilityDelta(const Measurement& first, const Measurement& second) {
  return std::max(std::fabs(first.value_to_ip - second.value_to_ip),
                  std::fabs(first.exploitability_pot - second.exploitability_pot));
}

}  // namespace

int main() {
  const std::vector<BenchmarkCase> cases = {
      FixedBalancedCase(),
      FixedWeightedOverlapCase(),
      SampledChanceCase(),
  };

  std::cout << std::setprecision(12);
  std::cout << "schema_version,case,seed,iterations,ip_combos,oop_combos,information_sets,"
               "value_to_ip,exploitability_pot,max_final_exploitability,elapsed_ms,"
               "repeat_delta,status\n";

  bool benchmarkPassed = true;
  for (const BenchmarkCase& benchmark : cases) {
    std::vector<Measurement> measurements;
    measurements.reserve(benchmark.checkpoints.size());
    for (int iterations : benchmark.checkpoints)
      measurements.push_back(Measure(benchmark, iterations));

    const Measurement& first = measurements.front();
    const Measurement& final = measurements.back();
    const Measurement repeatedFinal = Measure(benchmark, final.iterations);
    const double repeatDelta = ReproducibilityDelta(final, repeatedFinal);
    const bool deterministic =
        repeatDelta <= 1e-12 && final.information_sets == repeatedFinal.information_sets;
    const bool converged = final.exploitability_pot <= benchmark.maximum_final_exploitability &&
                           final.exploitability_pot <= first.exploitability_pot;

    bool casePassed = deterministic && converged && IsFinite(repeatedFinal);
    for (const Measurement& measurement : measurements)
      casePassed = casePassed && IsFinite(measurement);
    benchmarkPassed = benchmarkPassed && casePassed;

    for (std::size_t index = 0; index < measurements.size(); ++index) {
      const Measurement& measurement = measurements[index];
      const bool isFinal = index + 1 == measurements.size();
      std::cout << "1," << benchmark.name << ',' << benchmark.options.seed << ','
                << measurement.iterations << ',' << measurement.ip_combos << ','
                << measurement.oop_combos << ',' << measurement.information_sets << ','
                << measurement.value_to_ip << ',' << measurement.exploitability_pot << ','
                << benchmark.maximum_final_exploitability << ',' << measurement.elapsed_ms << ',';
      if (isFinal)
        std::cout << repeatDelta;
      std::cout << ',' << (isFinal ? (casePassed ? "PASS" : "FAIL") : "MEASURED") << '\n';
    }
  }

  return benchmarkPassed ? 0 : 1;
}
