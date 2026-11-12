// RFC 0047 S3/G5 native test: the certified in-process river serve entry.
//
// Pins serve point B's responder non-regression contract on the exact seedless
// river kernel:
//   1. malformed baseline vectors are InvalidInput (fail before solving);
//   2. a serve locked to its own independently-walked best response is
//      Certified at the production 1e-9*pot tolerance and is deterministic;
//   3. a tightened baseline fails CLOSED (CertificationRejected, never
//      Certified) with concrete violating margin records;
//   4. the production 1e-9 tolerance is the binding kill gate: a gap between
//      1e-9*pot and 1e-6*pot is rejected at 1e-9 but accepted at the explicit
//      1e-6 instrument tolerance (the tighter value must never be relaxed);
//   5. the reached subtree stays inside the G1 node envelope.
// Iteration counts are short; the certification math, not convergence depth,
// is the system under test.
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "../benchmarks/rfc0047/river_serve.hpp"
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

RiverSolveConfig serve_cfg(std::uint64_t iters, std::size_t threads = 1) {
  RiverSolveConfig c;
  c.def = make_river_root_def(*find_stratum("w1"), 1);
  c.iterations = iters;
  c.linear_averaging = true;
  c.threads = threads;
  return c;
}

bool test_invalid_input() {
  RiverSolveResult ref = solve_river_subgame(serve_cfg(300), {});
  const std::size_t C = ref.combos_per_seat;
  std::vector<double> good(C, 0.0);
  std::vector<double> short_b(C / 2, 0.0);

  RiverServeRequest req;
  req.solve = serve_cfg(300);
  req.baseline_seat0 = std::span<const double>(short_b);  // wrong size
  req.baseline_seat1 = std::span<const double>(good);
  CHECK(serve_river_certified(req).status == ServeStatus::InvalidInput);

  RiverServeRequest req2;
  req2.solve = serve_cfg(300);
  req2.baseline_seat0 = std::span<const double>(good);
  req2.baseline_seat1 = std::span<const double>(short_b);
  CHECK(serve_river_certified(req2).status == ServeStatus::InvalidInput);
  return true;
}

bool test_certified_against_own_baseline_and_deterministic() {
  RiverSolveConfig cfg = serve_cfg(400);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  const std::size_t C = ref.combos_per_seat;

  RiverServeRequest req;
  req.solve = cfg;  // identical config -> bitwise identical BR
  req.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  req.baseline_seat1 = std::span<const double>(ref.root_br_seat1);

  RiverServeResult a = serve_river_certified(req);
  CHECK(a.status == ServeStatus::Certified);
  CHECK(a.violations.empty());
  // Every slack is BR - BR - tol*pot = -tol*pot exactly, so the true maximum
  // (not a hard-coded zero) must equal the single negative safety margin.
  const double expected_slack = -kServeCertPotTolerance * static_cast<double>(cfg.def.pot);
  CHECK(a.worst_slack == expected_slack);
  CHECK(a.combos_per_seat == C);

  // Determinism: a second in-process call reproduces status, slack, and value.
  RiverServeResult b = serve_river_certified(req);
  CHECK(b.status == a.status);
  CHECK(b.worst_slack == a.worst_slack);
  CHECK(b.solve.nash_conv_chips == a.solve.nash_conv_chips);
  CHECK(b.solve.root_cfv_seat0 == a.solve.root_cfv_seat0);
  return true;
}

bool test_fail_closed_on_tightened_baseline() {
  RiverSolveConfig cfg = serve_cfg(400);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  const std::size_t C = ref.combos_per_seat;

  // Lock a baseline one full chip BELOW the independently walked BR: every
  // combo then violates even before the tiny tolerance term.
  std::vector<double> tight0(C), tight1(C);
  for (std::size_t i = 0; i < C; ++i) {
    tight0[i] = ref.root_br_seat0[i] - 1.0;
    tight1[i] = ref.root_br_seat1[i] - 1.0;
  }
  RiverServeRequest req;
  req.solve = cfg;
  req.baseline_seat0 = std::span<const double>(tight0);
  req.baseline_seat1 = std::span<const double>(tight1);

  RiverServeResult r = serve_river_certified(req);
  CHECK(r.status == ServeStatus::CertificationRejected);  // fail closed
  CHECK(!r.violations.empty());
  CHECK(r.worst_slack > 0.0);
  for (const ServeMarginRecord& m : r.violations) {
    CHECK(m.slack > 0.0);
    CHECK(m.seat < 2 && m.combo < C);
  }
  return true;
}

bool test_production_tolerance_is_the_binding_gate() {
  RiverSolveConfig cfg = serve_cfg(400);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  const std::size_t C = ref.combos_per_seat;
  const double pot = static_cast<double>(cfg.def.pot);

  // Gap of 5e-8*pot sits between the 1e-9 production tolerance and the 1e-6
  // instrument tolerance: 1e-9 < 5e-8 < 1e-6.
  const double gap_pot = 5e-8;
  std::vector<double> base0(C), base1(C);
  for (std::size_t i = 0; i < C; ++i) {
    base0[i] = ref.root_br_seat0[i] - gap_pot * pot;
    base1[i] = ref.root_br_seat1[i] - gap_pot * pot;
  }

  RiverServeRequest production;
  production.solve = cfg;
  production.baseline_seat0 = std::span<const double>(base0);
  production.baseline_seat1 = std::span<const double>(base1);
  // tolerance_pot defaults to kServeCertPotTolerance (1e-9)
  CHECK(production.tolerance_pot == kServeCertPotTolerance);
  CHECK(serve_river_certified(production).status == ServeStatus::CertificationRejected);

  RiverServeRequest instrument = production;
  instrument.tolerance_pot = 1e-6;  // the looser benchmark instrument tolerance
  CHECK(serve_river_certified(instrument).status == ServeStatus::Certified);
  return true;
}

bool test_node_envelope() {
  RiverSolveConfig cfg = serve_cfg(300);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  RiverServeRequest req;
  req.solve = cfg;
  req.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  req.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  RiverServeResult r = serve_river_certified(req);
  CHECK(r.status == ServeStatus::Certified);
  CHECK(r.solve.action_nodes <= 12);  // the reached online subtree, not 88k/117k
  return true;
}

// Review hardening: non-finite values must fail closed. A NaN compares false in
// every inequality, so without an explicit isfinite guard a NaN baseline or
// tolerance would otherwise certify an unprovable combo (fail-open).
bool test_non_finite_inputs_rejected() {
  RiverSolveConfig cfg = serve_cfg(300);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  const std::size_t C = ref.combos_per_seat;
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();

  for (double bad : {nan, inf}) {
    std::vector<double> b0 = ref.root_br_seat0;
    std::vector<double> b1 = ref.root_br_seat1;
    b0[C / 2] = bad;
    RiverServeRequest req;
    req.solve = cfg;
    req.baseline_seat0 = std::span<const double>(b0);
    req.baseline_seat1 = std::span<const double>(b1);
    CHECK(serve_river_certified(req).status == ServeStatus::InvalidInput);
  }

  RiverServeRequest nt;  // non-finite tolerance
  nt.solve = cfg;
  nt.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  nt.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  nt.tolerance_pot = nan;
  CHECK(serve_river_certified(nt).status == ServeStatus::InvalidInput);
  return true;
}

// Review hardening: the enumerated request bounds. Chips is unsigned, so the
// pot guard is reachable only at pot == 0 (negative pots cannot be encoded);
// the tolerance sign and empty-baseline checks cover the other malformed arms.
bool test_request_bounds_rejected() {
  RiverSolveConfig cfg = serve_cfg(300);
  RiverSolveResult ref = solve_river_subgame(cfg, {});

  RiverServeRequest neg;
  neg.solve = cfg;
  neg.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  neg.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  neg.tolerance_pot = -1e-9;
  CHECK(serve_river_certified(neg).status == ServeStatus::InvalidInput);

  RiverServeRequest zeropot;
  zeropot.solve = cfg;
  zeropot.solve.def.pot = 0;  // unsigned: the only reachable non-positive pot
  zeropot.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  zeropot.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  CHECK(serve_river_certified(zeropot).status == ServeStatus::InvalidInput);

  const std::vector<double> empty;
  RiverServeRequest nobase;
  nobase.solve = cfg;
  nobase.baseline_seat0 = std::span<const double>(empty);
  nobase.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  CHECK(serve_river_certified(nobase).status == ServeStatus::InvalidInput);
  return true;
}

// Review hardening: a malformed GameDef makes the kernel throw
// std::invalid_argument; the serve entry must absorb it into InvalidInput and
// never let an exception cross the no-throw API boundary.
bool test_malformed_def_maps_to_invalid_input() {
  RiverSolveConfig good = serve_cfg(300);
  RiverSolveResult ref = solve_river_subgame(good, {});
  RiverSolveConfig bad = good;
  bad.def.player_count = 3;  // kernel requires exactly two seats

  RiverServeRequest req;
  req.solve = bad;
  req.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  req.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  RiverServeResult r = serve_river_certified(req);  // must not throw
  CHECK(r.status == ServeStatus::InvalidInput);
  return true;
}

// Review residual-risk hardening: a finite but astronomical tolerance fraction
// overflows the chip-scale product (tol*pot -> +inf). That would otherwise
// certify everything with an infinite slack and contradict the no-infinite-slack
// invariant; it must be rejected as invalid input instead.
bool test_overflowing_tolerance_rejected() {
  RiverSolveConfig cfg = serve_cfg(300);
  RiverSolveResult ref = solve_river_subgame(cfg, {});
  RiverServeRequest req;
  req.solve = cfg;
  req.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
  req.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
  req.tolerance_pot = 1e308;  // finite as a fraction; *pot overflows to +inf
  CHECK(std::isfinite(req.tolerance_pot));
  CHECK(serve_river_certified(req).status == ServeStatus::InvalidInput);
  return true;
}

}  // namespace

int main() {
  if (!test_invalid_input() || !test_certified_against_own_baseline_and_deterministic() ||
      !test_fail_closed_on_tightened_baseline() ||
      !test_production_tolerance_is_the_binding_gate() || !test_node_envelope() ||
      !test_non_finite_inputs_rejected() || !test_request_bounds_rejected() ||
      !test_malformed_def_maps_to_invalid_input() || !test_overflowing_tolerance_rejected())
    return 1;
  std::printf("rfc0047_river_serve: all checks passed\n");
  return 0;
}
