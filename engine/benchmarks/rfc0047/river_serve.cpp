#include "river_serve.hpp"

#include <array>

namespace bs::rfc0047 {

RiverServeResult serve_river_certified(const RiverServeRequest& request) {
  RiverServeResult out;

  // Exact-key seedless re-solve. The result's root_br_* vectors are produced by
  // the kernel's independent eval(br=true) walk (average strategy + exact
  // settlement only), so they are the separately-walked responder best response
  // rather than a value read back from the CFR regret/average tables.
  RiverSolveResult solved = solve_river_subgame(request.solve, {});
  const std::size_t combos = solved.combos_per_seat;
  out.solve = solved;
  out.combos_per_seat = combos;

  const double pot = static_cast<double>(request.solve.def.pot);
  if (request.baseline_seat0.size() != combos || request.baseline_seat1.size() != combos ||
      request.tolerance_pot < 0.0 || pot <= 0.0) {
    out.status = ServeStatus::InvalidInput;
    return out;
  }

  const double tolerance_chips = request.tolerance_pot * pot;
  const std::array<const std::vector<double>*, 2> best_response{&solved.root_br_seat0,
                                                                &solved.root_br_seat1};
  const std::array<std::span<const double>, 2> baseline{request.baseline_seat0,
                                                        request.baseline_seat1};

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
