// RFC 0004 Stage 4 frozen heads-up matrix benchmark (schema_version 3 CSV).
//
// This runner is deliberately INDEPENDENT of the schema-version-2 runners:
// the v2 files stay byte-for-byte unchanged and this CSV is never
// reinterpreted as theirs (RFC 0004 lines 324-326). It exercises the frozen
// Stage 4 release suite pinned by RFC 0004 lines 235-237:
//   - dry (rainbow disconnected), two-tone connected, paired, and monotone
//     flop families;
//   - equal and asymmetric postflop stacks in every SPR class;
//   - effective stack-to-pot ratios 1, 4, and 10 at the flop root;
//   - two weighted small ranges (three combos per side, one cross-blocked
//     pair) and the standard multi-size action abstraction;
//   - every SPR 4/10 fixture fixes both turn and river so the exact modeled
//     best response stays tractable over the full no-limit betting tree, and
//     exactly one SPR-1 fixture keeps a free river to exercise the full
//     public-card chance tree.
//
// Every matrix fixture trains with FULL traversal. The --sampled-coverage
// mode reproduces the reason: it trains the actual SPR-1 fixed and free-river
// games with train_sampled at the pinned budgets and reports how many
// information sets were visited against the full-tree total. External
// sampling leaves rare sets unvisited, and RFC 0004 makes missing coverage a
// failure in quality fixtures. Full traversal enumerates every branch, so its
// policies are complete for their declared game roots and the exact
// information-set best response in HeadsUpTrainer::evaluate() certifies every
// published row. metric_class is therefore EXACT only for a finite full
// traversal row; a failed evaluation is INCOMPLETE, never EXACT. Full
// traversal consumes no PRNG entropy, so full rows are seed-independent: the
// pinned dry SPR-1 fixture is still published for seeds 1, 17, and 43
// (identical rows by construction, delta zero), and every other row uses
// seed 1 as its label.
//
// Gates:
//   - SPR-1 fixed fixtures use the pinned 0.002 normalized-NashConv gate and
//     the free-river fixture the pinned 0.02 gate (RFC 0004 lines 311-315),
//     with the iteration budgets frozen here (12,000 fixed; 500 free) that
//     clear them with margin on the reference machine.
//   - SPR 4/10 are new, larger games. Instead of asserting the small-game
//     0.002 threshold, each such fixture carries a measured regression gate:
//     the final normalized NashConv measured on the reference machine,
//     rounded up with margin and frozen here. The runner additionally
//     requires finite metrics, complete coverage, and strict improvement
//     over the first checkpoint. The measured value is recorded; the gate is
//     a reference-machine reproducibility guard, not an equilibrium claim.
//
// On POSIX each fixture runs in a forked child so the recorded
// getrusage(RUSAGE_SELF) ru_maxrss is that fixture's own process peak. The
// reported peak_rss_bytes is always in bytes (macOS reports bytes directly;
// Linux reports kilobytes and the runner multiplies by 1024). -1 means
// getrusage was unavailable. This target is deliberately NOT registered as a
// CTest because the larger fixtures take several minutes. Build/run it:
//   cmake --build --preset release --target benchmark-heads-up-matrix
// Positional arguments filter fixtures by id substring. Flags:
//   --freeze            measurement mode: print raw final NashConv as the
//                       candidate gate with gate_basis freeze-reference and
//                       status FREEZE; used to rebase measured gates.
//   --sampled-coverage  print sampled-traversal visited-vs-total coverage.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "heads_up_benchmark_support.hpp"

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define BS_MATRIX_POSIX 1
#endif

namespace {

using bs::solver::HeadsUpGame;
using bs::solver::HeadsUpPolicy;
using bs::solver::TrainingLimits;
using bs::solver::TrainingResult;
using bs::solver::TrainingStatus;
using bs::solver::WeightedHand;
using bs_heads_up_benchmark::card;
using bs_heads_up_benchmark::policy_distance;

constexpr int kMatrixVersion = 3;
// Pinned RFC 0004 revision set: full traversal for matrix rows, the pinned
// two-player kSimple average rule and SplitMix64 PRNG revision 1 for sampled
// traversal.
constexpr const char* kAlgorithmRevision = "rfc0004-rev1-full-kSimple-prng1";

// Measurement-freeze mode (--freeze) records raw reference-machine values.
bool g_freeze_mode = false;

// Frozen matched-root profile: limped 10-chip pot (each side contributed 5),
// 5-chip big blind, button on seat 1 so seat 0 acts first postflop. Stacks
// are postflop remaining stacks; SPR is min(stacks) / pot.
constexpr bs::poker::Chips kPot = 10;
constexpr bs::poker::Chips kBigBlind = 5;
constexpr std::size_t kButton = 1;

// The frozen deep fixtures deliberately exceed the RFC default 1 GiB
// solver-accounting budget; the benchmark opts into an explicit larger
// budget (RFC 0004: explicit limits are required for larger runs). The
// charge is conservative accounting, not allocated memory.
TrainingLimits matrix_limits() {
  TrainingLimits value;
  value.max_nodes = 5000000000ULL;
  value.max_information_sets = 20000000;
  value.max_bytes = std::size_t{8} << 30;
  value.time = std::chrono::minutes{30};
  return value;
}

struct Fixture {
  const char* id;
  const char* family;
  int spr;
  bool symmetric_stacks;
  std::array<int, 3> flop;
  std::array<bs::poker::Chips, 2> stacks;
  bool free_river;
  bool sampled;
  std::vector<std::uint64_t> seeds;
  std::vector<std::uint64_t> checkpoints;
  // NaN means "no pinned small-game gate": use the frozen measured_gate.
  double pinned_gate;
  // Frozen reference-machine gate for SPR 4/10 rows (see file header).
  double measured_gate;
  const char* gate_basis;
};

// Three weighted combos per side. AcAd/AcAs cross-block the ace of clubs, so
// one of the nine joint pairs is incompatible and the joint dealer drops it
// (eight compatible deals with non-uniform weights).
std::array<std::vector<WeightedHand>, 2> matrix_ranges() {
  return {
      std::vector<WeightedHand>{
          {{card("Ac"), card("Ad")}, 2},
          {{card("8c"), card("8d")}, 3},
          {{card("Kc"), card("Kd")}, 4},
      },
      std::vector<WeightedHand>{
          {{card("Ac"), card("As")}, 5},
          {{card("Tc"), card("Td")}, 7},
          {{card("Ah"), card("Kh")}, 6},
      },
  };
}

// Stable 64-bit FNV-1a token over the ordered range card ids and integer
// weights (side, then hand, then the two card ids and the weight). All matrix
// fixtures share one range set, so they share one digest; the token still
// changes if that ordered set ever does.
void fnv_feed(std::uint64_t& hash, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    hash ^= (value >> shift) & 0xffULL;
    hash *= 0x100000001b3ULL;
  }
}

std::string range_digest() {
  const std::array<std::array<std::tuple<const char*, const char*, int>, 3>, 2> declared = {{
      {{{"Ac", "Ad", 2}, {"8c", "8d", 3}, {"Kc", "Kd", 4}}},
      {{{"Ac", "As", 5}, {"Tc", "Td", 7}, {"Ah", "Kh", 6}}},
  }};
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::size_t side = 0; side < 2; ++side)
    for (const auto& [first, second, weight] : declared[side]) {
      fnv_feed(hash, static_cast<std::uint64_t>(card(first)));
      fnv_feed(hash, static_cast<std::uint64_t>(card(second)));
      fnv_feed(hash, static_cast<std::uint64_t>(weight));
    }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

HeadsUpGame make_game(const Fixture& fixture, int fixed_turn, int fixed_river) {
  HeadsUpGame game;
  // Root validity plus every board/runout-vs-range collision is rejected in
  // the HeadsUpTrainer constructor (reserve_card), so a collision aborts the
  // fixture by construction rather than silently training a bad game.
  game.root = {fixture.flop, fixture.stacks, {kBigBlind, kBigBlind}, kPot, kBigBlind, kButton};
  game.ranges = matrix_ranges();
  game.fixed_runout = {fixed_turn, fixture.free_river ? std::optional<int>{} : fixed_river};
  return game;
}

struct Measurement {
  bool complete = false;
  bool finite = false;
  TrainingResult result;
  double exploitability_pot = std::numeric_limits<double>::quiet_NaN();
  double elapsed_ms = 0;
  std::size_t missing_information_sets = 0;
  const char* metric_class = "INCOMPLETE";
};

// Full-tree information sets absent from policy. One full traversal visits
// every legal key of the declared game, so its key set is the denominator.
std::size_t count_missing(const HeadsUpGame& game, const HeadsUpPolicy& policy) {
  const bs::solver::HeadsUpTrainer enumerator(game);
  const TrainingResult complete = enumerator.train(1, matrix_limits());
  std::size_t missing = 0;
  for (const auto& entry : complete.policy.rows())
    if (policy.rows().find(entry.first) == policy.rows().end())
      ++missing;
  return missing;
}

Measurement measure(const HeadsUpGame& game, bool sampled, std::uint64_t seed,
                    std::uint64_t iterations) {
  const bs::solver::HeadsUpTrainer trainer(game);
  const auto started = std::chrono::steady_clock::now();
  Measurement measurement;
  TrainingResult trained = sampled ? trainer.train_sampled(iterations, seed, matrix_limits())
                                   : trainer.train(iterations, matrix_limits());
  measurement.complete = trained.status == TrainingStatus::Complete;
  measurement.result = std::move(trained);
  // evaluate() throws on any missing/incompatible policy row, so a returned
  // value means the policy is complete for every reachable information set of
  // the fixture's game root, ranges, and runout policy.
  try {
    const auto evaluated = trainer.evaluate(measurement.result.policy, matrix_limits());
    measurement.finite = measurement.complete && std::isfinite(evaluated.exploitability_pot) &&
                         evaluated.exploitability_pot >= 0;
    if (measurement.finite) {
      measurement.exploitability_pot = evaluated.exploitability_pot;
      measurement.missing_information_sets = 0;
      measurement.metric_class = sampled ? "SAMPLED-EXACT" : "EXACT";
    }
  } catch (const std::exception& error) {
    std::cerr << "evaluation failed: " << error.what() << '\n';
    measurement.finite = false;
    measurement.metric_class = "INCOMPLETE";
    try {
      measurement.missing_information_sets = count_missing(game, measurement.result.policy);
    } catch (const std::exception& inner) {
      std::cerr << "missing-set enumeration failed: " << inner.what() << '\n';
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  measurement.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
  return measurement;
}

// This fixture process's peak resident set size in bytes. macOS reports
// ru_maxrss in bytes; Linux reports kilobytes and is normalized to bytes.
// -1 when getrusage is unavailable.
std::int64_t peak_rss_bytes() {
#if defined(BS_MATRIX_POSIX)
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return -1;
  const auto value = static_cast<std::int64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
  return value;
#else
  return value * 1024;
#endif
#else
  return -1;
#endif
}

void print_header() {
  std::cout << std::setprecision(12);
  std::cout << "matrix_version,fixture_id,flop_family,spr,symmetric_stacks,combos_oop,combos_ip,"
               "traversal,runout,algorithm_revision,range_digest,seed,checkpoint,iterations,"
               "information_sets,missing_information_sets,nodes,accounted_bytes,peak_rss_bytes,"
               "elapsed_ms,first_exploitability_pot,metric_class,exploitability_pot,quality_gate,"
               "gate_basis,repeat_delta,coverage,held_out_seeds,status\n";
}

void print_row(const Fixture& fixture, const std::string& digest, std::uint64_t seed,
               std::size_t checkpoint_index, const Measurement& measured,
               double first_exploitability, double gate, const char* gate_basis,
               double repeat_delta, const char* status) {
  std::cout << kMatrixVersion << ',' << fixture.id << ',' << fixture.family << ',' << fixture.spr
            << ',' << (fixture.symmetric_stacks ? "equal" : "asymmetric") << ",3,3,"
            << (fixture.sampled ? "sampled" : "full") << ','
            << (fixture.free_river ? "free" : "fixed") << ',' << kAlgorithmRevision << ',' << digest
            << ',' << seed << ',' << (checkpoint_index + 1) << ','
            << measured.result.completed_iterations << ',' << measured.result.information_sets
            << ',' << measured.missing_information_sets << ',' << measured.result.nodes << ','
            << measured.result.accounted_bytes << ',' << peak_rss_bytes() << ','
            << measured.elapsed_ms << ',';
  if (checkpoint_index == 0)
    std::cout << ',';
  else
    std::cout << first_exploitability << ',';
  std::cout << measured.metric_class << ',' << measured.exploitability_pot << ',' << gate << ','
            << gate_basis << ',';
  if (checkpoint_index + 1 == fixture.checkpoints.size())
    std::cout << repeat_delta;
  std::cout << ',' << (measured.finite ? "complete" : "incomplete") << ",none," << status << '\n';
}

// Runs one fixture and prints its rows. Returns true when every gate passes.
bool run_fixture(const Fixture& fixture, int fixed_turn, int fixed_river) {
  const HeadsUpGame game = make_game(fixture, fixed_turn, fixed_river);
  const std::string digest = range_digest();
  bool fixture_passed = true;
  for (std::uint64_t seed : fixture.seeds) {
    std::vector<Measurement> measurements;
    measurements.reserve(fixture.checkpoints.size());
    double first_exploitability = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < fixture.checkpoints.size(); ++i) {
      Measurement measured = measure(game, fixture.sampled, seed, fixture.checkpoints[i]);
      if (i == 0)
        first_exploitability = measured.exploitability_pot;
      measurements.push_back(std::move(measured));
    }
    const Measurement& final_measurement = measurements.back();
    // Independent same-build repeat of the pinned final checkpoint.
    Measurement repeated = measure(game, fixture.sampled, seed, fixture.checkpoints.back());
    const double value_delta =
        std::abs(repeated.exploitability_pot - final_measurement.exploitability_pot);
    const double prng_delta = std::abs(static_cast<double>(static_cast<std::int64_t>(
        repeated.result.prng_state - final_measurement.result.prng_state)));
    const double policy_delta =
        policy_distance(final_measurement.result.policy, repeated.result.policy);
    const double repeat_delta = std::max({value_delta, prng_delta, policy_delta});
    const bool deterministic =
        repeat_delta <= 1e-12 &&
        repeated.result.information_sets == final_measurement.result.information_sets;

    const bool pinned = !std::isnan(fixture.pinned_gate);
    double gate;
    const char* gate_basis;
    if (g_freeze_mode) {
      // Freeze output prints the raw final value as the candidate to round.
      gate = final_measurement.exploitability_pot;
      gate_basis = "freeze-reference";
    } else if (pinned) {
      gate = fixture.pinned_gate;
      gate_basis = fixture.gate_basis;
    } else {
      gate = fixture.measured_gate;
      gate_basis = fixture.gate_basis;
    }
    const bool improved =
        final_measurement.finite && final_measurement.exploitability_pot < first_exploitability;
    const bool within_gate =
        !g_freeze_mode && final_measurement.finite && final_measurement.exploitability_pot <= gate;
    const bool passed = g_freeze_mode
                            ? (final_measurement.finite && repeated.finite && deterministic)
                            : (final_measurement.finite && repeated.finite && deterministic &&
                               improved && within_gate);
    fixture_passed = fixture_passed && passed;

    for (std::size_t i = 0; i < measurements.size(); ++i) {
      const bool final_checkpoint = i + 1 == measurements.size();
      const char* status = "MEASURED";
      if (final_checkpoint)
        status = g_freeze_mode ? "FREEZE" : (passed ? "PASS" : "FAIL");
      print_row(fixture, digest, seed, i, measurements[i], first_exploitability, gate, gate_basis,
                repeat_delta, status);
    }
    std::cout.flush();
  }
  return fixture_passed;
}

// Reproduces the sampled-vs-full coverage comparison on the actual matrix
// SPR-1 games at the pinned sampled budgets. Full traversal of one iteration
// visits every legal information set and supplies the denominator.
void print_sampled_coverage_header() {
  std::cout << std::setprecision(12);
  std::cout << "matrix_version,report,fixture_id,runout,seed,traversal,sampled_iterations,"
               "algorithm_revision,range_digest,visited_information_sets,"
               "full_information_sets,missing_information_sets,elapsed_ms,coverage,status\n";
}

bool run_sampled_coverage(const std::vector<Fixture>& fixtures, int fixed_turn, int fixed_river) {
  print_sampled_coverage_header();
  const std::string digest = range_digest();
  struct Case {
    const char* id;
    bool free_river;
    std::uint64_t iterations;
  };
  const Case cases[] = {
      {"matrix3-spr1-dry-equal-fixed", false, 3000000},
      {"matrix3-spr1-monotone-equal-free", true, 100000},
  };
  bool all_measured = true;
  for (const Case& coverage_case : cases) {
    const auto it = std::find_if(fixtures.begin(), fixtures.end(), [&](const Fixture& fixture) {
      return fixture.id == coverage_case.id;
    });
    if (it == fixtures.end())
      return false;
    const Fixture fixture = *it;
    const HeadsUpGame game = make_game(fixture, fixed_turn, fixed_river);
    const bs::solver::HeadsUpTrainer trainer(game);

    const TrainingResult full = trainer.train(1, matrix_limits());
    const auto sampled_started = std::chrono::steady_clock::now();
    const TrainingResult sampled =
        trainer.train_sampled(coverage_case.iterations, 1, matrix_limits());
    const auto finished = std::chrono::steady_clock::now();
    const double sampled_ms =
        std::chrono::duration<double, std::milli>(finished - sampled_started).count();

    std::size_t missing = 0;
    for (const auto& entry : full.policy.rows())
      if (sampled.policy.rows().find(entry.first) == sampled.policy.rows().end())
        ++missing;
    // This mode MEASURES sampled coverage; incomplete coverage is the expected
    // evidence, not a runner failure. The run fails only if training itself
    // did not complete.
    const bool measured =
        full.status == TrainingStatus::Complete && sampled.status == TrainingStatus::Complete;
    all_measured = all_measured && measured;
    std::cout << kMatrixVersion << ",sampled-coverage," << coverage_case.id << ','
              << (coverage_case.free_river ? "free" : "fixed") << ",1,external-sampling,"
              << coverage_case.iterations << ',' << kAlgorithmRevision << ',' << digest << ','
              << sampled.information_sets << ',' << full.information_sets << ',' << missing << ','
              << sampled_ms << ',' << (missing == 0 ? "complete" : "incomplete") << ','
              << "MEASURED" << '\n';
    std::cout.flush();
  }
  return all_measured;
}

void print_usage(const char* program) {
  std::cerr << "usage: " << program
            << " [fixture-id-substring ...] [--freeze] [--sampled-coverage]\n";
}

}  // namespace

int main(int argc, char** argv) {
  using bs::poker::Chips;
  const int fixed_turn = card("3s");
  const int fixed_river = card("5h");

  bool sampled_coverage_mode = false;
  std::vector<std::string> filters;
  for (int arg = 1; arg < argc; ++arg) {
    const std::string argument = argv[arg];
    if (argument == "--freeze") {
      g_freeze_mode = true;
    } else if (argument == "--sampled-coverage") {
      sampled_coverage_mode = true;
    } else if (argument.rfind("--", 0) == 0) {
      std::cerr << "unknown flag: " << argument << '\n';
      print_usage(argv[0]);
      return 2;
    } else {
      filters.push_back(argument);
    }
  }
  if (g_freeze_mode && sampled_coverage_mode) {
    std::cerr << "--freeze and --sampled-coverage are mutually exclusive\n";
    print_usage(argv[0]);
    return 2;
  }

  // Genuinely rainbow disconnected dry board. Card collisions against the
  // ranges and the fixed 3s/5h runout are rejected by the trainer constructor.
  const std::array<int, 3> kDryFlop = {card("Ks"), card("7h"), card("2c")};

  // Frozen catalog. Iteration budgets were tuned on the Apple M5 Pro
  // reference machine (48 GB, macOS 26.5.1) so the whole runner finishes in
  // about ten to twelve minutes; they are part of fixture identity. SPR 4/10
  // measured_gate values are frozen from that run and rounded up with margin.
  const std::vector<Fixture> fixtures = {
      // SPR 1: pinned small-game gates, full traversal. The dry equal fixture
      // is published under all three pinned seeds (full rows are
      // seed-independent); the monotone fixture keeps a free river.
      {"matrix3-spr1-dry-equal-fixed",
       "dry",
       1,
       true,
       kDryFlop,
       {Chips{10}, Chips{10}},
       false,
       false,
       {1, 17, 43},
       {1000, 12000},
       0.002,
       0.0,
       "pinned-rfc0004"},
      {"matrix3-spr1-connected-equal-fixed",
       "two-tone-connected",
       1,
       true,
       {card("9h"), card("8h"), card("4d")},
       {Chips{10}, Chips{10}},
       false,
       false,
       {1},
       {1000, 12000},
       0.002,
       0.0,
       "pinned-rfc0004"},
      {"matrix3-spr1-paired-asym-fixed",
       "paired",
       1,
       false,
       {card("Qd"), card("Qc"), card("6s")},
       {Chips{10}, Chips{20}},
       false,
       false,
       {1},
       {1000, 12000},
       0.002,
       0.0,
       "pinned-rfc0004"},
      {"matrix3-spr1-monotone-equal-free",
       "monotone",
       1,
       true,
       {card("Jh"), card("Th"), card("9h")},
       {Chips{10}, Chips{10}},
       true,
       false,
       {1},
       {100, 500},
       0.02,
       0.0,
       "pinned-rfc0004"},
      // SPR 4: larger games with frozen measured regression gates.
      {"matrix3-spr4-dry-equal-fixed",
       "dry",
       4,
       true,
       kDryFlop,
       {Chips{40}, Chips{40}},
       false,
       false,
       {1},
       {10, 100},
       std::numeric_limits<double>::quiet_NaN(),
       0.16,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr4-connected-asym-fixed",
       "two-tone-connected",
       4,
       false,
       {card("9h"), card("8h"), card("4d")},
       {Chips{40}, Chips{80}},
       false,
       false,
       {1},
       {10, 100},
       std::numeric_limits<double>::quiet_NaN(),
       0.40,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr4-paired-equal-fixed",
       "paired",
       4,
       true,
       {card("Qd"), card("Qc"), card("6s")},
       {Chips{40}, Chips{40}},
       false,
       false,
       {1},
       {10, 100},
       std::numeric_limits<double>::quiet_NaN(),
       0.36,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr4-monotone-asym-fixed",
       "monotone",
       4,
       false,
       {card("Jh"), card("Th"), card("9h")},
       {Chips{40}, Chips{80}},
       false,
       false,
       {1},
       {10, 100},
       std::numeric_limits<double>::quiet_NaN(),
       0.15,
       "measured-m5pro-2026-09-15"},
      // SPR 10: deepest frozen games with frozen measured regression gates.
      {"matrix3-spr10-dry-asym-fixed",
       "dry",
       10,
       false,
       kDryFlop,
       {Chips{100}, Chips{200}},
       false,
       false,
       {1},
       {1, 4},
       std::numeric_limits<double>::quiet_NaN(),
       4.6,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr10-connected-equal-fixed",
       "two-tone-connected",
       10,
       true,
       {card("9h"), card("8h"), card("4d")},
       {Chips{100}, Chips{100}},
       false,
       false,
       {1},
       {1, 4},
       std::numeric_limits<double>::quiet_NaN(),
       5.7,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr10-paired-asym-fixed",
       "paired",
       10,
       false,
       {card("Qd"), card("Qc"), card("6s")},
       {Chips{100}, Chips{200}},
       false,
       false,
       {1},
       {1, 4},
       std::numeric_limits<double>::quiet_NaN(),
       5.8,
       "measured-m5pro-2026-09-15"},
      {"matrix3-spr10-monotone-equal-fixed",
       "monotone",
       10,
       true,
       {card("Jh"), card("Th"), card("9h")},
       {Chips{100}, Chips{100}},
       false,
       false,
       {1},
       {1, 4},
       std::numeric_limits<double>::quiet_NaN(),
       5.5,
       "measured-m5pro-2026-09-15"},
  };

  if (sampled_coverage_mode)
    return run_sampled_coverage(fixtures, fixed_turn, fixed_river) ? 0 : 1;

  print_header();
  std::cout.flush();
  const bool has_filter = !filters.empty();
  bool passed = true;
  for (const Fixture& fixture : fixtures) {
    bool selected = !has_filter;
    for (const std::string& filter : filters)
      selected = selected || std::string(fixture.id).find(filter) != std::string::npos;
    if (!selected)
      continue;
#if defined(BS_MATRIX_POSIX)
    // Fork per fixture so RUSAGE_SELF peak RSS in the child is fixture-local.
    const pid_t child = fork();
    if (child == 0) {
      const bool ok = run_fixture(fixture, fixed_turn, fixed_river);
      std::cout.flush();
      _exit(ok ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
      std::cerr << "waitpid failed for " << fixture.id << '\n';
      return 2;
    }
    passed = passed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    passed = run_fixture(fixture, fixed_turn, fixed_river) && passed;
#endif
  }
  return passed ? 0 : 1;
}
