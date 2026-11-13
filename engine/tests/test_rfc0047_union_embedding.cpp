// RFC 0048 Path A Stage 2b (D184) native test: union-menu reference semantics.
// Behavioral gates only (real tree builds, seedless solves, exact walks); no
// source-text or log-pattern assertions. The frozen cap_embedding instrument
// is never modified; N1 is the permanent cross-instrument drift gate between
// the two evaluators on their shared identity domain.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
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
using bs::abstraction::CoverSeeds;
using bs::abstraction::Fraction;
using bs::abstraction::SizeSchedule;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::tree::AbstractTree;
using bs::tree::tree_shape_digest;
using bs::tree::TreeNode;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kIters = 100;
constexpr double kNumericTol = 1e-12;
constexpr double kEvOracleTol = 1e-6;

bs::poker::GameDef w1_river() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  return leaves.front().river_def;
}

bool action_less(const Action& a, const Action& b) {
  if (a.type != b.type)
    return static_cast<int>(a.type) < static_cast<int>(b.type);
  return a.target_total < b.target_total;
}

std::size_t action_index(const TreeNode& node, const Action& wanted) {
  for (std::size_t i = 0; i < node.actions.size(); ++i)
    if (node.actions[i] == wanted)
      return i;
  return static_cast<std::size_t>(-1);
}

bool aggressive(ActionType t) {
  return t == ActionType::Bet || t == ActionType::Raise;
}

// Hand-built shallow street-start river root: pot 4 (2+2 dead), 8 chips
// behind each seat, same frozen board family. Small enough to hand-audit
// every root menu and enumerate all terminal lines cheaply.
bs::poker::GameDef shallow_river() {
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

// N1: union(default, default) is structurally identity(0), and the union
// measure equals the frozen cap1 measure within 1e-12; the EV walk reproduces
// the bounded kernel's root CFV x 990 below 1e-6. Permanent cross-instrument
// gate (review advisory L2).
bool test_n1_identity_degeneration() {
  const bs::poker::GameDef def = w1_river();
  const ActionAbstraction bounded = make_grid_abstraction(SizingGrid::Identity, 1);
  const ActionAbstraction reference =
      make_union_abstraction(ActionAbstraction::identity(0), bounded);

  const AbstractTree ref_tree(def, reference);
  const AbstractTree cap0_tree(def, ActionAbstraction::identity(0));
  CHECK(ref_tree.size() == cap0_tree.size());
  CHECK(ref_tree.accounted_bytes() == cap0_tree.accounted_bytes());
  CHECK(tree_shape_digest(ref_tree) == tree_shape_digest(cap0_tree));
  // The union mints its own declared digest; equality is established by shape.
  CHECK(reference.id().digest != cap0_tree.action_id().digest);

  const CapMateriality frozen =
      measure_cap_materiality(def, 1, kIters, 1, CapExtensionCompletion::Uniform);
  const UnionMateriality union_m = measure_subset_materiality(def, bounded, reference, kIters, 1,
                                                              CapExtensionCompletion::Uniform);
  CHECK(std::string(union_m.reference_label) == kUnionReferenceLabel);
  CHECK(std::string(union_m.m2_marking) == kUnionM2Marking);
  CHECK(union_m.reference_shape_digest == tree_shape_digest(cap0_tree));
  CHECK(std::isfinite(union_m.embedded_exploitability_bb));
  CHECK(std::fabs(union_m.embedded_exploitability_bb - frozen.embedded_exploitability_bb) <=
        kNumericTol);
  CHECK(std::fabs(union_m.bounded_nash_conv_bb - frozen.online_nash_conv_bb) <= kNumericTol);
  CHECK(union_m.mapped_nodes == frozen.mapped_nodes);
  CHECK(union_m.unmapped_ev_reachable_nodes == 0);
  CHECK(union_m.pure_menu_term_bb >= -kNumericTol);

  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_abstraction = bounded;
  const RiverSolveResult kernel = solve_river_subgame(cfg, {});
  const UnionRootValues v =
      subset_root_values(def, bounded, reference, kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(v.ev_seat0.size() == kC && v.ev_seat1.size() == kC);
  double worst = 0.0;
  for (std::uint32_t i = 0; i < kC; ++i) {
    worst = std::max(worst, std::fabs(v.ev_seat0[i] - kernel.root_cfv_seat0[i] * 990.0));
    worst = std::max(worst, std::fabs(v.ev_seat1[i] - kernel.root_cfv_seat1[i] * 990.0));
    CHECK(v.br_seat0[i] + 1e-9 >= v.ev_seat0[i]);
    CHECK(v.br_seat1[i] + 1e-9 >= v.ev_seat1[i]);
  }
  CHECK(worst < kEvOracleTol);
  return true;
}

// N2: coarse is a subset-but-interleaved menu of identity(0) and maps through
// exact Action equality, including an end-to-end SOLVED fixture on the real
// w1 root; wide against identity(0) fails closed.
bool test_n2_subset_mapping_interleaved() {
  const bs::poker::GameDef def = w1_river();
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree coarse(def, make_grid_abstraction(SizingGrid::Coarse, 1));
  const AbstractTree wide(def, make_grid_abstraction(SizingGrid::Wide, 1));

  const SubsetMap m = map_bounded_subset(cap0, coarse);
  const TreeNode& cap0_root = cap0.node(cap0.root_index());
  const TreeNode& coarse_root = coarse.node(coarse.root_index());
  // Hand-pinned w1 interleaving: coarse Bet198 (bounded index 2) sits at
  // cap0 index 4, so a prefix-index port cannot produce this map.
  const Action jam{ActionType::Bet, 198};
  const std::size_t cap0_jam = action_index(cap0_root, jam);
  const std::size_t coarse_jam = action_index(coarse_root, jam);
  CHECK(cap0_jam == 4);
  CHECK(coarse_jam == 2);
  CHECK(m.edge_map[cap0.root_index()][cap0_jam] == static_cast<int>(coarse_jam));
  // Every bounded action is matched; every -1 edge is aggressive.
  for (std::size_t nr = 0; nr < cap0.size(); ++nr) {
    if (m.node_map[nr] == SubsetMap::kNoNode || !cap0.node(nr).is_action())
      continue;
    const TreeNode& bn = coarse.node(m.node_map[nr]);
    std::set<int> matched;
    for (std::size_t ar = 0; ar < cap0.node(nr).actions.size(); ++ar) {
      const int ab = m.edge_map[nr][ar];
      if (ab == -1) {
        CHECK(aggressive(cap0.node(nr).actions[ar].type));
      } else {
        CHECK(cap0.node(nr).actions[ar] == bn.actions[static_cast<std::size_t>(ab)]);
        CHECK(matched.insert(ab).second);
      }
    }
    CHECK(matched.size() == bn.actions.size());
  }

  // End-to-end solved interleaved embedding into the cap0 reference domain.
  const UnionMateriality solved = measure_subset_materiality(
      def, make_grid_abstraction(SizingGrid::Coarse, 1), ActionAbstraction::identity(0), kIters, 1,
      CapExtensionCompletion::Uniform);
  CHECK(std::isfinite(solved.embedded_exploitability_bb));
  CHECK(solved.bounded_action_nodes == solved.mapped_nodes);
  CHECK(solved.unmapped_ev_reachable_nodes == 0);
  CHECK(solved.pure_menu_term_bb >= -kNumericTol);
  CHECK(std::string(solved.reference_label) == kUnionReferenceLabel);

  bool threw = false;
  try {
    (void)map_bounded_subset(cap0, wide);  // Bet4 is foreign to identity(0)
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// N3: wide maps into its union; the union root menu is exactly the sorted
// set union of the two root menus, and no extension edge is passive.
bool test_n3_union_mapping() {
  const bs::poker::GameDef def = w1_river();
  const ActionAbstraction wide = make_grid_abstraction(SizingGrid::Wide, 1);
  const ActionAbstraction union_action =
      make_union_abstraction(ActionAbstraction::identity(0), wide);
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree wide_tree(def, wide);
  const AbstractTree union_tree(def, union_action);

  // Element-wise sorted set union of the root menus.
  std::vector<Action> expected = cap0.node(cap0.root_index()).actions;
  for (const Action& a : wide_tree.node(wide_tree.root_index()).actions)
    if (std::find(expected.begin(), expected.end(), a) == expected.end())
      expected.push_back(a);
  std::sort(expected.begin(), expected.end(), action_less);
  std::vector<Action> actual = union_tree.node(union_tree.root_index()).actions;
  std::sort(actual.begin(), actual.end(), action_less);
  CHECK(expected == actual);
  // The foreign wide target Bet4 survives in the union and is absent in cap0.
  const Action bet4{ActionType::Bet, 4};
  CHECK(action_index(union_tree.node(union_tree.root_index()), bet4) !=
        static_cast<std::size_t>(-1));
  CHECK(action_index(cap0.node(cap0.root_index()), bet4) == static_cast<std::size_t>(-1));

  const SubsetMap m = map_bounded_subset(union_tree, wide_tree);
  for (std::size_t nr = 0; nr < union_tree.size(); ++nr) {
    if (m.node_map[nr] == SubsetMap::kNoNode || !union_tree.node(nr).is_action())
      continue;
    const TreeNode& wn = wide_tree.node(m.node_map[nr]);
    std::set<int> matched;
    for (std::size_t ar = 0; ar < union_tree.node(nr).actions.size(); ++ar) {
      const int ab = m.edge_map[nr][ar];
      if (ab == -1)
        CHECK(aggressive(union_tree.node(nr).actions[ar].type));
      else
        CHECK(matched.insert(ab).second);
    }
    CHECK(matched.size() == wn.actions.size());
  }

  const UnionMateriality measured =
      measure_union_materiality(def, wide, kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(measured.reference_abstraction_digest == union_action.id().digest);
  CHECK(measured.reference_shape_digest == tree_shape_digest(union_tree));
  CHECK(std::isfinite(measured.embedded_exploitability_bb));
  CHECK(measured.unmapped_ev_reachable_nodes == 0);
  return true;
}

// N4: hand-audited shallow root. Exact root menu membership, clamp collision,
// a cap-2 Raise node exercising raise-fraction merge, the EV oracle, and the
// per-combo BR_union >= BR_bounded superset monotonicity positive control.
bool test_n4_enumerable_shallow_root() {
  const bs::poker::GameDef def = shallow_river();

  // Hand-audited identity(0) root menu. At a street-start check-or-bet node
  // base=0 and pot_after_call=4: fraction targets are ceil(4/3)=2,
  // ceil(3)=3, ceil(6)=6; MinAndCap seeds the min full bet 2 (collides with
  // 1/3) and the all-in cover 8. Hence Check + Bet2, Bet3, Bet6, Bet8.
  const AbstractTree id0(def, ActionAbstraction::identity(0));
  const std::vector<Action> expected_root{
      {ActionType::Check, 0}, {ActionType::Bet, 2}, {ActionType::Bet, 3},
      {ActionType::Bet, 6},   {ActionType::Bet, 8},
  };
  std::vector<Action> root = id0.node(id0.root_index()).actions;
  std::sort(root.begin(), root.end(), action_less);
  CHECK(root == expected_root);

  // Clamp collision: 2/1 of the 4-chip pot targets the all-in cover 8, and
  // 5/2 targets 10 which clamps to the same 8; unique() must leave exactly
  // one Bet8 edge alongside the min seed Bet2.
  SizeSchedule collision_schedule = bs::abstraction::default_size_schedule();
  collision_schedule[static_cast<std::size_t>(bs::poker::Street::River)].bets = {Fraction{2, 1},
                                                                                 Fraction{5, 2}};
  const ActionAbstraction collision =
      ActionAbstraction::declared(collision_schedule, CoverSeeds::MinAndCap, 1);
  const AbstractTree collision_tree(def, collision);
  const std::vector<Action> collision_expected{
      {ActionType::Check, 0}, {ActionType::Bet, 2}, {ActionType::Bet, 8}};
  std::vector<Action> collision_root = collision_tree.node(collision_tree.root_index()).actions;
  std::sort(collision_root.begin(), collision_root.end(), action_less);
  CHECK(collision_root == collision_expected);

  // Cap-2 coarse grid keeps a Raise-type node; its union with identity(0)
  // merges the raise fractions and still reaches that node.
  const ActionAbstraction bounded = make_grid_abstraction(SizingGrid::Coarse, 2);
  const ActionAbstraction reference =
      make_union_abstraction(ActionAbstraction::identity(0), bounded);
  const AbstractTree ref_tree(def, reference);
  bool has_raise_node = false;
  for (const TreeNode& n : ref_tree.nodes())
    if (n.is_action() && n.incoming_action.type == ActionType::Raise)
      has_raise_node = true;
  CHECK(has_raise_node);

  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kIters;
  cfg.linear_averaging = true;
  cfg.action_abstraction = bounded;
  const RiverSolveResult kernel = solve_river_subgame(cfg, {});
  const UnionRootValues v =
      subset_root_values(def, bounded, reference, kIters, 1, CapExtensionCompletion::Uniform);
  double worst = 0.0;
  for (std::uint32_t i = 0; i < kC; ++i) {
    worst = std::max(worst, std::fabs(v.ev_seat0[i] - kernel.root_cfv_seat0[i] * 990.0));
    worst = std::max(worst, std::fabs(v.ev_seat1[i] - kernel.root_cfv_seat1[i] * 990.0));
    // Superset max monotonicity: union BR weakly dominates the bounded BR.
    CHECK(v.br_seat0[i] + 1e-9 >= kernel.root_br_seat0[i] * 990.0);
    CHECK(v.br_seat1[i] + 1e-9 >= kernel.root_br_seat1[i] * 990.0);
  }
  CHECK(worst < kEvOracleTol);
  return true;
}

// N5: determinism and fail-close on every precondition failure.
bool test_n5_determinism_and_fail_close() {
  const bs::poker::GameDef def = w1_river();
  const ActionAbstraction coarse = make_grid_abstraction(SizingGrid::Coarse, 1);
  const UnionMateriality a = measure_subset_materiality(def, coarse, ActionAbstraction::identity(0),
                                                        kIters, 1, CapExtensionCompletion::Uniform);
  const UnionMateriality b = measure_subset_materiality(def, coarse, ActionAbstraction::identity(0),
                                                        kIters, 1, CapExtensionCompletion::Uniform);
  CHECK(a.embedded_exploitability_bb == b.embedded_exploitability_bb);
  CHECK(a.bounded_shape_digest == b.bounded_shape_digest);

  // same_game_def precondition: a different pot is a different game.
  bs::poker::GameDef other = def;
  other.pot = def.pot + 2;
  bool threw = false;
  try {
    const AbstractTree ref(def, ActionAbstraction::identity(0));
    const AbstractTree diff(other, coarse);
    (void)map_bounded_subset(ref, diff);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Bounded-not-subset with a deliberately foreign single target.
  SizeSchedule foreign_schedule = bs::abstraction::default_size_schedule();
  foreign_schedule[static_cast<std::size_t>(bs::poker::Street::River)].bets = {Fraction{11, 2}};
  const ActionAbstraction foreign =
      ActionAbstraction::declared(foreign_schedule, CoverSeeds::MinAndCap, 1);
  threw = false;
  try {
    const AbstractTree ref(def, ActionAbstraction::identity(0));
    const AbstractTree fb(def, foreign);
    (void)map_bounded_subset(ref, fb);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Malformed fraction (zero denominator) is refused at declaration.
  SizeSchedule malformed = bs::abstraction::default_size_schedule();
  malformed[static_cast<std::size_t>(bs::poker::Street::River)].bets = {Fraction{1, 0}};
  threw = false;
  try {
    (void)ActionAbstraction::declared(malformed, CoverSeeds::MinAndCap, 1);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // 32-action overflow surfaces as invalid_argument, never a truncated menu.
  SizeSchedule oversized = bs::abstraction::default_size_schedule();
  auto& bets = oversized[static_cast<std::size_t>(bs::poker::Street::River)].bets;
  bets.clear();
  for (std::uint64_t k = 1; k <= 31; ++k)
    bets.push_back(Fraction{k, 1});  // 31 distinct targets + min/cap seeds > 32
  threw = false;
  try {
    const ActionAbstraction big = ActionAbstraction::declared(oversized, CoverSeeds::MinAndCap, 1);
    const AbstractTree overflow(def, big);
    (void)overflow;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // DeclaredOnly is outside the union preconditions.
  threw = false;
  try {
    (void)make_union_abstraction(
        ActionAbstraction::declared(bs::abstraction::default_size_schedule(),
                                    CoverSeeds::DeclaredOnly, 1),
        ActionAbstraction::identity(0));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

}  // namespace

int main() {
  if (!test_n1_identity_degeneration() || !test_n2_subset_mapping_interleaved() ||
      !test_n3_union_mapping() || !test_n4_enumerable_shallow_root() ||
      !test_n5_determinism_and_fail_close())
    return 1;
  std::printf("rfc0047_union_embedding: all checks passed\n");
  return 0;
}
