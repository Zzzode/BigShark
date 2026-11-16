#include "rfc0050_stage1.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace bs::rfc0047
