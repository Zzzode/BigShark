// chart_digest.hpp — RFC 0008 stage 6 baseline content digest.
//
// The pinned baseline opponent is identified by commit SHA, exact files, AND a
// digest of the parsed chart data (RFC 0008:568-580): a prose description of
// "the charts" can be wrong, a digest cannot. This component hashes the exact
// decision-relevant data:
//   * the raw range spec STRINGS (so a grammar change is visible), and
//   * the SORTED post-expansion Range169 sets for every one of the 24 chart
//     fields, labeled by canonical field path (so parse/expand semantics are
//     pinned), and
//   * the 169-entry Chen percentile table in key order (it drives the <=12bb
//     jam branch but is not one of the 24 sets).
// It lives in bigshark_stage6_eval (offline only) and links bigshark_poker,
// which owns the charts. The digest is a deterministic SHA-256 hex string;
// nothing here uses ambient randomness or the network.
#pragma once

#include <string>

namespace bs::stage6 {

// Computes the pinned-baseline chart digest over the charts at THIS commit.
// The returned 64-character lowercase hex string is recorded verbatim in the
// stage-6 evidence; it changes iff a raw spec, an expanded range, or the
// percentile table changes.
std::string chart_digest_hex();

}  // namespace bs::stage6
