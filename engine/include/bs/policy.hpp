// policy.hpp — policy routing surface and declared decision sources.
//
// RFC 0008 L6: the policy layer names WHICH source selected each decision.
// The guarantee level that source earns is derived in guarantee.{hpp,cpp};
// this header stays free of guarantee/transport vocabulary.
#pragma once
#include <bs/decision.hpp>

namespace bs {

// The policy source that selected a decision. Storage sources (resident
// blueprint, resolving certifier) do not appear here: they are selected at
// the v1 protocol boundary, not inside the policy. MultistreetCfr is never
// produced on a decision path but is kept for switch totality over the RFC
// 0008 seven-source normative table.
enum class DecisionSource {
  PreflopChart,
  PostflopHeuristic,
  RiverLp,
  RiverDcfr,
  MultistreetCfr,
};

struct SourcedDecision {
  Decision decision;
  DecisionSource source;
};

// Policy backend control seam. Production callers use the default, which is
// bit-identical to the pre-stage-5 behavior (the river LP is attempted when
// it fits the time budget). Tests force the bounded-CFR backend deterministically.
struct RiverBackendHint {
  bool allow_exact = true;
};

Decision evaluatePolicy(const Ctx& context);

// Routed policy decision with the source that selected it. The default hint
// preserves every live decision; evaluatePolicy is the one-line wrapper.
SourcedDecision evaluatePolicySourced(const Ctx& context, RiverBackendHint backend = {});

}  // namespace bs
