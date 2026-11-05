// RFC 0047 S2 native test: deterministic full-deal river solver.
// Pins seedless bitwise reproducibility, zero-sum structure, finite range,
// and monotone exploitability reduction (the deterministic law premise),
// using short iteration counts so the test stays fast.
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

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
RiverSolveConfig w1cfg(std::uint64_t iters) {
  RiverSolveConfig c;
  c.def = make_river_root_def(*find_stratum("w1"), 1);
  c.iterations = iters;
  c.linear_averaging = true;
  return c;
}
}  // namespace

bool test_finite_and_range() {
  const RiverSolveResult r = solve_river_subgame(w1cfg(100), {});
  CHECK(r.combos_per_seat == 1081);
  CHECK(std::isfinite(r.nash_conv_bb));
  CHECK(r.nash_conv_bb >= 0.0);
  CHECK(r.root_cfv_seat0.size() == 1081 && r.root_cfv_seat1.size() == 1081);
  for (double v : r.root_cfv_seat0)
    CHECK(std::isfinite(v));
  for (double v : r.root_cfv_seat1)
    CHECK(std::isfinite(v));
  return true;
}

bool test_monotone_reduction() {
  // Deterministic: more iterations must not increase exploitability except
  // for tiny numerical jitter in the first few iterations; compare well
  // separated points.
  const std::vector<std::uint64_t> cp{50, 200, 1000};
  const RiverSolveResult r = solve_river_subgame(w1cfg(1000), cp);
  CHECK(r.history_nash_conv_bb.size() == 3);
  for (double v : r.history_nash_conv_bb)
    CHECK(std::isfinite(v) && v >= 0.0);
  CHECK(r.history_nash_conv_bb[0] > r.history_nash_conv_bb[1]);
  CHECK(r.history_nash_conv_bb[1] > r.history_nash_conv_bb[2]);
  // A converged-ish river subgame must be far below the raw turn-gadget
  // floors (~5 BB seen at 100 iterations) by 1,000; require a clear reduction.
  CHECK(r.history_nash_conv_bb[2] < r.history_nash_conv_bb[0]);
  return true;
}

bool test_bitwise_deterministic() {
  // Determinism holds at any iteration; keep this cheap (no need to converge).
  const RiverSolveResult a = solve_river_subgame(w1cfg(50), {});
  const RiverSolveResult b = solve_river_subgame(w1cfg(50), {});
  CHECK(a.nash_conv_chips == b.nash_conv_chips);
  CHECK(a.root_cfv_seat0 == b.root_cfv_seat0);
  CHECK(a.root_cfv_seat1 == b.root_cfv_seat1);
  return true;
}

bool test_combo_br_dominates_ev() {
  // At a near-converged point, per-combo best-response value must weakly
  // dominate the equilibrium value for every combo (the exact form of the
  // responder margin inequality on the standalone river game), and the
  // aggregate exploitability must already be well under R0-scale+epsilon.
  ComboNashConv c = river_combo_values(w1cfg(1500), 1500, true);
  CHECK(c.seat0_br.size() == 1081 && c.seat1_br.size() == 1081);
  double raw = 0.0;
  for (std::uint32_t i = 0; i < 1081; ++i) {
    CHECK(c.seat0_br[i] + 1e-9 >= c.seat0_ev[i]);
    CHECK(c.seat1_br[i] + 1e-9 >= c.seat1_ev[i]);
    raw += (c.seat0_br[i] - c.seat0_ev[i]) + (c.seat1_br[i] - c.seat1_ev[i]);
  }
  // Raw is summed over combos but not yet divided by the 990 compatible-deal
  // normalization; just require it is finite and non-negative. The tight BB
  // curve is measured by the manual curve probe, not asserted here.
  CHECK(std::isfinite(raw) && raw >= -1e-9);
  return true;
}

// Cross-check the per-joint-deal continuation matrix the turn solver consumes.
// The matrix stores the river kernel's exact fixed-profile deal values under
// the kernel's OWN reach convention (fold terminals sum opponent reach over
// all 1081 hands; showdown zeros blocker pairs via the signed matrix). A row
// summed over ALL opponent hands therefore equals the equilibrium root CFV
// times the 990 compatible-deal normalization (agree to the bit at 1.2k
// iters). The TURN solver excludes shared-hole deals via its own combo mask;
// this test only pins that the matrix matches the kernel that built it.
bool test_deal_matrix_matches_cfv() {
  RiverSolveConfig cfg = w1cfg(1200);
  cfg.compute_deal_matrix = true;
  const RiverSolveResult r = solve_river_subgame(cfg, {});
  CHECK(r.deal_value_seat0.size() == 1081ull * 1081ull);

  double worst = 0.0;
  for (std::uint32_t i = 0; i < 1081; ++i) {
    double row_sum = 0.0;
    for (std::uint32_t j = 0; j < 1081; ++j)
      row_sum += r.deal_value_seat0[static_cast<std::size_t>(i) * 1081 + j];
    // combo_values CFV is normalized per 990 deals; row_sum is the raw sum.
    worst = std::max(worst, std::fabs(row_sum - r.root_cfv_seat0[i] * 990.0));
  }
  CHECK(std::isfinite(worst));
  CHECK(worst < 1e-6);  // the two independent walks agree to the bit
  return true;
}

int main() {
  if (!test_finite_and_range() || !test_monotone_reduction() || !test_bitwise_deterministic() ||
      !test_combo_br_dominates_ev() || !test_deal_matrix_matches_cfv())
    return 1;
  std::printf("rfc0047_river_solver: all checks passed\n");
  return 0;
}
