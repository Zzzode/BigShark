// guarantee.hpp — RFC 0008 L6 closed guarantee-level set and the SINGLE
// normative source -> level mapping for policy sources.
//
// This header is owned by bigshark_policy and is protobuf-free. Storage
// sources (blueprint / certified resolve) are labeled at the protocol
// boundary from the lookup outcome, never from a second source table: a
// policy answer earns its level ONLY through guaranteeFor.
#pragma once
#include <bs/policy.hpp>

namespace bs {

// Ascending numeric order = ascending strength, matching the RFC 0008
// ladder exactly:
// operational_fallback < approximate < abstract_solved
//                       < exact_solved < certified_bound.
enum class Guarantee {
  OperationalFallback = 0,
  Approximate = 1,
  AbstractSolved = 2,
  ExactSolved = 3,
  CertifiedBound = 4,
};

int guaranteeRank(Guarantee level);
bool guaranteeMeets(Guarantee achieved, Guarantee required);

// Canonical on-wire tokens (SolverMetadata.guarantee_level, negotiated
// minor 2). One source of truth for the five-token vocabulary.
const char* guaranteeToken(Guarantee level);

// THE normative mapping for policy-producible sources. Default-less
// exhaustive switch (bigshark_policy compiles -Werror=switch): a new
// DecisionSource cannot ship without naming its level. Every reachable
// policy source today is Approximate: the river LP is solved over capped
// ranges (live cap 36), the chart/heuristic/experimental paths carry no
// measured error bound. Promoting a source to abstract/exact is stage 6's
// measured job and must change this table explicitly.
Guarantee guaranteeFor(DecisionSource source);

// Compile-time exhaustiveness belt-and-braces: instantiates the visitor for
// every DecisionSource enumerator so a newly added value that -Werror=switch
// somehow misses is still caught at template instantiation.
template <class F>
constexpr auto visitEverySource(F&& f) {
  f(DecisionSource::PreflopChart);
  f(DecisionSource::PostflopHeuristic);
  f(DecisionSource::RiverLp);
  f(DecisionSource::RiverDcfr);
  f(DecisionSource::MultistreetCfr);
}

}  // namespace bs
