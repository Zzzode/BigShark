// RFC 0029: the one normalisation every general-sum NashConv harness reports.
//
// Three different quantities are routinely called "NashConv", and the
// difference is a factor of (live seats) and (big blind). A harness that
// divides twice, or divides by the dealt seat count instead of the live one,
// publishes a number that looks like a quality measurement and is not
// comparable to any other figure in the tree. This header defines them once so
// `bigshark-subgame-nash-conv` and `bigshark-stage6-resident-nash-conv` cannot
// drift apart:
//
//   nash_conv        = sum over LIVE seats of (br_value - strategy_value),
//                      in chips per hand. NOT normalised.
//   exploitability   = nash_conv / live_seats, in chips per hand.
//   exploitability_bb = exploitability / big_blind, in big blinds per hand.
//
// "Live" means every seat the definition does not mark folded, which is the
// seat set both harnesses walk. It is deliberately NOT `player_count`: a
// folded seat contributes nothing to the sum and must not be counted in the
// divisor either.
#pragma once

#include <bs/game_definition.hpp>
#include <cstddef>

namespace bs::solver {

// The divisor nash_conv is normalised by. Exposed separately so a caller can
// report the seat count it actually used rather than assuming.
std::size_t live_seat_count(const poker::GameDef& def);

// The three normalisations, computed together from one nash_conv figure.
struct NashConvNormalisation {
  // Chips per hand, summed over live seats. Never divided.
  double nash_conv = 0.0;
  // Chips per hand per live seat.
  double exploitability = 0.0;
  // Big blinds per hand per live seat.
  double exploitability_bb = 0.0;
  // The live seat count used as the divisor, so a report can print it.
  std::size_t live_seats = 0;
};

// Computes all three. Throws std::invalid_argument when the definition has no
// live seat (the quotient is undefined) or a non-positive big blind (there is
// no unit to express the figure in).
NashConvNormalisation normalise_nash_conv(const poker::GameDef& def, double nash_conv);

}  // namespace bs::solver
