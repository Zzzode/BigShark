// v1_resident_mapper.hpp — reconstruct a bs::poker::GameState at the
// current decision node from a validated v1 HandState.
//
// RFC 0002 Stage 8 / RFC 0005 / RFC 0009 W2c-ii-a: resident blueprints cover
// postflop, no-ante, equal-matched-contribution roots for 2..10 seats. The
// RFC 0021 additionally reconstructs minor-2 multiway requests with supplied
// preflop history from hand start, preserving the true flop root and folds.
// Refunds and unsupported extended utility still refuse. Supplied replayable
// history is not a provider completeness or strategy-quality certificate.
// Reconstruction is fail-closed: anything outside the selected profile, an unparseable
// history, or a transition the poker engine rejects becomes a deterministic
// coverage miss that the caller maps to UNSUPPORTED_FEATURE for forced
// BLUEPRINT (or a heuristic fallback for AUTOMATIC). It never invents a root
// or clamps an amount.
//
// GameState is historyless, so the observed public-action path rides
// alongside the reconstructed state as a PublicAction log: it is the only
// record of which seat did what on which street to reach the node, and the
// resident/resolver key builders consume it explicitly.
#pragma once

#include <bigshark/engine/v1/engine.pb.h>

#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/v1_protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bs::v1 {

namespace pv = ::bigshark::engine::v1;

struct ReconstructedPostflop {
  // The seat-generic reconstruction at the current decision node.
  std::optional<bs::poker::GameState> state;
  // The observed public-action path from the selected root to `state`:
  // preflop actions for a preflop root, postflop actions for a flop root.
  // Hand-start reconstruction rebases before any flop action, so preflop
  // actions never enter existing flop-root service keys.
  std::vector<bs::poker::PublicAction> history;
  std::array<int, 2> hero_cards{-1, -1};
  std::size_t hero_actor = 0;
  // W2c-ii-a gate 1 differential oracle: the shipped two-seat HeadsUpState
  // reconstruction, populated only at two seats so tests can assert the
  // GameState path agrees with it field for field.
  std::optional<bs::poker::HeadsUpState> oracle_state;
};

// Legacy/default profile: a replayable postflop hand with equal matched
// contributions. Minor 2 selects exact hand-start replay for 3..10 seats
// when supplied history includes preflop events; it preserves unequal dead
// money from preflop folds. Strict failure never retries legacy inference.
// Returns true and fills `out` only when the selected profile's
// reconstructed node agrees with the structured chip fields (stacks, street
// commitments, pot, to call). On failure no state is returned and `miss`
// carries the resident-vocabulary coverage reason.
bool reconstructPostflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                         V1BlueprintMiss& miss, std::uint32_t negotiated_minor = 1);

// RFC 0005 Stage 9 resolver-path variant. It is identical to reconstructPostflop
// except it admits a postflop decision node reached facing an all-in: a
// non-acting seat may already be ALL_IN, which the BLUEPRINT gate still
// rejects. The acting hero must retain chips (a genuine fold/call decision).
// Exact ledger and terminal cross-checks are unchanged.
bool reconstructPostflopForResolve(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                                   V1BlueprintMiss& miss, std::uint32_t negotiated_minor = 1);

// RFC 0007 / RFC 0019 preflop reconstruction. Minor 1 keeps the heads-up
// profile; negotiated minor 2 admits 2..10 dealt seats, preserving folded
// seats and checking their replayed status. Equal hand-start stacks and the
// existing no-side-pot utility profile remain required. This accepts a
// request and builds the flop-terminal GameDef (board_size=0, preflop=true,
// blinds_posted derived from forced contributions, terminal=Flop) at the
// current preflop action node. The published artifact is heads-up 100 BB;
// a supported minor-2 request with a different artifact identity can
// reconstruct successfully but still miss at the resident root match.
// Folds, checks, calls, bets, and raises in the preflop action history are
// replayed as PublicAction{Street::Preflop, ...}. The same fail-closed
// contract applies: anything outside the profile is a deterministic coverage
// miss, never an invented root.
bool reconstructPreflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                        V1BlueprintMiss& miss, std::uint32_t negotiated_minor = 1);

}  // namespace bs::v1
