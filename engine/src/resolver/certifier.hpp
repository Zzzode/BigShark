// Independent responder best-response certification. This enumeration is
// written separately from the gadget CFR: it does not read regret or average
// tables and rebuilds the runout expectation and every reach factor itself,
// touching only exact engine settlement. The cross-check in tests is a third,
// independently written oracle.
#pragma once

#include <map>
#include <vector>

#include "counterfactual_reach.hpp"

namespace bs::resolver::detail {

struct Certification {
  ResolveStatus status = ResolveStatus::CertifyDeadline;
  bool certified = false;
  std::size_t nodes = 0;
  std::vector<MarginRecord> margins;
};

// Recomputes the responder best response against the WHOLE candidate for every
// positive-mass root infoset under the unchanged prefix weights.
Certification certify_candidate(const ReachModel& model,
                                const std::map<InformationKey, PolicyRow>& candidate,
                                const ResolveLimits& limits, Budget& budget);

}  // namespace bs::resolver::detail
