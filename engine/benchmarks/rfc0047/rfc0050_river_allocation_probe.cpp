// RFC 0050 Stage 0 Step 0b manual single-instance exact RiverGame
// allocation probe (NOT a CTest). Takes the hard-coded smallest
// decision-bearing anchor (rank 0 of the six-anchor pre-registration,
// derived from the sealed Step 0a census), constructs the full exact
// cap-0 RiverGame under the frozen offline TreeLimits with ZERO CFR
// iterations, and prints one itemized allocation row. A construction
// failure is printed as a typed memory NO-GO; there are no argv
// overrides and no anchor selection at run time.
//
// Usage: bigshark-rfc0050-river-allocation-probe
#include <cstdio>
#include <stdexcept>
#include <string>

#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "rfc0050_anchors.hpp"
#include "river_solver.hpp"

using namespace bs::rfc0047;

namespace {

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors) {
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  }
  throw std::runtime_error("rfc0050 Step 0b: hard-coded rank-0 anchor missing from census");
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);

  RiverSolveConfig cfg;
  cfg.def = anchor.representative.river_def;
  cfg.iterations = 0;  // construction only; no CFR traversal, no NashConv
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = bs::abstraction::ActionAbstraction::identity(0);
  cfg.tree_limits = offline_census_limits();

  const RiverAllocationReceipt r = probe_river_allocation(cfg);

  std::printf(
      "family,pot,contribution0,contribution1,status,failure,action_nodes,total_nodes,"
      "tree_accounted_bytes,showdown_matrix_bytes,regret_bytes,average_sum_bytes,reach_bytes,"
      "value_bytes,sigma_snapshot_bytes,scratch_bytes,bookkeeping_bytes,itemized_total_bytes,"
      "envelope_bytes,envelope_headroom_x,peak_rss_kb,construction_wall_ms\n");
  const long long headroom =
      r.allocated && r.itemized_total_bytes > 0
          ? static_cast<long long>(kOfflineCensusMaxBytes / r.itemized_total_bytes)
          : -1;
  std::printf(
      "%s,%llu,%llu,%llu,%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%lld,%ld,%.6f\n",
      anchor.family.c_str(), static_cast<unsigned long long>(anchor.key.pot),
      static_cast<unsigned long long>(anchor.key.contribution0),
      static_cast<unsigned long long>(anchor.key.contribution1),
      r.allocated ? "allocated" : "memory_no_go", r.failure.c_str(), r.action_nodes, r.total_nodes,
      r.tree_accounted_bytes, r.showdown_matrix_bytes, r.regret_bytes, r.average_sum_bytes,
      r.reach_bytes, r.value_bytes, r.sigma_snapshot_bytes, r.scratch_bytes, r.bookkeeping_bytes,
      r.itemized_total_bytes, static_cast<std::size_t>(kOfflineCensusMaxBytes), headroom,
      r.peak_rss_kb, r.construction_wall_ms);
  return r.allocated ? 0 : 4;  // typed NO-GO is a distinct non-zero RC
}
