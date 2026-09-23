#include <bs/guarantee.hpp>

namespace bs {

int guaranteeRank(Guarantee level) {
  return static_cast<int>(level);
}

bool guaranteeMeets(Guarantee achieved, Guarantee required) {
  return guaranteeRank(achieved) >= guaranteeRank(required);
}

const char* guaranteeToken(Guarantee level) {
  switch (level) {
    case Guarantee::OperationalFallback:
      return "operational_fallback";
    case Guarantee::Approximate:
      return "approximate";
    case Guarantee::AbstractSolved:
      return "abstract_solved";
    case Guarantee::ExactSolved:
      return "exact_solved";
    case Guarantee::CertifiedBound:
      return "certified_bound";
  }
  return "operational_fallback";
}

Guarantee guaranteeFor(DecisionSource source) {
  switch (source) {
    case DecisionSource::PreflopChart:
    case DecisionSource::PostflopHeuristic:
    case DecisionSource::RiverLp:
    case DecisionSource::RiverDcfr:
    case DecisionSource::MultistreetCfr:
      return Guarantee::Approximate;
  }
  return Guarantee::OperationalFallback;
}

}  // namespace bs
