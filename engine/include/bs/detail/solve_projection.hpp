// RFC 0008 §L4 internal: the two-seat GameDef -> HeadsUpRoot field projection.
//
// Exposed in a detail header ONLY so the conformance suite can unit-test the
// preflop arm directly: a full unconditioned preflop public tree is not
// materializable (its river chance fan-out exceeds the L3 node cap), so that
// profile cannot be driven end-to-end through solve() under default limits,
// but the projection is a pure function and is verified against the legacy
// HeadsUpState here. Callers go through solve(); nothing else should use this.
#pragma once

#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>

namespace bs::solver::detail {

// Projects a validated two-seat GameDef onto the 1:1 HeadsUpRoot by field copy
// (PlayerChips is a strict prefix of the unified GamePlayer, so no per-state
// projection is needed). A preflop root carries three -1 flop sentinels and
// the posted blinds; a rooted flop carries its three board cards with zero
// posted blinds.
poker::HeadsUpRoot project_heads_up_root(const poker::GameDef& def);

}  // namespace bs::solver::detail
