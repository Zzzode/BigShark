// RFC 0048 Path A stage-1 (D176) native test: solve-free public-tree footprint
// census for wide raise caps and bet-sizing grids. No equilibrium is run; every
// fixture builds public AbstractTree objects on the small w1 reached-river
// root (the turn-rooted cap-0 1 GiB boundary is exercised only by the manual
// probe). Behavioral assertions only; no source-text matching.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/strata.hpp"

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
using bs::tree::tree_resource_exhausted;
using bs::tree::tree_shape_digest;
using bs::tree::TreeLimits;

namespace {

bs::poker::GameDef w1_reached_river() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  return leaves.front().river_def;
}

// T1: identity(1) and declared(default schedule, MinAndCap, 1) build the SAME
// tree even though their AbstractionIds legitimately differ (different id
// families). The tree builder never consults the id, so compare the shape
// digest, node count, and byte accounting - never action_id() (review F5).
bool test_cross_construction_identity() {
  const bs::poker::GameDef def = w1_reached_river();
  const AbstractTree a(def, ActionAbstraction::identity(1));
  const AbstractTree b(def, make_grid_abstraction(SizingGrid::Identity, 1));
  CHECK(a.size() == b.size());
  CHECK(a.accounted_bytes() == b.accounted_bytes());
  CHECK(tree_shape_digest(a) == tree_shape_digest(b));
  return true;
}

// T2: tree construction is deterministic - rebuilding the same abstraction
// yields identical node count, bytes, and shape digest.
bool test_build_determinism() {
  const bs::poker::GameDef def = w1_reached_river();
  const AbstractTree a(def, ActionAbstraction::identity(3));
  const AbstractTree b(def, ActionAbstraction::identity(3));
  CHECK(a.size() == b.size());
  CHECK(a.accounted_bytes() == b.accounted_bytes());
  CHECK(tree_shape_digest(a) == tree_shape_digest(b));
  return true;
}

// T3: sizing-grid node ordering is WEAK (coarse <= identity <= wide). The
// fraction sets are NOT nested (wide drops 1/3 and adds other sizes), so no
// element-wise menu-subset is asserted; the ordering follows from the monotone
// clamp of each pot fraction onto [min,cap] targets. At least one strict step
// is required so the fixture cannot pass vacuously under a swapped grid.
bool test_grid_ordering() {
  const bs::poker::GameDef def = w1_reached_river();
  const GridBudget coarse = measure_grid_budget(
      def, "reached_river", make_grid_abstraction(SizingGrid::Coarse, 1), "coarse", 1, "coarse");
  const GridBudget identity =
      measure_grid_budget(def, "reached_river", make_grid_abstraction(SizingGrid::Identity, 1),
                          "identity", 1, "identity");
  const GridBudget wide = measure_grid_budget(
      def, "reached_river", make_grid_abstraction(SizingGrid::Wide, 1), "wide", 1, "wide");
  CHECK(!coarse.resource_boundary && !identity.resource_boundary && !wide.resource_boundary);
  CHECK(coarse.total_nodes <= identity.total_nodes);
  CHECK(identity.total_nodes <= wide.total_nodes);
  CHECK(coarse.total_nodes < wide.total_nodes);  // non-vacuous on this anchor
  return true;
}

// T4: raise-cap monotonicity. identity(k) node count is non-decreasing in k,
// with the uncapped identity(0) (cap = +infinity) the largest; at least one
// strict step keeps it non-vacuous.
bool test_cap_monotonicity() {
  const bs::poker::GameDef def = w1_reached_river();
  std::size_t prev = 0;
  std::size_t first = 0;
  std::size_t last = 0;
  bool strict = false;
  for (std::uint8_t k = 1; k <= 6; ++k) {
    const GridBudget g =
        measure_grid_budget(def, "reached_river", ActionAbstraction::identity(k), "cap", k, "");
    CHECK(!g.resource_boundary);
    if (k == 1)
      first = g.total_nodes;
    if (k == 6)
      last = g.total_nodes;
    if (k > 1 && g.total_nodes > prev)
      strict = true;
    CHECK(g.total_nodes >= prev);
    prev = g.total_nodes;
  }
  CHECK(strict);
  const GridBudget uncapped =
      measure_grid_budget(def, "reached_river", ActionAbstraction::identity(0), "cap0", 0, "");
  CHECK(!uncapped.resource_boundary);  // river cap-0 is only 33,693 nodes
  CHECK(uncapped.total_nodes >= last);
  CHECK(first >= 27);  // sanity against the known cap-1 reached-river shape
  return true;
}

// T5: resource envelope and the typed boundary path. A small in-limit tree is
// feasible on all three dimensions; a TIGHT custom TreeLimits (100 nodes) forces
// a deterministic millisecond tree_resource_exhausted that measure_grid_budget
// maps to a boundary row (not a throw), with feasibility flags false.
bool test_resource_verdict_and_boundary() {
  const bs::poker::GameDef def = w1_reached_river();
  const GridBudget feasible =
      measure_grid_budget(def, "reached_river", ActionAbstraction::identity(1), "ok", 1, "");
  CHECK(!feasible.resource_boundary);
  CHECK(feasible.under_tree_node_limit && feasible.under_byte_limit && feasible.under_1m_infoset);
  CHECK(feasible.river_combos == 1081);
  CHECK(feasible.turn_combos == 0);  // a river-rooted tree has no turn action rows
  CHECK(feasible.river_infosets ==
        static_cast<std::uint64_t>(feasible.river_action_nodes) * 1081ull + 1081ull);
  CHECK(feasible.max_actions_per_node >= feasible.mean_actions_per_action_node);

  TreeLimits tight;
  tight.max_nodes = 100;
  tight.max_bytes = std::size_t{1} << 40;  // permissive; isolate the node cap
  tight.max_depth = 256;
  // identity(3) has 315 nodes, so it must cross the 100-node limit.
  const GridBudget boundary = measure_grid_budget(
      def, "reached_river", ActionAbstraction::identity(3), "bound", 3, "", tight);
  CHECK(boundary.resource_boundary);
  CHECK(!boundary.boundary_reason.empty());
  CHECK(!boundary.under_tree_node_limit);
  CHECK(boundary.total_nodes == 0 && boundary.action_nodes == 0);

  // The same tight limit surfaces the typed exception on a direct build too.
  bool typed = false;
  bool other = false;
  try {
    AbstractTree t(def, ActionAbstraction::identity(3), tight);
    (void)t;
  } catch (const tree_resource_exhausted&) {
    typed = true;
  } catch (const std::invalid_argument&) {
    other = true;
  }
  CHECK(typed && !other);
  return true;
}

// T6: an invalid fraction (zero denominator) fails closed at abstraction
// construction via reduced(), before any tree is built.
bool test_invalid_grid_fail_closed() {
  bs::abstraction::SizeSchedule schedule = bs::abstraction::default_size_schedule();
  schedule[static_cast<std::size_t>(bs::poker::Street::River)].bets = {
      bs::abstraction::Fraction{1, 0}};
  bool threw = false;
  try {
    (void)bs::abstraction::ActionAbstraction::declared(schedule,
                                                       bs::abstraction::CoverSeeds::MinAndCap, 1);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

}  // namespace

int main() {
  if (!test_cross_construction_identity() || !test_build_determinism() || !test_grid_ordering() ||
      !test_cap_monotonicity() || !test_resource_verdict_and_boundary() ||
      !test_invalid_grid_fail_closed())
    return 1;
  std::printf("rfc0047_patha_grid_budget: all checks passed\n");
  return 0;
}
