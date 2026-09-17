// v1_resident_mapper.hpp — reconstruct a bs::poker::HeadsUpState at the
// current decision node from a validated v1 HandState.
//
// RFC 0002 Stage 8 / RFC 0005: resident blueprints cover postflop heads-up,
// no-ante, equal-matched-contribution roots only. The reconstruction is
// fail-closed: anything outside that profile, an unparseable history, or a
// transition the poker engine rejects becomes a deterministic coverage miss
// that the caller maps to UNSUPPORTED_FEATURE for forced BLUEPRINT (or a
// heuristic fallback for AUTOMATIC). It never invents a root or clamps an
// amount.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <optional>

#include "v1_mappers.hpp"

namespace bs::v1 {

struct ReconstructedPostflop {
  bs::poker::HeadsUpRoot root{};
  std::optional<bs::poker::HeadsUpState> state;
  std::array<int, 2> hero_cards{-1, -1};
  std::size_t hero_actor = 0;
};

// Returns true and fills `out` only when the request describes a replayable
// postflop heads-up hand whose reconstructed node agrees with the structured
// chip fields (stacks, street commitments, pot, to call). On failure no root
// is returned and `miss` carries the resident-vocabulary coverage reason.
bool reconstructPostflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                         V1BlueprintMiss& miss);

}  // namespace bs::v1
