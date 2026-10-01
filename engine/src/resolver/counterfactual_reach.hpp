// Builds the resolver's weighted augmented-game input: the unnormalized
// counterfactual weights, responder -x infosets and m(I), and the centered
// baseline margins b(I) from the locked blueprint.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "resolver_common.hpp"

namespace bs::resolver::detail {

struct ReachModel {
  GameState node;
  // Observed public-action path from the flop root to `node`. GameState is
  // historyless, so the gadget and certifier read it from here to build the
  // candidate information keys.
  std::vector<PublicAction> history;
  std::size_t hero = 0;
  std::size_t seat_count = 0;
  const UnifiedGame* game = nullptr;
  std::vector<Action> node_actions{};

  // Positive-weight joint deals in deterministic order: the hero's holding is
  // the outermost enumeration dimension, then the non-hero seats ascending.
  std::vector<GadgetDeal> deals{};
  // Constant terminal payoff matrix, parallel to deals and node actions: the
  // exact expected net chip utility for every seat, indexed [deal][action]
  // [seat]. Terminal-only continuation makes these constants (there is no
  // in-subtree decision), so the gadget CFR reads them rather than re-walking
  // settlement on every visit. The independent certifier does NOT use this
  // cache; it re-enumerates the runout itself.
  std::vector<std::vector<std::array<double, poker::kMaxUnifiedSeats>>> leaf_values{};
  double total_mass = 0;

  // Positive-mass non-hero "-x" infosets in first-appearance order, each
  // tagged with its seat index.
  std::vector<SeatInfoset> infosets{};
  // Declared, board-unblocked combinations per seat (for the terminal graph
  // check and zero-mass accounting).
  std::array<std::vector<std::array<int, 2>>, poker::kMaxUnifiedSeats> seat_declared{};
  // Hero combos with positive prefix reach at the node (candidate support).
  std::vector<std::array<int, 2>> live_hero{};
  // Declared, unblocked combos of each non-hero seat whose counterfactual
  // mass is zero (indexed by seat; only non-hero seats are populated).
  std::array<std::vector<std::array<int, 2>>, poker::kMaxUnifiedSeats> zero_mass{};
};

// Constructs the model. `history` is the observed public-action path from the
// blueprint game's flop root to `node`. Returns ResolveStatus::Certified on
// success; any other status names the fail-closed reason and `detail`
// describes it. Never throws for a coverage/identity failure.
ResolveStatus build_model(const GameState& node, std::span<const PublicAction> history,
                          const BlueprintSource& blueprint, Budget& budget, ReachModel& model,
                          std::string& detail);

}  // namespace bs::resolver::detail
