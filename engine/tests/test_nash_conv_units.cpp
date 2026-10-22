// RFC 0029: the shared NashConv normalisation.
//
// Two harnesses report a general-sum NashConv figure —
// `bigshark-subgame-nash-conv` and, from stage 2,
// `bigshark-stage6-resident-nash-conv`. Before this helper each divided by
// `live_seats` and `big_blind` itself, so a change to either divisor could land
// in one harness and not the other. These tests pin the shared definition to an
// independent restatement of the arithmetic, and pin the two inputs for which
// the quotient has no meaning.
#include <bs/nash_conv_units.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace bs::poker;

namespace {

int failures = 0;
void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// A definition with `seats` seats at `big_blind`, and the seats in `folded`
// marked folded. Stacks and contributions are irrelevant to the divisor.
GameDef make_def(std::size_t seats, Chips big_blind, std::initializer_list<std::size_t> folded) {
  GameDef def{};
  def.player_count = seats;
  def.big_blind = big_blind;
  for (std::size_t seat = 0; seat < seats; ++seat)
    def.stacks[seat] = 100;
  for (std::size_t seat : folded) {
    if (seat < seats)
      def.folded[seat] = true;
  }
  return def;
}

void test_three_normalisations_agree_with_inline_arithmetic() {
  // Six seats, one folded: the divisor is five, NOT six.
  const GameDef def = make_def(6, 2, {3});
  const double nash_conv = 17.5;
  const bs::solver::NashConvNormalisation units = bs::solver::normalise_nash_conv(def, nash_conv);

  check(units.live_seats == 5, "the divisor counts live seats, not dealt seats");
  check(std::abs(units.nash_conv - nash_conv) < 1e-12,
        "the raw figure is carried through undivided");
  // Independent restatement of the definition, not a call back into the helper.
  const double expected_exploitability = nash_conv / 5.0;
  const double expected_bb = expected_exploitability / 2.0;
  check(std::abs(units.exploitability - expected_exploitability) < 1e-12,
        "exploitability is nash_conv over live seats");
  check(std::abs(units.exploitability_bb - expected_bb) < 1e-12,
        "exploitability_bb divides the per-seat figure by the big blind");
  // The two normalisations are NOT the same quotient: dividing by the big
  // blind twice, or by seats twice, is the mistake this pins.
  check(std::abs(units.exploitability_bb - nash_conv / 10.0) < 1e-12,
        "the composed divisor is live_seats * big_blind");
}

void test_headsup_and_full_table() {
  const GameDef headsup = make_def(2, 4, {});
  const bs::solver::NashConvNormalisation hu = bs::solver::normalise_nash_conv(headsup, 8.0);
  check(hu.live_seats == 2, "heads-up has two live seats");
  check(std::abs(hu.exploitability - 4.0) < 1e-12, "heads-up exploitability divides by two");
  check(std::abs(hu.exploitability_bb - 1.0) < 1e-12, "heads-up in bb divides by 4");

  const GameDef six = make_def(6, 2, {});
  const bs::solver::NashConvNormalisation s = bs::solver::normalise_nash_conv(six, 6.0);
  check(s.live_seats == 6, "a full six-seat table has six live seats");
  check(std::abs(s.exploitability - 1.0) < 1e-12, "six-seat exploitability divides by six");
  check(std::abs(s.exploitability_bb - 0.5) < 1e-12, "six-seat in bb divides by 2");
}

void test_folding_every_seat_throws() {
  const GameDef def = make_def(3, 2, {0, 1, 2});
  check(bs::solver::live_seat_count(def) == 0, "an all-folded definition has no live seat");
  bool threw = false;
  try {
    (void)bs::solver::normalise_nash_conv(def, 1.0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "normalising by zero live seats throws instead of producing infinity");
}

void test_zero_big_blind_throws() {
  // Chips is std::uint64_t, so a negative big blind cannot exist and asserting
  // one would be vacuous. Zero is the only reachable non-positive value.
  const GameDef zero = make_def(2, 0, {});
  bool threw = false;
  try {
    (void)bs::solver::normalise_nash_conv(zero, 1.0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "a zero big blind has no unit and throws");
}

void test_live_seat_count_counts_non_folded() {
  check(bs::solver::live_seat_count(make_def(2, 2, {})) == 2, "two non-folded seats");
  check(bs::solver::live_seat_count(make_def(6, 2, {0, 5})) == 4, "two folds leave four live");
  check(bs::solver::live_seat_count(make_def(10, 2, {1, 2, 3, 4, 5, 6, 7, 8})) == 2,
        "eight folds leave two live");
}

}  // namespace

int main() {
  test_three_normalisations_agree_with_inline_arithmetic();
  test_headsup_and_full_table();
  test_folding_every_seat_throws();
  test_zero_big_blind_throws();
  test_live_seat_count_counts_non_folded();
  if (failures) {
    std::printf("NASH CONV UNITS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("NASH CONV UNITS PASSED");
  return 0;
}
