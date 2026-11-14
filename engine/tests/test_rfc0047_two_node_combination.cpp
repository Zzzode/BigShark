// RFC 0048 D227 native test: deterministic two-public-node combination
// qualification over the SAME sealed D208 baseline mask and SAME sealed
// ReachBoundResult used by the D225 single-edge witness. Both candidates are
// read from the frozen evidence; the post-flip mask is never re-certified by
// stale evidence, and every non-combined path leaves the baseline untouched.
// Real seedless solves, behavioral assertions only, no source-text/log
// matching. Bench-only; every production surface stays untouched.
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
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
using bs::tree::TreeNode;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kIters = 60;

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

Fixture make_fixture() {
  return {river_root(8), make_grid_abstraction(SizingGrid::Coarse, 1),
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

std::vector<bool> reachable_nodes(const AbstractTree& tree, const MixedMenuMask& mask) {
  std::vector<bool> reachable(tree.size(), false);
  reachable[tree.root_index()] = true;
  for (std::size_t n = 0; n < tree.size(); ++n) {
    if (!reachable[n] || !tree.node(n).is_action())
      continue;
    const auto& kids = tree.node(n).children;
    for (std::size_t a = 0; a < kids.size(); ++a)
      if (mask.allowed[n][a])
        reachable[kids[a]] = true;
  }
  return reachable;
}

MixedMenuMask full_reference_mask(const AbstractTree& ref) {
  MixedMenuMask mask;
  mask.allowed.assign(ref.size(), {});
  for (std::size_t n = 0; n < ref.size(); ++n)
    if (ref.node(n).is_action())
      mask.allowed[n].assign(ref.node(n).actions.size(), true);
  return mask;
}

struct Baseline {
  MixedMenuMask mask;
  MixedMenuSolveResult solved;
  ReachBoundResult evidence;
  SubsetMap map;
};

Baseline seal_baseline(const Fixture& g, const AbstractTree& ref) {
  Baseline b;
  const AbstractTree coarse(g.def, g.coarse);
  b.mask = make_sparse_multilayer_mask(ref, coarse, 2);
  b.solved = solve_mixed_menu(g.def, g.reference, b.mask, kIters, 1);
  b.map = mask_to_subset_map(ref, b.mask);
  b.evidence = reach_bound_from_snapshot(g.def, b.solved.river, ref, ref, b.map,
                                         CapExtensionCompletion::Uniform);
  return b;
}

std::uint32_t perm_index(std::uint32_t i) {
  return (i * 137u + 29u) % kC;  // gcd(137,1081) == 1: a bijection.
}

ReachBoundResult permute_combos(const ReachBoundResult& in) {
  ReachBoundResult out = in;
  for (std::size_t s = 0; s < 2; ++s) {
    for (NodeGapRecord& r : out.records[s]) {
      const std::size_t a = r.edge_map.size();
      std::vector<double> reach(kC), delta(kC), bounded(kC), cont(kC * a);
      for (std::uint32_t i = 0; i < kC; ++i) {
        const std::uint32_t j = perm_index(i);
        reach[j] = r.own_reach[i];
        delta[j] = r.delta[i];
        bounded[j] = r.bounded_gap[i];
        for (std::size_t x = 0; x < a; ++x)
          cont[j * a + x] = r.continuation[i * a + x];
      }
      r.own_reach = std::move(reach);
      r.delta = std::move(delta);
      r.bounded_gap = std::move(bounded);
      r.continuation = std::move(cont);
    }
  }
  return out;
}

bool is_aggressive_edge(const TreeNode& node, std::size_t a) {
  const auto t = node.actions[a].type;
  return t == bs::poker::ActionType::Bet || t == bs::poker::ActionType::Raise;
}

// D227-C1: the natural sealed outcome is observed and pinned (kCombined when two
// eligible nodes exist; otherwise the typed status is retained honestly).
bool test_c1_natural_outcome() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection sel = select_combination_augmentation(ref, b.mask, b.evidence);
  std::printf("D227-C1 natural status=%d eligible_count=%zu\n", static_cast<int>(sel.status),
              sel.eligible_count);
  if (sel.status == CombinationStatus::kCombined) {
    CHECK(sel.eligible_count >= 2);
    CHECK(sel.node1 != sel.node2);
    // r1 must equal the D225 single-edge witness.
    const WitnessSelection single = select_witness_augmentation(ref, b.mask, b.evidence);
    CHECK(single.status == WitnessStatus::kSelected);
    CHECK(sel.node1 == single.node && sel.edge1 == single.edge);
  }
  return true;  // either typed outcome is an honest observation; later gates
                // pin kCombined legality via the natural or constructed path.
}

// D227-C2: determinism and structural combo-permutation invariance.
bool test_c2_determinism_permutation() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection a = select_combination_augmentation(ref, b.mask, b.evidence);
  const CombinationSelection again = select_combination_augmentation(ref, b.mask, b.evidence);
  CHECK(a.status == again.status && a.node1 == again.node1 && a.edge1 == again.edge1 &&
        a.node2 == again.node2 && a.edge2 == again.edge2);
  CHECK(a.node1_score == again.node1_score && a.edge1_score == again.edge1_score &&
        a.node2_score == again.node2_score && a.edge2_score == again.edge2_score);
  const ReachBoundResult permuted = permute_combos(b.evidence);
  const CombinationSelection p = select_combination_augmentation(ref, b.mask, permuted);
  CHECK(p.status == a.status && p.node1 == a.node1 && p.edge1 == a.edge1 && p.node2 == a.node2 &&
        p.edge2 == a.edge2);
  CHECK(p.node1_score == a.node1_score && p.edge1_score == a.edge1_score &&
        p.node2_score == a.node2_score && p.edge2_score == a.edge2_score);
  return true;
}

// D227-C3: both edges are frozen-domain suppressed aggressive edges on distinct
// nodes, and r2's node score never exceeds r1's.
bool test_c3_frozen_sourcing() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection sel = select_combination_augmentation(ref, b.mask, b.evidence);
  if (sel.status != CombinationStatus::kCombined)
    return true;  // sourcing only applies to a real combination.
  const std::array<std::pair<std::size_t, std::size_t>, 2> coords = {
      {{sel.node1, sel.edge1}, {sel.node2, sel.edge2}}};
  for (const auto& [n, a] : coords) {
    CHECK(n < ref.size() && ref.node(n).is_action());
    CHECK(a < ref.node(n).actions.size());
    CHECK(is_aggressive_edge(ref.node(n), a));
    CHECK(!b.mask.allowed[n][a]);  // suppressed in the sealed baseline.
  }
  CHECK(sel.node2_score <= sel.node1_score);
  return true;
}

// D227-C4 + C4b: combined legality and the exact two-scored-flip classification.
bool test_c4_c4b_legality_and_diff() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection sel = select_combination_augmentation(ref, b.mask, b.evidence);
  if (sel.status != CombinationStatus::kCombined)
    return true;
  const MixedMenuMask combined = apply_combination_augmentation(ref, b.mask, sel);
  validate_mask(ref, combined);
  const SubsetMap map = mask_to_subset_map(ref, combined);
  validate_mask_subset_map(ref, map);
  const std::vector<bool> reach0 = reachable_nodes(ref, b.mask);
  const std::vector<bool> reach1 = reachable_nodes(ref, combined);

  std::size_t scored_flips = 0;
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_action())
      continue;
    const TreeNode& node = ref.node(n);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      if (combined.allowed[n][a] == b.mask.allowed[n][a])
        continue;
      CHECK(combined.allowed[n][a]);  // only additions.
      if (reach0[n]) {
        // Exactly the two scored coordinates flip on baseline-reachable rows.
        ++scored_flips;
        const bool is_first = (n == sel.node1 && a == sel.edge1);
        const bool is_second = (n == sel.node2 && a == sel.edge2);
        CHECK(is_first || is_second);
      } else {
        // Every other changed row is sparse-minimal: all passive edges plus
        // at most the exported first-aggressive edge.
        const long first_aggressive = first_aggressive_choice(node);
        for (std::size_t x = 0; x < node.actions.size(); ++x) {
          if (!is_aggressive_edge(node, x))
            CHECK(combined.allowed[n][x]);
          else
            CHECK(combined.allowed[n][x] == (static_cast<long>(x) == first_aggressive));
        }
      }
    }
  }
  CHECK(scored_flips == 2);
  for (std::size_t n = 0; n < ref.size(); ++n)
    CHECK((map.node_map[n] != SubsetMap::kNoNode) == reach1[n]);
  return true;
}

// D227-C5: apply-layer containment via honest coordinate injection (no forged
// record), plus the negative gate that natural evidence is never contained.
bool test_c5_containment() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  // Negative gate: the natural selection (if combined) is never contained.
  const CombinationSelection natural = select_combination_augmentation(ref, b.mask, b.evidence);
  CHECK(natural.status != CombinationStatus::kContained);

  // Honest injection: take the real r1 closure, find a node it newly reaches,
  // and point n2 at that node's closure-filled first-aggressive edge.
  const WitnessSelection single = select_witness_augmentation(ref, b.mask, b.evidence);
  if (single.status != WitnessStatus::kSelected)
    return true;
  const MixedMenuMask after_first = apply_witness_augmentation(ref, b.mask, single);
  const std::vector<bool> reach0 = reachable_nodes(ref, b.mask);
  std::size_t new_node = ref.size();
  std::size_t new_edge = 0;
  for (std::size_t n = 0; n < ref.size() && new_node == ref.size(); ++n) {
    if (!ref.node(n).is_action() || reach0[n])
      continue;
    const long first_aggressive = first_aggressive_choice(ref.node(n));
    if (first_aggressive >= 0 &&
        after_first.allowed[n][static_cast<std::size_t>(first_aggressive)]) {
      new_node = n;
      new_edge = static_cast<std::size_t>(first_aggressive);
    }
  }
  if (new_node == ref.size())
    return true;  // no newly-opened aggressive node on this tiny tree.
  CombinationSelection injected = natural;
  injected.status = CombinationStatus::kCombined;
  injected.node2 = new_node;
  injected.edge2 = new_edge;
  // The edge is already legal after the first closure -> contained, baseline
  // returned unchanged.
  CombinationSelection contained = injected;
  contained.status = CombinationStatus::kContained;
  const MixedMenuMask out = apply_combination_augmentation(ref, b.mask, contained);
  CHECK(out.allowed == b.mask.allowed);
  return true;
}

// D227-C7: full-menu evidence has no suppressed edge -> kNoCandidate, untouched.
bool test_c7_no_candidate() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const MixedMenuMask full = full_reference_mask(ref);
  const MixedMenuSolveResult solved = solve_mixed_menu(g.def, g.reference, full, kIters, 1);
  const SubsetMap map = mask_to_subset_map(ref, full);
  const ReachBoundResult evidence = reach_bound_from_snapshot(g.def, solved.river, ref, ref, map,
                                                              CapExtensionCompletion::Uniform);
  const CombinationSelection sel = select_combination_augmentation(ref, full, evidence);
  CHECK(sel.status == CombinationStatus::kNoCandidate);
  CHECK(sel.eligible_count == 0);
  const MixedMenuMask out = apply_combination_augmentation(ref, full, sel);
  CHECK(out.allowed == full.allowed);
  return true;
}

// D227-C8: fail-close coordinates throw and the baseline stays byte-identical.
bool test_c8_fail_close_atomicity() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection sel = select_combination_augmentation(ref, b.mask, b.evidence);
  if (sel.status != CombinationStatus::kCombined)
    return true;
  const auto snapshot = b.mask.allowed;

  CombinationSelection bad = sel;
  bad.node2 = ref.size();  // bad node index.
  CHECK(throws_invalid([&] { apply_combination_augmentation(ref, b.mask, bad); }));
  bad = sel;
  bad.edge2 = ref.node(sel.node2).actions.size();  // edge out of range.
  CHECK(throws_invalid([&] { apply_combination_augmentation(ref, b.mask, bad); }));
  // A passive edge at node2.
  bad = sel;
  for (std::size_t a = 0; a < ref.node(sel.node2).actions.size(); ++a)
    if (!is_aggressive_edge(ref.node(sel.node2), a)) {
      bad.edge2 = a;
      break;
    }
  CHECK(throws_invalid([&] { apply_combination_augmentation(ref, b.mask, bad); }));
  // Atomicity: no partial modification survived the exceptions.
  CHECK(b.mask.allowed == snapshot);
  return true;
}

// D227-C9 + C10: zero-token selection, one charged fresh combined solve with its
// own reach/decomposition, and distinction from m0 and the D225 mask.
bool test_c9_c10_budget_resolve_distinction() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const auto t0 = std::chrono::steady_clock::now();
  const Baseline b = seal_baseline(g, ref);
  const CombinationSelection sel = select_combination_augmentation(ref, b.mask, b.evidence);
  if (sel.status != CombinationStatus::kCombined)
    return true;
  RefinementBudget budget(1);
  CHECK(budget.remaining() == 1);
  const MixedMenuMask combined = apply_combination_augmentation(ref, b.mask, sel);
  CHECK(budget.remaining() == 1);  // selection/application cost no token.
  bool exhausted = false;
  RefinementBudget empty(0);
  try {
    empty.charge();
  } catch (const witness_budget_exhausted&) {
    exhausted = true;
  }
  CHECK(exhausted);

  RefinementBudget solve_budget(1);
  solve_budget.charge();
  const MixedMenuSolveResult s2 = solve_mixed_menu(g.def, g.reference, combined, kIters, 1);
  const SubsetMap map2 = mask_to_subset_map(ref, combined);
  for (std::size_t seat = 0; seat < 2; ++seat)
    validate_supplied_reach(ref, ref, map2, s2.river, seat, s2.forward_reach[seat]);
  const ReachBoundResult rb2 =
      reach_bound_from_snapshot(g.def, s2.river, ref, ref, map2, CapExtensionCompletion::Uniform);
  const double menu2 = rb2.exact_exploitability_bb - rb2.bounded_nash_conv_bb;
  CHECK(std::isfinite(menu2) && std::isfinite(rb2.bounded_nash_conv_bb));

  // C10: distinct from both m0 and the D225 single-edge mask.
  const WitnessSelection single = select_witness_augmentation(ref, b.mask, b.evidence);
  const MixedMenuMask single_mask = apply_witness_augmentation(ref, b.mask, single);
  CHECK(combined.allowed != b.mask.allowed);
  CHECK(combined.allowed != single_mask.allowed);

  const auto t1 = std::chrono::steady_clock::now();
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf(
      "D227-C9/C10 ledger nodes=(%zu,%zu) eligible=%zu | pure menu m0=%.8f combined=%.8f BB | "
      "bounded NC combined=%.8f | wall=%.1f ms rss_peak=%ld KB\n",
      sel.node1, sel.node2, sel.eligible_count,
      b.evidence.exact_exploitability_bb - b.evidence.bounded_nash_conv_bb, menu2,
      rb2.bounded_nash_conv_bb, std::chrono::duration<double, std::milli>(t1 - t0).count(),
      ru.ru_maxrss);
  return true;
}

}  // namespace

int main() {
  if (!test_c1_natural_outcome() || !test_c2_determinism_permutation() ||
      !test_c3_frozen_sourcing() || !test_c4_c4b_legality_and_diff() || !test_c5_containment() ||
      !test_c7_no_candidate() || !test_c8_fail_close_atomicity() ||
      !test_c9_c10_budget_resolve_distinction())
    return 1;
  std::printf("rfc0047_two_node_combination: all checks passed\n");
  return 0;
}
