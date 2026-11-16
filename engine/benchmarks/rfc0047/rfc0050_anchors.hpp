// RFC 0050 Stage 0 Step 0a hard-coded six-anchor pre-registration.
//
// This list is the Stage 1/2 anchor set named in the accepted RFC 0050 as
// amended by Amendment 1 (decision-bearing eligibility): the first six
// ledgers of the eleven-ledger cap-0 census whose trees carry at least one
// action node, in deterministic ascending measured (action nodes, projected
// infosets, max menu width) order. The two terminal all-in ledgers
// (pot 400, contributions 200/200, one root node, no menu) stay in the
// eleven-row census receipt but are not anchors: a zero-action root cannot
// host an allocation or convergence gate.
//
// Transcribed from the sealed Step 0a census receipt under
// docs/plans/ablation/rfc0050/ and pinned against re-measurement by the
// rfc0050_ledger_census native test; there is no run-time anchor selection
// and no argv override. Amending the order, keys, or eligibility rule is an
// RFC amendment requiring fresh independent review.
//
// The family string is "limped" or "srp"; the key is the ledger's
// (pot, contribution0, contribution1) tuple.
#pragma once

#include "ledger_census.hpp"

namespace bs::rfc0047 {

inline const std::array<AnchoredLedger, kRfc0050AnchorCount>& rfc0050_six_anchors() {
  static constexpr std::array<AnchoredLedger, kRfc0050AnchorCount> kAnchors{{
      {"srp", LedgerKey{32, 16, 16}},
      {"srp", LedgerKey{20, 10, 10}},
      {"limped", LedgerKey{16, 8, 8}},
      {"srp", LedgerKey{14, 7, 7}},
      {"srp", LedgerKey{12, 6, 6}},
      {"limped", LedgerKey{10, 5, 5}},
  }};
  return kAnchors;
}

}  // namespace bs::rfc0047
