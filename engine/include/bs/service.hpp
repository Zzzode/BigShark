#pragma once

#include <bs/policy.hpp>

namespace bs {

Decision decide(const Ctx& context);

// Routed decision plus the source that produced it. Storage sources
// (blueprint/resolve) are selected at the v1 boundary, never here.
SourcedDecision decideSourced(const Ctx& context);

}  // namespace bs
