#include "rfc0050_stage1.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bs::rfc0047 {

using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;

Rfc0050Stage1Domain build_stage1_domain(const bs::poker::GameDef& def,
                                        const bs::tree::TreeLimits& limits) {
  const ActionAbstraction exact = ActionAbstraction::identity(0);
  // Two distinct tree objects with identical inputs: the reach-bound
  // instrument consumes separate reference and bounded arguments even when
  // the menus are structurally the same.
  AbstractTree reference(def, exact, limits);
  AbstractTree bounded(def, exact, limits);
  if (reference.size() != bounded.size())
    throw std::invalid_argument("rfc0050 stage1: cap-0 tree size mismatch");
  SubsetMap subset = map_bounded_subset(reference, bounded);
  // Identity proof: a full-identity subset map has no unmapped node and no
  // extension (-1) edge. Any other result means the exact menu claim is
  // false, so fail closed before any convergence receipt is produced.
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (subset.node_map[n] == SubsetMap::kNoNode)
      throw std::invalid_argument("rfc0050 stage1: unmapped node in exact domain");
    for (int edge : subset.edge_map[n])
      if (edge < 0)
        throw std::invalid_argument("rfc0050 stage1: extension edge in exact domain");
  }
  return Rfc0050Stage1Domain{exact, std::move(reference), std::move(bounded), std::move(subset)};
}

bool pure_menu_reads_zero(double pure_menu_term_bb) {
  return std::isfinite(pure_menu_term_bb) && std::fabs(pure_menu_term_bb) <= kRfc0050PureMenuZeroBb;
}

Rfc0050Calibration extrapolate_stage1_calibration(double ms_per_iteration,
                                                  double ms_per_rung_scoring) {
  Rfc0050Calibration out;
  out.ms_per_iteration = ms_per_iteration;
  out.ms_per_rung_scoring = ms_per_rung_scoring;
  constexpr double kMsPerS = 1000.0;
  out.projected_20k_wall_s =
      ms_per_iteration * static_cast<double>(kRfc0050Stage1Rungs.back()) / kMsPerS;
  out.projected_full_ladder_wall_s =
      out.projected_20k_wall_s +
      static_cast<double>(kRfc0050Stage1Rungs.size()) * ms_per_rung_scoring / kMsPerS;
  out.iterations_inside_wall = std::floor(kRfc0050Stage1WallMs / ms_per_iteration);
  // A rung's total wall is its solve time plus one scoring per rung reached.
  for (std::size_t i = 0; i < kRfc0050Stage1Rungs.size(); ++i) {
    const double rung_wall_ms = static_cast<double>(kRfc0050Stage1Rungs[i]) * ms_per_iteration +
                                static_cast<double>(i + 1) * ms_per_rung_scoring;
    if (rung_wall_ms <= kRfc0050Stage1WallMs)
      out.last_rung_inside_wall = kRfc0050Stage1Rungs[i];
  }
  return out;
}

ScalingVerdict classify_scaling(double speedup_at_28_threads) {
  if (speedup_at_28_threads >= kRfc0050CheapSpeedup)
    return ScalingVerdict::LongWallAmendmentCheap;
  if (speedup_at_28_threads >= kRfc0050IncompressibleSpeedup)
    return ScalingVerdict::PlateauRiskDominates;
  return ScalingVerdict::ComputeIncompressible;
}

AmdahlBreakdown amdahl_from_speedup(double speedup, std::size_t threads) {
  AmdahlBreakdown b;
  const double n = static_cast<double>(threads);
  if (threads <= 1) {
    // Amdahl is undefined on a single worker; signal with NaN and let the
    // caller leave the fraction columns blank rather than print a fake zero.
    b.serial_fraction = std::numeric_limits<double>::quiet_NaN();
    b.parallel_fraction = std::numeric_limits<double>::quiet_NaN();
    return b;
  }
  const double inv_s = 1.0 / speedup;
  b.serial_fraction = (inv_s - 1.0 / n) / (1.0 - 1.0 / n);
  // Clamp numerical overshoot at the endpoints; a measured superlinear or
  // sub-1 result maps to the nearest physical bound rather than a negative
  // or >1 fraction.
  if (b.serial_fraction < 0.0)
    b.serial_fraction = 0.0;
  if (b.serial_fraction > 1.0)
    b.serial_fraction = 1.0;
  b.parallel_fraction = 1.0 - b.serial_fraction;
  return b;
}

DecisiveSlopeResult classify_decisive_slope(double e_400, double e_800, double e_1600) {
  DecisiveSlopeResult out;
  if (!(std::isfinite(e_400) && std::isfinite(e_800) && std::isfinite(e_1600)) || e_400 <= 0.0 ||
      e_800 <= 0.0 || e_1600 <= 0.0)
    throw std::invalid_argument("decisive slope: rungs must be positive finite");
  // Plateau first: a near-flat last interval means no power-law crossing.
  const double relative_change = std::fabs(e_1600 - e_800) / e_800;
  if (relative_change < kD326PlateauRelative) {
    out.plateau = true;
    out.regime = DecisiveSlopeRegime::Plateau;
  }
  // OLS on log2 values; the rung iterations are 400/800/1600, equally spaced
  // in log2, but a general OLS keeps the routine independent of that spacing.
  const double xs[3] = {std::log2(400.0), std::log2(800.0), std::log2(1600.0)};
  const double ys[3] = {std::log2(e_400), std::log2(e_800), std::log2(e_1600)};
  double mx = 0.0, my = 0.0;
  for (int i = 0; i < 3; ++i) {
    mx += xs[i];
    my += ys[i];
  }
  mx /= 3.0;
  my /= 3.0;
  double cov = 0.0, var = 0.0;
  for (int i = 0; i < 3; ++i) {
    cov += (xs[i] - mx) * (ys[i] - my);
    var += (xs[i] - mx) * (xs[i] - mx);
  }
  out.slope = cov / var;
  if (!out.plateau) {
    const double magnitude = std::fabs(out.slope);
    if (magnitude >= kD326SteepMagnitude)
      out.regime = DecisiveSlopeRegime::SteepLawHolds;
    else if (magnitude >= kD326DecayedMagnitude)
      out.regime = DecisiveSlopeRegime::Decaying;
    else
      out.regime = DecisiveSlopeRegime::DecayedConfirmed;
  }
  return out;
}

}  // namespace bs::rfc0047
