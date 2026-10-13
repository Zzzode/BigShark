// RFC 0013 Stage 2: shared postflop game configuration.
//
// The representative postflop GameDef and SPR-dependent action abstraction
// are needed by both the oracle (which solves the games) and the on-the-fly
// evaluator (which rebuilds the tree to compute missing entries). They live
// here so both sides construct identical trees.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>

namespace bs::gto {

// Build the representative postflop GameDef for a canonical flop class and
// SPR bucket. The pot is fixed at 100 chips (scale-invariant); each seat
// contributed 50 (equal, required by 2-seat rooted-board validation). The
// stack is set from the representative SPR so that SPR = stack / pot.
poker::GameDef make_postflop_def(const std::array<int, 3>& canonical_flop, std::size_t spr_bucket);

// SPR-dependent postflop action abstraction (r1).
//
// Tree size grows exponentially with SPR because deeper stacks allow more
// bets/raises per street. Using the full menu at high SPR produces trees
// that exceed the byte/node caps. This function reduces the menu at higher
// SPR to keep trees tractable while preserving the most important bet sizes.
//
//   Buckets 0-1 (SPR 0.5-1.5):  default  (1/3, 3/4, 3/2 bets; 1/2, 1/1 raises)
//   Buckets 2-3 (SPR 3.0-5.5):  reduced  (1/2, 1/1 bets; 1/1 raises)
//   Buckets 4-5 (SPR 9.5-15):   minimal  (1/1 bets; 1/1 raises)
abstraction::ActionAbstraction make_postflop_abstraction(std::size_t spr_bucket);

}  // namespace bs::gto
