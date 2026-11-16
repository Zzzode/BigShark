// RFC 0050 Stage 1 native test: the exact-menu convergence domain, the
// D296 pure-menu zero guard, and the calibration extrapolation. Real
// behavioral assertions through the tree builder and kernel, no
// source-text matching: the cap-0 domain is a full-identity subset map on
// both the shallow fixture and the real sealed anchor, a short exact solve
// reads a zero pure-menu term through the reach-bound instrument, the
// guard rejects synthetic nonzero readings, and the budget extrapolation
// arithmetic is pinned.
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/ledger_census.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/rfc0050_anchors.hpp"
#include "../benchmarks/rfc0047/rfc0050_stage1.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;

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

bool test_stage1_domain_is_full_identity() {
  const auto def = shallow_river();
  Rfc0050Stage1Domain domain = build_stage1_domain(def, offline_census_limits());
  CHECK(domain.reference_tree.size() == domain.bounded_tree.size());
  CHECK(domain.reference_tree.size() > 1);
  // build_stage1_domain already throws on any kNoNode / -1 edge; re-prove
  // the identity explicitly so the contract is visible in the test.
  for (std::size_t n = 0; n < domain.reference_tree.size(); ++n) {
    CHECK(domain.subset.node_map[n] != SubsetMap::kNoNode);
    for (int edge : domain.subset.edge_map[n])
      CHECK(edge >= 0);
  }
  // The domain menu is the exact cap-0 identity.
  CHECK(domain.menu.max_raises_per_street() == 0);
  return true;
}

bool test_sealed_anchor_domain_matches_census() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  const LedgerAnchor* found = nullptr;
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      found = &a;
  CHECK(found != nullptr);
  Rfc0050Stage1Domain domain =
      build_stage1_domain(found->representative.river_def, offline_census_limits());
  // Sealed Step 0a figures for the srp (32,16,16) rank-0 anchor.
  CHECK(domain.bounded_tree.size() == 4797);
  std::size_t action_nodes = 0;
  for (const auto& node : domain.bounded_tree.nodes())
    if (node.is_action())
      ++action_nodes;
  CHECK(action_nodes == 1600);
  return true;
}

bool test_exact_solve_pure_menu_reads_zero() {
  const auto def = shallow_river();
  Rfc0050Stage1Domain domain = build_stage1_domain(def, offline_census_limits());
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = 30;
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = domain.menu;
  cfg.tree_limits = offline_census_limits();
  const std::vector<std::uint64_t> cps{10, 30};
  const RiverSolveResult solved = solve_river_subgame(cfg, cps);
  CHECK(!solved.stopped_early && solved.iterations_completed == 30);

  RiverSolveResult snap;
  snap.avg_sigma = solved.avg_sigma;
  snap.nash_conv_bb = solved.nash_conv_bb;
  const ReachBoundResult rb =
      reach_bound_from_snapshot(def, snap, domain.reference_tree, domain.bounded_tree,
                                domain.subset, CapExtensionCompletion::Uniform);
  const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
  CHECK(pure_menu_reads_zero(pure_menu_bb));
  // On the exact domain the two exploitability measures are the same game,
  // so e_embed must equal the bounded NashConv within the same FP scale.
  CHECK(std::fabs(rb.exact_exploitability_bb - rb.bounded_nash_conv_bb) <= kRfc0050PureMenuZeroBb);
  CHECK(rb.bounded_nash_conv_bb > 0.0);
  CHECK(std::isfinite(rb.exact_exploitability_bb));
  return true;
}

bool test_pure_menu_guard_rejects_nonzero() {
  CHECK(pure_menu_reads_zero(0.0));
  CHECK(pure_menu_reads_zero(kRfc0050PureMenuZeroBb));
  CHECK(pure_menu_reads_zero(-kRfc0050PureMenuZeroBb * 0.5));
  CHECK(!pure_menu_reads_zero(kRfc0050PureMenuZeroBb * 10.0));
  CHECK(!pure_menu_reads_zero(0.0276));  // the sealed bounded-menu floor
  CHECK(!pure_menu_reads_zero(std::nan("")));
  CHECK(!pure_menu_reads_zero(std::numeric_limits<double>::infinity()));
  return true;
}

bool test_calibration_extrapolation_pinned() {
  // 10 ms/iteration, 100 ms per scoring: hand-computable figures.
  const Rfc0050Calibration c = extrapolate_stage1_calibration(10.0, 100.0);
  CHECK(c.projected_20k_wall_s == 200.0);          // 20,000 x 10 ms
  CHECK(c.projected_full_ladder_wall_s == 200.9);  // plus 9 x 100 ms
  CHECK(c.iterations_inside_wall == 180000.0);     // 1,800,000 / 10
  // Every frozen rung fits at this throughput.
  CHECK(c.last_rung_inside_wall == kRfc0050Stage1Rungs.back());
  // A slow kernel: 100 ms/iteration and 10 s per scoring -> only the early
  // rungs fit the wall; the estimator must name the highest fitting one.
  const Rfc0050Calibration slow = extrapolate_stage1_calibration(100.0, 10'000.0);
  CHECK(slow.iterations_inside_wall == 18000.0);
  CHECK(slow.last_rung_inside_wall == 12'000);  // 1.2e6 + 8e4 = 1.28e6 <= 1.8e6
  CHECK(slow.projected_20k_wall_s == 2000.0);
  // Frozen ladder/wall constants are the RFC 0049 values.
  CHECK(kRfc0050Stage1Rungs.size() == 9);
  CHECK(kRfc0050Stage1Rungs.front() == 100);
  CHECK(kRfc0050Stage1Rungs.back() == 20'000);
  CHECK(kRfc0050Stage1WallMs == 1'800'000.0);
  return true;
}

bool test_scaling_verdict_bands_and_amdahl() {
  // Pre-registered D303 thresholds; the 5x-10x gap falls into the
  // conservative middle band, never the favorable cheap band.
  CHECK(classify_scaling(10.0) == ScalingVerdict::LongWallAmendmentCheap);
  CHECK(classify_scaling(15.3) == ScalingVerdict::LongWallAmendmentCheap);
  CHECK(classify_scaling(9.99) == ScalingVerdict::PlateauRiskDominates);
  CHECK(classify_scaling(5.0) == ScalingVerdict::PlateauRiskDominates);
  CHECK(classify_scaling(3.0) == ScalingVerdict::PlateauRiskDominates);
  CHECK(classify_scaling(2.99) == ScalingVerdict::ComputeIncompressible);
  CHECK(classify_scaling(1.0) == ScalingVerdict::ComputeIncompressible);
  // Perfect linear speedup on n workers implies a zero serial fraction.
  const AmdahlBreakdown perfect = amdahl_from_speedup(8.0, 8);
  CHECK(perfect.serial_fraction == 0.0 && perfect.parallel_fraction == 1.0);
  // Serial execution implies a fully serial program.
  const AmdahlBreakdown serial = amdahl_from_speedup(1.0, 8);
  CHECK(serial.serial_fraction == 1.0 && serial.parallel_fraction == 0.0);
  // Worked example: S=4 on 8 workers -> p = (0.25-0.125)/(1-0.125) = 1/7.
  const AmdahlBreakdown ex = amdahl_from_speedup(4.0, 8);
  CHECK(std::fabs(ex.serial_fraction - 1.0 / 7.0) <= 1e-12);
  CHECK(std::fabs(ex.parallel_fraction - 6.0 / 7.0) <= 1e-12);
  return true;
}

bool test_multithread_determinism_shallow() {
  // The kernel contract: thread count changes wall time only, never the
  // numeric result. Solve the shallow exact fixture at 1, 2, and 4 threads
  // and require identical NashConv, e_embed, zero pure-menu, and hash.
  const auto def = shallow_river();
  Rfc0050Stage1Domain domain = build_stage1_domain(def, offline_census_limits());
  std::vector<std::uint64_t> hashes;
  double first_nc = 0.0;
  double first_e = 0.0;
  for (std::size_t threads : std::array<std::size_t, 3>{1, 2, 4}) {
    RiverSolveConfig cfg;
    cfg.def = def;
    cfg.iterations = 20;
    cfg.threads = threads;
    cfg.linear_averaging = true;
    cfg.rm_plus = true;
    cfg.dcfr = false;
    cfg.action_abstraction = domain.menu;
    cfg.tree_limits = offline_census_limits();
    const std::vector<std::uint64_t> cps{20};
    const RiverSolveResult solved = solve_river_subgame(cfg, cps);
    CHECK(!solved.stopped_early && solved.iterations_completed == 20);
    RiverSolveResult snap;
    snap.avg_sigma = solved.avg_sigma;
    snap.nash_conv_bb = solved.nash_conv_bb;
    const ReachBoundResult rb =
        reach_bound_from_snapshot(def, snap, domain.reference_tree, domain.bounded_tree,
                                  domain.subset, CapExtensionCompletion::Uniform);
    CHECK(pure_menu_reads_zero(rb.exact_exploitability_bb - rb.bounded_nash_conv_bb));
    hashes.push_back(hash_average_snapshot(solved.avg_sigma));
    if (threads == 1) {
      first_nc = rb.bounded_nash_conv_bb;
      first_e = rb.exact_exploitability_bb;
    } else {
      CHECK(rb.bounded_nash_conv_bb == first_nc);
      CHECK(rb.exact_exploitability_bb == first_e);
      CHECK(hashes.back() == hashes.front());
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!test_stage1_domain_is_full_identity() || !test_sealed_anchor_domain_matches_census() ||
      !test_exact_solve_pure_menu_reads_zero() || !test_pure_menu_guard_rejects_nonzero() ||
      !test_calibration_extrapolation_pinned() || !test_scaling_verdict_bands_and_amdahl() ||
      !test_multithread_determinism_shallow())
    return 1;
  std::printf("rfc0050_stage1: all checks passed\n");
  return 0;
}
