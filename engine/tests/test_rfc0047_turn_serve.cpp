// RFC 0047 S3/G5 native test: the certified in-process TURN serve entry
// (serve point A). This is a NEW certification gate; it does not replace the
// existing turn-solver identity/reach tests (the <=1e-7 hybrid identity,
// <=1e-10 seat reach difference, and <=1+1e-9 arrival mass stay in
// test_rfc0047_turn_solver and are not re-claimed here).
//
// EVIDENCE BOUNDARY (stated honestly): serve_turn_certified is a new entry that
// does not exist in any prior commit, so a behavioral RED "at baseline" is not
// definable (the symbol was absent). What is natively verifiable here is the
// new entry's actual runtime behavior against repo-native bounded fixtures and
// an independently constructed sealed margin (a separate solve object; the entry
// only ever receives b_turn as an external input and never derives it from its
// own solve, so it cannot be self-consistent). The turn gap at these short
// iteration counts is large by design (the test exercises certification
// control flow, not convergence), so the production B0 budget honestly rejects
// it; the B0 threshold machinery is exercised with an explicitly sized budget.
//
// Fixtures:
//   1. malformed spans/values -> InvalidInput (return before solving); a
//      negative G3 bound -> CoverageMiss;
//   2. responder gate against a separately-solved sealed root BR, bitwise
//      deterministic, exact -1e-9*pot worst margin;
//   3. tightened sealed margin fails CLOSED with concrete violation records;
//   4. B0 ledger composition is exact and the threshold switch is observed both
//      ways under a budget sized to the measured bound; the production-default
//      verdict is asserted as measured, never assumed to pass;
//   5. a malformed GameDef maps to InvalidInput without throwing.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/turn_serve.hpp"
#include "../benchmarks/rfc0047/turn_solver.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

namespace {

struct Key {
  std::array<std::uint64_t, 5> ledger{};
  std::array<int, 5> board5{};
  bool operator==(const Key& o) const { return ledger == o.ledger && board5 == o.board5; }
};
struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept {
    std::size_t h = 1469598103934665603ULL;
    for (std::uint64_t x : k.ledger) {
      h ^= static_cast<std::size_t>(x);
      h *= 1099511628211ULL;
    }
    for (int c : k.board5) {
      h ^= static_cast<std::size_t>(static_cast<unsigned>(c));
      h *= 1099511628211ULL;
    }
    return h;
  }
};

struct TurnFixture {
  const StratumSpec* spec = nullptr;
  std::vector<FrontierLeaf> leaves;
  std::vector<std::vector<double>> group_matrix;
  std::vector<double> group_river_nc;
  std::vector<std::size_t> group_of_leaf;
};

TurnFixture build_fixture(std::uint64_t river_iters, std::size_t threads) {
  TurnFixture f;
  f.spec = find_stratum("w1");
  if (f.spec == nullptr)
    throw std::runtime_error("unknown stratum");
  f.leaves = enumerate_frontiers(*f.spec, 1);
  if (f.leaves.size() != 432)
    throw std::runtime_error("expected 432 w1 frontier leaves");
  std::unordered_map<Key, std::size_t, KeyHash> group_of;
  f.group_of_leaf.resize(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i) {
    const auto& d = f.leaves[i].river_def;
    Key k{{d.pot, d.contributions[0], d.contributions[1], d.stacks[0], d.stacks[1]},
          f.leaves[i].board5};
    auto [it, inserted] = group_of.emplace(k, group_of.size());
    f.group_of_leaf[i] = it->second;
  }
  const std::size_t ng = group_of.size();
  f.group_matrix.resize(ng);
  f.group_river_nc.resize(ng);
  std::atomic<std::size_t> next{0};
  std::exception_ptr err;
  auto worker = [&]() {
    try {
      for (;;) {
        const std::size_t g = next.fetch_add(1);
        if (g >= ng || err)
          return;
        const FrontierLeaf* rep = nullptr;
        for (std::size_t i = 0; i < f.leaves.size(); ++i)
          if (f.group_of_leaf[i] == g) {
            rep = &f.leaves[i];
            break;
          }
        RiverSolveConfig cfg;
        cfg.def = rep->river_def;
        cfg.iterations = river_iters;
        cfg.linear_averaging = true;
        cfg.compute_deal_matrix = true;
        const RiverSolveResult r = solve_river_subgame(cfg, {});
        f.group_matrix[g] = std::move(r.deal_value_seat0);
        f.group_river_nc[g] = r.nash_conv_chips;
      }
    } catch (...) {
      err = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < std::min(threads, ng); ++t)
    pool.emplace_back(worker);
  for (auto& th : pool)
    th.join();
  if (err)
    std::rethrow_exception(err);
  return f;
}

struct Shared {
  TurnSolveConfig cfg;
  std::vector<TurnLeafValue> tv;
  std::vector<double> leaf_river_nc;
  TurnSolveResult sealed;  // the independently constructed sealed reference
};

Shared build_shared(const TurnFixture& f, std::uint64_t turn_iters, std::size_t threads) {
  Shared s;
  s.tv.resize(f.leaves.size());
  s.leaf_river_nc.resize(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i) {
    const std::size_t g = f.group_of_leaf[i];
    s.tv[i] = {f.leaves[i].child_node, f.leaves[i].board5, f.group_matrix[g].data()};
    s.leaf_river_nc[i] = f.group_river_nc[g];
  }
  s.cfg.def = make_stratum_def(*f.spec, 1);
  s.cfg.iterations = turn_iters;
  s.cfg.linear_averaging = true;
  s.cfg.threads = threads;
  s.cfg.leaves = s.tv;
  s.sealed = solve_turn_subgame(s.cfg);
  return s;
}

TurnServeRequest base_request(const Shared& s) {
  TurnServeRequest req;
  req.solve = s.cfg;
  req.baseline_turn_seat0 = std::span<const double>(s.sealed.root_br_seat0);
  req.baseline_turn_seat1 = std::span<const double>(s.sealed.root_br_seat1);
  req.leaf_river_nc_chips = std::span<const double>(s.leaf_river_nc);
  req.g3_propagated_error_bb = 0.0;
  return req;
}

// All invalid/coverage branches validate before the solve, so they neither
// invoke the kernel nor depend on convergence.
bool test_invalid_and_coverage(const Shared& s) {
  const std::size_t C = s.sealed.root_br_seat0.size();
  CHECK(C == kTurnC);
  std::vector<double> short_b(C / 2, 0.0);

  TurnServeRequest bad_size = base_request(s);
  bad_size.baseline_turn_seat0 = std::span<const double>(short_b);
  CHECK(serve_turn_certified(bad_size).status == TurnServeStatus::InvalidInput);

  TurnServeRequest nan_base = base_request(s);
  std::vector<double> nb = s.sealed.root_br_seat0;
  nb[C / 3] = std::numeric_limits<double>::quiet_NaN();
  nan_base.baseline_turn_seat0 = std::span<const double>(nb);
  CHECK(serve_turn_certified(nan_base).status == TurnServeStatus::InvalidInput);

  TurnServeRequest nan_nc = base_request(s);
  std::vector<double> nc = s.leaf_river_nc;
  nc[0] = std::numeric_limits<double>::infinity();
  nan_nc.leaf_river_nc_chips = std::span<const double>(nc);
  CHECK(serve_turn_certified(nan_nc).status == TurnServeStatus::InvalidInput);

  TurnServeRequest neg_tol = base_request(s);
  neg_tol.tolerance_pot = -1e-9;
  CHECK(serve_turn_certified(neg_tol).status == TurnServeStatus::InvalidInput);

  TurnServeRequest zero_pot = base_request(s);
  zero_pot.solve.def.pot = 0;
  CHECK(serve_turn_certified(zero_pot).status == TurnServeStatus::InvalidInput);

  TurnServeRequest nan_g3 = base_request(s);
  nan_g3.g3_propagated_error_bb = std::numeric_limits<double>::quiet_NaN();
  CHECK(serve_turn_certified(nan_g3).status == TurnServeStatus::InvalidInput);

  std::vector<double> half_nc(s.leaf_river_nc.begin(),
                              s.leaf_river_nc.begin() + s.leaf_river_nc.size() / 2);
  TurnServeRequest short_nc = base_request(s);
  short_nc.leaf_river_nc_chips = std::span<const double>(half_nc);
  CHECK(serve_turn_certified(short_nc).status == TurnServeStatus::InvalidInput);

  // Negative river NashConv is an out-of-range bounded input (Major-1): it must
  // be rejected, never allowed to shrink the B0 river term into a false pass.
  std::vector<double> neg_nc = s.leaf_river_nc;
  neg_nc[0] = -1.0;
  TurnServeRequest negative_nc = base_request(s);
  negative_nc.leaf_river_nc_chips = std::span<const double>(neg_nc);
  CHECK(serve_turn_certified(negative_nc).status == TurnServeStatus::InvalidInput);

  // A finite-but-astronomical tolerance overflows the chip-scale product.
  TurnServeRequest huge_tol = base_request(s);
  huge_tol.tolerance_pot = 1e308;
  CHECK(std::isfinite(huge_tol.tolerance_pot));
  CHECK(serve_turn_certified(huge_tol).status == TurnServeStatus::InvalidInput);

  // The B0 budget must be strictly positive (chips are unsigned, so <=0 is the
  // only reachable non-positive arm here).
  TurnServeRequest zero_b0 = base_request(s);
  zero_b0.b0_bb = 0.0;
  CHECK(serve_turn_certified(zero_b0).status == TurnServeStatus::InvalidInput);

  TurnServeRequest coverage = base_request(s);
  coverage.g3_propagated_error_bb = -1.0;  // finite-negative: coverage absent
  CHECK(serve_turn_certified(coverage).status == TurnServeStatus::CoverageMiss);
  return true;
}

bool test_responder_gate_and_determinism(const Shared& s) {
  TurnServeRequest req = base_request(s);
  TurnServeResult a = serve_turn_certified(req);
  CHECK(a.responder_gate_pass);
  CHECK(a.violations.empty());
  const double expected_slack = -kTurnServeCertPotTolerance * static_cast<double>(s.cfg.def.pot);
  CHECK(a.worst_slack == expected_slack);
  CHECK(a.combos_per_seat == kTurnC);

  // A second in-process entry call (its own internal solve) reproduces the
  // sealed-margin gate and the NashConv bit-for-bit (seedless determinism).
  TurnServeResult b = serve_turn_certified(req);
  CHECK(b.responder_gate_pass == a.responder_gate_pass);
  CHECK(b.worst_slack == a.worst_slack);
  CHECK(b.solve.nash_conv_chips == a.solve.nash_conv_chips);
  return true;
}

bool test_responder_fail_closed(const Shared& s) {
  std::vector<double> tight0(kTurnC), tight1(kTurnC);
  for (std::uint32_t i = 0; i < kTurnC; ++i) {
    tight0[i] = s.sealed.root_br_seat0[i] - 1.0;
    tight1[i] = s.sealed.root_br_seat1[i] - 1.0;
  }
  TurnServeRequest req = base_request(s);
  req.baseline_turn_seat0 = std::span<const double>(tight0);
  req.baseline_turn_seat1 = std::span<const double>(tight1);
  TurnServeResult r = serve_turn_certified(req);
  CHECK(!r.responder_gate_pass);
  CHECK(!r.violations.empty() && r.worst_slack > 0.0);
  for (const TurnMarginRecord& m : r.violations)
    CHECK(m.slack > 0.0 && m.seat < 2 && m.combo < kTurnC);
  CHECK(r.status == TurnServeStatus::CertificationRejected);  // never promoted
  return true;
}

bool test_b0_ledger_and_threshold(const Shared& s) {
  CHECK(kTurnServeB0Bb == 0.010);
  TurnServeRequest measured = base_request(s);  // g3 = 0, production default budget
  TurnServeResult r = serve_turn_certified(measured);
  CHECK(std::isfinite(r.hybrid.turn_gap_bb) && std::isfinite(r.hybrid.river_term_bb));
  CHECK(r.turn_gap_bb >= 0.0 && r.river_term_bb >= 0.0);
  CHECK(r.local_bound_bb == r.turn_gap_bb + r.g3_propagated_bb + r.river_term_bb);
  std::printf(
      "B0 components (short solve): turn_gap=%.6e river=%.6e local=%.6e "
      "production_b0=%.3e verdict=%s\n",
      r.turn_gap_bb, r.river_term_bb, r.local_bound_bb, kTurnServeB0Bb,
      r.b0_gate_pass ? "pass" : "reject");
  // Honest production-budget verdict: exactly the comparison, never assumed.
  CHECK(r.b0_gate_pass == (r.local_bound_bb <= kTurnServeB0Bb));

  // Threshold switching independent of convergence: size a custom budget to the
  // measured bound so g3=0 has positive room, then cross it with a small g3.
  const double custom_b0 = r.local_bound_bb + 1e-6;
  TurnServeRequest under = base_request(s);
  under.b0_bb = custom_b0;
  under.g3_propagated_error_bb = 0.0;
  CHECK(serve_turn_certified(under).b0_gate_pass == true);
  TurnServeRequest over = base_request(s);
  over.b0_bb = custom_b0;
  over.g3_propagated_error_bb = 2e-6;  // exceeds the 1e-6 room
  CHECK(serve_turn_certified(over).b0_gate_pass == false);
  return true;
}

bool test_malformed_def_no_throw(const Shared& s) {
  TurnSolveConfig bad = s.cfg;
  bad.def.player_count = 3;  // kernel requires two seats -> invalid_argument
  TurnServeRequest req = base_request(s);
  req.solve = bad;
  TurnServeResult r = serve_turn_certified(req);  // must not throw
  CHECK(r.status == TurnServeStatus::InvalidInput);
  return true;
}

}  // namespace

int main() {
  // One shared short fixture (river 20, 4 threads) and one sealed turn solve;
  // entry calls re-solve internally, so total full turn solves stays small.
  static TurnFixture fixture = build_fixture(20, 4);
  // Threaded turn solves are bitwise identical across counts (the fused Pool
  // bitwise identity is pinned in the river/turn solver tests), so 4 threads
  // keep the sealed/entry determinism exact while bounding ctest wall time.
  static Shared shared = build_shared(fixture, 40, 4);
  if (!test_invalid_and_coverage(shared) || !test_responder_gate_and_determinism(shared) ||
      !test_responder_fail_closed(shared) || !test_b0_ledger_and_threshold(shared) ||
      !test_malformed_def_no_throw(shared))
    return 1;
  std::printf("rfc0047_turn_serve: all checks passed\n");
  return 0;
}
