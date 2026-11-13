// RFC 0047 S3/G5 native test: bounded cap-k -> cap-0 menu-materiality embedding
// (D164 parameterized over the online raise cap; the production default stays
// cap-1). Pins the paired-DFS map invariants, fail-closed validation, the
// frozen e_embed identity direction (superset-menu BR weakly dominates the
// bounded game's own NashConv), completion-arm ordering, cap-widening
// monotonicity, and seedless bitwise determinism across thread counts. Short
// solves keep the test fast; no cap-0 CFR is run anywhere in this component.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
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
using bs::tree::AbstractTree;
using bs::tree::TreeNode;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::size_t kNoMap = static_cast<std::size_t>(-1);
constexpr std::uint64_t kIters = 200;
constexpr std::uint8_t kCap1 = 1;
constexpr std::uint8_t kCap2 = 2;

bs::poker::GameDef w1_river() {
  return make_river_root_def(*find_stratum("w1"), 1);
}

bool aggressive(bs::poker::ActionType t) {
  return t == bs::poker::ActionType::Bet || t == bs::poker::ActionType::Raise;
}

// The bounded average-policy snapshot is exposed per action node, row-major
// over the 1,081 combos, non-negative and normalized to 1; non-action indices
// stay empty. Exercised for cap-1 (the production menu) and cap-2 (the first
// widened menu the D164 curve measures).
bool test_sigma_snapshot(std::uint8_t cap) {
  RiverSolveConfig cfg;
  cfg.def = w1_river();
  cfg.iterations = 100;
  cfg.linear_averaging = true;
  cfg.action_cap = cap;
  const RiverSolveResult r = solve_river_subgame(cfg, {});
  AbstractTree tree(cfg.def, bs::abstraction::ActionAbstraction::identity(cap));
  CHECK(r.avg_sigma.size() == tree.size());
  for (std::size_t n = 0; n < tree.size(); ++n) {
    if (!tree.node(n).is_action()) {
      CHECK(r.avg_sigma[n].empty());
      continue;
    }
    const std::size_t menu = tree.node(n).actions.size();
    CHECK(r.avg_sigma[n].size() == kC * menu);
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < menu; ++a) {
        const double p = r.avg_sigma[n][i * menu + a];
        CHECK(std::isfinite(p) && p >= 0.0);
        sum += p;
      }
      CHECK(std::fabs(sum - 1.0) < 1e-12);
    }
  }
  return true;
}

// Paired-DFS structural invariants for a bounded cap: shared ordered prefix,
// suffix edges aggressive only, every bounded action node appears in the map,
// and every unmapped cap-0 node hangs off a suffix edge of a mapped subtree.
bool test_paired_map_structure(std::uint8_t cap) {
  const bs::poker::GameDef def = w1_river();
  AbstractTree t0(def, bs::abstraction::ActionAbstraction::identity(0));
  AbstractTree tb(def, bs::abstraction::ActionAbstraction::identity(cap));
  const std::vector<std::size_t> map = map_bounded_to_cap0(t0, tb);
  CHECK(map.size() == t0.size());
  CHECK(map[t0.root_index()] == tb.root_index());

  std::vector<bool> covered(tb.size(), false);
  std::size_t mapped_action = 0;
  for (std::size_t n0 = 0; n0 < t0.size(); ++n0) {
    const std::size_t nb = map[n0];
    if (nb == kNoMap)
      continue;
    CHECK(nb < tb.size());
    covered[nb] = true;
    const TreeNode& a = t0.node(n0);
    const TreeNode& b = tb.node(nb);
    if (!a.is_action())
      continue;
    ++mapped_action;
    CHECK(b.is_action());
    CHECK(b.actions.size() <= a.actions.size());
    for (std::size_t i = 0; i < b.actions.size(); ++i)
      CHECK(a.actions[i] == b.actions[i]);
    for (std::size_t i = b.actions.size(); i < a.actions.size(); ++i)
      CHECK(aggressive(a.actions[i].type));
  }
  for (std::size_t nb = 0; nb < tb.size(); ++nb)
    if (tb.node(nb).is_action())
      CHECK(covered[nb]);

  for (std::size_t u = 0; u < t0.size(); ++u) {
    if (map[u] != kNoMap)
      continue;
    std::size_t child = u;
    std::size_t parent = t0.node(child).parent;
    while (map[parent] == kNoMap) {
      child = parent;
      parent = t0.node(child).parent;
    }
    const TreeNode& anchor = t0.node(parent);
    const std::size_t menu_b = tb.node(map[parent]).actions.size();
    auto it = std::find(anchor.children.begin(), anchor.children.end(), child);
    CHECK(it != anchor.children.end());
    const std::size_t edge = static_cast<std::size_t>(it - anchor.children.begin());
    CHECK(edge >= menu_b);
    CHECK(aggressive(anchor.actions[edge].type));
  }
  CHECK(mapped_action > 0);
  return true;
}

// Reversed tree order, malformed defs, and the forbidden cap-0 online cap must
// fail closed with std::invalid_argument, never a partial map or silent result.
bool test_fail_closed() {
  const bs::poker::GameDef def = w1_river();
  AbstractTree t0(def, bs::abstraction::ActionAbstraction::identity(0));
  AbstractTree t1(def, bs::abstraction::ActionAbstraction::identity(1));
  bool threw = false;
  try {
    (void)map_bounded_to_cap0(t1, t0);  // bounded menu cannot fit inside itself
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  bs::poker::GameDef bad = def;
  bad.board_size = 3;
  threw = false;
  try {
    (void)measure_cap_materiality(bad, kCap1, kIters, 1, CapExtensionCompletion::Uniform);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // cap 0 is the uncapped reference; its equilibrium is explicitly out of scope.
  threw = false;
  try {
    (void)measure_cap_materiality(def, 0, kIters, 1, CapExtensionCompletion::Uniform);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// Cross-shape trees must fail closed. A turn-rooted tree contains a river-deal
// chance node where a river-rooted tree has an action/terminal node; pairing
// them exercises the defensive node-kind guard and must throw
// invalid_argument, never return a partial or silently mis-paired map.
bool test_cross_shape_fail_closed() {
  const bs::poker::GameDef turn = make_stratum_def(*find_stratum("w1"), 1);
  const bs::poker::GameDef river = w1_river();
  AbstractTree tt(turn, bs::abstraction::ActionAbstraction::identity(1));
  AbstractTree tr(river, bs::abstraction::ActionAbstraction::identity(1));
  bool threw = false;
  try {
    (void)map_bounded_to_cap0(tt, tr);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// Frozen measurement direction on the real w1 root for a bounded cap:
//   e_embed >= e_Gk (BR over a superset menu is weakly larger),
//   finite, non-negative, gate verdict consistent with the 0.002 BB bar,
//   and the structural counts match the measured cap-0/bounded tree sizes.
bool test_embedding_dominance(std::uint8_t cap) {
  const bs::poker::GameDef def = w1_river();
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_cap = cap;
  const RiverSolveResult bounded = solve_river_subgame(cfg, {});

  const CapMateriality m =
      measure_cap_materiality(def, cap, kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(m.online_cap == cap);
  CHECK(std::isfinite(m.embedded_exploitability_bb));
  CHECK(m.embedded_exploitability_bb >= 0.0);
  CHECK(m.cap0_action_nodes > bounded.action_nodes);
  CHECK(m.online_action_nodes == bounded.action_nodes);
  CHECK(m.unmapped_ev_reachable_nodes == 0);
  CHECK(m.mapped_nodes == bounded.action_nodes);
  // Frozen identity: e_embed = e_Gk + non-negative pure menu term.
  CHECK(m.embedded_exploitability_bb + 1e-12 >= bounded.nash_conv_bb);
  CHECK(m.gate_pass == (m.embedded_exploitability_bb <= kCapMaterialityGateBb));
  return true;
}

// The BrFavorable arm bounds every completion of the extension subtree and so
// must strictly dominate the Uniform arm on the cap-1 counterexample fixture
// (cap-0 has 11,232 action nodes vs cap-1's 10: extension subtrees are reached
// by the BR walk); equality would mean the completion dispatch is inert.
bool test_completion_arm_ordering() {
  const bs::poker::GameDef def = w1_river();
  const CapMateriality uniform =
      measure_cap_materiality(def, kCap1, kIters, 1, CapExtensionCompletion::Uniform);
  const CapMateriality favorable =
      measure_cap_materiality(def, kCap1, kIters, 1, CapExtensionCompletion::BrFavorable);
  CHECK(std::isfinite(favorable.embedded_exploitability_bb));
  CHECK(favorable.embedded_exploitability_bb > uniform.embedded_exploitability_bb);
  CHECK(favorable.mapped_nodes == uniform.mapped_nodes);
  CHECK(favorable.cap0_action_nodes == uniform.cap0_action_nodes);
  CHECK(favorable.unmapped_ev_reachable_nodes == 0);
  return true;
}

// Evaluator correctness oracle. The embedded EV walk never enters a cap-0-only
// extension, so its raw per-combo root values are completion-independent and
// must reproduce the bounded kernel's OWN root CFV times the 990
// compatible-deal normalization to floating-point precision. Run for cap-1 and
// cap-2 so the parameterized embedding is independently proven, not just the
// original cap-1 path. BR values must weakly dominate EV for every combo.
bool test_ev_matches_kernel_cfv(std::uint8_t cap) {
  const bs::poker::GameDef def = w1_river();
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_cap = cap;
  const RiverSolveResult bounded = solve_river_subgame(cfg, {});

  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    const EmbeddedRootValues v = embed_root_values(def, cap, kIters, 1, arm);
    CHECK(v.ev_seat0.size() == kC && v.ev_seat1.size() == kC);
    double worst = 0.0;
    for (std::uint32_t i = 0; i < kC; ++i) {
      worst = std::max(worst, std::fabs(v.ev_seat0[i] - bounded.root_cfv_seat0[i] * 990.0));
      worst = std::max(worst, std::fabs(v.ev_seat1[i] - bounded.root_cfv_seat1[i] * 990.0));
      CHECK(v.br_seat0[i] + 1e-9 >= v.ev_seat0[i]);
      CHECK(v.br_seat1[i] + 1e-9 >= v.ev_seat1[i]);
    }
    CHECK(std::isfinite(worst));
    CHECK(worst < 1e-6);  // independent walks over the shared support agree
  }
  return true;
}

// Seedless premise: the measurement is bitwise reproducible across repeated
// serial runs and across worker thread counts. Exercised at both cap-1 and
// cap-2 (the bounded solve is the only threaded stage; the embedding walk is a
// deterministic serial recursion).
bool test_bitwise_determinism(std::uint8_t cap) {
  const bs::poker::GameDef def = w1_river();
  const CapMateriality a =
      measure_cap_materiality(def, cap, kIters, 1, CapExtensionCompletion::Uniform);
  const CapMateriality b =
      measure_cap_materiality(def, cap, kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(a.embedded_exploitability_bb == b.embedded_exploitability_bb);
  const CapMateriality p =
      measure_cap_materiality(def, cap, kIters, 8, CapExtensionCompletion::Uniform);
  CHECK(a.embedded_exploitability_bb == p.embedded_exploitability_bb);
  CHECK(a.gate_pass == p.gate_pass);
  return true;
}

// D164 cap-widening direction on THIS fixture. identity(2)'s menu strictly
// contains identity(1)'s on this root, so the cap-2 bounded game has strictly
// more action nodes (the structural menu-scrambling tripwire). The strict
// e_embed(cap2) < e_embed(cap1) inequality below is an EMPIRICAL deterministic
// property of this root and the fixed 200-iteration budget, NOT a theorem: for
// exact restricted equilibria only weak nesting of the pure-menu term holds,
// and finite-iteration cap-1/cap-2 average policies are distinct objects whose
// own NashConv need not be ordered (the 300-iteration production curve in fact
// rises from cap-5 to cap-6 once the online-NashConv floor dominates). The
// seedless solver makes this assertion bitwise reproducible, and at cap-1 vs
// cap-2 the measured margin is ~6x, so it cannot flake on this binary.
bool test_cap_widening_monotonicity() {
  const bs::poker::GameDef def = w1_river();
  const CapMateriality c1 =
      measure_cap_materiality(def, kCap1, kIters, 1, CapExtensionCompletion::Uniform);
  const CapMateriality c2 =
      measure_cap_materiality(def, kCap2, kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(c1.online_cap == kCap1 && c2.online_cap == kCap2);
  CHECK(c2.online_action_nodes > c1.online_action_nodes);
  CHECK(c2.cap0_action_nodes == c1.cap0_action_nodes);  // same cap-0 reference
  CHECK(std::isfinite(c2.embedded_exploitability_bb));
  CHECK(c2.embedded_exploitability_bb < c1.embedded_exploitability_bb);
  return true;
}

// When a root's cap-0 and bounded menus coincide, the embedding is the same
// game and the two independent walks must agree to numerical precision. The
// frozen strata's all-in frontier lines yield terminal-only roots (identical
// menus); scan the real enumerator for one.
bool test_equal_menu_identity_when_available() {
  for (const StratumSpec& s : kStrata) {
    const std::vector<FrontierLeaf> leaves = enumerate_frontiers(s, 1);
    for (const FrontierLeaf& lf : leaves) {
      AbstractTree t0(lf.river_def, bs::abstraction::ActionAbstraction::identity(0));
      AbstractTree t1(lf.river_def, bs::abstraction::ActionAbstraction::identity(1));
      if (t0.size() != t1.size())
        continue;
      RiverSolveConfig cfg;
      cfg.def = lf.river_def;
      cfg.iterations = kIters;
      cfg.linear_averaging = true;
      const RiverSolveResult bounded = solve_river_subgame(cfg, {});
      const CapMateriality m =
          measure_cap_materiality(lf.river_def, kCap1, kIters, 1, CapExtensionCompletion::Uniform);
      CHECK(std::fabs(m.embedded_exploitability_bb - bounded.nash_conv_bb) < 1e-9);
      std::printf("rfc0047_cap_embedding: equal-menu identity exercised on %s\n", s.name);
      return true;
    }
  }
  std::printf("rfc0047_cap_embedding: no equal-menu stratum; identity fixture skipped\n");
  return true;
}

}  // namespace

int main() {
  if (!test_sigma_snapshot(kCap1) || !test_sigma_snapshot(kCap2) ||
      !test_paired_map_structure(kCap1) || !test_paired_map_structure(kCap2) ||
      !test_fail_closed() || !test_cross_shape_fail_closed() || !test_embedding_dominance(kCap1) ||
      !test_embedding_dominance(kCap2) || !test_completion_arm_ordering() ||
      !test_ev_matches_kernel_cfv(kCap1) || !test_ev_matches_kernel_cfv(kCap2) ||
      !test_bitwise_determinism(kCap1) || !test_bitwise_determinism(kCap2) ||
      !test_cap_widening_monotonicity() || !test_equal_menu_identity_when_available())
    return 1;
  std::printf("rfc0047_cap_embedding: all checks passed\n");
  return 0;
}
