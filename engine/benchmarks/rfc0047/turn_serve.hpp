// RFC 0047 S3/G5 serve point A: in-process certified depth-limited TURN serve.
//
// The typed runtime call entry for an online turn decision (an in-process call,
// not a deployment). It solves the turn-rooted cap-1 gadget whose post-chance
// river frontier leaves are BOUNDED external inputs, then certifies two
// independent predicates that must BOTH hold:
//
//  (A) Per-combo responder non-regression at the turn root against a SEALED
//      trunk margin supplied by the caller (never derived from this same solve,
//      so it cannot be a self-consistent oracle):
//          BR_s(c) <= b_turn_s(c) + tolerance_pot * root_pot
//      for every live turn combo of both seats, BR being the kernel's
//      independently-walked evaluate(br=true) root value.
//
//  (B) Whole-strategy B0 hybrid NashConv. The local analytic bound composes the
//      finite gadget solution error (the independent hybrid turn gap), the
//      reach-propagated G3 oracle error (an externally certified BOUNDED input,
//      not recomputed here), and the river R0 term (the per-leaf river
//      subgame NashConv): local_bound = turn_gap + g3_propagated + river_term,
//      required <= B0. The empirical independent-walker HybridNashConv is
//      returned alongside, so both the local bound and the empirical value are
//      reported (the G6 dual-reporting discipline).
//
// A Certified result requires both predicates and complete, finite, in-tolerance
// bounded inputs; any missing coverage, non-finite/out-of-range value, malformed
// game, or B0 overshoot fails closed and is never promoted from approximate.
// Benchmark-only under the rfc0047 link guard; composition-root promotion into
// bigshark_resolver is a separate later gate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "turn_solver.hpp"

namespace bs::rfc0047 {

// Production responder-certification tolerance as a fraction of the root pot
// (RFC 0005 standard), the same predeclared 1e-9 kill gate as serve point B.
inline constexpr double kTurnServeCertPotTolerance = 1e-9;
// Whole-strategy summed-NashConv budget B0 in BB (RFC 0047 B0 ledger).
inline constexpr double kTurnServeB0Bb = 0.010;

enum class TurnServeStatus {
  Certified,              // responder gate AND B0 bound both pass
  CertificationRejected,  // a responder violation or B0 overshoot; no promote
  CoverageMiss,           // a required bounded oracle/river input is absent
  InvalidInput,           // malformed game, spans, tolerance, or non-finite value
};

struct TurnMarginRecord {
  std::size_t seat = 0;
  std::uint32_t combo = 0;
  double best_response = 0.0;  // independently walked root BR, per-deal chips
  double baseline = 0.0;       // sealed b_turn(c), same per-deal chip units
  double slack = 0.0;          // BR - baseline - tolerance*pot; <= 0 passes
};

struct TurnServeRequest {
  TurnSolveConfig solve;
  // Sealed per-combo turn-root responder margin for each seat, each of length
  // kTurnC (1128) in per-compatible-deal chips (the same units as the exported
  // root_br_* vectors). Provenance is a sealed trunk reference solve, not this
  // request's own solve.
  std::span<const double> baseline_turn_seat0;
  std::span<const double> baseline_turn_seat1;
  // One river subgame NashConv per config leaf (chips), feeding the empirical
  // hybrid walker's R0 river term. Length must equal solve.leaves.size() and
  // every entry must be finite and >= 0 (NashConv cannot be negative); a
  // negative value is rejected as InvalidInput rather than shrinking the B0 term.
  std::span<const double> leaf_river_nc_chips;
  // G3 reach-propagated oracle-error bound in BB, an externally certified
  // bounded input. Must be finite and >= 0; a negative value signals missing
  // coverage and fails closed (CoverageMiss).
  double g3_propagated_error_bb = 0.0;
  // Fraction of root pot added to the sealed margin (production 1e-9 default).
  double tolerance_pot = kTurnServeCertPotTolerance;
  // Whole-strategy B0 budget in BB.
  double b0_bb = kTurnServeB0Bb;
};

struct TurnServeResult {
  TurnServeStatus status = TurnServeStatus::InvalidInput;
  TurnSolveResult solve;
  // Empirical independent-walker evidence (turn gap + river term, chips and BB).
  HybridNashConv hybrid;
  // Responder-gate detail: every violating margin record (empty iff gate passes).
  std::vector<TurnMarginRecord> violations;
  double worst_slack = 0.0;  // true max responder slack (negative when passing)
  // B0 ledger in BB.
  double turn_gap_bb = 0.0;
  double river_term_bb = 0.0;
  double g3_propagated_bb = 0.0;
  double local_bound_bb = 0.0;  // turn_gap + g3_propagated + river_term
  bool responder_gate_pass = false;
  bool b0_gate_pass = false;
  std::size_t combos_per_seat = 0;
};

// Exact depth-limited turn solve followed by independent responder certification
// and B0 hybrid composition. Never promotes an uncertified strategy; every
// malformed or insufficiently-proven input yields a typed non-Certified status.
// Malformed-input kernel exceptions (std::invalid_argument) are mapped to
// InvalidInput; resource/overflow errors (tree_resource_exhausted, bad_alloc)
// intentionally propagate to the host's resource handler.
TurnServeResult serve_turn_certified(const TurnServeRequest& request);

}  // namespace bs::rfc0047
