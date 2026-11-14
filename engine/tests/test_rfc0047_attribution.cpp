// RFC 0048 D233 native test: comparable counterfactual attribution and atomic
// candidate admission. The suite runs with ZERO solver/BR/CFR calls: masks are
// produced only by the zero-solve sparse-mask and witness apply builders with
// structurally injected coordinates, and every SealedCandidateTerms tuple uses
// synthetic scalars. Behavioral assertions only; no source-text or log
// matching. Bench-only; production surfaces stay untouched.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/mixed_menu.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"

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
constexpr double kTol = 1e-12;
constexpr std::array<std::uint64_t, 4> kDigests = {11, 22, 33, 44};
constexpr const char* kLabel = "ref-label";
constexpr const char* kMarking = "m2-marking";

bs::poker::GameDef river_root() {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {8, 8, 0, 0, 0, 0, 0, 0, 0, 0};
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
  return {river_root(), make_grid_abstraction(SizingGrid::Coarse, 1),
          ActionAbstraction::identity(0)};
}

bool is_aggressive(const TreeNode& node, std::size_t a) {
  const auto t = node.actions[a].type;
  return t == bs::poker::ActionType::Bet || t == bs::poker::ActionType::Raise;
}

// Root DFS closure over allowed edges (zero-solve structural helper).
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

// A zero-solve mask chain B < S < C plus the structural coordinates that
// distinguish them. Eligible coordinates are found purely by scanning the
// immutable reference and baseline mask: no ReachBoundResult, no solve.
struct Chain {
  MixedMenuMask baseline;
  MixedMenuMask single;
  MixedMenuMask combined;
  MaskCoordinate first;
  MaskCoordinate second;
};

Chain build_chain(const Fixture& g, const AbstractTree& ref) {
  const AbstractTree coarse(g.def, g.coarse);
  Chain c;
  c.baseline = make_sparse_multilayer_mask(ref, coarse, 2);
  const std::vector<bool> reachable = reachable_nodes(ref, c.baseline);

  // Collect one suppressed aggressive edge per baseline-reachable node, DFS
  // node order: the D227 combination requires two DISTINCT public nodes.
  std::vector<MaskCoordinate> eligible;
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (!reachable[n] || !ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a) {
      if (is_aggressive(ref.node(n), a) && !c.baseline.allowed[n][a]) {
        eligible.push_back({n, a});
        break;
      }
    }
  }
  if (eligible.size() < 2) {
    std::printf("fixture needs two eligible suppressed edges, found %zu\n", eligible.size());
    std::fflush(stdout);
    throw std::runtime_error("D233 fixture lacks two eligible edges");
  }
  c.first = eligible.front();
  c.second = eligible[1];

  WitnessSelection ws;
  ws.status = WitnessStatus::kSelected;
  ws.node = c.first.node;
  ws.edge = c.first.edge;
  c.single = apply_witness_augmentation(ref, c.baseline, ws);

  CombinationSelection cs;
  cs.status = CombinationStatus::kCombined;
  cs.node1 = c.first.node;
  cs.edge1 = c.first.edge;
  cs.node2 = c.second.node;
  cs.edge2 = c.second.edge;
  c.combined = apply_combination_augmentation(ref, c.baseline, cs);

  // Structural chain invariants the admission gate relies on.
  for (std::size_t n = 0; n < ref.size(); ++n)
    for (std::size_t a = 0; a < c.baseline.allowed[n].size(); ++a) {
      if (c.baseline.allowed[n][a] && !c.single.allowed[n][a])
        throw std::runtime_error("D233 fixture: single mask is not a baseline superset");
      if (c.single.allowed[n][a] && !c.combined.allowed[n][a])
        throw std::runtime_error("D233 fixture: combined mask is not a single superset");
    }
  return c;
}

SealedCandidateTerms make_terms(const bs::poker::GameDef& def, const AbstractTree& ref,
                                const MixedMenuMask& mask, std::uint64_t policy_hash, double exact,
                                double bounded,
                                CapExtensionCompletion arm = CapExtensionCompletion::Uniform,
                                std::array<std::uint64_t, 4> digests = kDigests,
                                double normalization = 1080.0) {
  return SealedCandidateTerms(def, digests, kLabel, kMarking, hash_mask(ref, mask), policy_hash,
                              normalization, arm, exact, bounded);
}

CandidateBundleEntry make_entry(const MixedMenuMask& mask, std::vector<MaskCoordinate> scored,
                                SealedCandidateTerms terms) {
  return {mask, std::move(scored), std::move(terms)};
}

std::vector<MaskCoordinate> single_coords(const Chain& c) {
  return {{c.first.node, c.first.edge}};
}

std::vector<MaskCoordinate> combined_coords(const Chain& c) {
  return {{c.first.node, c.first.edge}, {c.second.node, c.second.edge}};
}

// T1: same-source positive bundle is comparable and marginals match hand math.
bool test_t1_comparable() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  const auto before_b = c.baseline.allowed;
  const auto before_s = c.single.allowed;
  const auto before_c = c.combined.allowed;
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.20, 0.20)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.00, 0.30)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.90, 0.40)));
  CHECK(report.status == AttributionStatus::kComparable);
  CHECK(report.reason == AttributionReason::kNone);
  CHECK(std::fabs(report.pure_menu_baseline - 1.00) < kTol);
  CHECK(std::fabs(report.pure_menu_single - 0.70) < kTol);
  CHECK(std::fabs(report.pure_menu_combined - 0.50) < kTol);
  CHECK(std::fabs(report.observed_menu_omission_baseline_to_single - (-0.30)) < kTol);
  CHECK(std::fabs(report.observed_menu_omission_single_to_combined - (-0.20)) < kTol);
  CHECK(std::fabs(report.bounded_nash_conv[0].raw() - 0.20) < kTol);
  CHECK(std::fabs(report.bounded_nash_conv[1].raw() - 0.30) < kTol);
  CHECK(std::fabs(report.bounded_nash_conv[2].raw() - 0.40) < kTol);
  // Success-path invariance (A7).
  CHECK(c.baseline.allowed == before_b);
  CHECK(c.single.allowed == before_s);
  CHECK(c.combined.allowed == before_c);
  return true;
}

// T2: digest mismatch -> typed incomparable, no attribution produced.
bool test_t2_digest_mismatch() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  auto bad_digests = kDigests;
  bad_digests[0] = 999;
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4, CapExtensionCompletion::Uniform,
                            bad_digests)));
  CHECK(report.status == AttributionStatus::kIncomparableSource);
  CHECK(report.reason == AttributionReason::kDigestMismatch);
  return true;
}

// T3: dimensional mismatch -> kMaskWidthMismatch; term/mask swap -> binding
// rejection kTermMaskBindingMismatch.
bool test_t3_width_and_binding() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  // Width arm: widen one action row beyond its reference menu.
  MixedMenuMask wide = c.single;
  wide.allowed[c.first.node].push_back(false);
  auto r_width = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(wide, single_coords(c), make_terms(g.def, ref, wide, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), make_terms(g.def, ref, c.combined, 3, 0.9, 0.4)));
  CHECK(r_width.status == AttributionStatus::kIncomparableSource);
  CHECK(r_width.reason == AttributionReason::kMaskWidthMismatch);

  // Binding arm: combined scalars carry the baseline mask hash.
  SealedCandidateTerms swapped(g.def, kDigests, kLabel, kMarking, hash_mask(ref, c.baseline), 1003,
                               1080.0, CapExtensionCompletion::Uniform, 0.9, 0.4);
  auto r_bind = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), swapped));
  CHECK(r_bind.status == AttributionStatus::kIncomparableSource);
  CHECK(r_bind.reason == AttributionReason::kTermMaskBindingMismatch);
  return true;
}

// T4: duplicate node in the combined scored set -> kDuplicateNode.
bool test_t4_duplicate_node() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  auto dup =
      std::vector<MaskCoordinate>{{c.first.node, c.first.edge}, {c.first.node, c.first.edge}};
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, dup, make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(report.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(report.reason == AttributionReason::kDuplicateNode);
  return true;
}

// T5: dropped allowed bit -> kNotStrictSuperset; suppressed passive edge ->
// validate_mask content failure mapped to kMaskContentInvalid.
bool test_t5_superset_and_content() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  const std::vector<bool> reachable = reachable_nodes(ref, c.baseline);

  // Superset arm: remove an allowed aggressive edge at a reachable row while
  // keeping the row non-empty and every passive edge legal.
  MixedMenuMask reduced = c.baseline;
  bool removed = false;
  for (std::size_t n = 0; n < ref.size() && !removed; ++n) {
    if (!reachable[n] || !ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < reduced.allowed[n].size(); ++a) {
      if (reduced.allowed[n][a] && is_aggressive(ref.node(n), a)) {
        reduced.allowed[n][a] = false;
        removed = true;
        break;
      }
    }
  }
  CHECK(removed);
  auto r_super = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(reduced, single_coords(c), make_terms(g.def, ref, reduced, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(r_super.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_super.reason == AttributionReason::kNotStrictSuperset);

  // Content arm: suppress a passive edge; dimensions stay legal so G1 passes,
  // then A1 validate_mask fails and is mapped to kMaskContentInvalid.
  MixedMenuMask passive_bad = c.baseline;
  bool flipped = false;
  for (std::size_t n = 0; n < ref.size() && !flipped; ++n) {
    if (!reachable[n] || !ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < passive_bad.allowed[n].size(); ++a) {
      if (passive_bad.allowed[n][a] && !is_aggressive(ref.node(n), a)) {
        passive_bad.allowed[n][a] = false;
        flipped = true;
        break;
      }
    }
  }
  CHECK(flipped);
  auto r_content = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(passive_bad, single_coords(c),
                 make_terms(g.def, ref, passive_bad, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(r_content.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_content.reason == AttributionReason::kMaskContentInvalid);
  return true;
}

// T6: NaN term survives the non-validating constructor and is rejected at A5.
bool test_t6_nonfinite() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, nan, 0.4)));
  CHECK(report.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(report.reason == AttributionReason::kMissingOrNonFiniteTerm);
  return true;
}

// T7: scored-coordinate count violations on both augmented slots.
bool test_t7_count_violation() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  // Single slot claims two coordinates.
  auto r_single = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, combined_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(r_single.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_single.reason == AttributionReason::kSingleEdgeCountViolation);
  // Combined slot claims only one coordinate.
  auto r_combined = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, single_coords(c), make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(r_combined.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_combined.reason == AttributionReason::kCombinedNodeCountViolation);
  return true;
}

// T8: non-additivity — the NC values are list-only; the only derived numeric
// fields are the two sign-unconstrained menu marginals.
bool test_t8_non_additive() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.2, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.95, 0.4)));
  CHECK(report.status == AttributionStatus::kComparable);
  CHECK(std::fabs(report.bounded_nash_conv[0].raw() - 0.2) < kTol);
  CHECK(std::fabs(report.bounded_nash_conv[1].raw() - 0.3) < kTol);
  CHECK(std::fabs(report.bounded_nash_conv[2].raw() - 0.4) < kTol);
  CHECK(std::fabs(report.observed_menu_omission_baseline_to_single -
                  (report.pure_menu_single - report.pure_menu_baseline)) < kTol);
  CHECK(std::fabs(report.observed_menu_omission_single_to_combined -
                  (report.pure_menu_combined - report.pure_menu_single)) < kTol);
  return true;
}

// T9: masks unchanged before/after a rejection and a successful call.
bool test_t9_atomicity() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  const auto snap_b = c.baseline.allowed;
  const auto snap_s = c.single.allowed;
  const auto snap_c = c.combined.allowed;
  auto bad = kDigests;
  bad[1] = 777;
  (void)attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4, CapExtensionCompletion::Uniform,
                            bad)));
  CHECK(c.baseline.allowed == snap_b);
  CHECK(c.single.allowed == snap_s);
  CHECK(c.combined.allowed == snap_c);
  (void)attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.2, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(c.baseline.allowed == snap_b);
  CHECK(c.single.allowed == snap_s);
  CHECK(c.combined.allowed == snap_c);
  return true;
}

// T10: normalization, completion arm, and GameDef identity mismatches.
bool test_t10_identity_mismatches() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  auto r_norm = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 3, 0.9, 0.4, CapExtensionCompletion::Uniform,
                            kDigests, 999.0)));
  CHECK(r_norm.status == AttributionStatus::kIncomparableSource);
  CHECK(r_norm.reason == AttributionReason::kNormalizationMismatch);

  auto r_arm = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(
          c.single, single_coords(c),
          make_terms(g.def, ref, c.single, 2, 1.0, 0.3, CapExtensionCompletion::BrFavorable)),
      make_entry(c.combined, combined_coords(c), make_terms(g.def, ref, c.combined, 3, 0.9, 0.4)));
  CHECK(r_arm.status == AttributionStatus::kIncomparableSource);
  CHECK(r_arm.reason == AttributionReason::kCompletionArmMismatch);

  auto def2 = g.def;
  def2.board[0] = bs::cardId("3c");
  auto r_def = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), make_terms(def2, ref, c.combined, 3, 0.9, 0.4)));
  CHECK(r_def.status == AttributionStatus::kIncomparableSource);
  CHECK(r_def.reason == AttributionReason::kGameDefMismatch);
  return true;
}

// T11 (review round-3 method A): keep the valid B,S,C mask chain in place so
// A2 passes, but inject the two-coordinate combination into the single slot.
bool test_t11_coordinate_role_conflict() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, combined_coords(c), make_terms(g.def, ref, c.single, 1002, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(report.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(report.reason == AttributionReason::kSingleEdgeCountViolation);
  return true;
}

// T12: negative menu term, inactive selection, and unreachable coordinate.
bool test_t12_negative_liveness_unreachable() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  // Negative pure-menu term: exact - bounded = -0.5.
  auto r_neg = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), make_terms(g.def, ref, c.combined, 3, 0.1, 0.6)));
  CHECK(r_neg.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_neg.reason == AttributionReason::kNegativeMenuTerm);

  // Inactive selection: no scored coordinate, mask left at the baseline.
  auto r_inactive = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), make_terms(g.def, ref, c.combined, 3, 0.9, 0.4)));
  CHECK(r_inactive.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_inactive.reason == AttributionReason::kSelectionNotActive);

  // Unreachable coordinate: a suppressed aggressive edge on a row outside the
  // baseline closure, injected while the real single mask stays in place.
  const std::vector<bool> reachable = reachable_nodes(ref, c.baseline);
  MaskCoordinate off_closure{ref.size(), 0};
  for (std::size_t n = 0; n < ref.size(); ++n) {
    if (reachable[n] || !ref.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref.node(n).actions.size(); ++a) {
      if (is_aggressive(ref.node(n), a) && !c.baseline.allowed[n][a]) {
        off_closure = {n, a};
        break;
      }
    }
    if (off_closure.node != ref.size())
      break;
  }
  CHECK(off_closure.node != ref.size());
  auto r_unreachable = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1, 1.0, 0.2)),
      make_entry(c.single, {{off_closure.node, off_closure.edge}},
                 make_terms(g.def, ref, c.single, 2, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c), make_terms(g.def, ref, c.combined, 3, 0.9, 0.4)));
  CHECK(r_unreachable.status == AttributionStatus::kInadmissibleCandidate);
  CHECK(r_unreachable.reason == AttributionReason::kCoordinateUnreachable);
  return true;
}

// T13: runtime single-field mutation matrix over the same_game_def identity
// set: every mutant flips both the public predicate and the FNV digest.
bool test_t13_game_def_mutation_matrix() {
  const bs::poker::GameDef base = river_root();
  const auto base_hash = hash_game_def(base);
  std::vector<bs::poker::GameDef> mutants;
  auto add = [&](bs::poker::GameDef m) { mutants.push_back(m); };
  {
    auto m = base;
    m.player_count = 1;
    add(m);
    m = base;
    m.button = 1;
    add(m);
    m = base;
    m.big_blind = 4;
    add(m);
    m = base;
    m.ante = 1;
    add(m);
    m = base;
    m.pot = 5;
    add(m);
    m = base;
    m.preflop = true;
    add(m);
    m = base;
    m.variant = static_cast<bs::poker::RulesVariant>(99);
    add(m);
    m = base;
    m.terminal = bs::poker::TerminalDepth::Flop;
    add(m);
    m = base;
    m.last_full_raise = 7;
    add(m);
    m = base;
    m.raises_this_street = 3;
    add(m);
    m = base;
    m.board_size = 4;
    add(m);
    m = base;
    m.board[4] = bs::cardId("3c");
    add(m);
    for (std::size_t p = 0; p < base.player_count; ++p) {
      m = base;
      m.stacks[p] += 1;
      add(m);
      m = base;
      m.contributions[p] += 1;
      add(m);
      m = base;
      m.blinds_posted[p] = 1;
      add(m);
      m = base;
      m.folded[p] = true;
      add(m);
      m = base;
      m.street_committed[p] = 1;
      add(m);
      m = base;
      m.raise_rights[p] = !m.raise_rights[p];
      add(m);
    }
  }
  for (const auto& m : mutants) {
    CHECK(!bs::poker::same_game_def(base, m));
    CHECK(hash_game_def(m) != base_hash);
  }
  // GameDef currently has no non-identity field, so the T13 negative arm is an
  // empty guard activated only if such a field is ever added.
  return true;
}

// Post-implementation review F3: G1a provenance branches outside the frozen
// T1-T13 matrix get synthetic arms so every implemented rejection reason has
// behavioral coverage.
bool test_label_mismatch() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  // Same mask/digests, different reference-label text (content comparison).
  SealedCandidateTerms relabeled(g.def, kDigests, "other-label", kMarking, hash_mask(ref, c.single),
                                 1002, 1080.0, CapExtensionCompletion::Uniform, 1.0, 0.3);
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), relabeled),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(report.status == AttributionStatus::kIncomparableSource);
  CHECK(report.reason == AttributionReason::kLabelMismatch);
  return true;
}

bool test_zero_policy_hash() {
  const Fixture g = make_fixture();
  const AbstractTree ref(g.def, g.reference);
  const Chain c = build_chain(g, ref);
  // Missing snapshot provenance: policy hash presence-only check.
  auto report = attribute_candidate_bundle(
      ref, g.def, make_entry(c.baseline, {}, make_terms(g.def, ref, c.baseline, 1001, 1.0, 0.2)),
      make_entry(c.single, single_coords(c), make_terms(g.def, ref, c.single, 0, 1.0, 0.3)),
      make_entry(c.combined, combined_coords(c),
                 make_terms(g.def, ref, c.combined, 1003, 0.9, 0.4)));
  CHECK(report.status == AttributionStatus::kIncomparableSource);
  CHECK(report.reason == AttributionReason::kDigestMismatch);
  return true;
}

}  // namespace

int main() {
  if (!test_t1_comparable() || !test_t2_digest_mismatch() || !test_t3_width_and_binding() ||
      !test_t4_duplicate_node() || !test_t5_superset_and_content() || !test_t6_nonfinite() ||
      !test_t7_count_violation() || !test_t8_non_additive() || !test_t9_atomicity() ||
      !test_t10_identity_mismatches() || !test_t11_coordinate_role_conflict() ||
      !test_t12_negative_liveness_unreachable() || !test_t13_game_def_mutation_matrix() ||
      !test_label_mismatch() || !test_zero_policy_hash())
    return 1;
  std::printf("rfc0047_attribution: all checks passed\n");
  return 0;
}
