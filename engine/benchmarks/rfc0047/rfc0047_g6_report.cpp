// RFC 0047 S3/G6 manual probe (NOT a CTest): exercises the real bench wiring
// from an exact river re-solve through the certified serve entry into the dual
// quality report, and emits one G6 record per unique reached river subgame.
//
// Each row carries BOTH required G6 quantities (RFC 0039 D1): the within-
// instrument convergence predicate (candidate summed NashConv vs the 0.005 BB
// CERT bar) AND the exact-hand gap plus the composed full-strategy M2 delta vs
// its gate (river R0 = 0.002 BB), so a single favorable convergence number can
// never be reported alone (no quality laundering). River is the fast serve
// point; the turn assembler uses identical report code and is covered by
// test_rfc0047_quality_report with controlled HybridNashConv inputs.
//
// Usage: bigshark-rfc0047-g6-report STRATUM MAX_ITERS STRIDE [OUT_CSV] [THREADS]
#include <sys/resource.h>

#include <array>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontier_enumerator.hpp"
#include "quality_report.hpp"
#include "river_serve.hpp"
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

struct Unique {
  bs::poker::GameDef def;
  Key key;
};

long peak_rss_kb() {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return -1;
  return ru.ru_maxrss;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s STRATUM MAX_ITERS STRIDE [OUT_CSV] [THREADS]\n", argv[0]);
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

  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);
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

  FILE* csv = nullptr;
  if (out_csv != nullptr) {
    csv = std::fopen(out_csv, "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot open '%s'\n", out_csv);
      return 2;
    }
    std::fprintf(csv,
                 "stratum,group,iterations,within_instrument_nc_bb,within_converged,"
                 "exact_gap_bb,exact_qual_pass,composed_m2_delta_bb,composed_gate_bb,"
                 "composed_gate_pass,serve_status,peak_rss_kb\n");
  }
  std::printf("g6-report stratum=%s unique=%zu stride=%zu iters=%llu\n", spec->name, groups.size(),
              stride, static_cast<unsigned long long>(max_iters));

  std::size_t sampled = 0;
  std::size_t certified_rows = 0;
  for (std::size_t gi = 0; gi < groups.size(); gi += stride) {
    RiverSolveConfig cfg;
    cfg.def = groups[gi].def;
    cfg.iterations = max_iters;
    cfg.linear_averaging = true;
    cfg.threads = threads;

    // The sealed responder margin is supplied externally to the serve entry;
    // this bench uses the deterministic kernel's own independently-walked root
    // BR as the locked reference (production would pass a separately sealed
    // trunk margin). The G6 record consumes the serve verdict, it does not
    // derive an oracle.
    const RiverSolveResult ref = solve_river_subgame(cfg, {});
    RiverServeRequest req;
    req.solve = cfg;
    req.baseline_seat0 = std::span<const double>(ref.root_br_seat0);
    req.baseline_seat1 = std::span<const double>(ref.root_br_seat1);
    const RiverServeResult served = serve_river_certified(req);
    const QualityDualReport q = river_quality_report(served);
    if (served.status == ServeStatus::Certified)
      ++certified_rows;

    if (csv != nullptr)
      std::fprintf(csv, "%s,%zu,%llu,%.8f,%d,%.8f,%d,%.8f,%.8f,%d,%d,%ld\n", spec->name, gi,
                   static_cast<unsigned long long>(max_iters), q.within_instrument_nash_conv_bb,
                   q.within_instrument_converged ? 1 : 0, q.exact_gap_bb, q.exact_qual_pass ? 1 : 0,
                   q.composed_m2_delta_bb, q.composed_gate_bb, q.composed_gate_pass ? 1 : 0,
                   static_cast<int>(served.status), peak_rss_kb());
    ++sampled;
  }
  if (csv != nullptr)
    std::fclose(csv);
  std::printf("g6-report sampled=%zu certified=%zu\n", sampled, certified_rows);
  return 0;
}
