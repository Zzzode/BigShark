// stage6/chart_preflop.hpp — the pinned chart decision as a single shared
// implementation.
//
// The pinned BaselineBehaviorPolicy plays the deployed heuristic on EVERY
// street, while the composed CandidateBehaviorPolicy plays the deployed charts
// only PREFLOP and its own frozen postflop artifact afterwards. Both must run
// the exact same preflop code path — a duplicated adapt/evaluate/map sequence
// would be a second strategy that could silently drift — so this component is
// the one implementation both call. It builds the deployed Ctx through the
// adapter, calls the exact evaluatePolicySourced entry point production uses
// (river backend forced off), and maps the result to a concrete legal action.
// It lives in bigshark_stage6_eval, the only target allowed to link the
// deployed policy.
#pragma once

#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>

namespace bs::stage6 {

// True while fewer than three public cards have been dealt: the composed
// candidate delegates exactly these decisions to the pinned charts.
bool is_preflop_state(const poker::GameState& state);

// The pinned chart action for `seat` holding `hole`. Mirrors
// BaselineBehaviorPolicy::distribution exactly (the R4 byte-identical corpus
// gate protects that path): the GameState->Ctx adapter, evaluatePolicySourced
// with the river solver disabled, and the legal-action map. Returns one
// concrete action; throws on an illegal deployed decision (never clamps).
poker::Action pinned_chart_action(const poker::GameState& state, std::size_t seat, HoleCards hole,
                                  const PolicyContext& context);

}  // namespace bs::stage6
