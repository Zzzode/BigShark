// response_mapper.hpp — RFC 0009 W4e DecisionResponse -> poker::Action mapper.
//
// Maps the engine's DecisionResponse (strategy or expanded_strategy) to a
// concrete poker::Action, replicating the engine's protocol sampler for the
// case where no selected_action is present. On any error (engine error,
// missing target_total, illegal action) falls back to check/call/fold,
// matching the host's operationalFallbackDecision.
#pragma once

#include <bigshark/engine/v1/engine.pb.h>

#include <bs/engine_client/engine_client.hpp>
#include <bs/heads_up.hpp>
#include <cstdint>
#include <string>

namespace bs::engine_client {

namespace pv = ::bigshark::engine::v1;

// The result of mapping a DecisionResponse: a concrete action, whether the
// policy fell back, a human-readable reason (empty when served), and the
// solver's guarantee level when present.
struct MappedDecision {
  bs::poker::Action action{};
  bool fell_back = false;
  std::string reason;
  std::string guarantee_level;
};

// Maps a DecisionResponse to a concrete action. `legal` is the live state's
// legal actions, used both for the aggressive verb mapping (a preflop RAISE
// lands on the state's Bet type at the big-blind option) and for the
// defense-in-depth legality check. `decision_seed` drives the protocol
// sampler when no selected_action is present.
MappedDecision map_decision_response(const pv::DecisionResponse& response,
                                     const bs::poker::LegalActions& legal,
                                     std::uint64_t decision_seed);

}  // namespace bs::engine_client
