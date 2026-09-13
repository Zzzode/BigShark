// range_tracker.h — per-street action-line range narrowing (L2 core).
//
// Seeds both sides over the non-board deck and applies a sequence of
// deterministic continuation weights driven by what each side did on the flop
// and turn (actor-tagged action codes produced by the JS HU round splitter):
//   aggression (b/r) keeps strong made hands + credible draws + a capped
//   bluff fraction of air (polar); a call keeps medium made hands + draws and
//   removes air; checks down-weight nutted hands that would have bet.
// This is an *approximate* constructor (not itself an equilibrium), but it
// differentiates ranges by the actual action line and feeds the exact river
// solver differentiated, weighted combos instead of a uniform slice.
#pragma once
#include <array>
#include <bs/range.hpp>
#include <vector>

namespace bs::gto {

struct TrackedRangesImpl {
  Range ip;   // solver first actor (acts at root)
  Range oop;  // solver responder
};

struct TrackOptionsImpl {
  int cap = 24;            // max positive-weight combos per side
  int preflop_raises = 0;  // 0 limped, 1 single raise/3bet, >=2 4bet+
  bool hero_was_aggressor = false;
};

// Build the two river ranges.
//   board        5 card ids
//   hero_combo   hero's global combo id (always retained with weight 1)
//   hero_is_ip   hero occupies the solver first-actor side on the river
//   flop/turn    actor-tagged line, e.g. "Hx,Ob,Hc" (see cpp-engine splitRounds)
TrackedRangesImpl TrackRiverRangesImpl(const std::vector<int>& board, int hero_combo,
                                       bool hero_is_ip, const std::string& flop_line,
                                       const std::string& turn_line, const TrackOptionsImpl& opt);

}  // namespace bs::gto
