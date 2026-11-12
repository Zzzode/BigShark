// RFC 0047 S3/G5 cap-1 vs cap-0 menu-materiality probe (manual, NOT a CTest).
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
//        (default: all six frozen strata; prints cap0 vs cap1 node counts)
//        bigshark-rfc0047-cap-materiality embed STRATUM ITERS [THREADS]
//        (single reached root; frozen e_embed under both completion arms)
//        bigshark-rfc0047-cap-materiality embed-gate STRATUM ITERS
//            [THREADS] [uniform|br_favorable]
//        (per-stratum MAX e_embed over every reached river root; offline batch)
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "tree_budget.hpp"

using namespace bs::rfc0047;

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

// Frozen Gate-3 embedding measurement on ONE reached river root, run under
// BOTH completion arms so the robustness envelope is visible in one invocation.
int embed_one(const StratumSpec& spec, std::uint64_t iters, std::size_t threads) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  if (leaves.empty())
    return 2;
  const bs::poker::GameDef& def = leaves.front().river_def;
  std::printf(
      "stratum,completion,cap0_action_nodes,cap1_action_nodes,mapped_nodes,cap1_nash_conv_bb,"
      "e_embed_bb,pure_menu_term_bb,gate_m_bb,gate_pass\n");
  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    const CapMateriality m = measure_cap_materiality(def, iters, threads, arm);
    std::printf("%s,%s,%zu,%zu,%zu,%.8f,%.8f,%.8f,%.4f,%d\n", spec.name,
                arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable",
                m.cap0_action_nodes, m.cap1_action_nodes, m.mapped_nodes, m.cap1_nash_conv_bb,
                m.embedded_exploitability_bb, m.embedded_exploitability_bb - m.cap1_nash_conv_bb,
                kCapMaterialityGateBb, m.gate_pass ? 1 : 0);
  }
  return 0;
}

// Per-stratum gate shape: the MAXIMUM e_embed over EVERY reached river root
// (seedless exact enumeration, no sampling half-width). This is an offline
// batch run (432-528 roots per stratum, each requiring its own cap-1 solve);
// only the binding worst value and coverage count are reported.
int embed_gate(const StratumSpec& spec, std::uint64_t iters, std::size_t threads,
               CapExtensionCompletion arm) {
  const StratumCapResult g = measure_stratum_cap_materiality(spec, iters, threads, arm);
  std::printf("stratum,completion,roots_measured,worst_e_embed_bb,gate_m_bb,gate_pass\n");
  std::printf("%s,%s,%zu,%.8f,%.4f,%d\n", spec.name,
              arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable", g.roots_measured,
              g.worst_embedded_exploitability_bb, kCapMaterialityGateBb, g.gate_pass ? 1 : 0);
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
  if (mode == "embed" || mode == "embed-gate") {
    // embed STRATUM ITERS [THREADS]
    // embed-gate STRATUM ITERS [THREADS] [uniform|br_favorable]
    if (argc < 4) {
      std::fprintf(stderr, "usage: %s %s STRATUM ITERS [THREADS] [uniform|br_favorable]\n", argv[0],
                   mode.c_str());
      return 2;
    }
    const StratumSpec* s = find_stratum(argv[2]);
    if (s == nullptr) {
      std::fprintf(stderr, "unknown stratum '%s'\n", argv[2]);
      return 2;
    }
    const std::uint64_t iters = std::stoull(argv[3]);
    const std::size_t threads = argc >= 5 ? std::stoul(argv[4]) : 1;
    if (mode == "embed")
      return embed_one(*s, iters, threads);
    const CapExtensionCompletion arm = (argc >= 6 && std::string(argv[5]) == "br_favorable")
                                           ? CapExtensionCompletion::BrFavorable
                                           : CapExtensionCompletion::Uniform;
    return embed_gate(*s, iters, threads, arm);
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
  std::fprintf(stderr, "unknown mode '%s' (supported: treesize, solve, curve, embed, embed-gate)\n",
               mode.c_str());
  return 2;
}
