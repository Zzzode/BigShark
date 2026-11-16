// RFC 0050 Stage 0 Step 0a: eleven-ledger cap-0 public-tree census.
//
// Bench-only, tree-building measurement (no solve). Enumerates the distinct
// betting ledgers of the two frozen turn roots — five limped ledgers from the
// w1 root and six single-raised-pot (SRP) ledgers from the raised-srp root —
// builds each ledger's exact cap-0 AbstractTree under the frozen RFC 0049
// offline envelope, and records action/total node counts, projected infosets,
// maximum per-node menu width, depth, and accounted bytes. The measured
// ascending order names the hard-coded six-anchor pre-registration that
// Stage 1/2 consume; nothing about that order is selected at run time.
//
// This component links only bigshark_rfc0047_bench and is unreachable from
// production composition roots by the rfc0047 link guard.
#pragma once

#include <array>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frontier_enumerator.hpp"
#include "offline_census.hpp"

namespace bs::rfc0047 {

// Frozen frontier constants pinned by the native frontier enumerator: the
// limped root has 432 post-chance leaves over five distinct ledgers and the
// raised root has 528 leaves over six distinct SRP ledgers.
inline constexpr std::size_t kRfc0050LimpedFrontierLeaves = 432;
inline constexpr std::size_t kRfc0050SrpFrontierLeaves = 528;
inline constexpr std::size_t kRfc0050LimpedLedgerCount = 5;
inline constexpr std::size_t kRfc0050SrpLedgerCount = 6;
inline constexpr std::size_t kRfc0050LedgerCount =
    kRfc0050LimpedLedgerCount + kRfc0050SrpLedgerCount;  // 11
inline constexpr std::size_t kRfc0050AnchorCount = 6;

// Distinct (pot, contribution0, contribution1) betting-ledger identity, the
// same 3-tuple rfc0047_frontier_check uses to count distinct ledgers. Stack
// behind is derived from the root family and the line pot and is not part of
// the identity.
struct LedgerKey {
  bs::poker::Chips pot = 0;
  bs::poker::Chips contribution0 = 0;
  bs::poker::Chips contribution1 = 0;
  constexpr bool operator==(const LedgerKey&) const = default;
  constexpr bool operator<(const LedgerKey& other) const {
    if (pot != other.pot)
      return pot < other.pot;
    if (contribution0 != other.contribution0)
      return contribution0 < other.contribution0;
    return contribution1 < other.contribution1;
  }
};

struct LedgerAnchor {
  std::string family;            // "limped" | "srp"
  std::size_t family_index = 0;  // first-seen DFS ordinal within the family
  LedgerKey key;
  FrontierLeaf representative;  // DFS first leaf for the key (first card)
  std::size_t leaf_count = 0;   // frontier leaves sharing this key
};

// Enumerate the eleven distinct ledgers in deterministic DFS first-seen
// order: the five limped ledgers followed by the six SRP ledgers.
std::vector<LedgerAnchor> enumerate_ledger_anchors();

struct LedgerCensusRow {
  LedgerAnchor anchor;
  OfflineCensusRow census;  // exact cap-0 tree, frozen offline envelope
};

// Build the cap-0 public tree for every one of the eleven ledgers and census
// it without solving. Trees that cross the offline envelope come back as a
// typed fits=false row rather than an exception.
std::vector<LedgerCensusRow> census_eleven_cap0_trees();

// The measured storage tuple: action nodes, projected infosets (action nodes
// x 1,081 concrete combos), and the widest per-node menu. Total allocation is
// monotone in this tuple because the storage formula is shared across ledgers.
struct CensusTuple {
  std::size_t action_nodes = 0;
  std::size_t projected_infosets = 0;
  std::size_t max_menu_width = 0;
  constexpr bool operator==(const CensusTuple&) const = default;
};

CensusTuple census_tuple(const LedgerCensusRow& row);

// Indices into the eleven-row vector in deterministic ascending measured
// order, stable on ties so equal-sized ledgers keep enumeration order and are
// tested as one group.
std::vector<std::size_t> census_ascending_order(const std::vector<LedgerCensusRow>& rows);

// RFC 0050 Amendment 1 decision-bearing eligibility. A terminal all-in root
// has zero action nodes: both stacks are committed at the river, so the tree
// hosts no menu and no solve. Including such a row in the anchor set would
// make the Stage 0 memory gate vacuous (nothing to allocate, nothing to
// converge), so the Stage 1/2 anchor set is the measured ascending order
// filtered to decision-bearing rows. The predicate is structural and
// measurement-derived, never a name-based ledger pick; the eleven-row census
// itself still reports every ledger, terminal rows included.
bool is_decision_bearing(const LedgerCensusRow& row);

// Ascending measured order restricted to decision-bearing ledgers; the hard-
// coded six-anchor pre-registration must equal its first six entries.
std::vector<std::size_t> decision_bearing_ascending_order(const std::vector<LedgerCensusRow>& rows);

// Hard-coded Stage 1/2 pre-registration, defined in rfc0050_anchors.hpp from
// the sealed eleven-row Step 0a census receipt: six anchors named by family
// and ledger key, in measured ascending order. No run-time selection, no argv.
struct AnchoredLedger {
  const char* family;
  LedgerKey key;
};
const std::array<AnchoredLedger, kRfc0050AnchorCount>& rfc0050_six_anchors();

}  // namespace bs::rfc0047
