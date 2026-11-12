// RFC 0047 S3/G6 native test: the dual quality reporting assemblers.
//
// The builders are pure field-assembly functions over an already-computed serve
// result, so they are unit-tested with controlled synthetic results (no solver
// runs): this pins the G6 within-instrument CERT predicate (0.005 BB), the
// exact-hand QUAL predicate (0.03 BB), the composed M2 gate (river R0 0.002 /
// turn B0), the seedless river within==exact identity, and the anti-laundering
// guarantee that a non-Certified serve still yields a fully populated report.
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "../benchmarks/rfc0047/quality_report.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

namespace {

bool test_river_predicates_and_identity() {
  // Seedless exact river: within-instrument gap and exact-hand gap must be the
  // same summed NashConv; the three gates are 0.005 / 0.03 / R0 0.002.
  const double cases[][2] = {
      {0.001, true},   // below all three
      {0.003, false},  // above CERT and R0, below QUAL
      {0.020, false},  // above CERT/R0, still below QUAL
      {0.040, false},  // above QUAL too
  };
  for (const auto& [gap, below_all] : cases) {
    RiverServeResult s;
    s.status = ServeStatus::Certified;
    s.solve.nash_conv_bb = gap;
    QualityDualReport r = river_quality_report(s);
    CHECK(r.kind == QualityServeKind::River);
    CHECK(r.within_instrument_nash_conv_bb == gap);
    CHECK(r.exact_gap_bb == gap);  // seedless CERT == QUAL identity
    CHECK(r.within_instrument_converged == (gap <= 0.005));
    CHECK(r.exact_qual_pass == (gap <= 0.03));
    CHECK(r.composed_m2_delta_bb == gap);
    CHECK(r.composed_gate_bb == 0.002);
    CHECK(r.composed_gate_pass == (gap <= 0.002));
    if (below_all)
      CHECK(r.within_instrument_converged && r.composed_gate_pass);
  }
  return true;
}

bool test_turn_three_distinct_measures() {
  TurnServeResult s;
  s.status = TurnServeStatus::CertificationRejected;  // anti-laundering case
  s.solve.nash_conv_bb = 0.07;                        // gadget own-walker gap
  s.hybrid.turn_gap_bb = 0.05;
  s.hybrid.river_term_bb = 0.01;  // independent walker total 0.06
  s.local_bound_bb = 0.075;       // + external G3 composed
  s.b0_bb = 0.010;
  s.b0_gate_pass = false;

  QualityDualReport r = turn_quality_report(s);
  CHECK(r.kind == QualityServeKind::Turn);
  CHECK(r.within_instrument_nash_conv_bb == 0.07);
  CHECK(!r.within_instrument_converged);            // 0.07 > 0.005
  CHECK(std::fabs(r.exact_gap_bb - 0.06) < 1e-12);  // 0.05 + 0.01 (float sum)
  CHECK(!r.exact_qual_pass);                        // 0.06 > 0.03
  CHECK(r.composed_m2_delta_bb == 0.075);
  CHECK(r.composed_gate_bb == 0.010);
  CHECK(!r.composed_gate_pass);  // mirrored from the serve verdict
  // Even though the serve was rejected, every dual field is populated: the
  // exact-hand gap can never be hidden behind a favorable converged label.
  CHECK(std::isfinite(r.within_instrument_nash_conv_bb) && std::isfinite(r.exact_gap_bb) &&
        std::isfinite(r.composed_m2_delta_bb));
  CHECK(r.status_int == static_cast<int>(TurnServeStatus::CertificationRejected));

  // A genuinely converged turn flips the within-instrument predicate.
  TurnServeResult good = s;
  good.solve.nash_conv_bb = 0.004;
  good.hybrid.turn_gap_bb = 0.003;
  good.hybrid.river_term_bb = 0.001;
  QualityDualReport rg = turn_quality_report(good);
  CHECK(rg.within_instrument_converged && rg.exact_qual_pass);
  return true;
}

bool test_non_certified_river_still_populated() {
  // Symmetric anti-laundering check on the river side: a rejected serve must
  // still expose the full dual record with a finite, correctly-gated gap rather
  // than hiding it behind the non-Certified status.
  RiverServeResult s;
  s.status = ServeStatus::CertificationRejected;
  s.solve.nash_conv_bb = 0.004;  // above the 0.002 composed R0, below 0.005 CERT
  QualityDualReport r = river_quality_report(s);
  CHECK(r.status_int == static_cast<int>(ServeStatus::CertificationRejected));
  CHECK(r.within_instrument_nash_conv_bb == 0.004);
  CHECK(r.within_instrument_converged);  // 0.004 <= 0.005
  CHECK(r.exact_gap_bb == 0.004);
  CHECK(r.exact_qual_pass);      // 0.004 <= 0.03
  CHECK(!r.composed_gate_pass);  // 0.004 > 0.002: the laundering guard
  CHECK(std::isfinite(r.composed_m2_delta_bb));
  return true;
}

bool test_non_finite_rejected() {
  RiverServeResult rs;
  rs.solve.nash_conv_bb = std::numeric_limits<double>::quiet_NaN();
  bool threw = false;
  try {
    (void)river_quality_report(rs);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  TurnServeResult ts;
  ts.solve.nash_conv_bb = 0.001;
  ts.hybrid.turn_gap_bb = std::numeric_limits<double>::infinity();
  ts.hybrid.river_term_bb = 0.0;
  ts.local_bound_bb = 0.002;
  ts.b0_bb = 0.010;
  threw = false;
  try {
    (void)turn_quality_report(ts);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

bool test_constants_match_gates() {
  CHECK(kInstrumentCertBarBb == 0.005);
  CHECK(kExactHoleQualBarBb == 0.03);
  CHECK(kTurnServeB0Bb == 0.010);
  CHECK(kServeCertPotTolerance == 1e-9 && kTurnServeCertPotTolerance == 1e-9);
  return true;
}

}  // namespace

int main() {
  if (!test_river_predicates_and_identity() || !test_turn_three_distinct_measures() ||
      !test_non_certified_river_still_populated() || !test_non_finite_rejected() ||
      !test_constants_match_gates())
    return 1;
  std::printf("rfc0047_quality_report: all checks passed\n");
  return 0;
}
