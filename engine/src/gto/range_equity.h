// range_equity.h — deterministic showdown equity of one combo vs a weighted
// combo range, filling the board from a flop (3) or turn (4) prefix. Used by
// the per-street range tracker so continuation decisions use real pot equity
// vs the opponent's modeled range instead of fixed hand-category tiers.
#pragma once
#include <array>
#include <bs/range.hpp>
#include <cstdint>
#include <vector>

namespace bs::gto {

// Equity of `hole` vs weighted range `opp` on the given board prefix.
// Opponent combos are sampled proportional to weight (card conflicts with
// hole/board are excluded); missing board cards are sampled uniformly. Returns
// P(win) + 0.5*P(chop). Deterministic in `seed`; `iterations` trades noise for
// time (800–2000 is plenty for range-continue gating).
double RangeVsRangeEquity(const std::array<int, 2>& hole, const std::vector<int>& board_prefix,
                          const Range& opp, int iterations, uint64_t seed);

}  // namespace bs::gto
