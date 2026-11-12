#include "quality_report.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace bs::rfc0047 {

// G5 river R0 budget for the composed full-strategy river term (BB).
namespace {
constexpr double kRiverR0GateBb = 0.002;

void require_finite(double value, const char* what) {
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string("quality_report: non-finite ") + what);
}
}  // namespace

QualityDualReport river_quality_report(const RiverServeResult& serve) {
  // Seedless exact kernel: the within-instrument convergence gap and the
  // exact-hand gap are the same exact full-deal summed NashConv (G2 V3
  // CERT == QUAL by construction); the composed term is that river R0 entering
  // the turn B0 ledger, gated at the stricter R0 budget.
  const double gap_bb = serve.solve.nash_conv_bb;
  require_finite(gap_bb, "river nash_conv_bb");
  QualityDualReport r;
  r.kind = QualityServeKind::River;
  r.within_instrument_nash_conv_bb = gap_bb;
  r.within_instrument_converged = gap_bb <= kInstrumentCertBarBb;
  r.exact_gap_bb = gap_bb;
  r.exact_qual_pass = gap_bb <= kExactHoleQualBarBb;
  r.composed_m2_delta_bb = gap_bb;
  r.composed_gate_bb = kRiverR0GateBb;
  r.composed_gate_pass = gap_bb <= kRiverR0GateBb;
  r.status_int = static_cast<int>(serve.status);
  return r;
}

QualityDualReport turn_quality_report(const TurnServeResult& serve) {
  // Within-instrument: the gadget's own summed NashConv (finite solution error
  // measured by its internal walker). Exact-hand: the INDEPENDENT hybrid
  // walker's empirical total (turn gap + reach-weighted river term), a separate
  // code path. Composed M2: the local B0 bound (adds the external G3 bound).
  const double within_bb = serve.solve.nash_conv_bb;
  const double exact_bb = serve.hybrid.turn_gap_bb + serve.hybrid.river_term_bb;
  require_finite(within_bb, "turn within nash_conv_bb");
  require_finite(exact_bb, "turn exact hybrid gap");
  require_finite(serve.local_bound_bb, "turn local_bound_bb");
  QualityDualReport r;
  r.kind = QualityServeKind::Turn;
  r.within_instrument_nash_conv_bb = within_bb;
  r.within_instrument_converged = within_bb <= kInstrumentCertBarBb;
  r.exact_gap_bb = exact_bb;
  r.exact_qual_pass = exact_bb <= kExactHoleQualBarBb;
  r.composed_m2_delta_bb = serve.local_bound_bb;
  r.composed_gate_bb = serve.b0_bb;
  r.composed_gate_pass = serve.b0_gate_pass;
  r.status_int = static_cast<int>(serve.status);
  return r;
}

}  // namespace bs::rfc0047
