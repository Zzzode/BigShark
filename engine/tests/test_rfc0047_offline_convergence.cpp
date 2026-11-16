// RFC 0049 Stage 1 Gate 2 native test: the additive default-off river
// convergence seams. Behavioral assertions through the real kernel:
// default-off solves are byte-identical to the pre-seam path, the snapshot
// sink fires once per reached rung, the per-iteration stop predicate latches
// an orderly early stop with flushed rows, and verdict routing matches the
// RFC 0049 Gate 2 decision table. No source-text matching.
#include <array>
#include <bs/eval.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/offline_convergence.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

namespace {

bs::poker::GameDef shallow_river() {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {8, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

RiverSolveConfig base_config(std::uint64_t iters) {
  RiverSolveConfig cfg;
  cfg.def = shallow_river();
  cfg.iterations = iters;
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  return cfg;
}

// Hard constraint 1: with neither seam installed the solve is byte-identical
// to the pre-RFC-0049 path and reports a full, non-early run.
bool test_default_off_byte_identical() {
  const std::vector<std::uint64_t> cps{20, 40, 60};
  const RiverSolveResult a = solve_river_subgame(base_config(60), cps);
  const RiverSolveResult b = solve_river_subgame(base_config(60), cps);
  CHECK(!a.stopped_early && !b.stopped_early);
  CHECK(a.iterations_completed == 60 && b.iterations_completed == 60);
  CHECK(a.history_nash_conv_bb.size() == 3);
  CHECK(hash_average_snapshot(a.avg_sigma) == hash_average_snapshot(b.avg_sigma));
  CHECK(a.nash_conv_bb == b.nash_conv_bb);
  // The optional TreeLimits field defaults to nullopt and must not change
  // the constructed tree's node count.
  CHECK(a.action_nodes == b.action_nodes && a.action_nodes > 0);
  return true;
}

// Hard constraint 1 (continued): explicitly passing the SAME implicit limits
// still reproduces the default snapshot, while a different limit object does
// not alter the numeric result on a tree that fits either envelope.
bool test_explicit_limits_match_default() {
  auto cfg = base_config(60);
  const std::vector<std::uint64_t> cps{30, 60};
  const RiverSolveResult def_result = solve_river_subgame(cfg, cps);
  cfg.tree_limits = offline_census_limits();
  const RiverSolveResult wide_result = solve_river_subgame(cfg, cps);
  CHECK(hash_average_snapshot(def_result.avg_sigma) ==
        hash_average_snapshot(wide_result.avg_sigma));
  CHECK(def_result.nash_conv_bb == wide_result.nash_conv_bb);
  return true;
}

bool test_sink_fires_once_per_rung() {
  auto cfg = base_config(60);
  std::vector<std::uint64_t> seen;
  std::vector<double> rung_nc;
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) {
    seen.push_back(cp.iteration);
    rung_nc.push_back(cp.nash_conv_bb);
  };
  const std::vector<std::uint64_t> cps{10, 25, 60};
  const RiverSolveResult r = solve_river_subgame(cfg, cps);
  CHECK(seen.size() == 3);
  CHECK(seen[0] == 10 && seen[1] == 25 && seen[2] == 60);
  CHECK(rung_nc.size() == 3);
  for (double nc : rung_nc)
    CHECK(std::isfinite(nc) && nc > 0.0);
  CHECK(!r.stopped_early && r.iterations_completed == 60);
  // Sink snapshots and the returned history vector must agree on NC.
  CHECK(r.history_nash_conv_bb.size() == 3);
  for (std::size_t i = 0; i < 3; ++i)
    CHECK(r.history_nash_conv_bb[i] == rung_nc[i]);
  return true;
}

// Deterministic iteration-count predicate: latch after 30 iterations and
// prove the loop stops before completing the requested 60, with the rungs
// at or below 30 already flushed.
bool test_predicate_latches_orderly() {
  auto cfg = base_config(60);
  cfg.stop_predicate = [](std::uint64_t iteration, double) { return iteration > 30; };
  std::vector<std::uint64_t> seen;
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) { seen.push_back(cp.iteration); };
  const std::vector<std::uint64_t> cps{10, 20, 30, 40, 60};
  const RiverSolveResult r = solve_river_subgame(cfg, cps);
  CHECK(r.stopped_early);
  CHECK(r.iterations_completed == 30);
  CHECK(seen.size() == 3);
  CHECK(seen.back() == 30);
  return true;
}

// Wall-based predicate on a deliberately tiny budget: the in-process trip
// must fire before any hypothetical outer stop and leave at least one finite
// row. The ladder starts at iteration 1, whose iteration cannot be stopped
// (the first poll sees ~0 elapsed), so the first rung is always flushed
// before the wall latches.
bool test_wall_predicate_tiny_budget() {
  auto cfg = base_config(500);
  cfg.stop_predicate = [](std::uint64_t, double elapsed_ms) { return elapsed_ms > 25.0; };
  std::vector<RiverCheckpoint> rungs;
  cfg.checkpoint_sink = [&](const RiverCheckpoint& cp) { rungs.push_back(cp); };
  const std::vector<std::uint64_t> cps{1, 50, 100, 200, 500};
  const RiverSolveResult r = solve_river_subgame(cfg, cps);
  CHECK(r.stopped_early);
  CHECK(r.iterations_completed < 500);
  CHECK(!rungs.empty());
  CHECK(rungs.front().iteration == 1);
  for (const RiverCheckpoint& cp : rungs)
    CHECK(std::isfinite(cp.nash_conv_bb));
  return true;
}

bool test_verdict_routing() {
  auto row = [](std::uint64_t t, double e) {
    OfflineRungRow r;
    r.iteration = t;
    r.e_embed_bb = e;
    r.bounded_nash_conv_bb = e;
    r.exact_exploitability_bb = e;
    r.pure_menu_term_bb = 0.0;
    r.wall_ms = 1.0;
    return r;
  };
  // No finite rows -> inconclusive.
  CHECK(classify_convergence({}, false).verdict == OfflineConvergenceVerdict::Inconclusive);
  // Crossing rung -> pass even if later rows are worse.
  auto pass = classify_convergence({row(100, 0.01), row(200, 0.0019)}, false);
  CHECK(pass.verdict == OfflineConvergenceVerdict::Pass);
  // Full ladder, no crossing -> convergence no-go.
  auto nogo = classify_convergence({row(100, 0.05), row(200, 0.03)}, false);
  CHECK(nogo.verdict == OfflineConvergenceVerdict::ConvergenceNoGo);
  // Partial curve plus a latched stop -> budget no-go, never an inconclusive
  // rerun loop.
  auto budget = classify_convergence({row(100, 0.05), row(200, 0.04)}, true);
  CHECK(budget.verdict == OfflineConvergenceVerdict::BudgetNoGo);
  // Non-finite instrumentation -> inconclusive regardless of the stop flag.
  auto broken = classify_convergence({row(100, std::nan(""))}, true);
  CHECK(broken.verdict == OfflineConvergenceVerdict::Inconclusive);
  return true;
}

// RFC 0049 review Minor 2: pin the sealed w1 Coarse cap-1 300-iteration
// control natively (the constant every D184-D253 receipt reproduces), so the
// byte-identity guarantee is not asserted only on the shallow fixture.
bool test_sealed_w1_control_native_pin() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  CHECK(!leaves.empty());
  RiverSolveConfig cfg;
  cfg.def = leaves.front().river_def;
  cfg.iterations = 300;
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = make_grid_abstraction(SizingGrid::Coarse, 1);
  const RiverSolveResult r = solve_river_subgame(cfg, {});
  // The sealed receipt carries 8-decimal precision, so pin within half its
  // last digit rather than a double-literal == (the raw double differs from
  // the decimal literal at the ULP even though it prints identically).
  CHECK(std::abs(r.nash_conv_bb - 0.02761552) <= 5e-9);
  CHECK(hash_average_snapshot(r.avg_sigma) == 1922761369290757593ULL);
  CHECK(!r.stopped_early && r.iterations_completed == 300);
  return true;
}

// RFC 0049 review Minor 1: measure the single steady_clock poll cost on this
// host so the RFC's structural "outside FP, order 1e-5 relative cost" claim
// has a recorded figure. Non-asserting (timing is machine-dependent); the
// measurement only has to be finite and vastly below the shortest sealed
// per-iteration cost (~17 ms).
bool test_measure_poll_cost() {
  constexpr int kSamples = 10'000;
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t sink = 0;
  for (int i = 0; i < kSamples; ++i)
    sink += static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto t1 = std::chrono::steady_clock::now();
  const double ns_per_poll = std::chrono::duration<double, std::nano>(t1 - t0).count() / kSamples;
  CHECK(std::isfinite(ns_per_poll) && ns_per_poll > 0.0);
  CHECK(ns_per_poll < 1'000'000.0);  // <1 ms; sealed iterations are >= ~17 ms
  std::printf("D0049 measured steady_clock poll cost: %.1f ns (sink %llu)\n", ns_per_poll,
              static_cast<unsigned long long>(sink));
  return true;
}

}  // namespace

int main() {
  if (!test_default_off_byte_identical() || !test_explicit_limits_match_default() ||
      !test_sink_fires_once_per_rung() || !test_predicate_latches_orderly() ||
      !test_wall_predicate_tiny_budget() || !test_verdict_routing() ||
      !test_sealed_w1_control_native_pin() || !test_measure_poll_cost())
    return 1;
  std::printf("rfc0047_offline_convergence: all checks passed\n");
  return 0;
}
