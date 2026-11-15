// RFC 0048 D249 native test: the Fine sizing grid is a real, legal, distinct
// declared abstraction before the expensive 300-iteration anchor runs. Real
// tree construction and real 60-iteration river solves only; no text matching,
// no substitute allocator, and no quality-gate relaxation. The 300-iter
// single-anchor measurement lives in the manual fine-grid probe, not here.
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

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/mixed_menu.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
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
using bs::abstraction::Fraction;
using bs::abstraction::SizeSchedule;
using bs::tree::AbstractTree;
using bs::tree::tree_shape_digest;
using bs::tree::TreeLimits;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kTinyIters = 60;
constexpr double kDistinctL1 = 1e-12;

bs::poker::GameDef w1_river() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty())
    throw std::runtime_error("w1 frontier empty");
  return leaves.front().river_def;
}

bs::poker::GameDef w1_turn() {
  return make_stratum_def(*find_stratum("w1"), 1);
}

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

RiverSolveResult solve_mode(const bs::poker::GameDef& def, const ActionAbstraction& bounded,
                            bool linear) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = kTinyIters;
  cfg.linear_averaging = linear;
  cfg.threads = 1;
  cfg.action_abstraction = bounded;
  return solve_river_subgame(cfg, {});
}

bool menus_differ_somewhere(const AbstractTree& a, const AbstractTree& b) {
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t n = 0; n < common; ++n) {
    if (!a.node(n).is_action() || !b.node(n).is_action())
      continue;
    if (a.node(n).actions != b.node(n).actions)
      return true;
  }
  return false;
}

// N1: the Fine schedule really carries bets {1/3,1/2,3/4} and raises
// {1/2,1}. The declared fractions are asserted directly through the public
// schedule accessor on every postflop street plus preflop equality with the
// default blind-relative menu, then a production chooser scan proves the
// fractions survive builder clamping as distinct legal in-menu bets. The
// smallest check-to-river leaves clamp fine fractions onto minimum legal
// increments, so the behavioral part scans the frozen w1 frontiers for a
// separable leaf and reports the qualifying count rather than assuming one.
bool test_n1_schedule_contents() {
  const ActionAbstraction fine = make_grid_abstraction(SizingGrid::Fine, 1);
  const SizeSchedule& schedule = fine.schedule();
  const SizeSchedule defaults = bs::abstraction::default_size_schedule();
  const std::vector<Fraction> expected_bets{Fraction{1, 3}, Fraction{1, 2}, Fraction{3, 4}};
  const std::vector<Fraction> expected_raises{Fraction{1, 2}, Fraction{1, 1}};
  for (bs::poker::Street st :
       {bs::poker::Street::Flop, bs::poker::Street::Turn, bs::poker::Street::River}) {
    const auto& sizes = schedule[static_cast<std::size_t>(st)];
    CHECK(sizes.bets == expected_bets);
    CHECK(sizes.raises == expected_raises);
  }
  // Preflop is never overridden: it must stay the default blind-relative menu.
  const auto& preflop = schedule[static_cast<std::size_t>(bs::poker::Street::Preflop)];
  const auto& default_preflop = defaults[static_cast<std::size_t>(bs::poker::Street::Preflop)];
  CHECK(preflop.bets == default_preflop.bets);
  CHECK(preflop.raises == default_preflop.raises);

  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  const std::array<Fraction, 3> bet_fractions{Fraction{1, 3}, Fraction{1, 2}, Fraction{3, 4}};
  std::size_t separable = 0;
  bool verified = false;
  for (const FrontierLeaf& leaf : leaves) {
    const AbstractTree tree(leaf.river_def, fine);
    const std::vector<PublicState> pub = replay_public_states(tree);
    const std::size_t root = tree.root_index();
    const bs::tree::TreeNode& root_node = tree.node(root);
    const std::size_t actor = root_node.actor;
    const std::uint64_t due =
        std::max(pub[root].street_committed[0], pub[root].street_committed[1]) -
        pub[root].street_committed[actor];
    if (due != 0)
      continue;  // opening-bet fractions are only directly readable at a street start.
    std::set<long> chosen;
    for (const Fraction& f : bet_fractions) {
      const std::uint64_t ideal = pub[root].pot * f.numerator / f.denominator;
      const long idx = nearest_aggressive_choice(root_node, ideal, 0);
      if (idx < 0 ||
          root_node.actions[static_cast<std::size_t>(idx)].type != bs::poker::ActionType::Bet)
        break;
      chosen.insert(idx);
    }
    if (chosen.size() != 3)
      continue;  // clamp-collision leaf: keep scanning for a separable one.
    // Every chosen bet is genuinely present in the menu (the chooser only
    // returns legal in-menu indices) and the three stay distinct.
    for (long idx : chosen)
      CHECK(static_cast<std::size_t>(idx) < root_node.actions.size());
    ++separable;
    verified = true;
  }
  CHECK(verified);
  CHECK(separable >= 1);
  std::printf("D249 N1 separable frontier leaves: %zu of %zu\n", separable, leaves.size());

  const AbstractTree turn_tree(w1_turn(), fine);
  CHECK(turn_tree.size() > 0);
  CHECK(sizing_grid_name(SizingGrid::Fine) == std::string("fine"));
  return true;
}

// N2: Fine is distinct from every closed grid in digest and in at least one
// real menu on the w1 anchor tree (the tiny root can hide novelty in clamp
// collisions, so the anchor tree is the pinned surface).
bool test_n2_distinct_abstraction() {
  const bs::poker::GameDef def = w1_river();
  const AbstractTree fine(def, make_grid_abstraction(SizingGrid::Fine, 1));
  const AbstractTree grids[] = {
      AbstractTree(def, make_grid_abstraction(SizingGrid::Coarse, 1)),
      AbstractTree(def, make_grid_abstraction(SizingGrid::Identity, 1)),
      AbstractTree(def, make_grid_abstraction(SizingGrid::Wide, 1)),
  };
  for (const AbstractTree& other : grids) {
    CHECK(fine.action_id().digest != other.action_id().digest);
    CHECK(menus_differ_somewhere(fine, other));
  }
  return true;
}

// N3: deterministic shape, TreeLimits boundary classification, and the
// measured Fine footprint against the closed grids (the smaller-tree claim is
// evidence here, never a premise).
bool test_n3_determinism_footprint_and_limits() {
  const bs::poker::GameDef def = w1_river();
  const AbstractTree a(def, make_grid_abstraction(SizingGrid::Fine, 1));
  const AbstractTree b(def, make_grid_abstraction(SizingGrid::Fine, 1));
  CHECK(tree_shape_digest(a) == tree_shape_digest(b));
  CHECK(a.size() == b.size() && a.accounted_bytes() == b.accounted_bytes());

  TreeLimits tight;
  tight.max_nodes = 5;  // the Fine anchor tree has ~21 nodes, so this must bind.
  tight.max_bytes = std::size_t{1} << 40;
  tight.max_depth = 256;
  const GridBudget boundary = measure_grid_budget(
      def, "reached_river", make_grid_abstraction(SizingGrid::Fine, 1), "fine", 1, "fine", tight);
  CHECK(boundary.resource_boundary);
  CHECK(boundary.accounted_bytes == 0 && boundary.total_nodes == 0);

  const GridBudget fine_budget = measure_grid_budget(
      def, "reached_river", make_grid_abstraction(SizingGrid::Fine, 1), "fine", 1, "fine");
  const GridBudget identity_budget =
      measure_grid_budget(def, "reached_river", make_grid_abstraction(SizingGrid::Identity, 1),
                          "identity", 1, "identity");
  CHECK(!fine_budget.resource_boundary && !identity_budget.resource_boundary);
  std::printf("D249 N3 fine action=%zu identity action=%zu fine bytes=%zu\n",
              fine_budget.action_nodes, identity_budget.action_nodes, fine_budget.accounted_bytes);
  return true;
}

// N4: the cap0-subset versus union-only embedding verdict is machine-
// determined; either typed outcome is pre-registered, both stay legal.
bool test_n4_embedding_verdict() {
  const bs::poker::GameDef def = w1_river();
  const ActionAbstraction fine = make_grid_abstraction(SizingGrid::Fine, 1);
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree fine_tree(def, fine);
  bool mapped = false;
  try {
    const SubsetMap subset = map_bounded_subset(cap0, fine_tree);
    mapped = true;
    // On the mapped branch every bounded action matches an exact cap0 action.
    for (std::size_t nr = 0; nr < cap0.size(); ++nr) {
      if (subset.node_map[nr] == SubsetMap::kNoNode || !cap0.node(nr).is_action())
        continue;
      const bs::tree::TreeNode& bounded_node = fine_tree.node(subset.node_map[nr]);
      std::set<int> matched;
      for (std::size_t ar = 0; ar < cap0.node(nr).actions.size(); ++ar) {
        const int ab = subset.edge_map[nr][ar];
        if (ab == -1) {
          const bool aggressive_edge =
              cap0.node(nr).actions[ar].type == bs::poker::ActionType::Bet ||
              cap0.node(nr).actions[ar].type == bs::poker::ActionType::Raise;
          CHECK(aggressive_edge);
        } else {
          CHECK(cap0.node(nr).actions[ar] == bounded_node.actions[static_cast<std::size_t>(ab)]);
          CHECK(matched.insert(ab).second);
        }
      }
      CHECK(matched.size() == bounded_node.actions.size());
    }
  } catch (const std::invalid_argument&) {
    mapped = false;  // reference-foreign target: union domain is the fallback.
  }
  const ActionAbstraction union_action =
      make_union_abstraction(ActionAbstraction::identity(0), fine);
  const AbstractTree union_tree(def, union_action);
  CHECK(union_tree.size() >= fine_tree.size());  // the union is never narrower.
  std::printf("D249 N4 fine cap0_direct_map=%s\n", mapped ? "mapped" : "rejected");
  return true;
}

// N5: a real Fine solve yields legal finite sigmas, the two averaging modes
// are distinct fresh policies, and the frozen reach-bound instrument returns
// finite bounded NC and pure menu terms on its appropriate reference domain.
bool test_n5_real_distinct_solve() {
  const bs::poker::GameDef def = shallow_river();
  const ActionAbstraction fine = make_grid_abstraction(SizingGrid::Fine, 1);
  const RiverSolveResult linear = solve_mode(def, fine, true);
  const RiverSolveResult uniform = solve_mode(def, fine, false);
  CHECK(linear.avg_sigma.size() == uniform.avg_sigma.size());
  for (std::size_t n = 0; n < linear.avg_sigma.size(); ++n) {
    CHECK(linear.avg_sigma[n].empty() == uniform.avg_sigma[n].empty());
    if (linear.avg_sigma[n].empty())
      continue;
    const std::size_t menu = linear.avg_sigma[n].size() / kC;
    for (std::uint32_t i = 0; i < kC; ++i) {
      double row = 0.0;
      for (std::size_t a = 0; a < menu; ++a) {
        const double x = linear.avg_sigma[n][i * menu + a];
        const double y = uniform.avg_sigma[n][i * menu + a];
        CHECK(std::isfinite(x) && x >= 0.0 && std::isfinite(y) && y >= 0.0);
        row += x;
      }
      CHECK(std::fabs(row - 1.0) <= 1e-12);
    }
  }
  const std::uint64_t h_lin = hash_average_snapshot(linear.avg_sigma);
  const std::uint64_t h_uni = hash_average_snapshot(uniform.avg_sigma);
  const double l1 = max_snapshot_l1(linear.avg_sigma, uniform.avg_sigma);
  CHECK(h_lin != h_uni);
  CHECK(std::isfinite(l1) && l1 >= kDistinctL1);

  const AbstractTree ref_tree(def, ActionAbstraction::identity(0));
  const AbstractTree bounded_tree(def, fine);
  bool finite_reach = false;
  try {
    const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
    const ReachBoundResult rb = reach_bound_from_snapshot(def, linear, ref_tree, bounded_tree,
                                                          subset, CapExtensionCompletion::Uniform);
    CHECK(std::isfinite(rb.bounded_nash_conv_bb) && std::isfinite(rb.exact_exploitability_bb));
    CHECK(std::isfinite(rb.exact_exploitability_bb - rb.bounded_nash_conv_bb));
    finite_reach = true;
  } catch (const std::invalid_argument&) {
    const ActionAbstraction union_action =
        make_union_abstraction(ActionAbstraction::identity(0), fine);
    const AbstractTree union_tree(def, union_action);
    const SubsetMap subset = map_bounded_subset(union_tree, bounded_tree);
    const ReachBoundResult rb = reach_bound_from_snapshot(def, linear, union_tree, bounded_tree,
                                                          subset, CapExtensionCompletion::Uniform);
    CHECK(std::isfinite(rb.bounded_nash_conv_bb) && std::isfinite(rb.exact_exploitability_bb));
    finite_reach = true;
  }
  CHECK(finite_reach);
  return true;
}

// N6: Fine has no historical bitwise anchor, so two independent in-process
// linear solves with fresh state must reproduce identical snapshots.
bool test_n6_linear_bitwise_repeat() {
  const bs::poker::GameDef def = shallow_river();
  const ActionAbstraction fine = make_grid_abstraction(SizingGrid::Fine, 1);
  const RiverSolveResult a = solve_mode(def, fine, true);
  const RiverSolveResult b = solve_mode(def, fine, true);
  CHECK(hash_average_snapshot(a.avg_sigma) == hash_average_snapshot(b.avg_sigma));
  CHECK(max_snapshot_l1(a.avg_sigma, b.avg_sigma) == 0.0);
  CHECK(a.nash_conv_bb == b.nash_conv_bb);
  return true;
}

}  // namespace

int main() {
  if (!test_n1_schedule_contents() || !test_n2_distinct_abstraction() ||
      !test_n3_determinism_footprint_and_limits() || !test_n4_embedding_verdict() ||
      !test_n5_real_distinct_solve() || !test_n6_linear_bitwise_repeat())
    return 1;
  std::printf("rfc0047_fine_grid: all checks passed\n");
  return 0;
}
