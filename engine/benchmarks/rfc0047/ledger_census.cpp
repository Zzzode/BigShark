#include "ledger_census.hpp"

#include <algorithm>
#include <bs/abstraction.hpp>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace bs::rfc0047 {

using bs::abstraction::ActionAbstraction;

namespace {

// Fold one frozen turn root's cap-1 frontier into its distinct betting
// ledgers, preserving DFS first-seen order. The ledger identity is the
// (pot, contribution0, contribution1) tuple rfc0047_frontier_check uses.
std::vector<LedgerAnchor> ledgers_of_stratum(const StratumSpec& spec, std::string_view family) {
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  std::map<LedgerKey, std::size_t> index_by_key;
  std::vector<LedgerAnchor> out;
  for (const FrontierLeaf& leaf : leaves) {
    const LedgerKey key{static_cast<bs::poker::Chips>(leaf.river_def.pot),
                        static_cast<bs::poker::Chips>(leaf.river_def.contributions[0]),
                        static_cast<bs::poker::Chips>(leaf.river_def.contributions[1])};
    if (auto found = index_by_key.find(key); found != index_by_key.end()) {
      ++out[found->second].leaf_count;
      continue;
    }
    LedgerAnchor anchor;
    anchor.family = std::string(family);
    anchor.family_index = out.size();
    anchor.key = key;
    anchor.representative = leaf;
    anchor.leaf_count = 1;
    index_by_key.emplace(key, out.size());
    out.push_back(std::move(anchor));
  }
  return out;
}

}  // namespace

std::vector<LedgerAnchor> enumerate_ledger_anchors() {
  const StratumSpec* limped = find_stratum("w1");
  const StratumSpec* srp = find_stratum("raised-srp");
  if (limped == nullptr || srp == nullptr)
    throw std::runtime_error("rfc0050: frozen strata w1/raised-srp missing");
  std::vector<LedgerAnchor> out = ledgers_of_stratum(*limped, "limped");
  if (out.size() != kRfc0050LimpedLedgerCount)
    throw std::runtime_error("rfc0050: limped frontier must yield five ledgers");
  const std::size_t limped_leaves =
      std::accumulate(out.begin(), out.end(), std::size_t{0},
                      [](std::size_t n, const LedgerAnchor& a) { return n + a.leaf_count; });
  if (limped_leaves != kRfc0050LimpedFrontierLeaves)
    throw std::runtime_error("rfc0050: limped frontier must yield 432 leaves");

  std::vector<LedgerAnchor> raised = ledgers_of_stratum(*srp, "srp");
  if (raised.size() != kRfc0050SrpLedgerCount)
    throw std::runtime_error("rfc0050: SRP frontier must yield six ledgers");
  const std::size_t srp_leaves =
      std::accumulate(raised.begin(), raised.end(), std::size_t{0},
                      [](std::size_t n, const LedgerAnchor& a) { return n + a.leaf_count; });
  if (srp_leaves != kRfc0050SrpFrontierLeaves)
    throw std::runtime_error("rfc0050: SRP frontier must yield 528 leaves");

  out.insert(out.end(), std::make_move_iterator(raised.begin()),
             std::make_move_iterator(raised.end()));
  if (out.size() != kRfc0050LedgerCount)
    throw std::runtime_error("rfc0050: eleven distinct ledgers required");
  return out;
}

std::vector<LedgerCensusRow> census_eleven_cap0_trees() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const ActionAbstraction exact_menu = ActionAbstraction::identity(0);
  std::vector<LedgerCensusRow> rows;
  rows.reserve(anchors.size());
  for (const LedgerAnchor& anchor : anchors) {
    const std::string label = anchor.family + "-ledger-" + std::to_string(anchor.family_index);
    LedgerCensusRow row;
    row.anchor = anchor;
    row.census = offline_census(anchor.representative.river_def, exact_menu, label, anchor.family,
                                /*raise_cap=*/0);
    rows.push_back(std::move(row));
  }
  return rows;
}

CensusTuple census_tuple(const LedgerCensusRow& row) {
  return CensusTuple{row.census.action_nodes, row.census.projected_infosets,
                     row.census.max_menu_width};
}

std::vector<std::size_t> census_ascending_order(const std::vector<LedgerCensusRow>& rows) {
  std::vector<std::size_t> order(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i)
    order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const CensusTuple ta = census_tuple(rows[a]);
    const CensusTuple tb = census_tuple(rows[b]);
    if (ta.action_nodes != tb.action_nodes)
      return ta.action_nodes < tb.action_nodes;
    if (ta.projected_infosets != tb.projected_infosets)
      return ta.projected_infosets < tb.projected_infosets;
    return ta.max_menu_width < tb.max_menu_width;
  });
  return order;
}

bool is_decision_bearing(const LedgerCensusRow& row) {
  return row.census.fits && row.census.action_nodes > 0;
}

std::vector<std::size_t> decision_bearing_ascending_order(
    const std::vector<LedgerCensusRow>& rows) {
  std::vector<std::size_t> out;
  for (std::size_t idx : census_ascending_order(rows)) {
    if (is_decision_bearing(rows[idx]))
      out.push_back(idx);
  }
  return out;
}

}  // namespace bs::rfc0047
