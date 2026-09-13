#include <bs/eval.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace bs;

static int ev(std::initializer_list<const char*> cards) {
  int ids[8], n = 0;
  for (auto c : cards)
    ids[n++] = cardId(c);
  return evaluate(ids, n).cat;
}

int main() {
  struct Case {
    const char* cs[7];
    int n, cat;
  };
  auto t5 = [&](const char* a, const char* b, const char* c, const char* d, const char* e,
                int cat) {
    int got = ev({a, b, c, d, e});
    std::printf("%s %s %s %s %s -> %d %s\n", a, b, c, d, e, got, got == cat ? "OK" : "FAIL");
    assert(got == cat);
  };
  t5("As", "Ks", "Qs", "Js", "Ts", 9);
  t5("9h", "8h", "7h", "6h", "5h", 9);
  t5("As", "Ah", "Ad", "Ac", "Kh", 8);
  t5("As", "Ah", "Ad", "Kc", "Kd", 7);
  t5("Ah", "4h", "7h", "2h", "Kh", 6);
  t5("9c", "8d", "7s", "6h", "5c", 5);
  t5("As", "Ah", "Ad", "Kc", "Qd", 4);
  t5("As", "Ah", "Kd", "Kc", "Qd", 3);
  t5("As", "Ah", "Kd", "9c", "2d", 2);
  t5("As", "Kd", "Qh", "9c", "2d", 1);
  t5("Ah", "2c", "3d", "4s", "5h", 5);  // wheel

  if (straightHigh(0b1111) != 0 || straightHigh((1 << 12) | 0b1111) != 3) {
    std::fputs("STRAIGHT BOUNDARY TESTS FAILED\n", stderr);
    return 1;
  }

  // 7-card best-five
  assert(ev({"As", "Ks", "Qs", "Js", "Ts", "2c", "3d"}) == 9);
  assert(ev({"2s", "2h", "2d", "7c", "7d", "Kc", "3h"}) == 7);  // boat
  assert(ev({"As", "Ah", "Ks", "Kh", "2s", "2h", "9d"}) == 3);  // two pair (best 5 = two pair)

  // ordering sanity: AA > KK on dry board compare scores
  int idsA[] = {cardId("As"), cardId("Ah"), cardId("Kc"), cardId("7d"), cardId("2h")};
  int idsK[] = {cardId("Ks"), cardId("Kh"), cardId("Ac"), cardId("7d"), cardId("2h")};
  assert(evaluate(idsA, 5).score != evaluate(idsK, 5).score);  // both one pair; AA higher
  assert(evaluate(idsA, 5).score > evaluate(idsK, 5).score);
  (void)idsA;
  (void)idsK;  // kept meaningful under NDEBUG
  std::puts("ALL EVAL TESTS PASSED");
}
