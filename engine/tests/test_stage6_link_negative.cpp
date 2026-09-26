// RFC 0008 stage 6 offline link-negative guard.
//
// This binary links the live decision surface -- decision service and both
// wire protocols -- and exercises one service call path, so the linker pulls
// every object those libraries actually need. A companion CTest (registered
// in CMakeLists.txt) runs `nm` over this executable and FAILS if any symbol
// from the bs::stage6 namespace (or the bigshark_behavior library) is present.
// The configure-time transitive-link guard catches the dependency declaration;
// this binary proves the final link really carries no offline object code.
#include <bs/decision.hpp>
#include <bs/policy.hpp>
#include <bs/v0_protocol.hpp>
#include <string>

int main() {
  // Drive one parse -> serialize path so service/v0 objects are pulled.
  const std::string request =
      R"({"handId":"guard","street":"preflop","position":"BTN","playersInHand":2,)"
      R"("blinds":[1,2],"hole":["As","Kh"],"legal":{"actions":["fold","raise"],)"
      R"("call":0,"raiseTo":{"min":4,"max":200}}})";
  bs::Ctx ctx = bs::v0::parseRequest(request);
  const std::string response = bs::v0::serializeResponse(bs::evaluatePolicy(ctx));
  return response.empty() ? 1 : 0;
}
