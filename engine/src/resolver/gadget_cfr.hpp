// Sibling CFR traversal over the resolving augmented game. It deliberately
// does not generalize the frozen RFC 0004 trainer: normal action transitions and
// settlement are reused, while the synthetic weighted root chance, the per-seat
// "-x" infosets, and the constant-payoff TERMINATE action live only here.
// Regret-matching action selection and the kSimple OWN-REACH average ownership
// match RFC 0004; the one Stage-9 change is LINEAR iteration weighting
// (iteration t's instantaneous regret and average contribution carry weight t).
// It keeps kSimple ownership while improving the average-strategy rate from
// O(1/T) to O(1/T^2) so the bounded deadline can reach the 1e-8 root-pot
// equilibrium gate on a tiny terminal game.
//
// RFC 0009 W2c-ii-c: the augmented game is hero-vs-field, where the field is
// the separable sum of the non-hero seats. Each non-hero seat owns one -x
// infoset. The CFR sweeps once per seat (hero and every non-hero seat); the
// hero's regret is the sum over non-hero seats of that seat's CONTINUE-weighted
// locked-continuation margin, and a non-hero seat's regret is its own
// TERMINATE/CONTINUE margin. At two seats this reduces operation-for-operation
// to the frozen heads-up gadget.
#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "counterfactual_reach.hpp"

namespace bs::resolver::detail {

struct GadgetOutput {
  ResolveStatus status = ResolveStatus::SolveDeadline;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;
  std::size_t information_sets = 0;
  // Whole-range candidate at the hero's current node.
  std::map<InformationKey, PolicyRow> candidate;
  // Average per-seat gadget policy {TERMINATE, CONTINUE} per -x infoset.
  std::map<GadgetKey, std::array<double, 2>> terminate;
};

// Runs the bounded full-traversal gadget CFR. The SplitMix domain stream is
// advanced from limits.public_seed to pin public identity; terminal-only full
// traversal consumes no sampled entropy.
GadgetOutput run_gadget_cfr(const ReachModel& model, const ResolveLimits& limits, Budget& budget);

}  // namespace bs::resolver::detail
