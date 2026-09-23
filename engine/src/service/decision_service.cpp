#include <bs/policy.hpp>
#include <bs/service.hpp>

namespace bs {

Decision decide(const Ctx& context) {
  return evaluatePolicy(context);
}

SourcedDecision decideSourced(const Ctx& context) {
  return evaluatePolicySourced(context);
}

}  // namespace bs
