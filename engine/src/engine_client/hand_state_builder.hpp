// hand_state_builder.hpp — RFC 0009 W4e GameState -> DecisionRequest builder.
//
// Translates the unified GameState + seat + hole cards + HandLog into the v1
// protobuf DecisionRequest the engine expects. The action history is
// reconstructed by exact replay from the root: the HandLog records only
// {seat, action}, so pot_before/stack_after/all_in are derived by replaying
// the deterministic machine through the logged actions.
//
// Preflop aggressive verb convention (critical): the unified engine types
// preflop aggression as Bet, but the server vocabulary is raise. The builder
// maps preflop Bet/Raise -> ACTION_TYPE_RAISE in both legal_actions and
// action_history, matching the adapter and the server's request mapper.
#pragma once

#include <bigshark/engine/v1/engine.pb.h>

#include <bs/behavior_policy.hpp>
#include <bs/engine_client/engine_client.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>

namespace bs::engine_client {

namespace pv = ::bigshark::engine::v1;

// Builds a DecisionRequest for the engine. Throws std::invalid_argument if the
// state is not an action state for `hero_seat`, or std::runtime_error if the
// replay invariant check fails (HandLog inconsistent with state).
pv::DecisionRequest build_decision_request(const bs::poker::GameState& state, std::size_t hero_seat,
                                           const bs::stage6::HoleCards& hole,
                                           const bs::stage6::HandLog* log,
                                           std::uint64_t decision_seed,
                                           const EngineClientConfig& config,
                                           const pv::GetCapabilitiesResponse& capabilities);

}  // namespace bs::engine_client
