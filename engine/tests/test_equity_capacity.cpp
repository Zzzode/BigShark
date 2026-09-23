// RFC 0008 stage 6 precursor: equityVsAll must accept one opponent per seat
// at the engine's full declared capacity (2..10 seats => up to 9 opponents).
//
// The Monte Carlo opponent-hand buffer used to be a fixed six-slot array while
// EquityOpts.minPct could carry one gate per opponent for any seat count, so a
// postflop decision at 8/9/10 seats indexed past the array (a stack
// out-of-bounds write on a legal request) and left the stage-6 baseline
// undefined exactly at the seat count (10) the RFC requires it to be measured
// at. This suite runs under AddressSanitizer in the asan preset; on the old
// fixed buffer it trapped the moment the seventh opponent drew a hand.
#include <array>
#include <bs/equity.hpp>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace bs;

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

std::array<int, 2> hole(const char* a, const char* b) {
  return {cardId(a), cardId(b)};
}

std::vector<int> board(std::initializer_list<const char*> cards) {
  std::vector<int> out;
  for (const char* card : cards)
    out.push_back(cardId(card));
  return out;
}

}  // namespace

int main() {
  // At every legal seat count the Monte Carlo estimate must run to completion
  // without a bounds violation and return a finite probability in [0, 1].
  // Gate zero keeps every drawn hand (the postflop continue filter is off), so
  // the opponent slots actually fill and the previously overflowing writes at
  // nDrawn == 6..8 are exercised.
  for (int opponents = 1; opponents <= 9; ++opponents) {
    EquityOpts opts;
    opts.minPct = std::vector<double>(opponents, 0.0);
    opts.iterations = 400;
    opts.postflopContinue = false;
    opts.seed = 987654321ULL;
    const EquityResult result = equityVsAll(hole("As", "Kh"), board({"2c", "7d", "9s"}), opts);
    check(result.iterations > 0, "the ten-seat estimate produces valid iterations");
    check(std::isfinite(result.equity) && result.equity >= 0.0 && result.equity <= 1.0,
          "the ten-seat equity estimate is a probability in [0, 1]");
  }

  // An oversized gate list (e.g. an untrusted external opponentPcts array) is
  // clamped to the nine-opponent capacity rather than writing past the buffer.
  {
    EquityOpts opts;
    opts.minPct = std::vector<double>(50, 0.0);
    opts.iterations = 200;
    opts.postflopContinue = false;
    opts.seed = 555;
    const EquityResult result = equityVsAll(hole("As", "Kh"), board({"2c", "7d", "9s"}), opts);
    check(std::isfinite(result.equity) && result.iterations > 0,
          "an oversized opponent-gate list is clamped, not overflowed");
  }

  // An empty gate list must not be read out of bounds: nOpp floors at one and
  // the missing gate falls back to keep-every-hand (the historical default).
  {
    EquityOpts opts;
    opts.minPct = {};
    opts.iterations = 200;
    opts.postflopContinue = false;
    opts.seed = 777;
    const EquityResult result = equityVsAll(hole("As", "Kh"), board({"2c", "7d", "9s"}), opts);
    check(std::isfinite(result.equity) && result.iterations > 0,
          "an empty opponent-gate list falls back safely, no out-of-bounds read");
  }

  // Ten-seat capacity stays reproducible from its seed (the published seed
  // list the measurement stage builds on relies on this).
  EquityOpts a;
  a.minPct = std::vector<double>(9, 0.0);
  a.iterations = 200;
  a.postflopContinue = false;
  a.seed = 42;
  EquityOpts b = a;
  const EquityResult ra = equityVsAll(hole("As", "Kh"), board({"2c", "7d", "9s"}), a);
  const EquityResult rb = equityVsAll(hole("As", "Kh"), board({"2c", "7d", "9s"}), b);
  check(ra.equity == rb.equity && ra.iterations == rb.iterations,
        "the full-table estimate is seed-reproducible");

  if (failures != 0) {
    std::fprintf(stderr, "EQUITY CAPACITY TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("EQUITY CAPACITY TESTS PASSED");
  return 0;
}
