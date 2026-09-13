#include <bs/decision.hpp>
#include <bs/v0_protocol.hpp>
#include <cstdio>
#include <string>

int main() {
  const std::string request = R"({
    "handId":"hand-1",
    "revision":7,
    "street":"turn",
    "blinds":[10,20],
    "position":"BTN",
    "playersInHand":3,
    "effectiveStackBb":84.5,
    "hole":["As","Kh"],
    "board":["Qs","Jh","2c","7d"],
    "pot":280,
    "legal":{
      "actions":["fold","call","raise"],
      "call":120,
      "potOdds":0.3,
      "raiseTo":{"min":360,"max":1690}
    }
  })";
  const bs::Ctx context = bs::v0::parseRequest(request);
  if (context.handId != "hand-1" || context.revision != 7 || context.street != "turn" ||
      context.hole.size() != 2 || context.board.size() != 4 || !context.legal.has("raise") ||
      context.legal.raiseMin != 360 || context.legal.raiseMax != 1690) {
    std::fputs("V0 REQUEST MAPPING FAILED\n", stderr);
    return 1;
  }

  const bs::Decision decision{"raise", 360, "test", 0.6, 0.7};
  for (int iteration = 0; iteration < 10'000; ++iteration) {
    const std::string response = bs::v0::serializeResponse(decision);
    if (response.find("\"action\":\"raise\"") == std::string::npos ||
        response.find("\"amount\":360") == std::string::npos) {
      std::fputs("V0 RESPONSE MAPPING FAILED\n", stderr);
      return 1;
    }
  }

  std::puts("V0 PROTOCOL TESTS PASSED");
  return 0;
}
