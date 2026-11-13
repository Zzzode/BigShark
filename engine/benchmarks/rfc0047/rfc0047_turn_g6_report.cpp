// RFC 0047 S3/G6 TURN manual probe (NOT a CTest): exercises the real bench
// wiring from an exact depth-limited turn solve through the certified turn
// serve entry (serve point A) into the dual quality report, and emits one G6
// turn record per run. The turn quality assembler (turn_quality_report) is
// otherwise covered only by the synthetic CTest with controlled inputs; this
// probe drives it with the real frontier -> grouped river continuation ->
// sealed turn reference -> serve pipeline on a frozen stratum.
//
// The G6 row carries BOTH required quantities (RFC 0039 D1): the within-
// instrument convergence predicate (gadget NashConv vs the 0.005 BB CERT bar)
// AND the exact-hand hybrid gap plus the composed full-strategy M2 delta vs
// B0 = 0.010 BB, with the full B0 ledger (turn gap + G3 + river term), so a
// single favorable number can never be reported alone (no quality laundering).
//
// The sealed responder margin is the deterministic turn kernel's own
// independently-walked root BR supplied as an EXTERNAL input (the serve never
// derives it from its own solve). The G3 propagated bound is an external
// certified input taken as an argument (default 0 for the harness; a production
// run passes the sealed RFC 0047 G3 envelope, e.g. 5.66e-3 BB).
//
// Usage: bigshark-rfc0047-turn-g6-report STRATUM RIVER_ITERS TURN_ITERS
//            [THREADS] [OUT_CSV] [G3_PROPAGATED_BB]
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frontier_enumerator.hpp"
#include "quality_report.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "turn_serve.hpp"
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

struct TurnFixture {
  const StratumSpec* spec = nullptr;
  std::vector<FrontierLeaf> leaves;
  std::vector<std::vector<double>> group_matrix;
  std::vector<double> group_river_nc;
  std::vector<std::size_t> group_of_leaf;
};

// Group frontier leaves by (pot/contributions/stacks ledger, five-card board)
// and solve one exact river subgame per group, retaining the seat-0 deal
// continuation matrix and its NashConv. Parallel over worker threads; the
// underlying river solves are seedless and bitwise deterministic.
TurnFixture build_fixture(const StratumSpec& spec, std::uint64_t river_iters, std::size_t threads) {
  TurnFixture f;
  f.spec = &spec;
  f.leaves = enumerate_frontiers(spec, 1);
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
        // Keep each per-group river solve serial: parallelism is already
        // provided by the outer group-worker pool, so an inner per-solve pool
        // here would oversubscribe N x N OS threads (matching the native
        // turn_serve fixture, which also leaves this at the serial default).
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

long peak_rss_kb() {
  rusage ru{};
  return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_maxrss : -1;
}

const char* status_name(TurnServeStatus s) {
  switch (s) {
    case TurnServeStatus::Certified:
      return "Certified";
    case TurnServeStatus::CertificationRejected:
      return "CertificationRejected";
    case TurnServeStatus::CoverageMiss:
      return "CoverageMiss";
    case TurnServeStatus::InvalidInput:
      return "InvalidInput";
  }
  return "Unknown";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s STRATUM RIVER_ITERS TURN_ITERS [THREADS] [OUT_CSV] "
                 "[G3_PROPAGATED_BB]\n",
                 argv[0]);
    return 2;
  }
  const StratumSpec* spec = find_stratum(argv[1]);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", argv[1]);
    return 2;
  }
  const std::uint64_t river_iters = std::stoull(argv[2]);
  const std::uint64_t turn_iters = std::stoull(argv[3]);
  const std::size_t threads = argc >= 5 ? std::stoul(argv[4]) : 1;
  const char* out_csv = argc >= 6 ? argv[5] : nullptr;
  const double g3_propagated_bb = argc >= 7 ? std::stod(argv[6]) : 0.0;

  const auto wall0 = std::chrono::steady_clock::now();
  const TurnFixture f = build_fixture(*spec, river_iters, threads);

  // Assemble the turn leaf views over the grouped river continuations and run
  // the sealed reference turn solve whose independently-walked root BR becomes
  // the external responder margin.
  std::vector<TurnLeafValue> tv(f.leaves.size());
  std::vector<double> leaf_river_nc(f.leaves.size());
  for (std::size_t i = 0; i < f.leaves.size(); ++i) {
    const std::size_t g = f.group_of_leaf[i];
    tv[i] = {f.leaves[i].child_node, f.leaves[i].board5, f.group_matrix[g].data()};
    leaf_river_nc[i] = f.group_river_nc[g];
  }
  TurnSolveConfig cfg;
  cfg.def = make_stratum_def(*spec, 1);
  cfg.iterations = turn_iters;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  cfg.leaves = tv;
  const TurnSolveResult sealed = solve_turn_subgame(cfg);

  TurnServeRequest req;
  req.solve = cfg;
  req.baseline_turn_seat0 = std::span<const double>(sealed.root_br_seat0);
  req.baseline_turn_seat1 = std::span<const double>(sealed.root_br_seat1);
  req.leaf_river_nc_chips = std::span<const double>(leaf_river_nc);
  req.g3_propagated_error_bb = g3_propagated_bb;
  const TurnServeResult served = serve_turn_certified(req);
  const QualityDualReport q = turn_quality_report(served);
  const double wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();

  const char* header =
      "stratum,river_iters,turn_iters,groups,leaves,serve_status,responder_gate_pass,"
      "b0_gate_pass,within_instrument_nc_bb,within_converged,exact_hybrid_gap_bb,"
      "exact_qual_pass,turn_gap_bb,river_term_bb,g3_propagated_bb,local_bound_bb,b0_bb,"
      "composed_m2_delta_bb,composed_gate_pass,worst_slack,wall_ms,peak_rss_kb\n";
  auto emit = [&](FILE* fp) {
    std::fprintf(fp,
                 "%s,%llu,%llu,%zu,%zu,%s,%d,%d,%.8f,%d,%.8f,%d,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%d,"
                 "%.8f,%.3f,%ld\n",
                 spec->name, static_cast<unsigned long long>(river_iters),
                 static_cast<unsigned long long>(turn_iters), f.group_matrix.size(),
                 f.leaves.size(), status_name(served.status), served.responder_gate_pass ? 1 : 0,
                 served.b0_gate_pass ? 1 : 0, q.within_instrument_nash_conv_bb,
                 q.within_instrument_converged ? 1 : 0, q.exact_gap_bb, q.exact_qual_pass ? 1 : 0,
                 served.turn_gap_bb, served.river_term_bb, served.g3_propagated_bb,
                 served.local_bound_bb, served.b0_bb, q.composed_m2_delta_bb,
                 q.composed_gate_pass ? 1 : 0, served.worst_slack, wall_ms, peak_rss_kb());
  };

  std::printf("%s", header);
  emit(stdout);
  if (out_csv != nullptr) {
    FILE* csv = std::fopen(out_csv, "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot open '%s'\n", out_csv);
      return 2;
    }
    std::fprintf(csv, "%s", header);
    emit(csv);
    std::fclose(csv);
  }
  return 0;
}
