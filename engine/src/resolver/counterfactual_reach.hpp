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
  HeadsUpState node;
  std::size_t hero = 0;
  std::size_t responder = 1;
  const HeadsUpGame* game = nullptr;
  std::vector<Action> node_actions;

  // Positive-weight joint deals in deterministic (hero, responder) order.
  std::vector<GadgetDeal> deals;
  // Constant terminal payoff matrix, parallel to deals: the exact expected
  // RESPONDER net chip utility for each ordered node action, averaged over the
  // remaining public chance. Terminal-only continuation makes these constants
  // (there is no in-subtree decision), so the gadget CFR reads them rather than
  // re-walking settlement on every visit. The independent certifier does NOT
  // use this cache; it re-enumerates the runout itself.
  std::vector<std::vector<double>> leaf_values;
  double total_mass = 0;

  // Positive-mass responder -x infosets in sorted-card order.
  std::vector<ResponderInfoset> infosets;
  // Declared, board-unblocked combinations (for the terminal graph check).
  std::vector<std::array<int, 2>> hero_declared;
  std::vector<std::array<int, 2>> responder_declared;
  // Hero combos with positive prefix reach at the node (candidate support).
  std::vector<std::array<int, 2>> live_hero;
  // Declared, unblocked responder combos whose counterfactual mass is zero.
  std::vector<std::array<int, 2>> zero_mass_responder;
};

// Constructs the model. Returns ResolveStatus::Certified on success; any other
// status names the fail-closed reason and `detail` describes it. Never throws
// for a coverage/identity failure.
ResolveStatus build_model(const HeadsUpState& node, const BlueprintSource& blueprint,
                          Budget& budget, ReachModel& model, std::string& detail);

}  // namespace bs::resolver::detail
