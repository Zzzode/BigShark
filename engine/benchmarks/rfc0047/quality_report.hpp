// RFC 0047 S3/G6 dual quality reporting (RFC 0039 D1), benchmark-only.
//
// G6 rejects "quality laundering": a serve/oracle result must record BOTH the
// within-instrument convergence predicate AND the exact-hand gap together with
// the composed full-strategy M2 delta. Reporting only the converged number is
// not allowed. This header is the thin aggregation layer over the already
// certified serve results; it invents no new solver math and changes no serve
// API. Every value is copied from an existing RiverServeResult/TurnServeResult,
// and a report is fully populated even when the serve is not Certified, so the
// exact-hand gap can never be hidden behind a favorable within-instrument label.
//
// Bars reuse the established seedless-kernel convention (rfc0047_gates.ts G2 and
// rfc0047_oracle_build): the within-instrument CERT summed-NashConv bar is
// 0.005 BB and the exact-hole QUAL bar is 0.03 BB; for a seedless exact kernel
// the two gap values are equal by construction. The composed M2 delta uses the
// G5 budget of the serve point (river R0 = 0.002 BB; turn whole-strategy B0 =
// 0.010 BB).
#pragma once

#include "river_serve.hpp"
#include "turn_serve.hpp"

namespace bs::rfc0047 {

// G2/G6 within-instrument convergence bar (candidate exact summed NashConv).
inline constexpr double kInstrumentCertBarBb = 0.005;
// G2/G6 exact-hole summed-NashConv bar (HU).
inline constexpr double kExactHoleQualBarBb = 0.03;

enum class QualityServeKind {
  River,  // serve point B
  Turn,   // serve point A
};

// One always-populated dual quality record for one certified serve attempt.
struct QualityDualReport {
  QualityServeKind kind = QualityServeKind::River;
  // (1) Within-instrument convergence: the candidate's own exact full-deal
  // SUMMED NashConv in BB and its CERT predicate against the 0.005 bar.
  double within_instrument_nash_conv_bb = 0.0;
  bool within_instrument_converged = false;
  // (2a) Exact-hand independent gap in BB and the QUAL predicate (0.03 bar).
  // Equal to the within-instrument gap for the seedless exact river kernel; on
  // the turn it is the independently-walked hybrid walker's total, a distinct
  // code path from the gadget's own NashConv.
  double exact_gap_bb = 0.0;
  bool exact_qual_pass = false;
  // (2b) Composed full-strategy M2 delta in BB and the serve point's G5 budget
  // (river R0 / turn B0), including every bounded error term.
  double composed_m2_delta_bb = 0.0;
  double composed_gate_bb = 0.0;
  bool composed_gate_pass = false;
  // The certification verdict is carried through unchanged; a non-Certified
  // serve still yields a fully populated report (never laundered away).
  int status_int = 0;
};

// Assemble the G6 record from an existing serve result (pure field assembly;
// throws std::invalid_argument only on a non-finite input gap).
QualityDualReport river_quality_report(const RiverServeResult& serve);
QualityDualReport turn_quality_report(const TurnServeResult& serve);

}  // namespace bs::rfc0047
