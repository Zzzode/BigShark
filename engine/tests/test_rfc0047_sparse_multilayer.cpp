// RFC 0048 D208 native test: sparse multi-layer menu selection that isolates
// the deep abstraction (menu) error from the solver update error on tiny
// river roots. Reuses the D202 MixedMenuGame solver, mask validators, and the
// D192/D195 decomposition unchanged; real seedless solves, behavioral
// assertions only, no source-text or log matching. Frozen cap-0 reference and
// every production surface stay untouched.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/mixed_menu.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/union_menu.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kIters = 60;
constexpr double kChipTol = 1e-9;
constexpr double kDistTol = 1e-12;

bs::poker::GameDef river_root(std::uint64_t stack_behind) {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {stack_behind, stack_behind, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

struct Fixture {
  bs::poker::GameDef def;
  ActionAbstraction coarse;
  ActionAbstraction reference;
};

Fixture make_fixture(std::uint64_t stack_behind = 8) {
  return {river_root(stack_behind), make_grid_abstraction(SizingGrid::Coarse, 1),
          ActionAbstraction::identity(0)};
}

bool throws_invalid(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

// Public betting depth: number of public action edges on the parent chain.
std::size_t public_depth(const AbstractTree& tree, std::size_t n) {
  std::size_t depth = 0;
  std::size_t cur = n;
  const std::size_t root = tree.root_index();
  while (cur != root) {
    ++depth;
    cur = tree.node(cur).parent;
  }
  return depth;
}

std::size_t reachable_action_nodes(const AbstractTree& tree, const MixedMenuMask& mask) {
  std::vector<bool> reachable(tree.size(), false);
  reachable[tree.root_index()] = true;
  std::size_t count = 0;
  for (std::size_t n = 0; n < tree.size(); ++n) {
    if (!reachable[n] || !tree.node(n).is_action())
      continue;
    ++count;
    const auto& kids = tree.node(n).children;
    for (std::size_t a = 0; a < kids.size(); ++a)
      if (mask.allowed[n][a])
        reachable[kids[a]] = true;
  }
  return count;
}

double pure_menu_uniform(const Fixture& f, const AbstractTree& ref_tree,
                         const MixedMenuSolveResult& solved, const SubsetMap& map) {
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref_tree, ref_tree,
                                                        map, CapExtensionCompletion::Uniform);
  return rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
}

// S1: a B=1 sparse game still has an observable deep restricted node with a
// positive menu gap under the full reference BR (the deep deviation exists).
// Parameterized by stack with the T9/M8 8->20-chip fallback.
bool has_deep_restricted_gap(std::uint64_t stack_behind) {
  const Fixture g = make_fixture(stack_behind);
  const AbstractTree ref_tree(g.def, g.reference);
  const AbstractTree coarse_tree(g.def, g.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 1);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(g.def, g.reference, mask, kIters, 1);
  const ReachBoundResult rb = reach_bound_from_snapshot(g.def, solved.river, ref_tree, ref_tree,
                                                        map, CapExtensionCompletion::Uniform);
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s]) {
      if (r.reference_node == ref_tree.root_index())
        continue;  // the widened root is not the deep deviation
      for (std::uint32_t i = 0; i < kC; ++i)
        if (r.delta[i] - r.bounded_gap[i] > kChipTol)
          return true;
    }
  return false;
}

bool test_s1_red_existence() {
  bool deep_gap = has_deep_restricted_gap(8);
  if (!deep_gap) {
    std::printf("S1 diagnostic: no deep restricted gap on 8-chip; retrying 20-chip\n");
    deep_gap = has_deep_restricted_gap(20);
  }
  CHECK(deep_gap);
  return true;
}

// S2: deterministic public selection plus the three fail-closed preconditions.
bool test_s2_selection_and_preconditions() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);

  // Non-positive budget throws.
  CHECK(throws_invalid([&] { make_sparse_multilayer_mask(ref_tree, coarse_tree, 0); }));

  // Determinism: repeated builds are identical.
  const MixedMenuMask a = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const MixedMenuMask b = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  CHECK(a.allowed.size() == b.allowed.size());
  for (std::size_t n = 0; n < a.allowed.size(); ++n)
    for (std::size_t k = 0; k < a.allowed[n].size(); ++k)
      CHECK(a.allowed[n][k] == b.allowed[n][k]);

  // Public-only ordering: every widened non-root node is at a depth no smaller
  // than the root's, and selection never depends on combo (mask has no combo
  // dimension structurally — assert row width equals the public menu).
  const SubsetMap base = map_bounded_subset(ref_tree, coarse_tree);
  const std::size_t root = ref_tree.root_index();
  std::size_t widened = 0;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!ref_tree.node(n).is_action() || n == root)
      continue;
    if (base.node_map[n] == SubsetMap::kNoNode)
      continue;
    bool widened_here = false;
    for (std::size_t k = 0; k < ref_tree.node(n).actions.size(); ++k) {
      const bool base_allows = base.edge_map[n][k] != -1;
      if (!base_allows && a.allowed[n][k])
        widened_here = true;
    }
    if (widened_here) {
      ++widened;
      CHECK(public_depth(ref_tree, n) >= public_depth(ref_tree, root));
    }
  }
  CHECK(widened >= 1);  // budget 2 widened at least one non-root node

  // Over-budget clamp (non-tautological): independently count the eligible
  // suppressed coarse nodes, then show the huge budget clamps to exactly that
  // count — make_sparse(N) equals make_sparse(huge) and exactly N nodes are
  // widened beyond the coarse baseline.
  std::size_t eligible_count = 0;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (base.node_map[n] == SubsetMap::kNoNode || !ref_tree.node(n).is_action())
      continue;
    if (std::any_of(base.edge_map[n].begin(), base.edge_map[n].end(),
                    [](int edge) { return edge == -1; }))
      ++eligible_count;
  }
  CHECK(eligible_count >= 2);
  const MixedMenuMask at_count = make_sparse_multilayer_mask(ref_tree, coarse_tree, eligible_count);
  const MixedMenuMask clamped = make_sparse_multilayer_mask(ref_tree, coarse_tree, 100000);
  for (std::size_t n = 0; n < clamped.allowed.size(); ++n)
    for (std::size_t k = 0; k < clamped.allowed[n].size(); ++k)
      CHECK(at_count.allowed[n][k] == clamped.allowed[n][k]);
  std::size_t widened_total = 0;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (base.node_map[n] == SubsetMap::kNoNode || !ref_tree.node(n).is_action())
      continue;
    bool node_widened = false;
    for (std::size_t k = 0; k < ref_tree.node(n).actions.size(); ++k)
      if (base.edge_map[n][k] == -1 && clamped.allowed[n][k])
        node_widened = true;
    if (node_widened)
      ++widened_total;
  }
  CHECK(widened_total == eligible_count);
  validate_mask(ref_tree, clamped);  // the clamped mask still passes validation

  // Root-with-no-suppressed-edge throws: reference-as-coarse makes every edge
  // map to identity, so the root carries no -1 and the root-first precondition
  // is unmeetable.
  CHECK(throws_invalid([&] { make_sparse_multilayer_mask(ref_tree, ref_tree, 1); }));
  return true;
}

// S3: structural edge-wise subset inclusion against the full cap-0 reference
// menu (the review F2 structural half), minimal-menu legality at opened
// reference-only nodes, and an observed (not guaranteed) reachable-count
// comparison to the D202 root mask. The sparse B=2 mask is NOT nested in the
// D202 root-only mask (it widens a deeper node D202 leaves coarse), so the
// guaranteed nesting is edge-wise against the full reference menu, not a
// closure comparison against the D202 mask.
bool test_s3_sparse_cascade_bound() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask sparse = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const MixedMenuMask d202_root = make_root_only_widening_mask(ref_tree, coarse_tree);

  // The all-true reference mask is always legal: its closure is the whole
  // reference tree and every action node keeps its full menu.
  MixedMenuMask full_reference;
  full_reference.allowed.assign(ref_tree.size(), {});
  for (std::size_t n = 0; n < ref_tree.size(); ++n)
    if (ref_tree.node(n).is_action())
      full_reference.allowed[n].assign(ref_tree.node(n).actions.size(), true);
  validate_mask(ref_tree, full_reference);

  // Structural inclusion (always true): every sparse-allowed edge is a real
  // reference menu edge, i.e. the sparse menu is an edge-wise subset of cap-0.
  CHECK(sparse.allowed.size() == full_reference.allowed.size());
  for (std::size_t n = 0; n < sparse.allowed.size(); ++n)
    for (std::size_t k = 0; k < sparse.allowed[n].size(); ++k)
      if (sparse.allowed[n][k])
        CHECK(full_reference.allowed[n][k]);

  // Sparse closure for the minimal-menu legality check.
  std::vector<bool> sparse_reachable(ref_tree.size(), false);
  sparse_reachable[ref_tree.root_index()] = true;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!sparse_reachable[n] || !ref_tree.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref_tree.node(n).children.size(); ++a)
      if (sparse.allowed[n][a])
        sparse_reachable[ref_tree.node(n).children[a]] = true;
  }

  // Every opened reference-only node keeps the sparse minimal menu: passive
  // edges all retained, at most one aggressive edge.
  const SubsetMap base = map_bounded_subset(ref_tree, coarse_tree);
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!ref_tree.node(n).is_action() || base.node_map[n] != SubsetMap::kNoNode ||
        !sparse_reachable[n])
      continue;
    const TreeNode& node = ref_tree.node(n);
    std::size_t aggressive_allowed = 0;
    for (std::size_t k = 0; k < node.actions.size(); ++k) {
      const bool aggressive = node.actions[k].type == bs::poker::ActionType::Bet ||
                              node.actions[k].type == bs::poker::ActionType::Raise;
      if (sparse.allowed[n][k]) {
        if (aggressive)
          ++aggressive_allowed;
      } else {
        CHECK(aggressive);  // a passive edge is never suppressed
      }
    }
    CHECK(aggressive_allowed <= 1);
  }

  // Envelope bound: the sparse closure never exceeds the full reference tree.
  const std::size_t sparse_count = reachable_action_nodes(ref_tree, sparse);
  const std::size_t reference_action_count =
      std::count_if(sparse.allowed.begin(), sparse.allowed.end(),
                    [](const std::vector<bool>& row) { return !row.empty(); });
  CHECK(sparse_count <= reference_action_count);

  // Observed comparison against D202 (no guaranteed ordering: sparse adds a
  // deeper widened node but uses minimal menus in opened subtrees).
  const std::size_t d202_count = reachable_action_nodes(ref_tree, d202_root);
  std::printf("S3 observed reachable action nodes: sparse=%zu d202_root=%zu\n", sparse_count,
              d202_count);
  return true;
}

// S4: re-expanded policy legality with exact zero bits (M3 conditions).
bool test_s4_policy_legality() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (map.node_map[n] == SubsetMap::kNoNode) {
      CHECK(solved.river.avg_sigma[n].empty());
      continue;
    }
    if (!ref_tree.node(n).is_action())
      continue;
    const std::size_t width = ref_tree.node(n).actions.size();
    const auto& row = solved.river.avg_sigma[n];
    CHECK(row.size() == width * kC);
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < width; ++a) {
        const double pr = row[i * width + a];
        CHECK(std::isfinite(pr) && pr >= 0.0);
        if (map.edge_map[n][a] == -1)
          CHECK(pr == 0.0);
        else
          sum += pr;
      }
      CHECK(std::fabs(sum - 1.0) <= kDistTol);
    }
  }
  return true;
}

// S5: full-menu BR preservation — a suppressed edge still carries measurable
// option value; the reference BR keeps every deviation action.
bool test_s5_full_br_preservation() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref_tree, ref_tree,
                                                        map, CapExtensionCompletion::Uniform);
  bool positive_option = false;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s])
      for (std::uint32_t i = 0; i < kC; ++i) {
        const double menu_gap = r.delta[i] - r.bounded_gap[i];
        CHECK(menu_gap >= -kChipTol);
        if (menu_gap > kChipTol)
          positive_option = true;
      }
  CHECK(positive_option);
  return true;
}

// S6: bitwise forward-reach conservation and root full reach.
bool test_s6_reach_conservation() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  const std::size_t root = ref_tree.root_index();
  for (std::size_t seat = 0; seat < 2; ++seat) {
    for (double x : solved.forward_reach[seat][root])
      CHECK(x == 1.0);
    validate_supplied_reach(ref_tree, ref_tree, map, solved.river, seat,
                            solved.forward_reach[seat]);
  }
  return true;
}

// S7: empirical menu-error isolation on the fixed fixture — B>=2 pure menu term
// below B=1 against the SAME cap-0 reference; bounded NC reported independently.
bool test_s7_error_isolation() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);

  const MixedMenuMask m1 = make_sparse_multilayer_mask(ref_tree, coarse_tree, 1);
  const SubsetMap map1 = mask_to_subset_map(ref_tree, m1);
  const MixedMenuSolveResult s1 = solve_mixed_menu(f.def, f.reference, m1, kIters, 1);
  const double menu_b1 = pure_menu_uniform(f, ref_tree, s1, map1);

  const MixedMenuMask m2 = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const SubsetMap map2 = mask_to_subset_map(ref_tree, m2);
  const MixedMenuSolveResult s2 = solve_mixed_menu(f.def, f.reference, m2, kIters, 1);
  const double menu_b2 = pure_menu_uniform(f, ref_tree, s2, map2);

  // Both bounded NC values are independently finite (no cross-game ordering).
  CHECK(std::isfinite(s1.river.nash_conv_bb) && std::isfinite(s2.river.nash_conv_bb));
  std::printf("S7 observed pure menu term (BB): B1=%.8f B2=%.8f | bounded NC B1=%.8f B2=%.8f\n",
              menu_b1, menu_b2, s1.river.nash_conv_bb, s2.river.nash_conv_bb);
  if (!(menu_b2 < menu_b1 - kDistTol))
    std::printf("S7 diagnostic: empirical menu decrease refuted on 8-chip; FAILURE DISCIPLINE\n");
  CHECK(menu_b2 < menu_b1 - kDistTol);

  // Extension-free zero-gap pin still holds on the B=2 map.
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, s2.river, ref_tree, ref_tree, map2,
                                                        CapExtensionCompletion::Uniform);
  std::size_t pinned = 0;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s]) {
      if (!subtree_is_extension_free(ref_tree, map2, r.reference_node, s))
        continue;
      ++pinned;
      for (std::uint32_t i = 0; i < kC; ++i)
        CHECK(std::fabs(r.delta[i] - r.bounded_gap[i]) <= kChipTol);
    }
  CHECK(pinned > 0);
  return true;
}

// S8: determinism across repeated sparse solves.
bool test_s8_determinism() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const MixedMenuSolveResult a = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  const MixedMenuSolveResult b = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  for (std::size_t n = 0; n < ref_tree.size(); ++n)
    for (std::size_t k = 0; k < a.river.avg_sigma[n].size(); ++k)
      CHECK(a.river.avg_sigma[n][k] == b.river.avg_sigma[n][k]);
  CHECK(a.river.nash_conv_bb == b.river.nash_conv_bb);
  return true;
}

// S9: nodewise storage bound — compact slots never exceed the reference total.
bool test_s9_storage_bound() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_sparse_multilayer_mask(ref_tree, coarse_tree, 2);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  CHECK(solved.compact_slot_total > 0);
  CHECK(solved.compact_slot_total <= solved.reference_edge_total);
  return true;
}

}  // namespace

int main() {
  if (!test_s1_red_existence() || !test_s2_selection_and_preconditions() ||
      !test_s3_sparse_cascade_bound() || !test_s4_policy_legality() ||
      !test_s5_full_br_preservation() || !test_s6_reach_conservation() ||
      !test_s7_error_isolation() || !test_s8_determinism() || !test_s9_storage_bound())
    return 1;
  std::printf("rfc0047_sparse_multilayer: all checks passed\n");
  return 0;
}
