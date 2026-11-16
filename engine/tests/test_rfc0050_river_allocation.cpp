// RFC 0050 Stage 0 Step 0b native test: the exact-menu construction-only
// RiverGame allocation gate. Behavioral assertions through the real
// kernel, no source-text matching: a fully itemized allocation on the
// hard-coded smallest decision-bearing anchor with byte totals
// cross-checked against an independent tree walk, a typed memory NO-GO
// on an injected envelope, and itemization determinism across repeated
// constructions. Zero CFR iterations run, so no NashConv field exists.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "../benchmarks/rfc0047/ledger_census.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/rfc0050_anchors.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/turn_solver.hpp"

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
using bs::tree::TreeLimits;

namespace {

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rank-0 anchor missing");
}

RiverSolveConfig allocation_config(const bs::poker::GameDef& def) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = 0;
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = ActionAbstraction::identity(0);
  cfg.tree_limits = offline_census_limits();
  return cfg;
}

bool test_smallest_anchor_allocates_with_itemized_bytes() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  // The probe target must be the measured-smallest decision-bearing ledger.
  const std::vector<LedgerCensusRow> rows = census_eleven_cap0_trees();
  const std::vector<std::size_t> eligible = decision_bearing_ascending_order(rows);
  CHECK(&rows[eligible.front()].anchor == &anchor ||
        (rows[eligible.front()].anchor.family == anchor.family &&
         rows[eligible.front()].anchor.key == anchor.key));
  CHECK(rows[eligible.front()].census.action_nodes == 1600);  // sealed Step 0a count

  const RiverAllocationReceipt r =
      probe_river_allocation(allocation_config(anchor.representative.river_def));
  CHECK(r.allocated);
  CHECK(r.failure == "none");
  CHECK(r.action_nodes == 1600);
  CHECK(r.total_nodes == rows[eligible.front()].census.total_nodes);
  CHECK(r.total_nodes == 4797);  // sealed Step 0a count

  // Independent reference: sum every action node's menu width by walking a
  // freshly built tree; regret and average-sum each hold one kC*menu double
  // vector per action node.
  const AbstractTree ref(anchor.representative.river_def, ActionAbstraction::identity(0),
                         offline_census_limits());
  std::size_t menu_slots = 0;
  for (const auto& node : ref.nodes())
    if (node.is_action())
      menu_slots += node.actions.size();
  const std::size_t expected_table_bytes = menu_slots * kRiverC * sizeof(double);
  CHECK(r.regret_bytes == expected_table_bytes);
  CHECK(r.average_sum_bytes == expected_table_bytes);
  CHECK(r.regret_bytes == r.average_sum_bytes);

  // Per-node two-seat reach and one value vector, kC doubles each.
  CHECK(r.reach_bytes == r.total_nodes * 2 * kRiverC * sizeof(double));
  CHECK(r.value_bytes == r.total_nodes * kRiverC * sizeof(double));
  // No iteration ran, so the lazy average snapshot must not exist.
  CHECK(r.sigma_snapshot_bytes == 0);
  CHECK(r.showdown_matrix_bytes == kRiverC * kRiverC * sizeof(std::int8_t));
  CHECK(r.tree_accounted_bytes > 0);
  CHECK(r.scratch_bytes > 0);
  CHECK(r.bookkeeping_bytes > 0);

  const std::size_t summed = r.tree_accounted_bytes + r.showdown_matrix_bytes + r.regret_bytes +
                             r.average_sum_bytes + r.reach_bytes + r.value_bytes +
                             r.sigma_snapshot_bytes + r.scratch_bytes + r.bookkeeping_bytes;
  CHECK(r.itemized_total_bytes == summed);
  // The campaign gate: the itemized exact game must sit inside 8 GiB with
  // real headroom, and the process peak RSS must be measurable.
  CHECK(r.itemized_total_bytes < kOfflineCensusMaxBytes);
  CHECK(r.peak_rss_kb > 0);
  CHECK(r.construction_wall_ms >= 0.0);
  std::printf("D0050-0b exact allocation: itemized=%zu bytes peak_rss=%ld KB wall=%.2f ms\n",
              r.itemized_total_bytes, r.peak_rss_kb, r.construction_wall_ms);
  return true;
}

bool test_typed_memory_no_go_on_injected_limit() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  RiverSolveConfig cfg = allocation_config(anchors.front().representative.river_def);
  cfg.tree_limits = TreeLimits{10, 4, 4096};  // cannot hold any real river tree
  const RiverAllocationReceipt r = probe_river_allocation(cfg);
  CHECK(!r.allocated);
  CHECK(r.failure == "tree_resource_exhausted");
  CHECK(r.action_nodes == 0);
  CHECK(r.total_nodes == 0);
  CHECK(r.itemized_total_bytes == 0);
  CHECK(r.regret_bytes == 0 && r.average_sum_bytes == 0 && r.reach_bytes == 0);
  return true;
}

bool test_allocation_itemization_deterministic() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const RiverAllocationReceipt a =
      probe_river_allocation(allocation_config(anchor.representative.river_def));
  const RiverAllocationReceipt b =
      probe_river_allocation(allocation_config(anchor.representative.river_def));
  CHECK(a.allocated && b.allocated);
  CHECK(a.itemized_total_bytes == b.itemized_total_bytes);
  CHECK(a.regret_bytes == b.regret_bytes);
  CHECK(a.average_sum_bytes == b.average_sum_bytes);
  CHECK(a.reach_bytes == b.reach_bytes);
  CHECK(a.value_bytes == b.value_bytes);
  CHECK(a.tree_accounted_bytes == b.tree_accounted_bytes);
  CHECK(a.action_nodes == b.action_nodes);
  CHECK(a.total_nodes == b.total_nodes);
  return true;
}

}  // namespace

int main() {
  if (!test_smallest_anchor_allocates_with_itemized_bytes() ||
      !test_typed_memory_no_go_on_injected_limit() || !test_allocation_itemization_deterministic())
    return 1;
  std::printf("rfc0050_river_allocation: all checks passed\n");
  return 0;
}
