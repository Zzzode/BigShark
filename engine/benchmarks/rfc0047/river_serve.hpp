// RFC 0047 S3/G5 serve point B: in-process certified one-card river re-solve.
//
// This is the typed runtime call entry for an online river decision (an
// in-process call, not a deployment or external service). It runs the exact
// seedless river re-solve and then certifies the per-combo responder
// non-regression inequality against a LOCKED responder baseline:
//
//     BR_s(c) <= b_s(c) + tolerance_pot * root_pot   for every live (seat, c)
//
// The best response BR_s(c) is the kernel's independently-walked eval(br=true)
// value, which reads only the built average strategy and exact settlement and
// never the CFR regret/average tables - the rfc0047 analogue of the production
// resolver's independently-written certifier (resolver.hpp MarginRecord uses
// the identical slack form). A serve is returned as Certified only when every
// combo of every live seat passes; any violation fails closed and the caller
// must not promote the strategy from approximate. Benchmark-only under the
// rfc0047 link guard; the composition-root promotion into bigshark_resolver is
// a separate, later gate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "river_solver.hpp"

namespace bs::rfc0047 {

// Production responder-certification tolerance as a fraction of the root pot
// (RFC 0005 standard). RFC 0047 G5 predeclares this 1e-9 as a deliberate kill
// gate, ~1e3 tighter than the 1e-6 benchmark instrument; it is never relaxed.
inline constexpr double kServeCertPotTolerance = 1e-9;

enum class ServeStatus {
  Certified,              // every live (seat, combo) satisfies non-regression
  CertificationRejected,  // at least one combo violates; fail closed, no promote
  InvalidInput,           // malformed game, pot, or baseline vector size
};

struct ServeMarginRecord {
  std::size_t seat = 0;
  std::uint32_t combo = 0;
  double best_response = 0.0;  // independently walked BR, per-compatible-deal chips
  double baseline = 0.0;       // locked b_s(c), same chip normalization
  double slack = 0.0;          // BR - baseline - tolerance*pot; <= 0 passes
};

struct RiverServeRequest {
  RiverSolveConfig solve;
  // Locked per-combo responder baseline for each seat, each of length
  // combos_per_seat (1081) in the same per-compatible-deal chip units as the
  // kernel's root_br_* vectors. References caller storage valid for the call.
  std::span<const double> baseline_seat0;
  std::span<const double> baseline_seat1;
  // Fraction of root pot added to the baseline. Defaults to the production
  // 1e-9 kill gate; tests may pass 1e-6 to prove the tighter value is binding.
  // Must be finite and non-negative or the serve returns InvalidInput.
  double tolerance_pot = kServeCertPotTolerance;
};

struct RiverServeResult {
  ServeStatus status = ServeStatus::InvalidInput;
  // Diagnostic solve outputs. The status alone governs promotion: on
  // CertificationRejected these values must never be served as certified.
  RiverSolveResult solve;
  // All violating margin records (empty iff Certified); retained for reporting.
  std::vector<ServeMarginRecord> violations;
  // True maximum slack over every seat and combo (negative on a Certified
  // serve, i.e. the tightest remaining safety margin); +infinity is impossible
  // because non-finite inputs are rejected.
  double worst_slack = 0.0;
  std::size_t combos_per_seat = 0;
};

// Exact-key re-solve followed by independent per-combo responder
// certification. Never throws and never promotes an uncertified strategy:
// non-finite/negative tolerance, a tolerance whose chip-scale product overflows,
// non-positive or non-finite pot, malformed baseline size, any non-finite
// baseline or best-response value, or a malformed GameDef (the kernel's
// std::invalid_argument) all yield InvalidInput.
RiverServeResult serve_river_certified(const RiverServeRequest& request);

}  // namespace bs::rfc0047
