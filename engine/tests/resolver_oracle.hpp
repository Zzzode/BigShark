// Independently written resolver test oracle for RFC 0005 Stage 9.
//
// This is a SECOND, from-scratch enumeration used to cross-check both the
// production gadget CFR and the production certifier, the way Stage 6
// cross-checked response_value_with_reach. It does not include any resolver
// header and never calls the resolver traversal, the certifier, or the
// solver's response(). It rebuilds:
//   - the terminal leaf utilities through the public poker rules and exact
//     settlement (its own runout enumeration and uniform 1/N chance factors);
//   - the per-infoset counterfactual mass and centered baseline margin from
//     its own deal-weight reach factors;
//   - the responder best response and the augmented-game NashConv by
//     independently enumerating each player's unilateral pure deviations.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace bs::resolver_test {

struct OracleDeal {
  std::array<int, 2> hero{};
  std::array<int, 2> responder{};
  double weight = 1;  // unnormalized joint counterfactual weight
};

// Whole-range hero strategy: probability of each ordered node action per hero
// holding. The map must cover every hero combo that appears in a positive-
// weight deal.
using HeroStrategy = std::map<std::array<int, 2>, std::vector<double>>;
// Responder gadget strategy per responder holding: {TERMINATE, CONTINUE}.
using ResponderStrategy = std::map<std::array<int, 2>, std::array<double, 2>>;
// Baseline node policy used to center b(I), per hero holding.
using BaselineStrategy = HeroStrategy;

class TerminalOracle {
 public:
  TerminalOracle(bs::poker::HeadsUpState facing_node, std::vector<OracleDeal> deals,
                 std::vector<bs::poker::Action> actions,
                 std::array<std::optional<int>, 2> fixed_runout);

  // Exact expected RESPONDER net chip utility of one node action for one deal,
  // independently averaging the remaining uniform public chance.
  double leaf_responder(const OracleDeal& deal, std::size_t action) const;

  double total_mass() const { return total_mass_; }

  // Unnormalized mass m(I) for one responder holding (zero when absent).
  double mass(std::array<int, 2> responder) const;
  // Centered baseline margin b(I); asserts positive mass.
  double baseline_margin(std::array<int, 2> responder, const BaselineStrategy& baseline) const;
  // Candidate locked-continuation value divided by m(I).
  double candidate_margin(std::array<int, 2> responder, const HeroStrategy& candidate) const;

  struct NashReport {
    double nash_conv = 0;  // chance-normalized, sum of both players' gains
    double responder_gain = 0;
    double hero_gain = 0;
  };
  // Independently enumerates pure best responses against the given augmented-
  // game profile and returns the chance-normalized NashConv. The constant
  // TERMINATE payoff b(I) per responder infoset is supplied explicitly so the
  // enumeration depends on no resolver table.
  NashReport nash_conv(const ResponderStrategy& responder, const HeroStrategy& hero,
                       const std::map<std::array<int, 2>, double>& terminate_payoff) const;

  const std::vector<std::array<int, 2>>& responder_infosets() const { return responder_order_; }
  const std::vector<std::array<int, 2>>& hero_infosets() const { return hero_order_; }

 private:
  double runout_responder(const bs::poker::HeadsUpState& state,
                          const std::array<std::array<int, 2>, 2>& hands) const;

  bs::poker::HeadsUpState node_;
  std::vector<OracleDeal> deals_;
  std::vector<bs::poker::Action> actions_;
  std::array<std::optional<int>, 2> fixed_runout_;
  std::size_t hero_ = 1;
  std::size_t responder_ = 0;
  double total_mass_ = 0;
  std::vector<std::array<int, 2>> responder_order_;
  std::vector<std::array<int, 2>> hero_order_;
};

}  // namespace bs::resolver_test
