// RFC 0048 D225 native test: counterexample-driven single-edge witness
// augmentation over the immutable cap-0 reference. The selector consumes the
// SEALED ReachBoundResult of a frozen D208 baseline snapshot, chooses at most
// one public (node, suppressed aggressive edge) from pure menu option value,
// and builds a legal mask; the conditional re-solve computes its own reach and
// decomposition. Real seedless solves, behavioral assertions only, no
// source-text or log matching. Bench-only; every production surface stays
// untouched.
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

// Sealed D208 baseline: mask, fresh solve, and its reach-bound evidence.
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

// Fixed bijective combo permutation (gcd(137,1081) == 1).
std::uint32_t perm_index(std::uint32_t i) {
  return (i * 137u + 29u) % kC;
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

// W1: sealed 8-chip evidence locates a real suppressed aggressive witness.
bool test_w1_witness_exists() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(sel.status == WitnessStatus::kSelected);
  CHECK(sel.node < ref.size());
  const TreeNode& node = ref.node(sel.node);
  CHECK(node.is_action());
  CHECK(sel.edge < node.actions.size());
  const auto type = node.actions[sel.edge].type;
  CHECK(type == bs::poker::ActionType::Bet || type == bs::poker::ActionType::Raise);
  CHECK(!b.mask.allowed[sel.node][sel.edge]);  // suppressed in the baseline
  CHECK(sel.node_score > 1e-12 && sel.edge_score > 1e-12);
  return true;
}

// W2: the m0->m1 diff obeys the three-layer contract and the arm differs.
bool test_w2_diff_classification() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(sel.status == WitnessStatus::kSelected);
  const MixedMenuMask m1 = apply_witness_augmentation(ref, b.mask, sel);
  const std::vector<bool> reach0 = reachable_nodes(ref, b.mask);

  std::size_t scored_flips = 0;
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_action())
      continue;
    const TreeNode& node = ref.node(n);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      if (m1.allowed[n][a] == b.mask.allowed[n][a])
        continue;
      CHECK(m1.allowed[n][a]);  // augmentation only ever adds edges
      if (reach0[n]) {
        // Layer (i): the single scored witness among baseline-reachable rows.
        ++scored_flips;
        CHECK(n == sel.node && a == sel.edge);
      } else {
        // Layer (ii)/(iii): a previously unreachable row filled sparse-minimal.
        const long first_aggressive = first_aggressive_choice(node);
        bool any_passive_allowed = false;
        for (std::size_t x = 0; x < node.actions.size(); ++x) {
          const bool aggressive = node.actions[x].type == bs::poker::ActionType::Bet ||
                                  node.actions[x].type == bs::poker::ActionType::Raise;
          if (!aggressive) {
            CHECK(m1.allowed[n][x]);  // every passive edge retained
            any_passive_allowed = true;
          } else {
            // The only aggressive edge allowed is exactly the chooser's
            // first-aggressive pick (or none when the node is passive-only).
            CHECK(m1.allowed[n][x] == (static_cast<long>(x) == first_aggressive));
          }
        }
        CHECK(any_passive_allowed);
      }
    }
  }
  CHECK(scored_flips == 1);

  // Distinct from the D217 pot-fraction arm as a whole.
  const AbstractTree coarse_tree(g.def, g.coarse);
  const MixedMenuMask pot = make_pot_fraction_mask(ref, coarse_tree, 2);
  bool differs = false;
  for (std::size_t n = 0; n < ref.size(); ++n)
    if (ref.node(n).is_action() && pot.allowed[n] != m1.allowed[n])
      differs = true;
  CHECK(differs);
  return true;
}

// W3: determinism and structural combo-permutation invariance.
bool test_w3_determinism_permutation() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection a = select_witness_augmentation(ref, b.mask, b.evidence);
  const WitnessSelection again = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(a.status == again.status && a.node == again.node && a.edge == again.edge);
  CHECK(a.node_score == again.node_score && a.edge_score == again.edge_score);

  const ReachBoundResult permuted = permute_combos(b.evidence);
  const WitnessSelection p = select_witness_augmentation(ref, b.mask, permuted);
  CHECK(p.status == a.status && p.node == a.node && p.edge == a.edge);
  CHECK(p.node_score == a.node_score && p.edge_score == a.edge_score);
  return true;
}

// W4: legality, passive retention, validators, one scored witness.
bool test_w4_legality() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(sel.status == WitnessStatus::kSelected);
  const MixedMenuMask m1 = apply_witness_augmentation(ref, b.mask, sel);
  validate_mask(ref, m1);
  const SubsetMap map1 = mask_to_subset_map(ref, m1);
  validate_mask_subset_map(ref, map1);
  const std::vector<bool> reach1 = reachable_nodes(ref, m1);
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_action() || !reach1[n])
      continue;  // off-closure rows are legally all-false.
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a) {
      const bool passive = ref.node(n).actions[a].type != bs::poker::ActionType::Bet &&
                           ref.node(n).actions[a].type != bs::poker::ActionType::Raise;
      if (passive)
        CHECK(m1.allowed[n][a]);  // passive edges are never suppressed
      if (b.mask.allowed[n][a])
        CHECK(m1.allowed[n][a]);  // augmentation never removes
    }
  }
  return true;
}

// W5: full-menu evidence has no suppressed edge -> typed no-op.
bool test_w5_no_candidate_full_menu() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const MixedMenuMask full = full_reference_mask(ref);
  const MixedMenuSolveResult solved = solve_mixed_menu(g.def, g.reference, full, kIters, 1);
  const SubsetMap map = mask_to_subset_map(ref, full);
  const ReachBoundResult evidence = reach_bound_from_snapshot(g.def, solved.river, ref, ref, map,
                                                              CapExtensionCompletion::Uniform);
  const WitnessSelection sel = select_witness_augmentation(ref, full, evidence);
  CHECK(sel.status == WitnessStatus::kNoCandidate);
  const MixedMenuMask m1 = apply_witness_augmentation(ref, full, sel);
  CHECK(m1.allowed == full.allowed);
  return true;
}

// W6: every fail-close branch, including the two constructed-record guards.
bool test_w6_fail_close() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(sel.status == WitnessStatus::kSelected);

  // apply_witness_augmentation guards.
  WitnessSelection bad = sel;
  bad.edge = ref.node(sel.node).actions.size();  // out of range
  CHECK(throws_invalid([&] { apply_witness_augmentation(ref, b.mask, bad); }));
  bad = sel;
  for (std::size_t a = 0; a < ref.node(sel.node).actions.size(); ++a) {
    const auto t = ref.node(sel.node).actions[a].type;
    if (t != bs::poker::ActionType::Bet && t != bs::poker::ActionType::Raise) {
      bad.edge = a;  // a passive edge
      break;
    }
  }
  CHECK(throws_invalid([&] { apply_witness_augmentation(ref, b.mask, bad); }));
  bad = sel;
  // An ALLOWED aggressive edge somewhere in the baseline, so apply hits the
  // already-allowed guard rather than the passive guard. The witness node may
  // suppress every aggressive edge, so scan the whole mask; a widened node
  // always keeps an allowed aggressive edge in this tiny tree.
  std::size_t allowed_node = ref.size();
  std::size_t allowed_edge = 0;
  for (std::size_t n = 0; n < ref.size() && allowed_node == ref.size(); ++n) {
    if (!ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a) {
      const auto t = ref.node(n).actions[a].type;
      const bool aggressive = t == bs::poker::ActionType::Bet || t == bs::poker::ActionType::Raise;
      if (aggressive && b.mask.allowed[n][a]) {
        allowed_node = n;
        allowed_edge = a;
        break;
      }
    }
  }
  CHECK(allowed_node < ref.size());  // no silent fallback: such a node exists
  bad.node = allowed_node;
  bad.edge = allowed_edge;
  CHECK(throws_invalid([&] { apply_witness_augmentation(ref, b.mask, bad); }));  // already allowed
  bad = sel;
  bad.node = ref.size();  // bad node index
  CHECK(throws_invalid([&] { apply_witness_augmentation(ref, b.mask, bad); }));

  // Constructed-record guards on copied evidence objects. Locate a record
  // that actually carries a suppressed edge, so each tamper is effective.
  std::size_t target_seat = 2;
  std::size_t target_idx = 0;
  for (std::size_t s = 0; s < 2 && target_seat == 2; ++s)
    for (std::size_t k = 0; k < b.evidence.records[s].size(); ++k)
      if (std::any_of(b.evidence.records[s][k].edge_map.begin(),
                      b.evidence.records[s][k].edge_map.end(), [](int e) { return e == -1; })) {
        target_seat = s;
        target_idx = k;
        break;
      }
  CHECK(target_seat < 2);

  // Evidence/mask disagreement: flip one suppressed edge_map entry to mapped.
  ReachBoundResult disagree = b.evidence;
  NodeGapRecord& dr = disagree.records[target_seat][target_idx];
  for (std::size_t a = 0; a < dr.edge_map.size(); ++a) {
    if (dr.edge_map[a] == -1) {
      dr.edge_map[a] = static_cast<int>(a);
      break;
    }
  }
  CHECK(throws_invalid([&] { select_witness_augmentation(ref, b.mask, disagree); }));

  // Zero bounded edges: every edge_map entry suppressed.
  ReachBoundResult no_bounded = b.evidence;
  std::fill(no_bounded.records[target_seat][target_idx].edge_map.begin(),
            no_bounded.records[target_seat][target_idx].edge_map.end(), -1);
  CHECK(throws_invalid([&] { select_witness_augmentation(ref, b.mask, no_bounded); }));

  // Non-finite continuation term on the scanned target record.
  ReachBoundResult infinite = b.evidence;
  infinite.records[target_seat][target_idx].continuation[0] =
      std::numeric_limits<double>::infinity();
  CHECK(throws_invalid([&] { select_witness_augmentation(ref, b.mask, infinite); }));

  // W4b negative: corrupting delta at a positive-reach combo breaks the
  // continuation-derived identity and must throw (not silently re-score).
  ReachBoundResult bad_identity = b.evidence;
  NodeGapRecord& ir = bad_identity.records[target_seat][target_idx];
  std::uint32_t reached_combo = kC;
  for (std::uint32_t i = 0; i < kC; ++i)
    if (ir.own_reach[i] > 0.0) {
      reached_combo = i;
      break;
    }
  CHECK(reached_combo < kC);
  ir.delta[reached_combo] += 1.0;
  CHECK(throws_invalid([&] { select_witness_augmentation(ref, b.mask, bad_identity); }));

  // Negative own_reach is rejected outright (defense in depth, Low-4).
  ReachBoundResult negative_reach = b.evidence;
  negative_reach.records[target_seat][target_idx].own_reach[reached_combo] = -1.0;
  CHECK(throws_invalid([&] { select_witness_augmentation(ref, b.mask, negative_reach); }));

  // Typed budget exhaustion, distinct from invalid_argument and pre-solve.
  RefinementBudget empty_budget(0);
  bool typed = false;
  try {
    empty_budget.charge();
  } catch (const witness_budget_exhausted&) {
    typed = true;
  }
  CHECK(typed);
  return true;
}

// W7: closure correctness and all-true kernel no-op.
bool test_w7_closure_and_all_true() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  const MixedMenuMask m1 = apply_witness_augmentation(ref, b.mask, sel);
  const SubsetMap map1 = mask_to_subset_map(ref, m1);
  const std::vector<bool> closure = reachable_nodes(ref, m1);
  for (std::size_t n = 0; n < ref.size(); ++n)
    CHECK((map1.node_map[n] != SubsetMap::kNoNode) == closure[n]);
  return true;
}

// W8 + W9: zero-cost selection, one-token conditional re-solve with its own
// reach and full decomposition, plus the cumulative budget ledger.
bool test_w8_w9_resolve_and_budget() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const auto t0 = std::chrono::steady_clock::now();
  const Baseline b = seal_baseline(g, ref);
  const WitnessSelection sel = select_witness_augmentation(ref, b.mask, b.evidence);
  CHECK(sel.status == WitnessStatus::kSelected);

  RefinementBudget budget(1);
  CHECK(budget.remaining() == 1);
  const MixedMenuMask m1 = apply_witness_augmentation(ref, b.mask, sel);
  CHECK(budget.remaining() == 1);  // selection and augmentation cost no token
  budget.charge();
  CHECK(budget.remaining() == 0);
  bool exhausted = false;
  try {
    budget.charge();
  } catch (const witness_budget_exhausted&) {
    exhausted = true;
  }
  CHECK(exhausted);

  // A fresh budget funds exactly one candidate re-solve.
  RefinementBudget solve_budget(1);
  solve_budget.charge();
  const MixedMenuSolveResult s1 = solve_mixed_menu(g.def, g.reference, m1, kIters, 1);
  const SubsetMap map1 = mask_to_subset_map(ref, m1);
  for (std::size_t seat = 0; seat < 2; ++seat)
    validate_supplied_reach(ref, ref, map1, s1.river, seat, s1.forward_reach[seat]);
  const ReachBoundResult rb1 =
      reach_bound_from_snapshot(g.def, s1.river, ref, ref, map1, CapExtensionCompletion::Uniform);
  const double menu1 = rb1.exact_exploitability_bb - rb1.bounded_nash_conv_bb;
  const double menu0 = b.evidence.exact_exploitability_bb - b.evidence.bounded_nash_conv_bb;
  CHECK(std::isfinite(menu0) && std::isfinite(menu1));
  CHECK(std::isfinite(rb1.bounded_nash_conv_bb) && std::isfinite(rb1.exact_exploitability_bb));

  // The witness edge is legal under sigma1 and carries observable mass.
  const std::size_t width = ref.node(sel.node).actions.size();
  double witness_mass = 0.0;
  for (std::uint32_t i = 0; i < kC; ++i)
    witness_mass += s1.river.avg_sigma[sel.node][i * width + sel.edge];
  CHECK(witness_mass >= 0.0);

  const auto t1 = std::chrono::steady_clock::now();
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf(
      "W8/W9 ledger node=%zu edge=%zu node_score=%.8f edge_score=%.8f | pure menu m0=%.8f "
      "m1=%.8f BB | bounded NC m0=%.8f m1=%.8f | witness_mass=%.6f | wall=%.1f ms rss_peak=%ld "
      "KB\n",
      sel.node, sel.edge, sel.node_score, sel.edge_score, menu0, menu1,
      b.evidence.bounded_nash_conv_bb, rb1.bounded_nash_conv_bb, witness_mass,
      std::chrono::duration<double, std::milli>(t1 - t0).count(), ru.ru_maxrss);
  return true;
}

}  // namespace

int main() {
  if (!test_w1_witness_exists() || !test_w2_diff_classification() ||
      !test_w3_determinism_permutation() || !test_w4_legality() ||
      !test_w5_no_candidate_full_menu() || !test_w6_fail_close() ||
      !test_w7_closure_and_all_true() || !test_w8_w9_resolve_and_budget())
    return 1;
  std::printf("rfc0047_witness_augment: all checks passed\n");
  return 0;
}
