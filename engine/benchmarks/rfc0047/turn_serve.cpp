#include "turn_serve.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bs::rfc0047 {

namespace {

bool all_finite(std::span<const double> v) {
  for (double x : v)
    if (!std::isfinite(x))
      return false;
  return true;
}

}  // namespace

TurnServeResult serve_turn_certified(const TurnServeRequest& request) {
  TurnServeResult out;

  // Request-level validation before the solve. Malformed-input kernel exceptions
  // (invalid_argument) are mapped to a typed status below; resource/overflow
  // errors (tree_resource_exhausted, bad_alloc) intentionally propagate to a
  // host resource handler rather than being misclassified as invalid input.
  const double pot = static_cast<double>(request.solve.def.pot);
  const bool baselines_present =
      !request.baseline_turn_seat0.empty() && !request.baseline_turn_seat1.empty() &&
      request.baseline_turn_seat0.size() == request.baseline_turn_seat1.size();
  const bool river_nc_present = !request.leaf_river_nc_chips.empty();
  if (!(std::isfinite(request.tolerance_pot) && request.tolerance_pot >= 0.0) ||
      !(std::isfinite(request.b0_bb) && request.b0_bb > 0.0) || !std::isfinite(pot) || pot <= 0.0 ||
      !baselines_present || !river_nc_present || !all_finite(request.baseline_turn_seat0) ||
      !all_finite(request.baseline_turn_seat1) || !all_finite(request.leaf_river_nc_chips) ||
      !std::isfinite(request.g3_propagated_error_bb)) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  // Exact-span length checks up front so a malformed request never pays for a
  // solve: sealed margins are per turn combo; the river NashConv vector has one
  // entry per config leaf.
  if (request.baseline_turn_seat0.size() != kTurnC ||
      request.baseline_turn_seat1.size() != kTurnC ||
      request.leaf_river_nc_chips.size() != request.solve.leaves.size()) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  // NashConv is mathematically non-negative. A negative river subgame NashConv
  // is an out-of-range bounded input that, if accepted, could artificially
  // shrink the B0 river term and forge a Certified result.
  for (double v : request.leaf_river_nc_chips)
    if (v < 0.0) {
      out.status = TurnServeStatus::InvalidInput;
      return out;
    }
  // A finite but negative propagated bound means the G3 coverage is absent:
  // this is a coverage miss, not a malformed number.
  if (request.g3_propagated_error_bb < 0.0) {
    out.status = TurnServeStatus::CoverageMiss;
    return out;
  }
  // Detect tolerance*pot overflow up front (both operands are known and finite
  // here) so an astronomical fraction is rejected before paying for a solve.
  if (!std::isfinite(request.tolerance_pot * pot)) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }

  TurnSolveResult solved;
  try {
    solved = solve_turn_subgame(request.solve);
  } catch (const std::invalid_argument&) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  out.solve = solved;
  out.combos_per_seat = kTurnC;

  if (solved.root_br_seat0.size() != kTurnC || solved.root_br_seat1.size() != kTurnC) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  if (!all_finite(solved.root_br_seat0) || !all_finite(solved.root_br_seat1)) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }

  // Empirical independent-walker evidence: the hybrid turn gap (finite gadget
  // solution error) and the reach-weighted river R0 term. Computed by a separate
  // tree walk from the CFR path, so it is independent of the solved NashConv.
  try {
    out.hybrid =
        turn_hybrid_nash_conv(request.solve, std::span<const std::vector<double>>(solved.sigma),
                              request.leaf_river_nc_chips);
  } catch (const std::invalid_argument&) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  if (!std::isfinite(out.hybrid.turn_gap_bb) || !std::isfinite(out.hybrid.river_term_bb)) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }

  // B0 local analytic bound (BB): finite gadget gap + externally bounded G3
  // propagated oracle error + the river R0 term.
  out.turn_gap_bb = out.hybrid.turn_gap_bb;
  out.river_term_bb = out.hybrid.river_term_bb;
  out.g3_propagated_bb = request.g3_propagated_error_bb;
  out.local_bound_bb = out.turn_gap_bb + out.g3_propagated_bb + out.river_term_bb;
  out.b0_gate_pass = std::isfinite(out.local_bound_bb) && out.local_bound_bb <= request.b0_bb;

  // Per-combo responder non-regression at the turn root.
  const double tolerance_chips = request.tolerance_pot * pot;
  if (!std::isfinite(tolerance_chips)) {
    out.status = TurnServeStatus::InvalidInput;
    return out;
  }
  const std::array<const std::vector<double>*, 2> best_response{&solved.root_br_seat0,
                                                                &solved.root_br_seat1};
  const std::array<std::span<const double>, 2> baseline{request.baseline_turn_seat0,
                                                        request.baseline_turn_seat1};
  out.worst_slack = -std::numeric_limits<double>::infinity();
  for (std::size_t seat = 0; seat < 2; ++seat) {
    const std::vector<double>& br = *best_response[seat];
    const std::span<const double> base = baseline[seat];
    for (std::uint32_t combo = 0; combo < kTurnC; ++combo) {
      const double slack = br[combo] - base[combo] - tolerance_chips;
      if (slack > out.worst_slack)
        out.worst_slack = slack;
      if (slack > 0.0)
        out.violations.push_back(TurnMarginRecord{seat, combo, br[combo], base[combo], slack});
    }
  }
  out.responder_gate_pass = out.violations.empty();

  out.status = (out.responder_gate_pass && out.b0_gate_pass)
                   ? TurnServeStatus::Certified
                   : TurnServeStatus::CertificationRejected;
  return out;
}

}  // namespace bs::rfc0047
