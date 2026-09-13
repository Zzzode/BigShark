#include <bs/decision.hpp>
#include <bs/policy.hpp>
#include <bs/service.hpp>

namespace bs {

Decision decide(const Ctx& context) {
  return evaluatePolicy(context);
}

}  // namespace bs
