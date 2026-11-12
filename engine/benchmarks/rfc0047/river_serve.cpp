#include "river_serve.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bs::rfc0047 {

RiverServeResult serve_river_certified(const RiverServeRequest& request) {
  RiverServeResult out;

  // All request-level validation happens BEFORE the solve, and every kernel
  // throw is mapped below, so this entry never throws: a malformed request is
  // always reported as InvalidInput rather than an escaping exception.
  const double pot = static_cast<double>(request.solve.def.pot);
  const bool baselines_well_formed =
      request.baseline_seat0.size() == request.baseline_seat1.size() &&
      !request.baseline_seat0.empty();
  if (!(std::isfinite(request.tolerance_pot) && request.tolerance_pot >= 0.0) ||
      !(std::isfinite(pot) && pot > 0.0) || !baselines_well_formed) {
    out.status = ServeStatus::InvalidInput;
    return out;
  }
  // Non-finite baseline entries cannot satisfy a non-regression inequality;
  // reject them up front instead of letting a NaN comparison silently pass.
  for (double v : request.baseline_seat0)
    if (!std::isfinite(v)) {
      out.status = ServeStatus::InvalidInput;
      return out;
    }
  for (double v : request.baseline_seat1)
    if (!std::isfinite(v)) {
      out.status = ServeStatus::InvalidInput;
      return out;
    }

  // Exact-key seedless re-solve. A malformed GameDef (wrong seat/board/terminal
  // shape) makes the kernel throw std::invalid_argument; surface that as the
  // typed InvalidInput status the header promises.
  RiverSolveResult solved;
  try {
    solved = solve_river_subgame(request.solve, {});
  } catch (const std::invalid_argument&) {
    out.status = ServeStatus::InvalidInput;
    return out;
  }
  const std::size_t combos = solved.combos_per_seat;
  out.solve = solved;
  out.combos_per_seat = combos;

  if (request.baseline_seat0.size() != combos || request.baseline_seat1.size() != combos) {
    out.status = ServeStatus::InvalidInput;
    return out;
  }
  // The independently-walked best response must itself be finite; a non-finite
  // BR cannot be certified against any finite baseline.
  for (double v : solved.root_br_seat0)
    if (!std::isfinite(v)) {
      out.status = ServeStatus::InvalidInput;
      return out;
    }
  for (double v : solved.root_br_seat1)
    if (!std::isfinite(v)) {
      out.status = ServeStatus::InvalidInput;
      return out;
    }

  const double tolerance_chips = request.tolerance_pot * pot;
  // A finite-but-huge tolerance fraction can overflow the chip-scale product to
  // infinity, which would make the documented "no infinite slack" invariant
  // false; reject it rather than silently certifying with an infinite margin.
  if (!std::isfinite(tolerance_chips)) {
    out.status = ServeStatus::InvalidInput;
    return out;
  }
  const std::array<const std::vector<double>*, 2> best_response{&solved.root_br_seat0,
                                                                &solved.root_br_seat1};
  const std::array<std::span<const double>, 2> baseline{request.baseline_seat0,
                                                        request.baseline_seat1};

  // Seed below every real slack so worst_slack reports the true maximum
  // (negative on a certified serve), not a hard-coded zero.
  out.worst_slack = -std::numeric_limits<double>::infinity();

  // Per-combo responder non-regression, mirroring resolver.hpp MarginRecord:
  // pass iff BR_s(c) - b_s(c) - tol*pot <= 0 for every live seat and combo.
  for (std::size_t seat = 0; seat < 2; ++seat) {
    const std::vector<double>& br = *best_response[seat];
    const std::span<const double> base = baseline[seat];
    for (std::uint32_t combo = 0; combo < combos; ++combo) {
      const double slack = br[combo] - base[combo] - tolerance_chips;
      if (slack > out.worst_slack)
        out.worst_slack = slack;
      if (slack > 0.0)
        out.violations.push_back(ServeMarginRecord{seat, combo, br[combo], base[combo], slack});
    }
  }

  out.status = out.violations.empty() ? ServeStatus::Certified : ServeStatus::CertificationRejected;
  return out;
}

}  // namespace bs::rfc0047
