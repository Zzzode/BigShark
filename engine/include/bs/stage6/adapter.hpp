// stage6/adapter.hpp — GameState -> deployed Ctx observation adapter.
//
// RFC 0008 stage 6 measures the PINNED baseline heuristic through the exact
// same evaluatePolicySourced entry point production uses. The unified
// GameState is historyless by design (game_definition.hpp:20-26), but the
// heuristic's Ctx consumes observation-derived fields: preflop raise count,
// limpers, opener position, whether the seat made the last preflop raise,
// players-in-hand, and the street/pot/legal snapshot. The stage-6 simulator
// keeps an explicit HandLog (declared in bs/behavior_policy.hpp) and this
// adapter derives exactly those fields.
//
// The adapter is the ONE component allowed to include both L1
// (game_definition.hpp) and policy vocabulary (decision.hpp), which is why it
// lives in bigshark_stage6_eval rather than in poker, policy, or behavior.
// Its byte-identical-decisions gate (R4) compares its output against the
// deployed parseRequest -> evaluatePolicySourced path on a fixed corpus; a
// mismatch fails the harness typed rather than being clamped away.
#pragma once

#include <bs/behavior_policy.hpp>  // HoleCards / HandLog
#include <bs/decision.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bs::stage6 {

// Static adapter configuration constant for a fixture. Seeds are passed per
// decision so the byte-identical corpus can pin them.
struct AdapterConfig {
  // The deployed style vocabulary ("tag" is the production default).
  std::string style = "tag";
};

// Maps a unified action state for `seat` holding `hole` to the deployed Ctx,
// deriving every observation field from `log`. The returned Ctx has the river
// LP/DCFR solver disabled so no figure depends on a HiGHS-present build.
// Throws std::invalid_argument when `state` is not `seat`'s action phase.
Ctx adapt_to_ctx(const poker::GameState& state, std::size_t seat, const HoleCards& hole,
                 const HandLog& log, std::uint64_t decision_seed, const AdapterConfig& config = {});

// Canonical seat -> chart position vocabulary for N seats counted clockwise
// from the button (offset 0). Committed mapping (F7):
//   0 -> BTN, 1 -> SB (2+ seats), 2 -> BB,
//   3 -> UTG, interior -> MP, n-2 -> HJ, n-1 -> CO;
//   heads-up: 0 -> BTN, 1 -> BB (the button posts the small blind).
// The n=3/6/8 bucket switches the charts apply live in rfiBucket/
// openerBucket (decision.cpp:38-55); this mapping only has to name seats
// consistently with those buckets.
std::string position_vocabulary(std::size_t player_count, std::size_t button_relative_offset);

// Card id (rank*4+suit) -> the two-character name the Ctx uses ("As", "Th").
std::string card_name(int card_id);

}  // namespace bs::stage6
