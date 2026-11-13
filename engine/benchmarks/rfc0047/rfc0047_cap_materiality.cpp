// RFC 0047 S3/G5 bounded cap-k vs cap-0 menu-materiality probe (manual, NOT a
// CTest; D164 parameterizes the online raise cap, production default stays 1).
//
// Step 1 (this mode): structural feasibility. ActionAbstraction::identity(cap)
// uses cap = max raises per street, where 0 means NO cap (the exact full menu,
// the cap-0 reference) and 1 is the bounded online menu the kernels use. The
// flop-rooted cap-0 tree is known-infeasible (RFC 0046: >100M nodes / 55 GiB);
// this probe measures whether the much smaller rfc0047 turn-rooted / reached
// river subtrees admit a solvable cap-0 reference. It only builds and counts;
// no CFR runs in tree-size mode.
//
// Usage: bigshark-rfc0047-cap-materiality treesize [STRATUM...]
//        (default: all six frozen strata; prints cap-0 vs bounded node counts)
//        bigshark-rfc0047-cap-materiality embed STRATUM ITERS
//            [THREADS] [CAP]
//        (single reached root; frozen e_embed under both completion arms for
//        one bounded cap, default cap 1)
//        bigshark-rfc0047-cap-materiality capcurve STRATUM ITERS
//            [THREADS] [MAXCAP]
//        (D164 single-root bounded quality/cost curve: one row per bounded
//        cap 1..MAXCAP and completion arm, with online NC, pure menu term,
//        node count, wall/CPU ms and current RSS; a single-root crossing is a
//        candidate only, never a global MAX or a Certified/M2 pass)
//        bigshark-rfc0047-cap-materiality embed-gate STRATUM ITERS
//            [THREADS] [CAP] [uniform|br_favorable]
//        (per-stratum MAX e_embed over every reached river root; offline batch)
#include <sys/resource.h>
#include <unistd.h>

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "tree_budget.hpp"

using namespace bs::rfc0047;

namespace {
// Largest bounded raise cap accepted on the CLI. Far above any measured
// materiality-relevant cap (cap-6 already approaches the full cap-0 menu); the
// bound exists to reject uint8 narrowing and the out-of-scope cap-0.
constexpr unsigned long kMaxOnlineCap = 32;

// Bounded per-measurement resource observation (D164 same-caliber cost).
// wall_ms and cpu_ms are deltas around the measurement call. rss_peak_kb is the
// process high-water resident set from getrusage (ru_maxrss, KB on Linux): the
// authoritative per-cap footprint when the probe is run one cap per process.
// rss_current_kb is /proc/self/statm immediately after the call; inside a
// multi-cap capcurve process both RSS fields are process-level, so per-cap RSS
// comparisons must use single-cap `embed` invocations, not an in-process loop.
struct ResourceSample {
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
  long rss_peak_kb = 0;
  long rss_current_kb = 0;
};

double timeval_ms(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
}

long current_rss_kb() {
  std::ifstream status("/proc/self/statm");
  long resident_pages = 0;
  long total_pages = 0;
  status >> total_pages >> resident_pages;
  if (resident_pages <= 0)
    return -1;
  long page_kb = sysconf(_SC_PAGESIZE) / 1024L;
  return resident_pages * (page_kb > 0 ? page_kb : 4L);
}

ResourceSample sample_resource(const std::chrono::steady_clock::time_point& t0, const rusage& ru0) {
  rusage ru1{};
  getrusage(RUSAGE_SELF, &ru1);
  const auto t1 = std::chrono::steady_clock::now();
  ResourceSample s;
  s.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  s.cpu_ms = (timeval_ms(ru1.ru_utime) - timeval_ms(ru0.ru_utime)) +
             (timeval_ms(ru1.ru_stime) - timeval_ms(ru0.ru_stime));
  s.rss_peak_kb = ru1.ru_maxrss;  // Linux: kilobytes
  s.rss_current_kb = current_rss_kb();
  return s;
}
}  // namespace

namespace {

// Build one tree and report node count, or record the resource boundary if the
// uncapped menu cannot be represented within the 1 GiB TreeLimits byte cap.
std::string node_count_or_bound(const bs::poker::GameDef& def, std::uint8_t cap) {
  try {
    bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(cap));
    return std::to_string(tree.size());
  } catch (const bs::tree::tree_resource_exhausted&) {
    return "RESOURCE_EXHAUSTED_1GiB";
  }
}

int treesize(const std::vector<const StratumSpec*>& strata) {
  std::printf("stratum,root,cap,node_count_or_bound\n");
  for (const StratumSpec* s : strata) {
    // Full turn-rooted tree (all 48 river chance children, every betting line).
    const bs::poker::GameDef turn_def = make_stratum_def(*s, 1);
    for (std::uint8_t cap : {std::uint8_t{0}, std::uint8_t{1}})
      std::printf("%s,turn-rooted,%u,%s\n", s->name, static_cast<unsigned>(cap),
                  node_count_or_bound(turn_def, cap).c_str());
    // A single reached five-card river subtree (the actual online re-solve
    // object): one board, no 48-way chance fan-out. Use the first unique
    // frontier leaf's river GameDef so the comparison is same-root/unit.
    const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*s, 1);
    if (!leaves.empty()) {
      for (std::uint8_t cap : {std::uint8_t{0}, std::uint8_t{1}})
        std::printf("%s,reached-river,%u,%s\n", s->name, static_cast<unsigned>(cap),
                    node_count_or_bound(leaves.front().river_def, cap).c_str());
    }
  }
  return 0;
}

// Solve the SAME reached five-card river subgame under cap-0 (uncapped) and
// cap-1 (bounded) menus and report each equilibrium's own exact summed NashConv.
// This is the same-root/same-unit/same-ranges precondition for the materiality
// comparison; both games are exact seedless solves of their own menu. The strict
// cross-game exploitability of the cap-1 policy inside the cap-0 game is a
// separate embedding measurement added next.
int solve_compare(const StratumSpec& spec, std::uint64_t iters, std::size_t threads) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef def = leaves.front().river_def;
  std::printf("stratum,cap,action_nodes,nash_conv_bb,nash_conv_pot,combos\n");
  for (std::uint8_t cap : {std::uint8_t{0}, std::uint8_t{1}}) {
    RiverSolveConfig cfg;
    cfg.def = def;
    cfg.iterations = iters;
    cfg.linear_averaging = true;
    cfg.threads = threads;
    cfg.action_cap = cap;
    const RiverSolveResult r = solve_river_subgame(cfg, {});
    std::printf("%s,%u,%zu,%.8f,%.10f,%zu\n", spec.name, static_cast<unsigned>(cap), r.action_nodes,
                r.nash_conv_bb, r.nash_conv_pot, r.combos_per_seat);
  }
  return 0;
}

// One cap-0 solve with geometric checkpoints, printing the summed NashConv
// trajectory so the convergence cost of the exact reference is measured in a
// single run (no repeated solves). Used to decide whether a converged cap-0
// reference is attainable within the offline budget.
int curve(const StratumSpec& spec, std::uint64_t iters, std::size_t threads) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  if (leaves.empty())
    return 2;
  std::vector<std::uint64_t> cps;
  for (std::uint64_t c = 50; c <= iters; c *= 2)
    cps.push_back(c);
  if (cps.empty() || cps.back() != iters)
    cps.push_back(iters);
  RiverSolveConfig cfg;
  cfg.def = leaves.front().river_def;
  cfg.iterations = iters;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  cfg.action_cap = 0;  // uncapped exact reference
  const RiverSolveResult r = solve_river_subgame(cfg, std::span<const std::uint64_t>(cps));
  std::printf("stratum=cap0 action_nodes=%zu\niter,nash_conv_bb\n", r.action_nodes);
  for (std::size_t i = 0; i < cps.size() && i < r.history_nash_conv_bb.size(); ++i)
    std::printf("%llu,%.8f\n", static_cast<unsigned long long>(cps[i]), r.history_nash_conv_bb[i]);
  return 0;
}

const char* arm_name(CapExtensionCompletion arm) {
  return arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable";
}

// Shared CSV header for the single-root embedding rows with resource cost.
const char* kEmbedHeader =
    "cap,completion,cap0_action_nodes,online_action_nodes,mapped_nodes,online_nash_conv_bb,"
    "e_embed_bb,pure_menu_term_bb,gate_m_bb,gate_pass,wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n";

void print_embed_row(const CapMateriality& m, CapExtensionCompletion arm,
                     const ResourceSample& rs) {
  std::printf("%u,%s,%zu,%zu,%zu,%.8f,%.8f,%.8f,%.4f,%d,%.3f,%.3f,%ld,%ld\n",
              static_cast<unsigned>(m.online_cap), arm_name(arm), m.cap0_action_nodes,
              m.online_action_nodes, m.mapped_nodes, m.online_nash_conv_bb,
              m.embedded_exploitability_bb, m.embedded_exploitability_bb - m.online_nash_conv_bb,
              kCapMaterialityGateBb, m.gate_pass ? 1 : 0, rs.wall_ms, rs.cpu_ms, rs.rss_peak_kb,
              rs.rss_current_kb);
}

// Frozen Gate-3 embedding measurement on ONE reached river root for ONE bounded
// cap, run under BOTH completion arms so the robustness envelope is visible in
// one invocation.
int embed_one(const StratumSpec& spec, std::uint8_t cap, std::uint64_t iters, std::size_t threads) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;
  std::printf("%s", kEmbedHeader);
  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    rusage ru0{};
    getrusage(RUSAGE_SELF, &ru0);
    const auto t0 = std::chrono::steady_clock::now();
    const CapMateriality m = measure_cap_materiality(def, cap, iters, threads, arm);
    print_embed_row(m, arm, sample_resource(t0, ru0));
  }
  return 0;
}

// D164 single-root bounded quality/cost curve: sweep bounded caps 1..maxcap on
// the SAME anchor root under a comparable solve config and both completion
// arms. This locates the smallest cap whose pure menu term approaches M without
// a blind full-stratum/root sweep; a crossing here is a CANDIDATE only.
int cap_curve(const StratumSpec& spec, std::uint8_t maxcap, std::uint64_t iters,
              std::size_t threads) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;
  std::printf("%s", kEmbedHeader);
  for (std::uint8_t cap = 1; cap <= maxcap; ++cap) {
    for (const CapExtensionCompletion arm :
         {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
      rusage ru0{};
      getrusage(RUSAGE_SELF, &ru0);
      const auto t0 = std::chrono::steady_clock::now();
      const CapMateriality m = measure_cap_materiality(def, cap, iters, threads, arm);
      print_embed_row(m, arm, sample_resource(t0, ru0));
      std::fflush(stdout);  // preserve partial evidence if a later cap times out
    }
  }
  return 0;
}

// Per-stratum gate shape: the MAXIMUM e_embed over EVERY reached river root
// (seedless exact enumeration, no sampling half-width). This is an offline
// batch run (432-528 roots per stratum, each requiring its own bounded solve);
// only the binding worst value and coverage count are reported.
int embed_gate(const StratumSpec& spec, std::uint8_t cap, std::uint64_t iters, std::size_t threads,
               CapExtensionCompletion arm) {
  const StratumCapResult g = measure_stratum_cap_materiality(spec, cap, iters, threads, arm);
  std::printf("cap,stratum,completion,roots_measured,worst_e_embed_bb,gate_m_bb,gate_pass\n");
  std::printf("%u,%s,%s,%zu,%.8f,%.4f,%d\n", static_cast<unsigned>(cap), spec.name, arm_name(arm),
              g.roots_measured, g.worst_embedded_exploitability_bb, kCapMaterialityGateBb,
              g.gate_pass ? 1 : 0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc >= 2 ? argv[1] : "treesize";
  if (mode == "solve") {
    // solve STRATUM ITERS [THREADS]
    if (argc < 4) {
      std::fprintf(stderr, "usage: %s solve STRATUM ITERS [THREADS]\n", argv[0]);
      return 2;
    }
    const StratumSpec* s = find_stratum(argv[2]);
    if (s == nullptr) {
      std::fprintf(stderr, "unknown stratum '%s'\n", argv[2]);
      return 2;
    }
    const std::uint64_t iters = std::stoull(argv[3]);
    const std::size_t threads = argc >= 5 ? std::stoul(argv[4]) : 1;
    return solve_compare(*s, iters, threads);
  }
  if (mode == "curve") {
    // curve STRATUM ITERS [THREADS]
    if (argc < 4) {
      std::fprintf(stderr, "usage: %s curve STRATUM ITERS [THREADS]\n", argv[0]);
      return 2;
    }
    const StratumSpec* s = find_stratum(argv[2]);
    if (s == nullptr) {
      std::fprintf(stderr, "unknown stratum '%s'\n", argv[2]);
      return 2;
    }
    return curve(*s, std::stoull(argv[3]), argc >= 5 ? std::stoul(argv[4]) : 1);
  }
  if (mode == "embed" || mode == "capcurve" || mode == "embed-gate") {
    // embed      STRATUM ITERS [THREADS] [CAP]
    // capcurve   STRATUM ITERS [THREADS] [MAXCAP]
    // embed-gate STRATUM ITERS [THREADS] [CAP] [uniform|br_favorable]
    if (argc < 4) {
      std::fprintf(stderr, "usage: %s %s STRATUM ITERS [THREADS] [CAP] [uniform|br_favorable]\n",
                   argv[0], mode.c_str());
      return 2;
    }
    const StratumSpec* s = find_stratum(argv[2]);
    if (s == nullptr) {
      std::fprintf(stderr, "unknown stratum '%s'\n", argv[2]);
      return 2;
    }
    const std::uint64_t iters = std::stoull(argv[3]);
    const std::size_t threads = argc >= 5 ? std::stoul(argv[4]) : 1;
    const unsigned long cap_raw = argc >= 6 ? std::stoul(argv[5]) : 1;
    // Guard the uint8 narrowing and reject the out-of-scope uncapped cap-0
    // (whose equilibrium the frozen formula forbids) with a clean CLI error
    // instead of an uncaught invalid_argument abort.
    if (cap_raw < 1 || cap_raw > kMaxOnlineCap) {
      std::fprintf(stderr, "cap must be in [1, %u] (cap-0 equilibrium is out of scope)\n",
                   static_cast<unsigned>(kMaxOnlineCap));
      return 2;
    }
    const std::uint8_t cap = static_cast<std::uint8_t>(cap_raw);
    if (mode == "embed")
      return embed_one(*s, cap, iters, threads);
    if (mode == "capcurve")
      return cap_curve(*s, cap, iters, threads);
    const CapExtensionCompletion arm = (argc >= 7 && std::string(argv[6]) == "br_favorable")
                                           ? CapExtensionCompletion::BrFavorable
                                           : CapExtensionCompletion::Uniform;
    return embed_gate(*s, cap, iters, threads, arm);
  }
  std::vector<const StratumSpec*> strata;
  if (argc >= 3) {
    for (int i = 2; i < argc; ++i) {
      const StratumSpec* s = find_stratum(argv[i]);
      if (s == nullptr) {
        std::fprintf(stderr, "unknown stratum '%s'\n", argv[i]);
        return 2;
      }
      strata.push_back(s);
    }
  } else {
    for (const StratumSpec& s : kStrata)
      strata.push_back(&s);
  }
  if (mode == "treesize")
    return treesize(strata);
  std::fprintf(stderr,
               "unknown mode '%s' (supported: treesize, solve, curve, embed, capcurve, "
               "embed-gate)\n",
               mode.c_str());
  return 2;
}
