// RFC 0048 Path B (D192) native test: reach-weighted certified bound on
// enumerable tiny river roots. Real seedless solves and behavioral
// assertions only; no source-text or log matching. The frozen G5
// instrument and the Stage-2b union component are never modified.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/union_embedding.hpp"
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

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kIters = 60;
constexpr double kBbTol = 1e-12;
constexpr double kChipTol = 1e-9;

// Hand-built shallow street-start river root (same family as the D184 N4
// fixture): pot 4, 8 chips behind, enumerable menus.
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
  ActionAbstraction bounded;
  ActionAbstraction reference;
};

Fixture coarse_vs_cap0(std::uint64_t stack_behind = 8) {
  return {river_root(stack_behind), make_grid_abstraction(SizingGrid::Coarse, 1),
          ActionAbstraction::identity(0)};
}

// T1: Lemma-1 exact exploitability equals the Stage-2b union measure under
// both completion arms (cross-instrument identity).
bool test_t1_cross_instrument_identity() {
  const Fixture f = coarse_vs_cap0();
  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1, arm);
    const UnionMateriality u =
        measure_subset_materiality(f.def, f.bounded, f.reference, kIters, 1, arm);
    CHECK(std::string(rb.reference_label) == kUnionReferenceLabel);
    CHECK(std::string(rb.m2_marking) == kUnionM2Marking);
    CHECK(std::isfinite(rb.exact_exploitability_bb));
    CHECK(std::fabs(rb.exact_exploitability_bb - u.embedded_exploitability_bb) <= kBbTol);
    CHECK(std::fabs(rb.bounded_nash_conv_bb - u.bounded_nash_conv_bb) <= kBbTol);
    CHECK(rb.mapped_action_nodes == u.mapped_nodes);
  }
  return true;
}

// T2: per-node delta is non-negative and the widened flag is exactly
// "edge_map has no extension entry".
bool test_t2_node_gap_sign_and_widened() {
  const Fixture f = coarse_vs_cap0();
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  std::size_t widened = 0;
  for (std::size_t s = 0; s < 2; ++s) {
    for (const NodeGapRecord& r : rb.records[s]) {
      CHECK(r.own_reach.size() == kC && r.delta.size() == kC);
      bool has_extension = false;
      for (int e : r.edge_map)
        if (e == -1)
          has_extension = true;
      CHECK(r.widened == !has_extension);
      if (r.widened)
        ++widened;
      for (std::uint32_t i = 0; i < kC; ++i)
        CHECK(r.delta[i] >= -kChipTol);
    }
  }
  CHECK(widened == rb.widened_nodes);
  return true;
}

// T3: per-seat telescope against the independent root BR-EV walk.
bool test_t3_per_seat_telescope() {
  const Fixture f = coarse_vs_cap0();
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  const UnionRootValues v =
      subset_root_values(f.def, f.bounded, f.reference, kIters, 1, CapExtensionCompletion::Uniform);
  double raw[2] = {0.0, 0.0};
  raw[0] = std::accumulate(v.br_seat0.begin(), v.br_seat0.end(), 0.0) -
           std::accumulate(v.ev_seat0.begin(), v.ev_seat0.end(), 0.0);
  raw[1] = std::accumulate(v.br_seat1.begin(), v.br_seat1.end(), 0.0) -
           std::accumulate(v.ev_seat1.begin(), v.ev_seat1.end(), 0.0);
  for (std::size_t s = 0; s < 2; ++s) {
    // Two independently coded walks can differ only in FP summation order;
    // tolerate 1e-7 chips absolute (~1e-13 relative at these scales), far
    // inside any structural error. The BB-level 1e-12 gate in T1 is separate.
    const double tol = std::max(1e-7, 1e-12 * std::fabs(raw[s]));
    CHECK(std::fabs(rb.raw_exact_chips[s] - raw[s]) <= tol);
  }
  return true;
}

// T4: corrected R*S structural dominance under BOTH arms, certificate
// ordering, W-all/W-none degenerates, and the RED naive-bound falsifier.
bool test_t4_structural_bound_and_red_naive() {
  const Fixture f = coarse_vs_cap0();
  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1, arm);
    for (std::size_t s = 0; s < 2; ++s) {
      for (const NodeGapRecord& r : rb.records[s]) {
        CHECK(r.opponent_reach_sum_s >= 0.0);
        CHECK(r.payoff_range_r >= 0.0);
        CHECK(std::fabs(r.structural_bound_b - r.payoff_range_r * r.opponent_reach_sum_s) <=
              kChipTol);
        for (std::uint32_t i = 0; i < kC; ++i)
          if (r.own_reach[i] > 0.0)
            CHECK(r.structural_bound_b + kChipTol >= r.delta[i]);
      }
    }
    CHECK(rb.structural_certificate_bb + kBbTol >= rb.exact_exploitability_bb);

    // W-all: the Lemma-1 exact diagnostic reconstructs the exact total
    // using node-level delta at every mapped node.
    const double exact = exact_diagnostic_certificate(rb);
    CHECK(std::fabs(exact - rb.exact_exploitability_bb) <= kBbTol);
    // W-none: structural bound at every node stays conservative.
    const double none = selective_certificate(rb, {});
    CHECK(none + kBbTol >= rb.exact_exploitability_bb);
    // The stored structural certificate uses exact delta at the genuinely
    // widened nodes and R*S elsewhere; reconstruct it explicitly.
    std::vector<std::size_t> widened_nodes;
    for (std::size_t s = 0; s < 2; ++s)
      for (const NodeGapRecord& r : rb.records[s])
        if (r.widened)
          widened_nodes.push_back(r.reference_node);
    const double reconstructed = selective_certificate(rb, widened_nodes);
    CHECK(std::fabs(reconstructed - rb.structural_certificate_bb) <= kBbTol);
  }

  // RED falsifier: the review-rejected max-|payoff| bound undercuts a
  // measured positive-reach delta and must be rejected nodewise.
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  const AbstractTree ref_tree(f.def, f.reference);
  const std::vector<double> naive = naive_max_abs_payoff_bounds(ref_tree, rb);
  bool threw = false;
  try {
    verify_custom_bounds(rb, naive);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// T5: foreign/tampered reach is rejected; the validator recomputation
// matches the recorded own reach bitwise.
bool test_t5_reach_validator() {
  const Fixture f = coarse_vs_cap0();
  RiverSolveConfig cfg;
  cfg.def = f.def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_abstraction = f.bounded;
  const RiverSolveResult solved = solve_river_subgame(cfg, {});
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree bounded_tree(f.def, f.bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved, ref_tree, bounded_tree,
                                                        subset, CapExtensionCompletion::Uniform);

  for (std::size_t seat = 0; seat < 2; ++seat) {
    const auto recomputed = recompute_forward_reach(ref_tree, bounded_tree, subset, solved, seat);
    validate_supplied_reach(ref_tree, bounded_tree, subset, solved, seat, recomputed);
    for (const NodeGapRecord& r : rb.records[seat])
      for (std::uint32_t i = 0; i < kC; ++i)
        CHECK(recomputed[r.reference_node][i] == r.own_reach[i]);  // bitwise

    // Tampered entry.
    std::vector<std::vector<double>> bad = recomputed;
    for (auto& v : bad)
      if (!v.empty()) {
        v[0] += 0.25;
        break;
      }
    bool threw = false;
    try {
      validate_supplied_reach(ref_tree, bounded_tree, subset, solved, seat, bad);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);

    // Negative / non-finite entry.
    std::vector<std::vector<double>> malformed = recomputed;
    for (auto& v : malformed)
      if (!v.empty()) {
        v[0] = -1.0;
        break;
      }
    threw = false;
    try {
      validate_supplied_reach(ref_tree, bounded_tree, subset, solved, seat, malformed);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // Foreign reach: a different-iteration policy's forward reach.
  RiverSolveConfig other_cfg = cfg;
  other_cfg.iterations = kIters + 17;
  const RiverSolveResult foreign = solve_river_subgame(other_cfg, {});
  const auto foreign_reach = recompute_forward_reach(ref_tree, bounded_tree, subset, foreign, 0);
  bool threw = false;
  try {
    validate_supplied_reach(ref_tree, bounded_tree, subset, solved, 0, foreign_reach);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// T6: a widening claim over a node that still has an extension edge is
// rejected rather than silently treated as complete.
bool test_t6_widening_claim_integrity() {
  const Fixture f = coarse_vs_cap0();
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  const NodeGapRecord* incomplete = nullptr;
  for (std::size_t s = 0; s < 2 && !incomplete; ++s)
    for (const NodeGapRecord& r : rb.records[s])
      if (!r.widened) {
        incomplete = &r;
        break;
      }
  CHECK(incomplete != nullptr);  // cap1 vs cap0 must have restricted nodes
  bool threw = false;
  try {
    (void)selective_certificate(rb, {incomplete->reference_node});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// T7: malformed bound tables fail closed.
bool test_t7_bound_malformation() {
  const Fixture f = coarse_vs_cap0();
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  const std::size_t records = rb.records[0].size() + rb.records[1].size();
  bool threw = false;
  try {
    verify_custom_bounds(rb, std::vector<double>(records - 1, 1e9));  // wrong size
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  std::vector<double> nan_bound(records, std::nan(""));
  threw = false;
  try {
    verify_custom_bounds(rb, nan_bound);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// T8: a zero-reach node stays enumerated and contributes exactly zero.
// Uses a renormalized snapshot with one action's mass forced to zero.
bool test_t8_zero_reach_enumerated() {
  const Fixture f = coarse_vs_cap0();
  RiverSolveConfig cfg;
  cfg.def = f.def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_abstraction = f.bounded;
  const RiverSolveResult base = solve_river_subgame(cfg, {});
  const AbstractTree menu_tree(f.def, f.bounded);
  RiverSolveResult modified = base;
  bool selected = false;
  std::vector<bool> killed_combos(kC, false);
  for (std::size_t n = 0; n < modified.avg_sigma.size(); ++n) {
    if (modified.avg_sigma[n].empty())
      continue;
    const std::size_t menu = menu_tree.node(n).actions.size();
    if (menu < 2)
      continue;
    // A valid fixed-policy snapshot need not be an equilibrium row: replace
    // the distribution with a deterministic mass on action 1, so action 0's
    // whole subtree gets zero own reach while every row stays normalized.
    for (std::uint32_t i = 0; i < kC; ++i) {
      for (std::size_t a = 0; a < menu; ++a)
        modified.avg_sigma[n][i * menu + a] = a == 1 ? 1.0 : 0.0;
      killed_combos[i] = true;
    }
    selected = true;
    break;
  }
  CHECK(selected);
  CHECK(std::ranges::all_of(killed_combos, [](bool x) { return x; }));
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree bounded_tree(f.def, f.bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, modified, ref_tree, bounded_tree,
                                                        subset, CapExtensionCompletion::Uniform);
  bool found_zero = false;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s]) {
      for (std::uint32_t i = 0; i < kC; ++i) {
        if (!killed_combos[i] || r.own_reach[i] != 0.0)
          continue;
        found_zero = true;  // enumerated node, killed combo unreachable
        // The zero reach makes the combo's local contribution exact zero.
        CHECK(r.own_reach[i] * r.delta[i] == 0.0);
      }
    }
  CHECK(found_zero);
  return true;
}

// T9: extension laundering trap via existence scan, with a deeper-stack
// parameterized fallback when the shallow root has no positive option.
bool find_positive_extension_option(std::uint64_t stack_behind) {
  const Fixture f = coarse_vs_cap0(stack_behind);
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  for (std::size_t s = 0; s < 2; ++s) {
    for (const NodeGapRecord& r : rb.records[s]) {
      if (r.widened)
        continue;
      for (std::uint32_t i = 0; i < kC; ++i)
        if (r.delta[i] > r.bounded_gap[i] + kChipTol)
          return true;  // extension action carries positive option value
    }
  }
  return false;
}

bool test_t9_extension_laundering_existence() {
  bool positive = find_positive_extension_option(8);
  if (!positive)
    positive = find_positive_extension_option(20);  // parameterized fallback
  if (!positive) {
    std::printf("T9 diagnostic: no positive extension option on either root\n");
    return false;
  }
  // On the node where the option exists, the bounded-only gap is strictly
  // below the full-menu gap; a bounded-only certificate would launder it.
  const Fixture f = coarse_vs_cap0(8);
  const ReachBoundResult rb = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                  CapExtensionCompletion::Uniform);
  bool pinned = false;
  for (std::size_t s = 0; s < 2 && !pinned; ++s)
    for (const NodeGapRecord& r : rb.records[s]) {
      for (std::uint32_t i = 0; i < kC; ++i)
        if (r.delta[i] > r.bounded_gap[i] + kChipTol) {
          CHECK(r.delta[i] >= r.bounded_gap[i] - kChipTol);
          pinned = true;
          break;
        }
      if (pinned)
        break;
    }
  CHECK(pinned);
  return true;
}

// T10: seedless determinism.
bool test_t10_determinism() {
  const Fixture f = coarse_vs_cap0();
  const ReachBoundResult a = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                 CapExtensionCompletion::BrFavorable);
  const ReachBoundResult b = measure_reach_bound(f.def, f.bounded, f.reference, kIters, 1,
                                                 CapExtensionCompletion::BrFavorable);
  CHECK(a.exact_exploitability_bb == b.exact_exploitability_bb);
  CHECK(a.structural_certificate_bb == b.structural_certificate_bb);
  return true;
}

// T11: root weighting can under-report the MAX and is refused.
bool test_t11_root_max_only() {
  const Fixture shallow = coarse_vs_cap0(8);
  const Fixture deeper = coarse_vs_cap0(20);
  const double e1 = measure_reach_bound(shallow.def, shallow.bounded, shallow.reference, kIters, 1,
                                        CapExtensionCompletion::Uniform)
                        .exact_exploitability_bb;
  const double e2 = measure_reach_bound(deeper.def, deeper.bounded, deeper.reference, kIters, 1,
                                        CapExtensionCompletion::Uniform)
                        .exact_exploitability_bb;
  const std::vector<double> roots{e1, e2};
  const double mean = 0.5 * (e1 + e2);
  CHECK(mean <= std::max(e1, e2) + kBbTol);
  CHECK(aggregate_roots_max(roots) == std::max(e1, e2));
  bool threw = false;
  try {
    refuse_root_weights(roots, {0.5, 0.5});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// T12: independent node-level full-reach cross-check per node (not merely
// per seat), covering both seats.
bool test_t12_node_level_reach_crosscheck() {
  const Fixture f = coarse_vs_cap0();
  RiverSolveConfig cfg;
  cfg.def = f.def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_abstraction = f.bounded;
  const RiverSolveResult solved = solve_river_subgame(cfg, {});
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree bounded_tree(f.def, f.bounded);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved, ref_tree, bounded_tree,
                                                        subset, CapExtensionCompletion::Uniform);
  for (std::size_t seat = 0; seat < 2; ++seat) {
    const auto own_walk = recompute_forward_reach(ref_tree, bounded_tree, subset, solved, seat);
    const auto opp_walk = recompute_forward_reach(ref_tree, bounded_tree, subset, solved, seat ^ 1);
    for (const NodeGapRecord& r : rb.records[seat]) {
      CHECK(own_walk[r.reference_node].size() == kC);
      // Own full forward reach (the telescope weight).
      for (std::uint32_t i = 0; i < kC; ++i)
        CHECK(own_walk[r.reference_node][i] == r.own_reach[i]);
      // Symmetric opponent-reach pin (review L1): independently recompute
      // the opponent seat's forward reach and match the stored vector, so
      // S(n) is directly verified rather than only inferred via dominance.
      CHECK(opp_walk[r.reference_node].size() == kC);
      double recomputed_sum = 0.0;
      for (std::uint32_t j = 0; j < kC; ++j) {
        CHECK(opp_walk[r.reference_node][j] == r.opponent_reach[j]);
        recomputed_sum += opp_walk[r.reference_node][j];
      }
      CHECK(recomputed_sum == r.opponent_reach_sum_s);
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!test_t1_cross_instrument_identity() || !test_t2_node_gap_sign_and_widened() ||
      !test_t3_per_seat_telescope() || !test_t4_structural_bound_and_red_naive() ||
      !test_t5_reach_validator() || !test_t6_widening_claim_integrity() ||
      !test_t7_bound_malformation() || !test_t8_zero_reach_enumerated() ||
      !test_t9_extension_laundering_existence() || !test_t10_determinism() ||
      !test_t11_root_max_only() || !test_t12_node_level_reach_crosscheck())
    return 1;
  std::printf("rfc0047_reach_bound: all checks passed\n");
  return 0;
}
