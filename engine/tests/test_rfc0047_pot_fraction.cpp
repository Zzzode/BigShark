// RFC 0048 D217 native test: pot-fraction menu selection within the frozen
// cap-0 reference action domain. Single-factor arm versus D208 (identical W;
// only the restricted-node aggressive chooser changes). Reuses the D202/D208
// solver, evaluators, and validators unchanged; real seedless solves,
// behavioral assertions only, no source-text or log matching.
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

// P1: the pot-fraction selector must, on at least one real reference-only
// node, pick a legal aggressive action different from the D208 first-aggressive
// edge (8-chip, then the 20-chip T9-style fallback; same fixed fraction).
bool has_distinct_pot_choice(std::uint64_t stack_behind) {
  const Fixture g = make_fixture(stack_behind);
  const AbstractTree ref(g.def, g.reference);
  const AbstractTree coarse(g.def, g.coarse);
  const MixedMenuMask pot = make_pot_fraction_mask(ref, coarse, 2);
  const MixedMenuMask first = make_sparse_multilayer_mask(ref, coarse, 2);
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a)
      if (pot.allowed[n][a] != first.allowed[n][a])
        return true;
  }
  return false;
}

bool test_p1_distinct_choice() {
  bool distinct = has_distinct_pot_choice(8);
  if (!distinct) {
    std::printf("P1 diagnostic: no distinct pot choice on 8-chip; retrying 20-chip\n");
    distinct = has_distinct_pot_choice(20);
  }
  CHECK(distinct);
  // Name the concrete split point: first node where the two choosers keep a
  // different aggressive edge, with the surviving aggressive targets.
  const std::uint64_t stack_behind = has_distinct_pot_choice(8) ? 8 : 20;
  const Fixture g = make_fixture(stack_behind);
  const AbstractTree ref(g.def, g.reference);
  const AbstractTree coarse(g.def, g.coarse);
  const MixedMenuMask pot = make_pot_fraction_mask(ref, coarse, 2);
  const MixedMenuMask first = make_sparse_multilayer_mask(ref, coarse, 2);
  const std::vector<PublicState> pub = replay_public_states(ref);
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_action() || pot.allowed[n] == first.allowed[n])
      continue;
    const std::size_t actor = ref.node(n).actor;
    const std::uint64_t high = std::max(pub[n].street_committed[0], pub[n].street_committed[1]);
    const std::uint64_t due = high - pub[n].street_committed[actor];
    const std::uint64_t ideal = due == 0 ? pub[n].pot : pub[n].pot + 2 * due;
    std::printf(
        "P1 first mask split at node %zu (%llu-chip): pot=%llu due=%llu ideal_additional=%llu", n,
        static_cast<unsigned long long>(stack_behind), static_cast<unsigned long long>(pub[n].pot),
        static_cast<unsigned long long>(due), static_cast<unsigned long long>(ideal));
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a) {
      const auto& act = ref.node(n).actions[a];
      if (act.type != bs::poker::ActionType::Bet && act.type != bs::poker::ActionType::Raise)
        continue;
      std::printf(
          " [type=%s target=%llu additional=%llu first=%d pot=%d]",
          act.type == bs::poker::ActionType::Bet ? "bet" : "raise",
          static_cast<unsigned long long>(act.target_total),
          static_cast<unsigned long long>(act.target_total - pub[n].street_committed[actor]),
          first.allowed[n][a] ? 1 : 0, pot.allowed[n][a] ? 1 : 0);
    }
    std::printf("\n");
    break;
  }
  return true;
}

// P2: in-domain legality plus the review-F2 reconstruction cross-checks.
bool test_p2_legality_and_reconstruction() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);

  // Shared-builder preconditions are inherited: non-positive budget and a
  // root with no suppressed edge (reference-as-coarse) both fail closed.
  CHECK(throws_invalid([&] { make_pot_fraction_mask(ref, coarse, 0); }));
  CHECK(throws_invalid([&] { make_pot_fraction_mask(ref, ref, 1); }));

  const MixedMenuMask mask = make_pot_fraction_mask(ref, coarse, 2);
  const SubsetMap map = mask_to_subset_map(ref, mask);
  // The coarse subset distinguishes a restricted reference-only node
  // (base kNoNode) from a coarse-mapped node that may legally keep several
  // coarse aggressive edges; the at-most-one-aggressive rule binds only the
  // former, matching the D208 S3 gate.
  const SubsetMap base = map_bounded_subset(ref, coarse);
  validate_mask(ref, mask);
  validate_mask_subset_map(ref, map);

  // Review F2: consume the ACTUAL replay outputs and cross-check the
  // reconstructed due classification against every node's real menu, with
  // strict XOR (Check and Call are mutually exclusive at source). This is the
  // gate that catches a mover-credit bug in the replay.
  const std::vector<PublicState> states = replay_public_states(ref);
  CHECK(states.size() == ref.size());
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (map.node_map[n] == SubsetMap::kNoNode || !ref.node(n).is_action())
      continue;
    const TreeNode& node = ref.node(n);
    const std::size_t width = node.actions.size();
    const auto& row = mask.allowed[n];
    CHECK(row.size() == width);

    bool has_check = false, has_bet = false, has_fold = false, has_call = false, has_raise = false;
    for (const auto& a : node.actions) {
      has_check |= a.type == bs::poker::ActionType::Check;
      has_bet |= a.type == bs::poker::ActionType::Bet;
      has_fold |= a.type == bs::poker::ActionType::Fold;
      has_call |= a.type == bs::poker::ActionType::Call;
      has_raise |= a.type == bs::poker::ActionType::Raise;
    }
    const bool zero_due_menu = has_check || has_bet;
    const bool positive_due_menu = has_fold || has_call || has_raise;
    // Strict structural XOR: a node is exactly one of a betting or a calling
    // situation.
    CHECK(zero_due_menu != positive_due_menu);

    // Reconstruct due from the replay's street commitments at THIS node
    // (the node's own actor is the decision maker here).
    const std::size_t actor = node.actor;
    const std::uint64_t high =
        std::max(states[n].street_committed[0], states[n].street_committed[1]);
    const std::uint64_t due = high - states[n].street_committed[actor];
    // Reliable cross-checks against the materialized menu. A positive due is
    // always a calling situation (Fold/Call present, no Check). A zero-due
    // node is never a calling situation (no Fold/Call); it may either offer a
    // Bet alongside Check or check through, so the iff is asserted only in the
    // positive direction, with the structural XOR above separating the classes.
    if (due > 0) {
      CHECK(positive_due_menu);
      CHECK(has_fold && has_call && !has_check);
    } else {
      CHECK(zero_due_menu);
      CHECK(!has_fold && !has_call);
    }
    // Every selected aggressive edge's type matches the reconstructed due.
    for (std::size_t a = 0; a < width; ++a) {
      if (!mask.allowed[n][a])
        continue;
      const auto t = node.actions[a].type;
      if (t == bs::poker::ActionType::Bet)
        CHECK(due == 0);
      if (t == bs::poker::ActionType::Raise)
        CHECK(due > 0);
    }

    std::size_t aggressive_allowed = 0;
    for (std::size_t a = 0; a < width; ++a) {
      const bool aggressive = node.actions[a].type == bs::poker::ActionType::Bet ||
                              node.actions[a].type == bs::poker::ActionType::Raise;
      if (mask.allowed[n][a]) {
        if (aggressive)
          ++aggressive_allowed;
      } else {
        CHECK(aggressive);  // passive edges are never suppressed
      }
    }
    // At most one aggressive edge survives at a restricted reference-only
    // node; coarse-mapped nodes keep whatever aggressive edges their coarse
    // subset allows.
    if (base.node_map[n] == SubsetMap::kNoNode)
      CHECK(aggressive_allowed <= 1);
  }

  // The replay invariant must also hold at every terminal node (the F1 OOB
  // site): live pot equals the root dead money plus the street commitments
  // with no kNoNode indexing.
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!ref.node(n).is_terminal())
      continue;
    CHECK(states[n].pot ==
          f.def.pot + states[n].street_committed[0] + states[n].street_committed[1]);
  }
  return true;
}

// P2b: the replay's street-start precondition fails loud on a valid mid-street
// root (one whose GameDef already carries street_committed), instead of
// silently seeding fresh zero commitments against a live wager.
bool test_p2b_mid_street_root_rejected() {
  bs::poker::GameDef def = river_root(8);
  // A live river wager: seat 0 bet 2 on top of the 2-chip blind, so the bet
  // chips enter both its gross contributions and the pot; seat 1 still faces
  // the wager with zero street commitment.
  def.contributions = {4, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 6;
  def.stacks = {6, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  def.street_committed = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.last_full_raise = 2;
  def.raises_this_street = 1;
  def.raise_rights = {true, true, false, false, false, false, false, false, false, false};
  const AbstractTree mid(def, ActionAbstraction::identity(0));
  bool threw = false;
  try {
    replay_public_states(mid);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// P3: determinism, public-only structure, and the F4 synthetic equidistance
// tie-break (lower target wins).
bool test_p3_determinism_and_tiebreak() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask a = make_pot_fraction_mask(ref, coarse, 2);
  const MixedMenuMask b = make_pot_fraction_mask(ref, coarse, 2);
  CHECK(a.allowed.size() == b.allowed.size());
  for (std::size_t n = 0; n < a.allowed.size(); ++n)
    for (std::size_t k = 0; k < a.allowed[n].size(); ++k)
      CHECK(a.allowed[n][k] == b.allowed[n][k]);

  // F4: exercise the PRODUCTION comparator directly with a synthetic node.
  // street_base 0 makes additional == target_total; ideal 8 is exactly
  // equidistant from raise targets 6 and 10 (distance 2 each), so the lower
  // target at the lower sorted index must win. A passive edge is ignored.
  bs::poker::Action passive{bs::poker::ActionType::Call, 0};
  bs::poker::Action low{bs::poker::ActionType::Raise, 6};
  bs::poker::Action high{bs::poker::ActionType::Raise, 10};
  TreeNode synth{};
  synth.actor = 0;
  synth.actions = {passive, low, high};
  const long chosen_equidistant = nearest_aggressive_choice(synth, 8, 0);
  CHECK(chosen_equidistant == 1);  // index of target 6, not index 2 (target 10)

  // A clearly nearer target wins without a tie.
  CHECK(nearest_aggressive_choice(synth, 7, 0) == 1);    // 6 is distance 1 vs 10 distance 3
  CHECK(nearest_aggressive_choice(synth, 100, 0) == 2);  // largest target nearest a huge ideal

  // No aggressive edge -> -1 (passive-only node fallback).
  TreeNode passive_only{};
  passive_only.actor = 0;
  passive_only.actions = {bs::poker::Action{bs::poker::ActionType::Fold, 0},
                          bs::poker::Action{bs::poker::ActionType::Call, 0}};
  CHECK(nearest_aggressive_choice(passive_only, 8, 0) == -1);
  return true;
}

// P4: re-expanded policy legality with exact zero bits.
bool test_p4_policy_legality() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask mask = make_pot_fraction_mask(ref, coarse, 2);
  const SubsetMap map = mask_to_subset_map(ref, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (map.node_map[n] == SubsetMap::kNoNode) {
      CHECK(solved.river.avg_sigma[n].empty());
      continue;
    }
    if (!ref.node(n).is_action())
      continue;
    const std::size_t width = ref.node(n).actions.size();
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

// P5: full cap-0 BR still sees every deviation action; positive option exists.
bool test_p5_full_br_preservation() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask mask = make_pot_fraction_mask(ref, coarse, 2);
  const SubsetMap map = mask_to_subset_map(ref, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref, ref, map,
                                                        CapExtensionCompletion::Uniform);
  bool positive = false;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s])
      for (std::uint32_t i = 0; i < kC; ++i) {
        const double gap = r.delta[i] - r.bounded_gap[i];
        CHECK(gap >= -kChipTol);
        if (gap > kChipTol)
          positive = true;
      }
  CHECK(positive);
  return true;
}

// P6: bitwise reach conservation and root full reach.
bool test_p6_reach_conservation() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask mask = make_pot_fraction_mask(ref, coarse, 2);
  const SubsetMap map = mask_to_subset_map(ref, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  for (std::size_t seat = 0; seat < 2; ++seat) {
    for (double x : solved.forward_reach[seat][ref.root_index()])
      CHECK(x == 1.0);
    validate_supplied_reach(ref, ref, map, solved.river, seat, solved.forward_reach[seat]);
  }
  return true;
}

double pure_menu_uniform(const Fixture& f, const AbstractTree& ref,
                         const MixedMenuSolveResult& solved, const SubsetMap& map) {
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref, ref, map,
                                                        CapExtensionCompletion::Uniform);
  return rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
}

// P7: same-input error ledger vs the D208 first-aggressive arm at identical W.
// Runs on a fixture where the two choosers actually produce distinct masks
// (8-chip first, 20-chip T9 fallback); on a no-difference fixture the ledger
// cannot attribute anything to the single factor. No cross-game NC ordering.
bool ledger_on_fixture(std::uint64_t stack_behind) {
  const Fixture f = make_fixture(stack_behind);
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);

  const MixedMenuMask m_first = make_sparse_multilayer_mask(ref, coarse, 2);
  const MixedMenuMask m_pot = make_pot_fraction_mask(ref, coarse, 2);
  const SubsetMap map_first = mask_to_subset_map(ref, m_first);
  const SubsetMap map_pot = mask_to_subset_map(ref, m_pot);
  const MixedMenuSolveResult s_first = solve_mixed_menu(f.def, f.reference, m_first, kIters, 1);
  const MixedMenuSolveResult s_pot = solve_mixed_menu(f.def, f.reference, m_pot, kIters, 1);

  const double menu_first = pure_menu_uniform(f, ref, s_first, map_first);
  const double menu_pot = pure_menu_uniform(f, ref, s_pot, map_pot);
  std::printf(
      "P7 ledger (%llu-chip) pure-menu first=%.8f pot=%.8f | bounded NC first=%.8f "
      "pot=%.8f\n",
      static_cast<unsigned long long>(stack_behind), menu_first, menu_pot,
      s_first.river.nash_conv_bb, s_pot.river.nash_conv_bb);
  CHECK(std::isfinite(s_first.river.nash_conv_bb) && std::isfinite(s_pot.river.nash_conv_bb));
  CHECK(menu_first > 0.0 && menu_pot > 0.0);
  return true;
}

bool test_p7_same_input_ledger() {
  if (has_distinct_pot_choice(8))
    return ledger_on_fixture(8);
  std::printf("P7: 8-chip masks identical; running ledger on 20-chip where choosers differ\n");
  CHECK(has_distinct_pot_choice(20));
  return ledger_on_fixture(20);
}

// P8: determinism across repeated pot-fraction solves.
bool test_p8_determinism() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask pmask = make_pot_fraction_mask(ref, coarse, 2);
  const MixedMenuSolveResult a = solve_mixed_menu(f.def, f.reference, pmask, kIters, 1);
  const MixedMenuSolveResult b = solve_mixed_menu(f.def, f.reference, pmask, kIters, 1);
  for (std::size_t n = 0; n < ref.size(); ++n)
    for (std::size_t k = 0; k < a.river.avg_sigma[n].size(); ++k)
      CHECK(a.river.avg_sigma[n][k] == b.river.avg_sigma[n][k]);
  CHECK(a.river.nash_conv_bb == b.river.nash_conv_bb);
  return true;
}

// P9: nodewise storage bound.
bool test_p9_storage_bound() {
  const Fixture f = make_fixture();
  const AbstractTree ref(f.def, f.reference);
  const AbstractTree coarse(f.def, f.coarse);
  const MixedMenuMask mask = make_pot_fraction_mask(ref, coarse, 2);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  CHECK(solved.compact_slot_total > 0);
  CHECK(solved.compact_slot_total <= solved.reference_edge_total);
  return true;
}

}  // namespace

int main() {
  if (!test_p1_distinct_choice() || !test_p2_legality_and_reconstruction() ||
      !test_p2b_mid_street_root_rejected() || !test_p3_determinism_and_tiebreak() ||
      !test_p4_policy_legality() || !test_p5_full_br_preservation() ||
      !test_p6_reach_conservation() || !test_p7_same_input_ledger() || !test_p8_determinism() ||
      !test_p9_storage_bound())
    return 1;
  std::printf("rfc0047_pot_fraction: all checks passed\n");
  return 0;
}
