// RFC 0050 Stage 0 Step 0a manual single-instance eleven-ledger cap-0
// public-tree census probe (NOT a CTest). Enumerates the eleven distinct
// betting ledgers of the frozen limped and SRP turn roots, builds each
// ledger's exact cap-0 AbstractTree under the bench offline envelope, and
// prints one structural census row per ledger without solving, followed by
// the deterministic measured ascending order that names the hard-coded
// six-anchor pre-registration. Envelope constants and the cap-0 menu are
// hard-coded; there are no argv overrides. Tree-only work, so no wall
// tripwire applies.
//
// Usage: bigshark-rfc0050-ledger-census-probe
#include <cstdio>
#include <string>

#include "ledger_census.hpp"
#include "rfc0050_anchors.hpp"

using namespace bs::rfc0047;

namespace {

void print_ledger_enumeration(const std::vector<LedgerAnchor>& anchors) {
  std::printf("# ledger enumeration: %zu distinct ledgers\n", anchors.size());
  std::printf("ordinal,family,family_index,pot,contribution0,contribution1,leaf_count\n");
  for (std::size_t i = 0; i < anchors.size(); ++i) {
    const LedgerAnchor& a = anchors[i];
    std::printf("%zu,%s,%zu,%llu,%llu,%llu,%zu\n", i, a.family.c_str(), a.family_index,
                static_cast<unsigned long long>(a.key.pot),
                static_cast<unsigned long long>(a.key.contribution0),
                static_cast<unsigned long long>(a.key.contribution1), a.leaf_count);
  }
}

void print_cap0_census(const std::vector<LedgerCensusRow>& rows) {
  std::printf(
      "# cap-0 public-tree census (no solve), offline envelope %zu nodes / %zu depth / %zu bytes\n",
      kOfflineCensusMaxNodes, kOfflineCensusMaxDepth, kOfflineCensusMaxBytes);
  std::printf(
      "label,family,family_index,pot,contribution0,contribution1,status,total_nodes,action_nodes,"
      "max_menu_width,menu_within_pack_limit,depth,projected_infosets,accounted_bytes\n");
  for (const LedgerCensusRow& r : rows) {
    const LedgerAnchor& a = r.anchor;
    const OfflineCensusRow& c = r.census;
    std::printf("%s,%s,%zu,%llu,%llu,%llu,%s,%zu,%zu,%zu,%d,%zu,%zu,%zu\n", c.label.c_str(),
                a.family.c_str(), a.family_index, static_cast<unsigned long long>(a.key.pot),
                static_cast<unsigned long long>(a.key.contribution0),
                static_cast<unsigned long long>(a.key.contribution1),
                c.fits ? "fits" : "census_rejected", c.total_nodes, c.action_nodes,
                c.max_menu_width, c.menu_within_pack_limit ? 1 : 0, c.depth, c.projected_infosets,
                c.accounted_bytes);
  }
}

void print_measured_order(const std::vector<LedgerCensusRow>& rows,
                          const std::vector<std::size_t>& order) {
  std::printf("# measured ascending (action_nodes, projected_infosets, max_menu_width) order\n");
  std::printf(
      "rank,ordinal,family,family_index,pot,contribution0,contribution1,action_nodes,"
      "projected_infosets,max_menu_width,eligible,status\n");
  for (std::size_t rank = 0; rank < order.size(); ++rank) {
    const LedgerCensusRow& r = rows[order[rank]];
    const LedgerAnchor& a = r.anchor;
    std::printf("%zu,%zu,%s,%zu,%llu,%llu,%llu,%zu,%zu,%zu,%d,%s\n", rank, order[rank],
                a.family.c_str(), a.family_index, static_cast<unsigned long long>(a.key.pot),
                static_cast<unsigned long long>(a.key.contribution0),
                static_cast<unsigned long long>(a.key.contribution1), r.census.action_nodes,
                r.census.projected_infosets, r.census.max_menu_width,
                is_decision_bearing(r) ? 1 : 0, r.census.fits ? "fits" : "census_rejected");
  }
  const std::vector<std::size_t> eligible = decision_bearing_ascending_order(rows);
  std::printf(
      "# decision-bearing ledgers: %zu; hard-coded six anchors must equal eligible "
      "ranks 0..5\n",
      eligible.size());
  const auto& anchors = rfc0050_six_anchors();
  for (std::size_t i = 0; i < anchors.size(); ++i) {
    const LedgerAnchor& measured = rows[eligible[i]].anchor;
    const bool match =
        anchors[i].key == measured.key && std::string(anchors[i].family) == measured.family;
    std::printf("anchor%zu,%s,pot=%llu,contribution0=%llu,contribution1=%llu,match=%d\n", i,
                anchors[i].family, static_cast<unsigned long long>(anchors[i].key.pot),
                static_cast<unsigned long long>(anchors[i].key.contribution0),
                static_cast<unsigned long long>(anchors[i].key.contribution1), match ? 1 : 0);
  }
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  if (anchors.size() != kRfc0050LedgerCount) {
    std::fprintf(stderr, "expected %zu ledgers, got %zu\n",
                 static_cast<std::size_t>(kRfc0050LedgerCount), anchors.size());
    return 2;
  }
  print_ledger_enumeration(anchors);

  const std::vector<LedgerCensusRow> rows = census_eleven_cap0_trees();
  if (rows.size() != kRfc0050LedgerCount) {
    std::fprintf(stderr, "expected %zu census rows, got %zu\n",
                 static_cast<std::size_t>(kRfc0050LedgerCount), rows.size());
    return 3;
  }
  print_cap0_census(rows);
  print_measured_order(rows, census_ascending_order(rows));
  return 0;
}
