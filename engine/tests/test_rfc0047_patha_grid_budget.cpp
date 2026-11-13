// RFC 0048 Path A stage-2 (D179) native test: solve-free public-tree footprint
// census with the corrected U1/U2/U3 unit taxonomy and the embedding
// prefix-compatibility canary. No equilibrium is run; every fixture builds
// small public AbstractTree objects. Behavioral assertions only.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/cv_oracle.hpp"
#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/tree_budget.hpp"

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

// T1 cross-construction shape identity: identity(1) and declared(default,
// MinAndCap, 1) build the SAME tree though their AbstractionIds differ; compare
// shape digest/size/bytes, never action_id.
bool test_cross_construction_identity() {
  const bs::poker::GameDef def = w1_reached_river();
  const AbstractTree a(def, ActionAbstraction::identity(1));
  const AbstractTree b(def, make_grid_abstraction(SizingGrid::Identity, 1));
  CHECK(a.size() == b.size());
  CHECK(a.accounted_bytes() == b.accounted_bytes());
  CHECK(tree_shape_digest(a) == tree_shape_digest(b));
  return true;
}

bool test_build_determinism() {
  const bs::poker::GameDef def = w1_reached_river();
  const AbstractTree a(def, ActionAbstraction::identity(3));
  const AbstractTree b(def, ActionAbstraction::identity(3));
  CHECK(a.size() == b.size() && a.accounted_bytes() == b.accounted_bytes());
  CHECK(tree_shape_digest(a) == tree_shape_digest(b));
  return true;
}

// T3 weak sizing-grid ordering with at least one strict step (fraction sets are
// not nested; monotone-clamp refinement, no element-wise subset assertion).
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
  CHECK(coarse.total_nodes < wide.total_nodes);
  return true;
}

bool test_cap_monotonicity() {
  const bs::poker::GameDef def = w1_reached_river();
  std::size_t prev = 0;
  bool strict = false;
  for (std::uint8_t k = 1; k <= 6; ++k) {
    const GridBudget g =
        measure_grid_budget(def, "reached_river", ActionAbstraction::identity(k), "cap", k, "");
    CHECK(!g.resource_boundary);
    if (k > 1 && g.total_nodes > prev)
      strict = true;
    CHECK(g.total_nodes >= prev);
    prev = g.total_nodes;
  }
  CHECK(strict);
  const GridBudget uncapped =
      measure_grid_budget(def, "reached_river", ActionAbstraction::identity(0), "cap0", 0, "");
  CHECK(!uncapped.resource_boundary);
  CHECK(uncapped.total_nodes >= prev);
  return true;
}

// F5 meter calibration: on the w1 TURN root the U1 online-resident figure must
// equal tree_budget's conservative_online_sum, and U2 equals the analytic
// leaf x seat x combo count. U1/U2/U3 are non-equivalent by construction.
bool test_meter_calibration_and_units() {
  const StratumSpec& spec = *find_stratum("w1");
  const bs::poker::GameDef turn_def = make_stratum_def(spec, 1);
  const TreeBudget ref = measure_tree_budget(spec, 1);
  const GridBudget g =
      measure_grid_budget(turn_def, "turn", ActionAbstraction::identity(1), "turn_cap1", 1, "");
  CHECK(!g.resource_boundary);
  // U1 = turn trunk + largest single reached river subtree.
  CHECK(g.u1_turn_infosets == ref.turn_infosets);
  CHECK(g.u1_reached_river_infosets_max == ref.reached_river_infosets_max);
  CHECK(g.u1_online_resident_infosets == ref.conservative_sum);
  CHECK(g.u1_online_resident_infosets == 26461ull);  // pinned constant
  CHECK(g.u1_under_1m_infoset);                      // 26,461 << 1,000,000
  // U2 = 432 frontier leaves x 2 seats x 1081 combos (w1 has 9 chance x 48).
  CHECK(g.u2_artifact_rows == 432ull * 2ull * 1081ull);
  CHECK(g.u2_artifact_rows == 933984ull);
  CHECK(g.u2_artifact_bytes == g.u2_artifact_rows * sizeof(OracleRow));
  // U3 grand total strictly exceeds the online-resident U1 on a fanned trunk.
  CHECK(g.u3_build_grand_total_infosets > g.u1_online_resident_infosets);
  CHECK(g.u3_all_river_infosets > g.u1_reached_river_infosets_max);

  // Reached-river root: no chance fanout, so it IS the single subtree and U2 is
  // one leaf x 2 x 1081.
  const GridBudget r = measure_grid_budget(w1_reached_river(), "reached_river",
                                           ActionAbstraction::identity(1), "river_cap1", 1, "");
  CHECK(!r.resource_boundary);
  CHECK(r.chance_nodes == 0);
  CHECK(r.u1_turn_infosets == 0);
  CHECK(r.u1_reached_river_infosets_max == 10ull * 1081ull + 1081ull);  // 10 cap1 actions
  CHECK(r.u2_artifact_rows == 2ull * 1081ull);                          // 2,162
  return true;
}

// F5/F6 resource boundary: a tight custom limit forces the typed exhaustion;
// the boundary row keeps bytes==0 and EVERY envelope verdict false, so a
// placeholder can never be read as a feasible tree.
bool test_resource_boundary_pins() {
  const bs::poker::GameDef def = w1_reached_river();
  TreeLimits tight;
  tight.max_nodes = 100;
  tight.max_bytes = std::size_t{1} << 40;
  tight.max_depth = 256;
  const GridBudget boundary = measure_grid_budget(
      def, "reached_river", ActionAbstraction::identity(3), "bound", 3, "", tight);
  CHECK(boundary.resource_boundary);
  CHECK(!boundary.boundary_reason.empty());
  CHECK(boundary.accounted_bytes == 0);
  CHECK(boundary.total_nodes == 0 && boundary.action_nodes == 0);
  CHECK(!boundary.under_tree_node_limit && !boundary.under_byte_limit);
  CHECK(!boundary.u1_under_1m_infoset);
  CHECK(boundary.u1_online_resident_infosets == 0 && boundary.u2_artifact_rows == 0 &&
        boundary.u3_build_grand_total_infosets == 0);
  return true;
}

// F1 embedding prefix canary: only an identity-schedule bounded menu is an
// ordered prefix of identity(0). Declared coarse/wide grids produce targets the
// cap-0 reference does not contain (or exceed its menu size), so
// map_bounded_to_cap0 fails closed with invalid_argument. This pins that the
// frozen embedding identity cannot be applied to non-identity grids without a
// new union-reference formula (a separate addendum, not D179).
bool test_embedding_prefix_canary() {
  const bs::poker::GameDef def = w1_reached_river();
  const AbstractTree cap0(def, ActionAbstraction::identity(0));
  const AbstractTree grid_identity(def, make_grid_abstraction(SizingGrid::Identity, 1));
  const AbstractTree coarse(def, make_grid_abstraction(SizingGrid::Coarse, 1));
  const AbstractTree wide(def, make_grid_abstraction(SizingGrid::Wide, 1));
  // The identity grid is embeddable.
  const std::size_t ok_size = map_bounded_to_cap0(cap0, grid_identity).size();
  CHECK(ok_size == cap0.size());
  for (const AbstractTree* t : {&coarse, &wide}) {
    bool threw = false;
    try {
      (void)map_bounded_to_cap0(cap0, *t);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  return true;
}

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
      !test_cap_monotonicity() || !test_meter_calibration_and_units() ||
      !test_resource_boundary_pins() || !test_embedding_prefix_canary() ||
      !test_invalid_grid_fail_closed())
    return 1;
  std::printf("rfc0047_patha_grid_budget: all checks passed\n");
  return 0;
}
