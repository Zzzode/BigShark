// decision.hpp — platform-neutral decision domain types.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bs {

struct Legal {
  std::vector<std::string> actions;
  int call = 0;
  double potOdds = 0;
  int raiseMin = 0, raiseMax = 0;
  bool has(const char* a) const {
    for (auto& x : actions)
      if (x == a)
        return true;
    return false;
  }
};

struct Ctx {
  std::string handId;
  long long revision = 0;
  uint64_t seed = 0;
  std::string style = "tag";
  std::string street = "preflop";  // preflop/flop/turn/river
  int sb = 10, bb = 20;
  std::string position = "BTN";
  int playersInHand = 2;
  double effectiveStackBb = 100;
  std::vector<std::string> hole;  // ["As","Kh"]
  std::vector<std::string> board;
  int pot = 0;
  Legal legal;
  int raises = 0;  // preflop raise count before hero
  // River equilibrium control. `riverGtoOn` enables the Nash solver; the
  // current public line is in riverLine (which may be "" at the very first
  // river action, so it cannot double as the enable flag).
  bool riverGtoOn = false;
  std::string riverLine;
  std::string flopLine;  // actor-tagged per-street actions, e.g. "Hx,Ob,Hc"
  std::string turnLine;
  double riverBetFrac = 0.75;
  double riverRaiseFrac = 1.0;
  std::string openerPosition;
  bool heroWasRaiser = false;
  bool heroPreflopAggressor = false;
  int limpers = 0;
  std::vector<double> opponentPcts;  // per-opponent preflop strength gate
};

struct Decision {
  std::string action;
  int amount = 0;
  std::string reason;
  double equity = -1;
  double mdf = -1;
};

}  // namespace bs
