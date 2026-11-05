// RFC 0047 G3 native test: two-street depth-limited turn solver.
// Pins the independent telescoping identity (the exact-BR hybrid walker's
// zero-perturbation turn gap equals the turn solver's own summed NashConv),
// the exported per-leaf counterfactual reach (non-negative, seat-symmetric
// total, within the [0,1] arrival mass), and perturbation teeth (a coherent
// same-sign leaf-CV shift evaluated against the true leaves strictly raises
// the hybrid gap). Short iteration counts keep the test fast; the identity is
// algebraic and does not require convergence.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
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

struct Group {
  Key key;
  std::vector<double> matrix;
  double river_nc = 0.0;
  double pot = 0.0;
};

struct Fixture {
  const StratumSpec* spec = nullptr;
  std::vector<FrontierLeaf> leaves;
  std::vector<Group> groups;
  std::vector<std::size_t> group_of_leaf;
  std::vector<double> river_nc;
};

Fixture build_fixture(const char* name, std::uint64_t river_iters, std::size_t threads) {
  Fixture f;
  f.spec = find_stratum(name);
  if (f.spec == nullptr)
    throw std::runtime_error("unknown stratum in turn solver test");
  f.leaves = enumerate_frontiers(*f.spec, 1);
  // w1 cap-1 has 432 post-chance frontier leaves (pinned by the
  // frontier-enumerator native test: 432 limped / 528 raised).
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
  f.groups.resize(group_of.size());
  for (auto& [k, idx] : group_of)
    f.groups[idx].key = k;

  const std::size_t ng = f.groups.size();
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
        f.groups[g].matrix = std::move(r.deal_value_seat0);
        f.groups[g].river_nc = r.nash_conv_chips;
        f.groups[g].pot = static_cast<double>(rep->river_def.pot);
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

  f.river_nc.resize(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i)
    f.river_nc[i] = f.groups[f.group_of_leaf[i]].river_nc;
  return f;
}

void attach(TurnSolveConfig& tc, const Fixture& f,
            const std::vector<const std::vector<double>*>& mats, std::vector<TurnLeafValue>& tv,
            std::uint64_t turn_iters, std::size_t threads) {
  tv.resize(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i)
    tv[i] = {f.leaves[i].child_node, f.leaves[i].board5, mats[i]->data()};
  tc.def = make_stratum_def(*f.spec, 1);
  tc.iterations = turn_iters;
  tc.linear_averaging = true;
  tc.threads = threads;
  tc.leaves = tv;
}

}  // namespace

bool test_identity_and_reach(const Fixture& f) {
  constexpr std::uint64_t kTurn = 120;
  std::vector<const std::vector<double>*> mats(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i)
    mats[i] = &f.groups[f.group_of_leaf[i]].matrix;
  std::vector<TurnLeafValue> tv;
  TurnSolveConfig tc;
  attach(tc, f, mats, tv, kTurn, 4);

  const TurnSolveResult r = solve_turn_subgame(tc);
  CHECK(r.leaf_reaches.size() == f.leaves.size());
  CHECK(std::isfinite(r.nash_conv_bb) && r.nash_conv_bb >= 0.0);

  const HybridNashConv h = turn_hybrid_nash_conv(tc, r.sigma, f.river_nc);
  const double diff = std::fabs(h.turn_gap_bb - r.nash_conv_bb);
  std::printf("identity: solver=%.8f walker=%.8f diff=%.3e river=%.8f\n", r.nash_conv_bb,
              h.turn_gap_bb, diff, h.river_term_bb);
  CHECK(diff <= 1e-7);
  CHECK(h.river_term_bb >= 0.0);

  double mass = 0.0;
  for (std::size_t li = 0; li < f.leaves.size(); ++li) {
    const TurnLeafReach& rho = r.leaf_reaches[li];
    double s0 = 0.0, s1 = 0.0;
    for (std::uint32_t c = 0; c < kRiverC; ++c) {
      CHECK(std::isfinite(rho.rho[0][c]) && std::isfinite(rho.rho[1][c]));
      CHECK(rho.rho[0][c] >= 0.0 && rho.rho[1][c] >= 0.0);
      s0 += rho.rho[0][c];
      s1 += rho.rho[1][c];
    }
    CHECK(std::fabs(s0 - s1) <= 1e-10);
    mass += s0;
  }
  std::printf("river arrival mass=%.6f\n", mass);
  CHECK(mass > 0.0 && mass <= 1.0 + 1e-9);
  return true;
}

bool test_perturbation_teeth(const Fixture& f) {
  constexpr std::uint64_t kTurn = 120;
  constexpr double kLevel = 1e-2;
  std::vector<const std::vector<double>*> mats(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i)
    mats[i] = &f.groups[f.group_of_leaf[i]].matrix;
  std::vector<TurnLeafValue> tv;
  TurnSolveConfig tc;
  attach(tc, f, mats, tv, kTurn, 4);
  const TurnSolveResult base = solve_turn_subgame(tc);
  const HybridNashConv h0 = turn_hybrid_nash_conv(tc, base.sigma, f.river_nc);

  // Coherent same-sign +level*pot shift of every seat-0 combo (and symmetric
  // seat-1 shift), applied to every unique leaf matrix.
  std::vector<std::vector<double>> pert(f.groups.size());
  for (std::size_t g = 0; g < f.groups.size(); ++g) {
    pert[g] = f.groups[g].matrix;
    const double amp = f.groups[g].pot * kLevel;
    double* mp = pert[g].data();
    for (std::uint32_t i = 0; i < kRiverC; ++i)
      for (std::uint32_t j = 0; j < kRiverC; ++j)
        mp[static_cast<std::size_t>(i) * kRiverC + j] += amp;
  }
  std::vector<const std::vector<double>*> pmats(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i)
    pmats[i] = &pert[f.group_of_leaf[i]];
  std::vector<TurnLeafValue> ptv;
  TurnSolveConfig pc;
  attach(pc, f, pmats, ptv, kTurn, 4);
  const TurnSolveResult pr = solve_turn_subgame(pc);
  const HybridNashConv h1 = turn_hybrid_nash_conv(tc, pr.sigma, f.river_nc);
  const double delta = (h1.turn_gap_bb + h1.river_term_bb) - (h0.turn_gap_bb + h0.river_term_bb);
  std::printf("teeth: coherent level=%.0e delta_hybrid=%.6f BB\n", kLevel, delta);
  CHECK(delta > 1e-3);
  return true;
}

int main() {
  // Both cases share one expensive 240-leaf river fixture (river 80).
  static Fixture fixture = build_fixture("w1", 80, 4);
  if (!test_identity_and_reach(fixture))
    return 1;
  if (!test_perturbation_teeth(fixture))
    return 1;
  std::printf("test_rfc0047_turn_solver: OK\n");
  return 0;
}
