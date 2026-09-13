#include <array>
#include <bs/equity.hpp>
#include <bs/eval.hpp>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace bs;

static std::array<int, 2> hole(const char* a, const char* b) {
  return {cardId(a), cardId(b)};
}
static std::vector<int> board(std::initializer_list<const char*> cs) {
  std::vector<int> v;
  for (auto c : cs)
    v.push_back(cardId(c));
  return v;
}

int main() {
  int wheelDraw[] = {cardId("As"), cardId("2h"), cardId("3d"), cardId("4c")};
  int noStraightDraw[] = {cardId("2s"), cardId("3h"), cardId("8d"), cardId("Kc")};
  if (!isPotentialDraw(wheelDraw, 4) || isPotentialDraw(noStraightDraw, 4)) {
    std::fputs("DRAW CLASSIFICATION TESTS FAILED\n", stderr);
    return 1;
  }

  struct Spot {
    const char* name;
    std::array<int, 2> h;
    std::vector<int> b;
    double gate;
  };
  Spot spots[] = {
      {"AA vs top70% preflop ", hole("As", "Ah"), {}, .30},
      {"72o vs top70% preflop", hole("7s", "2d"), {}, .30},
      {"AsKs FD on 2s7sQh   ", hole("As", "Ks"), board({"2s", "7s", "Qh"}), .30},
      {"AhKh overcards A72r ", hole("Ah", "Kh"), board({"As", "7s", "2h"}), .30},
      {"77 underpair on AK2  ", hole("7h", "7d"), board({"As", "Kc", "2s"}), .30},
      {"77 set on AK7        ", hole("7h", "7d"), board({"As", "Kc", "7s"}), .30},
      {"TPTK AK on A72       ", hole("Ah", "Kd"), board({"Ac", "7s", "2d"}), .30},
  };

  for (auto& sp : spots) {
    EquityOpts o;
    o.minPct = {sp.gate};
    o.iterations = 6000;
    o.seed = 12345;
    auto t0 = std::chrono::high_resolution_clock::now();
    auto r = equityVsAll(sp.h, sp.b, o);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("%s eq=%.3f n=%d  %.1fms\n", sp.name, r.equity, r.iterations, ms);
  }

  // multiway sanity: AA 3 ways should be lower than heads-up
  EquityOpts h;
  h.minPct = {.3};
  h.iterations = 6000;
  h.seed = 1;
  EquityOpts m;
  m.minPct = {.3, .3, .3};
  m.iterations = 6000;
  m.seed = 1;
  auto eh = equityVsAll(hole("As", "Ah"), {}, h);
  auto em = equityVsAll(hole("As", "Ah"), {}, m);
  std::printf("AA heads-up %.3f vs 3-way %.3f %s\n", eh.equity, em.equity,
              eh.equity > em.equity ? "OK" : "FAIL");
}
