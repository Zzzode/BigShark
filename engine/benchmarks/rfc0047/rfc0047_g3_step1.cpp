// RFC 0047 G3 step-1 probe (measurement-only, manual, NOT a CTest).
//
// Zero-perturbation pipeline sanity: solve every unique post-chance river
// leaf of one stratum with the deterministic river kernel (also producing the
// per-joint-deal continuation matrix), feed those matrices as the turn
// tree's post-chance frontier leaves, and solve the two-street turn game.
// Prints the turn summed NashConv (BB) at sparse iteration checkpoints.
//
// A correct depth-limited turn solve whose leaves carry accurate river
// continuation values must converge to a small NashConv (bounded by the leaf
// oracle error); this establishes the unperturbed baseline before G3 injects
// additive CV perturbations and measures the error->exploitability slope.
//
//   bigshark-rfc0047-g3-step1 STRATUM RIVER_ITERS TURN_ITERS [THREADS]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "turn_solver.hpp"

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
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s STRATUM RIVER_ITERS TURN_ITERS [THREADS]\n", argv[0]);
    return 2;
  }
  const StratumSpec* spec = find_stratum(argv[1]);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", argv[1]);
    return 2;
  }
  const std::uint64_t river_iters = std::stoull(argv[2]);
  const std::uint64_t turn_iters = std::stoull(argv[3]);
  unsigned hw = std::thread::hardware_concurrency();
  const std::size_t n_threads = argc >= 5 ? std::stoul(argv[4]) : (hw ? hw : 1);
  const bool rm_plus = !(argc >= 6 && std::string(argv[5]) == "van");

  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);

  std::unordered_map<Key, std::size_t, KeyHash> group_of;
  std::vector<std::size_t> group_of_leaf(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const auto& d = leaves[i].river_def;
    Key k{{d.pot, d.contributions[0], d.contributions[1], d.stacks[0], d.stacks[1]},
          leaves[i].board5};
    auto [it, inserted] = group_of.emplace(k, group_of.size());
    group_of_leaf[i] = it->second;
  }
  const std::size_t n_groups = group_of.size();

  std::vector<Group> groups(n_groups);
  for (auto& [k, idx] : group_of)
    groups[idx].key = k;

  const auto wall0 = std::chrono::steady_clock::now();
  std::atomic<std::size_t> next{0}, done{0};
  std::mutex log;
  std::exception_ptr err;
  auto worker = [&]() {
    try {
      for (;;) {
        const std::size_t g = next.fetch_add(1);
        if (g >= n_groups || err)
          return;
        // Find one leaf of this group for its GameDef.
        const FrontierLeaf* rep = nullptr;
        for (std::size_t i = 0; i < leaves.size(); ++i)
          if (group_of_leaf[i] == g) {
            rep = &leaves[i];
            break;
          }
        RiverSolveConfig cfg;
        cfg.def = rep->river_def;
        cfg.iterations = river_iters;
        cfg.linear_averaging = true;
        cfg.compute_deal_matrix = true;
        const RiverSolveResult r = solve_river_subgame(cfg, {});
        groups[g].matrix = std::move(r.deal_value_seat0);
        const std::size_t d = done.fetch_add(1) + 1;
        if (d % 12 == 0 || d == n_groups) {
          std::lock_guard<std::mutex> lk(log);
          std::fprintf(stderr, "river leaves solved %zu/%zu\n", d, n_groups);
        }
      }
    } catch (...) {
      err = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < std::min(n_threads, n_groups); ++t)
    pool.emplace_back(worker);
  for (auto& th : pool)
    th.join();
  if (err)
    std::rethrow_exception(err);

  // Map each turn-tree frontier child node to its group's matrix.
  std::vector<TurnLeafValue> tleaves(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const Group& grp = groups[group_of_leaf[i]];
    tleaves[i] = {leaves[i].child_node, leaves[i].board5, grp.matrix.data()};
  }

  const std::uint64_t cps[] = {50, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600};
  std::vector<std::uint64_t> checkpoints;
  for (std::uint64_t c : cps)
    if (c <= turn_iters)
      checkpoints.push_back(c);

  TurnSolveConfig tcfg;
  tcfg.def = make_stratum_def(*spec, 1);
  tcfg.iterations = turn_iters;
  tcfg.linear_averaging = true;
  tcfg.leaves = tleaves;
  tcfg.checkpoints = checkpoints;
  tcfg.threads = n_threads;
  tcfg.rm_plus = rm_plus;

  const TurnSolveResult tr = solve_turn_subgame(tcfg);
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();

  std::printf(
      "stratum=%s leaves=%zu unique_groups=%zu river_iters=%llu "
      "turn_iters=%llu threads=%zu rm=%s wall=%.1fs\n",
      spec->name, leaves.size(), n_groups, static_cast<unsigned long long>(river_iters),
      static_cast<unsigned long long>(turn_iters), n_threads, rm_plus ? "rm+" : "vanilla", wall);
  std::printf("turn_action_nodes=%zu\n", tr.turn_action_nodes);
  std::printf("iters,nash_conv_bb\n");
  for (std::size_t k = 0; k < tr.history_nash_conv_bb.size(); ++k)
    std::printf("%llu,%.8f\n", static_cast<unsigned long long>(checkpoints[k]),
                tr.history_nash_conv_bb[k]);
  std::printf("final,%.8f\n", tr.nash_conv_bb);
  return 0;
}
