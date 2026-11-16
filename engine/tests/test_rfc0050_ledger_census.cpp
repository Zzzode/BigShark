// RFC 0050 Stage 0 Step 0a native test: the eleven-ledger cap-0 public-tree
// census and the hard-coded six-anchor pre-registration. Behavioral
// assertions through the real tree builder, no source-text matching:
// frontier ledger constants, cap-0 census structure and determinism,
// card-independence of the action shape inside a ledger, the measured
// ascending order, and the R2-1 consistency gate pinning the hard-coded
// six-anchor list to that measured order.
#include <bs/abstraction.hpp>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/ledger_census.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/rfc0050_anchors.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
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
using bs::tree::TreeLimits;

namespace {

bool test_frontier_ledger_constants() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  CHECK(anchors.size() == kRfc0050LedgerCount);
  std::size_t limped = 0;
  std::size_t srp = 0;
  std::size_t limped_leaves = 0;
  std::size_t srp_leaves = 0;
  std::set<LedgerKey> limped_keys;
  std::set<LedgerKey> srp_keys;
  for (const LedgerAnchor& a : anchors) {
    CHECK(a.leaf_count > 0);
    // The (pot, contributions) tuple is unique WITHIN a family; the same
    // betting pot can arise from both turn roots, so the anchor identity is
    // the composite (family, key), never the key alone.
    if (a.family == "limped") {
      CHECK(limped_keys.insert(a.key).second);
      CHECK(a.family_index == limped);
      ++limped;
      limped_leaves += a.leaf_count;
    } else {
      CHECK(a.family == "srp");
      CHECK(srp_keys.insert(a.key).second);
      CHECK(a.family_index == srp);
      ++srp;
      srp_leaves += a.leaf_count;
    }
  }
  CHECK(limped == kRfc0050LimpedLedgerCount);
  CHECK(srp == kRfc0050SrpLedgerCount);
  CHECK(limped_leaves == kRfc0050LimpedFrontierLeaves);  // 432 = 9 lines x 48
  CHECK(srp_leaves == kRfc0050SrpFrontierLeaves);        // 528 over 6 ledgers
  // Every representative is a real five-card river-rooted frontier leaf.
  for (const LedgerAnchor& a : anchors) {
    CHECK(a.representative.river_def.board_size == 5);
    CHECK(a.representative.river_def.player_count == 2);
    CHECK(a.representative.river_def.terminal == bs::poker::TerminalDepth::River);
  }
  return true;
}

bool test_cap0_census_structure() {
  const std::vector<LedgerCensusRow> rows = census_eleven_cap0_trees();
  CHECK(rows.size() == kRfc0050LedgerCount);
  const ActionAbstraction bounded = ActionAbstraction::identity(1);
  bool any_exact_menu_wider = false;
  std::size_t terminal_all_in = 0;
  std::size_t decision_bearing = 0;
  for (const LedgerCensusRow& r : rows) {
    // Every frozen ledger fits the 25M-node / 8 GiB envelope; a non-fitting
    // row would be a typed census_rejected, not an exception.
    CHECK(r.census.fits);
    CHECK(r.census.total_nodes > 0);
    CHECK(r.census.projected_infosets == r.census.action_nodes * kRiverC);
    CHECK(r.census.max_menu_width <= kOfflinePackMaxMenuActions);
    CHECK(r.census.menu_within_pack_limit);
    if (r.census.action_nodes == 0) {
      // Terminal all-in root: both stacks are fully committed at the river,
      // so the cap-0 tree is a single node with no menu and no solve.
      CHECK(r.census.max_menu_width == 0);
      CHECK(r.census.total_nodes == 1);
      CHECK(r.census.depth == 1);  // root-only tree depth convention
      ++terminal_all_in;
      continue;
    }
    ++decision_bearing;
    CHECK(r.census.max_menu_width > 0);
    CHECK(r.census.depth > 0);
    // The exact cap-0 menu must be at least as wide somewhere as the bounded
    // cap-1 menu on the same representative leaf.
    const OfflineCensusRow bounded_row =
        offline_census(r.anchor.representative.river_def, bounded, "bounded-check", "control", 1);
    CHECK(bounded_row.fits);
    if (r.census.max_menu_width > bounded_row.max_menu_width)
      any_exact_menu_wider = true;
  }
  CHECK(terminal_all_in == 2);  // one all-in ledger per turn root
  CHECK(decision_bearing == kRfc0050LedgerCount - 2);
  CHECK(any_exact_menu_wider);  // cap-0 genuinely adds menu edges
  return true;
}

// Action-tree shape is card-independent: two frontier leaves in the same
// ledger (different river cards, identical betting shape) must produce the
// same cap-0 structural tuple even though their boards differ.
bool test_card_independence_within_ledger() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const ActionAbstraction exact = ActionAbstraction::identity(0);
  for (const StratumSpec* spec :
       std::array<const StratumSpec*, 2>{find_stratum("w1"), find_stratum("raised-srp")}) {
    const std::string expected_family = std::string(spec->name) == "raised-srp" ? "srp" : "limped";
    const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);
    std::set<LedgerKey> checked;
    for (const LedgerAnchor& a : anchors) {
      if (a.family != expected_family || !checked.insert(a.key).second)
        continue;
      std::vector<FrontierLeaf> same_ledger;
      for (const FrontierLeaf& leaf : leaves) {
        const LedgerKey key{static_cast<bs::poker::Chips>(leaf.river_def.pot),
                            static_cast<bs::poker::Chips>(leaf.river_def.contributions[0]),
                            static_cast<bs::poker::Chips>(leaf.river_def.contributions[1])};
        if (key == a.key)
          same_ledger.push_back(leaf);
      }
      CHECK(same_ledger.size() >= 2);  // 48 cards per betting line group
      const OfflineCensusRow first =
          offline_census(same_ledger.front().river_def, exact, "card-a", a.family, 0);
      const OfflineCensusRow second =
          offline_census(same_ledger.back().river_def, exact, "card-b", a.family, 0);
      CHECK(first.fits && second.fits);
      CHECK(first.action_nodes == second.action_nodes);
      CHECK(first.total_nodes == second.total_nodes);
      CHECK(first.max_menu_width == second.max_menu_width);
      CHECK(first.depth == second.depth);
    }
  }
  return true;
}

bool test_ascending_order_and_r2_1_anchor_gate() {
  const std::vector<LedgerCensusRow> rows = census_eleven_cap0_trees();
  const std::vector<std::size_t> order = census_ascending_order(rows);
  CHECK(order.size() == rows.size());
  std::set<std::size_t> permutation;
  for (std::size_t idx : order)
    CHECK(permutation.insert(idx).second);
  for (std::size_t rank = 1; rank < order.size(); ++rank) {
    const CensusTuple prev = census_tuple(rows[order[rank - 1]]);
    const CensusTuple cur = census_tuple(rows[order[rank]]);
    // Lexicographic non-decreasing order on the measured tuple.
    const bool ordered = (prev.action_nodes < cur.action_nodes) ||
                         (prev.action_nodes == cur.action_nodes &&
                          (prev.projected_infosets < cur.projected_infosets ||
                           (prev.projected_infosets == cur.projected_infosets &&
                            prev.max_menu_width <= cur.max_menu_width)));
    CHECK(ordered);
  }

  // Amendment 1: the terminal all-in rows sort first on zero action nodes
  // but are not anchor-eligible; exactly nine decision-bearing ledgers
  // remain, and the two excluded rows are the (400, 200, 200) all-in roots.
  const std::vector<std::size_t> eligible = decision_bearing_ascending_order(rows);
  CHECK(eligible.size() == kRfc0050LedgerCount - 2);
  std::size_t terminal = 0;
  for (const LedgerCensusRow& r : rows) {
    if (!is_decision_bearing(r)) {
      CHECK(r.census.action_nodes == 0);
      CHECK((r.anchor.key == LedgerKey{400, 200, 200}));
      ++terminal;
    }
  }
  CHECK(terminal == 2);
  // D292 explicit exclusion guard: every non-eligible row must be absent
  // from the eligible ordering, and no terminal all-in (family, key) may
  // appear in the hard-coded anchor set. The predicate itself gets a
  // guard, not just the happy-path ordering.
  std::set<std::size_t> eligible_set(eligible.begin(), eligible.end());
  std::set<std::pair<std::string, LedgerKey>> terminal_keys;
  for (std::size_t idx = 0; idx < rows.size(); ++idx) {
    if (!is_decision_bearing(rows[idx])) {
      CHECK(!eligible_set.contains(idx));
      terminal_keys.emplace(rows[idx].anchor.family, rows[idx].anchor.key);
    }
  }
  CHECK(terminal_keys.size() == 2);

  // R2-1 consistency gate: the hard-coded six-anchor pre-registration must
  // equal the first six ELIGIBLE measured ledgers in order, by family AND
  // composite ledger key. Review nit 1: also pin the sealed absolute
  // action-node counts from the Step 0a receipt, so a builder drift that
  // coincidentally re-transcribes a wrong order cannot pass silently.
  static constexpr std::array<std::size_t, kRfc0050AnchorCount> kSealedActionNodes{
      1600, 2632, 3344, 3984, 4880, 5720};
  const auto& hard_coded = rfc0050_six_anchors();
  CHECK(hard_coded.size() == kRfc0050AnchorCount);
  for (std::size_t i = 0; i < kRfc0050AnchorCount; ++i) {
    const LedgerAnchor& measured = rows[eligible[i]].anchor;
    CHECK(hard_coded[i].key == measured.key);
    CHECK(std::string(hard_coded[i].family) == measured.family);
    CHECK(is_decision_bearing(rows[eligible[i]]));
    CHECK(rows[eligible[i]].census.action_nodes == kSealedActionNodes[i]);
  }
  // Sealed T6 cross-validation anchors: the 7,824-action tie pair and the
  // 11,232-action w1 root must exist with exactly those counts.
  std::size_t seven_k = 0;
  std::size_t w1_found = 0;
  for (const LedgerCensusRow& r : rows) {
    if (r.census.action_nodes == 7824 && r.anchor.key == LedgerKey{8, 4, 4})
      ++seven_k;
    if (r.census.action_nodes == 11232 && r.anchor.family == "limped" &&
        r.anchor.key == LedgerKey{4, 2, 2})
      ++w1_found;
  }
  CHECK(seven_k == 2);  // one per turn root
  CHECK(w1_found == 1);
  // Every hard-coded anchor is unique on the composite (family, key).
  std::set<std::pair<std::string, LedgerKey>> anchor_keys;
  for (const AnchoredLedger& a : hard_coded) {
    const bool inserted = anchor_keys.emplace(a.family, a.key).second;
    CHECK(inserted);
    // The all-in rows must never reach the anchor pre-registration.
    CHECK(!terminal_keys.contains({std::string(a.family), a.key}));
  }
  return true;
}

bool test_census_deterministic() {
  const std::vector<LedgerCensusRow> a = census_eleven_cap0_trees();
  const std::vector<LedgerCensusRow> b = census_eleven_cap0_trees();
  CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].anchor.key == b[i].anchor.key);
    CHECK(a[i].anchor.family == b[i].anchor.family);  // review nit 2
    CHECK(a[i].anchor.family_index == b[i].anchor.family_index);
    CHECK(a[i].anchor.leaf_count == b[i].anchor.leaf_count);
    CHECK(a[i].census.action_nodes == b[i].census.action_nodes);
    CHECK(a[i].census.total_nodes == b[i].census.total_nodes);
    CHECK(a[i].census.max_menu_width == b[i].census.max_menu_width);
    CHECK(a[i].census.accounted_bytes == b[i].census.accounted_bytes);
  }
  CHECK(census_ascending_order(a) == census_ascending_order(b));
  return true;
}

// A cap-0 tree that crosses an injected envelope comes back as a typed
// census_rejected row, the same contract Step 0b relies on for its memory
// NO-GO typing.
bool test_injected_limit_typed_rejection() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const ActionAbstraction exact = ActionAbstraction::identity(0);
  const TreeLimits tiny{10, 4, 4096};
  const OfflineCensusRow row = offline_census(anchors.front().representative.river_def, exact, tiny,
                                              "tiny-cap0", "limped", 0);
  CHECK(!row.fits);
  CHECK(row.action_nodes == 0);
  CHECK(row.total_nodes == 0);
  return true;
}

}  // namespace

int main() {
  if (!test_frontier_ledger_constants() || !test_cap0_census_structure() ||
      !test_card_independence_within_ledger() || !test_ascending_order_and_r2_1_anchor_gate() ||
      !test_census_deterministic() || !test_injected_limit_typed_rejection())
    return 1;
  std::printf("rfc0050_ledger_census: all checks passed\n");
  return 0;
}
