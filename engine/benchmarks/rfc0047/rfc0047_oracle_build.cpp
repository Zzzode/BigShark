// RFC 0047 S2 manual builder: solve EVERY post-chance river frontier of one
// frozen turn-rooted stratum deterministically and write a BSNCV01 oracle
// artifact with the per-seat per-combo counterfactual continuation value and
// a one-sided certified chip error measured against a higher-accuracy
// reference solve.
//
// The turn tree has 9 (limped) / 11 (single-raised) non-fold betting lines
// and 48 river cards per line -> 432 / 528 post-chance leaves. Distinct turn
// betting lines can end in the SAME river ledger (pot, gross contributions,
// stacks behind); with the same five-card board the river subgame is then
// identical, so solves are deduplicated on the full game tuple and their
// results are reused for every leaf in the group. Each leaf still gets its own
// keyed rows because online lookup arrives through a specific turn line.
//
// Certified error (G2): each unique subgame is solved twice - a candidate at
// cand_iters and a higher-accuracy reference at ref_iters. Per (seat, combo):
//   certified_error = |CV_cand - CV_ref| + max(0, BR_ref - CV_ref)
// i.e. the measured candidate-vs-reference deviation PLUS the reference's own
// per-combo best-response residual, so the bound is against the true
// equilibrium value, not merely another approximate solve. A per-subgroup
// G2 sidecar (<out>.cert.csv) records CERT max and convergence diagnostics.
//
//   bigshark-rfc0047-oracle-build STRATUM SHAPE_ID CAND_ITERS REF_ITERS
//                                 OUT.BSNCV [MAX_GROUPS]
//
// Optional max_groups (default 0 = all deduplicated subgames) caps the number
// of unique subgames solved, in deterministic first-DFS order; it supports the
// G2 frozen coverage-frame sampling without solving the whole stratum.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cv_oracle.hpp"
#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"

using namespace bs::rfc0047;

namespace {

// Full game-relevant identity of a river subgame within one stratum (button
// and big blind are constant across the stratum's leaves).
struct GameKey {
  std::array<std::uint64_t, 5> ledger{};  // pot, contrib0, contrib1, stack0, stack1
  std::array<int, 5> board5{};
  bool operator==(const GameKey& o) const {
    return ledger == o.ledger && board5 == o.board5;
  }
};

struct GameKeyHash {
  std::size_t operator()(const GameKey& k) const noexcept {
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

GameKey key_of(const FrontierLeaf& leaf) {
  GameKey k;
  const auto& d = leaf.river_def;
  k.ledger = {d.pot, d.contributions[0], d.contributions[1], d.stacks[0], d.stacks[1]};
  k.board5 = leaf.board5;
  return k;
}

}  // namespace

struct GroupResult {
  std::size_t group = 0;
  std::uint32_t root_pot = 0;
  std::uint32_t combos = 0;
  double cand_nc_bb = 0.0;
  double ref_nc_bb = 0.0;
  double drift_max_bb = 0.0;
  double residual_max_bb = 0.0;
  double cert_max_bb = 0.0;
  double drift_rms_bb = 0.0;
  double residual_rms_bb = 0.0;
  double cert_rms_bb = 0.0;
  std::vector<double> cfv0;
  std::vector<double> cfv1;
  std::vector<double> err0;
  std::vector<double> err1;
};

// Pure: solve one unique subgame at both iteration budgets and compute its
// per-combo candidate values and one-sided reference-certified errors. Safe to
// run concurrently: each call owns its RiverGame/per-instance scratch, and the
// read-only static holding tables are shared without mutation.
GroupResult solve_group(std::size_t group, const bs::poker::GameDef& def,
                       std::uint64_t cand_iters, std::uint64_t ref_iters) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.linear_averaging = true;
  cfg.iterations = cand_iters;
  const RiverSolveResult cand = solve_river_subgame(cfg, {});
  cfg.iterations = ref_iters;
  const RiverSolveResult ref = solve_river_subgame(cfg, {});

  const double bb = def.big_blind > 0 ? static_cast<double>(def.big_blind) : 1.0;
  GroupResult out;
  out.group = group;
  out.root_pot = static_cast<std::uint32_t>(def.pot);
  out.combos = static_cast<std::uint32_t>(cand.root_cfv_seat0.size());
  out.cand_nc_bb = cand.nash_conv_bb;
  out.ref_nc_bb = ref.nash_conv_bb;
  out.cfv0 = cand.root_cfv_seat0;
  out.cfv1 = cand.root_cfv_seat1;
  out.err0.assign(out.combos, 0.0);
  out.err1.assign(out.combos, 0.0);

  double drift_max = 0.0, residual_max = 0.0, cert_max = 0.0;
  double ss_drift = 0.0, ss_residual = 0.0, ss_cert = 0.0;
  for (std::uint32_t seat = 0; seat < 2; ++seat) {
    const std::vector<double>& cv = seat == 0 ? cand.root_cfv_seat0
                                             : cand.root_cfv_seat1;
    const std::vector<double>& rv = seat == 0 ? ref.root_cfv_seat0
                                             : ref.root_cfv_seat1;
    const std::vector<double>& rb = seat == 0 ? ref.root_br_seat0
                                             : ref.root_br_seat1;
    std::vector<double>& err = seat == 0 ? out.err0 : out.err1;
    for (std::uint32_t c = 0; c < out.combos; ++c) {
      const double residual = std::max(0.0, rb[c] - rv[c]);
      const double drift = std::fabs(cv[c] - rv[c]);
      const double e = drift + residual;
      err[c] = std::max(e, 1e-12);
      drift_max = std::max(drift_max, drift);
      residual_max = std::max(residual_max, residual);
      cert_max = std::max(cert_max, err[c]);
      ss_drift += drift * drift;
      ss_residual += residual * residual;
      ss_cert += err[c] * err[c];
    }
  }
  const double n = static_cast<double>(2u * out.combos);
  out.drift_max_bb = drift_max / bb;
  out.residual_max_bb = residual_max / bb;
  out.cert_max_bb = cert_max / bb;
  out.drift_rms_bb = std::sqrt(ss_drift / n) / bb;
  out.residual_rms_bb = std::sqrt(ss_residual / n) / bb;
  out.cert_rms_bb = std::sqrt(ss_cert / n) / bb;
  return out;
}

int main(int argc, char** argv) {
  if (argc < 6 || argc > 9) {
    std::fprintf(stderr,
                 "usage: %s STRATUM SHAPE_ID CAND_ITERS REF_ITERS OUT.BSNCV "
                 "[MAX_GROUPS] [GROUP_STRIDE] [THREADS]\n"
                 "  MAX_GROUPS  0 = all deduplicated subgroups (default 0)\n"
                 "  GROUP_STRIDE solve groups g with g%%stride==0 (default 1);\n"
                 "              e.g. 48 samples one card from each 48-card ledger\n"
                 "  THREADS     worker count (default hardware concurrency)\n",
                 argv[0]);
    return 2;
  }
  const StratumSpec* spec = find_stratum(argv[1]);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", argv[1]);
    return 2;
  }
  const auto shape_id = static_cast<std::uint8_t>(std::stoul(argv[2]));
  const std::uint64_t cand_iters = std::stoull(argv[3]);
  const std::uint64_t ref_iters = std::stoull(argv[4]);
  const std::string out_path = argv[5];
  const std::size_t max_groups = argc >= 7 ? std::stoul(argv[6]) : 0;
  const std::size_t stride = argc >= 8 ? std::max<std::size_t>(1, std::stoul(argv[7])) : 1;
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0)
    hw = 1;
  const std::size_t threads_req = argc >= 9 ? std::stoul(argv[8]) : hw;
  if (ref_iters < cand_iters) {
    std::fprintf(stderr, "ref_iters must be >= cand_iters\n");
    return 2;
  }

  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);

  // Deduplicate identical river subgames (deterministic first-DFS order).
  std::unordered_map<GameKey, std::size_t, GameKeyHash> group_of_key;
  std::vector<std::size_t> group_per_leaf(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const GameKey k = key_of(leaves[i]);
    auto [it, inserted] = group_of_key.emplace(k, group_of_key.size());
    group_per_leaf[i] = it->second;
  }
  const std::size_t unique_games = group_of_key.size();

  // First leaf index and member leaves for each group.
  std::vector<std::size_t> first_leaf(unique_games, leaves.size());
  std::vector<std::vector<std::size_t>> member_leaves(unique_games);
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const std::size_t g = group_per_leaf[i];
    member_leaves[g].push_back(i);
    first_leaf[g] = std::min(first_leaf[g], i);
  }

  // Selected groups: stride sampling in ascending group order, then cap.
  std::vector<std::size_t> jobs;
  for (std::size_t g = 0; g < unique_games; ++g)
    if (g % stride == 0)
      jobs.push_back(g);
  if (max_groups != 0 && jobs.size() > max_groups)
    jobs.resize(max_groups);
  const std::size_t n_jobs = jobs.size();
  const std::size_t n_threads = std::max<std::size_t>(1,
      std::min<std::size_t>(threads_req, n_jobs));

  // Concurrent solves, deterministic results stored per job slot.
  std::vector<GroupResult> results(n_jobs);
  std::atomic<std::size_t> next_job{0};
  std::atomic<std::size_t> done{0};
  std::mutex log_mu;
  std::exception_ptr worker_error = nullptr;
  const auto wall_start = std::chrono::steady_clock::now();

  auto worker = [&]() {
    try {
      for (;;) {
        const std::size_t slot = next_job.fetch_add(1);
        if (slot >= n_jobs || worker_error)
          return;
        const std::size_t g = jobs[slot];
        results[slot] =
            solve_group(g, leaves[first_leaf[g]].river_def, cand_iters, ref_iters);
        const std::size_t d = done.fetch_add(1) + 1;
        if (d % 12 == 0 || d == n_jobs) {
          std::lock_guard<std::mutex> lk(log_mu);
          std::fprintf(stderr, "solved %zu/%zu subgames on %zu threads\n", d,
                       n_jobs, n_threads);
        }
      }
    } catch (...) {
      worker_error = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < n_threads; ++t)
    pool.emplace_back(worker);
  for (auto& th : pool)
    th.join();
  if (worker_error)
    std::rethrow_exception(worker_error);

  // Deterministic serial output: G2 sidecar rows then the artifact rows, both
  // in ascending selected-group order (independent of thread completion).
  const std::string csv_path = out_path + ".cert.csv";
  std::FILE* csv = std::fopen(csv_path.c_str(), "w");
  if (csv == nullptr) {
    std::fprintf(stderr, "cannot open %s\n", csv_path.c_str());
    return 2;
  }
  std::fprintf(csv,
               "stratum,shape_id,group,pot_chips,cand_nash_conv_bb,"
               "ref_nash_conv_bb,drift_max_bb,residual_max_bb,cert_max_bb,"
               "drift_rms_bb,residual_rms_bb,cert_rms_bb\n");

  OracleWriter writer(shape_id, static_cast<std::uint32_t>(spec->pot));
  double worst_cand_nc_bb = 0.0, worst_ref_nc_bb = 0.0;
  double worst_drift_rms_bb = 0.0;
  double worst_drift_max_bb = 0.0;
  std::size_t emitted_rows = 0;
  for (std::size_t slot = 0; slot < n_jobs; ++slot) {
    const GroupResult& r = results[slot];
    worst_cand_nc_bb = std::max(worst_cand_nc_bb, r.cand_nc_bb);
    worst_ref_nc_bb = std::max(worst_ref_nc_bb, r.ref_nc_bb);
    worst_drift_rms_bb = std::max(worst_drift_rms_bb, r.drift_rms_bb);
    worst_drift_max_bb = std::max(worst_drift_max_bb, r.drift_max_bb);
    std::fprintf(csv,
                 "%s,%u,%zu,%u,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n",
                 spec->name, shape_id, r.group, r.root_pot, r.cand_nc_bb,
                 r.ref_nc_bb, r.drift_max_bb, r.residual_max_bb, r.cert_max_bb,
                 r.drift_rms_bb, r.residual_rms_bb, r.cert_rms_bb);
    for (std::size_t li : member_leaves[r.group]) {
      const FrontierLeaf& leaf = leaves[li];
      for (std::uint8_t seat = 0; seat < 2; ++seat) {
        const std::vector<double>& cfv = seat == 0 ? r.cfv0 : r.cfv1;
        const std::vector<double>& err = seat == 0 ? r.err0 : r.err1;
        for (std::uint32_t combo = 0; combo < r.combos; ++combo) {
          OracleRow row;
          row.key.shape_id = shape_id;
          for (int b = 0; b < 5; ++b)
            row.key.board5[b] = static_cast<std::uint8_t>(leaf.board5[b]);
          row.key.frontier_leaf = leaf.frontier_key;
          row.key.seat = seat;
          row.key.combo_dense = static_cast<std::uint16_t>(combo);
          row.cv_chips = cfv[combo];
          row.certified_error_chips = err[combo];
          row.root_pot_chips = r.root_pot;
          writer.add(row);
          ++emitted_rows;
        }
      }
    }
  }
  std::fclose(csv);
  writer.write(out_path);
  const double wall_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start)
          .count();

  // G2 gates the SUMMED NashConv (CERT/QUAL), not a per-combo quantity: the
  // seedless exact V3 kernel's within-instrument and exact-hole gaps both
  // equal its full-deal summed NashConv (bars 0.005 / 0.03 BB). The per-combo
  // drift e_max / RMS are recorded only and propagated through real turn reach
  // rho by G3; a seed half-width is 0 by seedlessness.
  std::fprintf(stderr,
               "wrote %s (+ %s): unique_total=%zu solved_groups=%zu rows=%zu "
               "threads=%zu cand=%llu ref=%llu wall=%.1fs\n"
               "  G2 CERT/QUAL summed NashConv cand max %.6f BB (bars 0.005/0.03)\n"
               "  reference summed NashConv max %.6f BB (seed half-width 0 by seedlessness)\n"
               "  [G3 input, not G2-gated] per-combo drift e_max %.6f, drift RMS %.6f BB\n",
               out_path.c_str(), csv_path.c_str(), unique_games, n_jobs,
               emitted_rows, n_threads,
               static_cast<unsigned long long>(cand_iters),
               static_cast<unsigned long long>(ref_iters), wall_s,
               worst_cand_nc_bb, worst_ref_nc_bb, worst_drift_max_bb,
               worst_drift_rms_bb);
  return 0;
}
