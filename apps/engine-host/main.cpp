// main.cpp — v0 engine host entry point.
//   default:  one decision, JSON context on stdin -> JSON decision on stdout
//   --serve:  persistent line-delimited coprocess: read one JSON object per
//             stdin line, print one JSON decision per stdout line (flush each).
#include <bs/decision.hpp>
#include <bs/service.hpp>
#include <bs/v0_protocol.hpp>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

static std::string decideJson(const std::string& raw) {
  bs::Decision d;
  try {
    bs::Ctx ctx = bs::v0::parseRequest(raw);
    d = bs::decide(ctx);
  } catch (const std::exception&) {
    return "{\"action\":\"fold\",\"amount\":0,\"reason\":\"parse-error\"}";
  }
  return bs::v0::serializeResponse(d);
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  bool serve = false;
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--serve")
      serve = true;

  if (serve) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        continue;
      std::cout << decideJson(line) << '\n' << std::flush;
    }
    return 0;
  }

  std::ostringstream ss;
  ss << std::cin.rdbuf();
  std::cout << decideJson(ss.str()) << '\n';
  return 0;
}
