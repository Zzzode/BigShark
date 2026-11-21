// RFC 0047 S3 / G5 serve point B (manual measurement probe, NOT a CTest).
//
// Online ONE-CARD exact-key river re-solve envelope. Each online turn decision
// that reaches the river re-solves exactly ONE reached frontier leaf's river
// subgame (its line-specific five-card GameDef from the frontier enumerator).
// This probe measures, per unique reachable river subgame and per iteration
// checkpoint, the quantities G5/G1 require:
//   - summed NashConv in BB (the R0 river-term gate candidate; UCB <= 0.002),
//   - max per-combo CFR convergence residual |BR-CFV|/root_pot (an
//     equilibrium-convergence diagnostic; NOT the G5 1e-9 responder gate,
//     which is a certifier non-regression inequality BR_s(c) <= locked
//     baseline b_s(c) + 1e-9*pot, a separate rfc0046/resolver component),
//   - wall time and action-node count (G1 per-request timing/nodes inputs),
//   - process peak RSS via getrusage (G1 <= 1 GiB envelope input).
// Seedless: every solve enumerates all compatible deals, deterministic.
//
//   bigshark-rfc0047-online-river STRATUM MAX_ITERS STRIDE [OUT_CSV]
// STRIDE samples every Nth unique subgame (1 = every subgame).
#include <sys/resource.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"

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

long peak_rss_kb() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return -1;
  return ru.ru_maxrss;  // kilobytes on Linux
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s STRATUM MAX_ITERS STRIDE [OUT_CSV] [THREADS] [ONLY_ITER]\n"
                 "                          [GROUP_START] [GROUP_END]\n",
                 argv[0]);
    return 2;
  }
  const StratumSpec* spec = find_stratum(argv[1]);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", argv[1]);
    return 2;
  }
  const std::uint64_t max_iters = std::stoull(argv[2]);
  const std::size_t stride = std::max<std::size_t>(1, std::stoul(argv[3]));
  const char* out_csv = argc >= 5 ? argv[4] : nullptr;
  const std::size_t threads = argc >= 6 ? std::stoul(argv[5]) : 1;
  // Optional single-checkpoint override for full-census runs: when set, emit
  // only this iteration count (must be <= MAX_ITERS) instead of the full ladder,
  // so a 240-subgame census pays one cold solve per subgame, not eight.
  const bool only_one = argc >= 7;
  const std::uint64_t only_iter = only_one ? std::stoull(argv[6]) : 0;
  if (only_one && (only_iter == 0 || only_iter > max_iters)) {
    std::fprintf(stderr, "ONLY_ITER must be in (0, MAX_ITERS]\n");
    return 2;
  }
  // Optional contiguous group-window bounds for resumable chunked census:
  // only unique-subgame indices [GROUP_START, GROUP_END) are solved. Defaults
  // to the whole set; validated against the enumerated group count below.
  const std::size_t group_start = argc >= 8 ? std::stoul(argv[7]) : 0;
  const std::size_t group_end_arg = argc >= 9 ? std::stoul(argv[8]) : 0;
  // Unique reachable river subgames (the online one-card serve objects).
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);
  struct Unique {
    bs::poker::GameDef def;
    Key key;
  };
  std::unordered_map<Key, std::size_t, KeyHash> seen;
  std::vector<Unique> groups;
  for (const FrontierLeaf& lf : leaves) {
    Key k;
    const auto& d = lf.river_def;
    k.ledger = {static_cast<std::uint64_t>(d.pot), static_cast<std::uint64_t>(d.contributions[0]),
                static_cast<std::uint64_t>(d.contributions[1]),
                static_cast<std::uint64_t>(d.stacks[0]), static_cast<std::uint64_t>(d.stacks[1])};
    k.board5 = lf.board5;
    if (seen.emplace(k, groups.size()).second)
      groups.push_back({d, k});
  }
  const std::size_t group_end = group_end_arg == 0 ? groups.size() : group_end_arg;
  if (group_start > group_end || group_end > groups.size()) {
    std::fprintf(stderr, "require 0 <= GROUP_START <= GROUP_END <= %zu\n", groups.size());
    return 2;
  }

  FILE* csv = nullptr;
  if (out_csv != nullptr) {
    csv = std::fopen(out_csv, "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot open '%s'\n", out_csv);
      return 2;
    }
    std::fprintf(csv,
                 "stratum,group,total_groups,iterations,action_nodes,wall_ms,"
                 "nash_conv_bb,nash_conv_pot,combo_resid_pot,peak_rss_kb\n");
  }
  std::printf("stratum=%s unique_river_subgames=%zu stride=%zu max_iters=%llu\n", spec->name,
              groups.size(), stride, static_cast<unsigned long long>(max_iters));

  const std::vector<std::uint64_t> all_cp{250,  500,  1000, 2000,  2500, 3000,
                                          3500, 4000, 8000, 16000, 32000};
  std::vector<std::uint64_t> cps;
  if (only_one) {
    cps.push_back(only_iter);
  } else {
    for (std::uint64_t c : all_cp)
      if (c <= max_iters)
        cps.push_back(c);
  }

  std::size_t sampled = 0;
  for (std::size_t gi = group_start; gi < group_end; gi += stride) {
    const Unique& u = groups[gi];
    const double pot = static_cast<double>(u.def.pot);
    for (std::uint64_t it : cps) {
      RiverSolveConfig cfg;
      cfg.def = u.def;
      cfg.iterations = it;
      cfg.linear_averaging = true;
      cfg.compute_deal_matrix = false;
      cfg.threads = threads;
      const auto t0 = std::chrono::steady_clock::now();
      const RiverSolveResult r = solve_river_subgame(cfg, {});
      const auto t1 = std::chrono::steady_clock::now();
      const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      // Per-combo CFR convergence residual (diagnostic; the G5 1e-9
      // responder gate is the certifier non-regression, not this quantity).
      double resid = 0.0;
      for (std::uint32_t c = 0; c < r.combos_per_seat; ++c) {
        resid = std::max(resid, std::fabs(r.root_br_seat0[c] - r.root_cfv_seat0[c]));
        resid = std::max(resid, std::fabs(r.root_br_seat1[c] - r.root_cfv_seat1[c]));
      }
      const double combo_resid_pot = resid / pot;
      if (csv != nullptr)
        std::fprintf(csv, "%s,%zu,%zu,%llu,%zu,%.4f,%.8f,%.10f,%.3e,%ld\n", spec->name, gi,
                     groups.size(), static_cast<unsigned long long>(it), r.action_nodes, wall_ms,
                     r.nash_conv_bb, r.nash_conv_pot, combo_resid_pot, peak_rss_kb());
      if (csv != nullptr)
        std::fflush(csv);  // durable per-row progress for monitors
    }
    ++sampled;
  }
  if (csv != nullptr)
    std::fclose(csv);
  std::printf("sampled_subgames=%zu done\n", sampled);
  return 0;
}
