#include "resolver_oracle.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <stdexcept>

namespace bs::resolver_test {
namespace {

using bs::poker::Action;
using bs::poker::HeadsUpState;
using bs::poker::Phase;

bool share_card(std::array<int, 2> a, std::array<int, 2> b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

}  // namespace

TerminalOracle::TerminalOracle(HeadsUpState facing_node, std::vector<OracleDeal> deals,
                               std::vector<Action> actions,
                               std::array<std::optional<int>, 2> fixed_runout)
    : node_(std::move(facing_node)),
      deals_(std::move(deals)),
      actions_(std::move(actions)),
      fixed_runout_(fixed_runout) {
  if (!node_.actor())
    throw std::invalid_argument("oracle node is not an action node");
  hero_ = *node_.actor();
  responder_ = 1 - hero_;
  for (const OracleDeal& deal : deals_) {
    if (!(deal.weight > 0) || !std::isfinite(deal.weight))
      throw std::invalid_argument("oracle deal has non-positive weight");
    if (share_card(deal.hero, deal.responder))
      throw std::invalid_argument("oracle deal hands share a card");
    total_mass_ += deal.weight;
    if (std::find(responder_order_.begin(), responder_order_.end(), deal.responder) ==
        responder_order_.end())
      responder_order_.push_back(deal.responder);
    if (std::find(hero_order_.begin(), hero_order_.end(), deal.hero) == hero_order_.end())
      hero_order_.push_back(deal.hero);
  }
  std::sort(responder_order_.begin(), responder_order_.end());
  std::sort(hero_order_.begin(), hero_order_.end());
  if (!(total_mass_ > 0))
    throw std::invalid_argument("oracle has no mass");
}

double TerminalOracle::runout_responder(const HeadsUpState& state,
                                        const std::array<std::array<int, 2>, 2>& hands) const {
  if (state.phase() == Phase::Folded)
    return static_cast<double>(state.settle_fold().net_utility[responder_]);
  if (state.phase() == Phase::Showdown)
    return static_cast<double>(state.settle_showdown(hands).net_utility[responder_]);
  if (state.phase() != Phase::Deal)
    throw std::invalid_argument("oracle reached an unexpected action node");

  // Independently enumerate the uniform public-chance support.
  const std::size_t slot = state.board().size() - 3;
  std::vector<int> cards;
  if (fixed_runout_[slot]) {
    cards.push_back(*fixed_runout_[slot]);
  } else {
    std::array<bool, 52> blocked{};
    for (int card : state.board())
      blocked[card] = true;
    for (const auto& hand : hands)
      for (int card : hand)
        blocked[card] = true;
    for (auto fixed : fixed_runout_)
      if (fixed)
        blocked[*fixed] = true;
    for (int card = 0; card < 52; ++card)
      if (!blocked[card])
        cards.push_back(card);
  }
  if (cards.empty())
    throw std::invalid_argument("oracle runout support is empty");
  double total = 0;
  for (int card : cards)
    total += runout_responder(state.after_card(card), hands);
  return total / static_cast<double>(cards.size());
}

double TerminalOracle::leaf_responder(const OracleDeal& deal, std::size_t action) const {
  std::array<std::array<int, 2>, 2> hands{};
  hands[hero_] = deal.hero;
  hands[responder_] = deal.responder;
  const HeadsUpState after = node_.after_action(hero_, actions_[action]);
  return runout_responder(after, hands);
}

double TerminalOracle::mass(std::array<int, 2> responder) const {
  double total = 0;
  for (const OracleDeal& deal : deals_)
    if (deal.responder == responder)
      total += deal.weight;
  return total;
}

double TerminalOracle::baseline_margin(std::array<int, 2> responder,
                                       const BaselineStrategy& baseline) const {
  double weighted = 0;
  double m = 0;
  for (const OracleDeal& deal : deals_) {
    if (deal.responder != responder)
      continue;
    const auto row = baseline.find(deal.hero);
    if (row == baseline.end())
      throw std::invalid_argument("oracle baseline missing a hero combo");
    if (row->second.size() != actions_.size())
      throw std::invalid_argument("oracle baseline action count mismatch");
    double value = 0;
    for (std::size_t a = 0; a < actions_.size(); ++a)
      value += row->second[a] * leaf_responder(deal, a);
    weighted += deal.weight * value;
    m += deal.weight;
  }
  if (!(m > 0))
    throw std::invalid_argument("oracle baseline requested at zero-mass infoset");
  return weighted / m;
}

double TerminalOracle::candidate_margin(std::array<int, 2> responder,
                                        const HeroStrategy& candidate) const {
  double weighted = 0;
  double m = 0;
  for (const OracleDeal& deal : deals_) {
    if (deal.responder != responder)
      continue;
    const auto row = candidate.find(deal.hero);
    if (row == candidate.end())
      throw std::invalid_argument("oracle candidate missing a hero combo");
    if (row->second.size() != actions_.size())
      throw std::invalid_argument("oracle candidate action count mismatch");
    double value = 0;
    for (std::size_t a = 0; a < actions_.size(); ++a)
      value += row->second[a] * leaf_responder(deal, a);
    weighted += deal.weight * value;
    m += deal.weight;
  }
  if (!(m > 0))
    throw std::invalid_argument("oracle candidate requested at zero-mass infoset");
  return weighted / m;
}

TerminalOracle::NashReport TerminalOracle::nash_conv(
    const ResponderStrategy& responder, const HeroStrategy& hero,
    const std::map<std::array<int, 2>, double>& terminate_payoff) const {
  NashReport report;

  // ---- Responder: replace the whole -x infoset with TERMINATE or CONTINUE.
  for (const auto& r : responder_order_) {
    const double m = mass(r);
    const auto rsp = responder.find(r);
    const auto payoff = terminate_payoff.find(r);
    if (rsp == responder.end() || payoff == terminate_payoff.end())
      throw std::invalid_argument("oracle nash_conv missing responder strategy or payoff");
    const double q_terminate = rsp->second[0];
    const double q_continue = rsp->second[1];
    double continue_weighted = 0;
    for (const OracleDeal& deal : deals_) {
      if (deal.responder != r)
        continue;
      const auto row = hero.find(deal.hero);
      if (row == hero.end())
        throw std::invalid_argument("oracle nash_conv missing hero strategy");
      double value = 0;
      for (std::size_t a = 0; a < actions_.size(); ++a)
        value += row->second[a] * leaf_responder(deal, a);
      continue_weighted += deal.weight * value;
    }
    const double continue_value = continue_weighted / m;
    const double profile_value = q_terminate * payoff->second + q_continue * continue_value;
    const double best_value = std::max(payoff->second, continue_value);
    report.responder_gain += (m / total_mass_) * (best_value - profile_value);
  }

  // ---- Hero: replace one hero infoset with its best single action. --------
  // Deals arrive only through CONTINUE, with mass w(h,r)*q_continue(r). Hero
  // utility is the negated responder leaf; sums stay unnormalized so the
  // arrival mass cancels against the 1/total_mass_ normalization.
  for (const auto& h : hero_order_) {
    std::vector<double> action_sum(actions_.size(), 0.0);
    double arrival_mass = 0;
    double profile_weighted = 0;
    const auto hero_row = hero.find(h);
    if (hero_row == hero.end())
      throw std::invalid_argument("oracle nash_conv missing hero strategy");
    for (const OracleDeal& deal : deals_) {
      if (deal.hero != h)
        continue;
      const auto rsp = responder.find(deal.responder);
      const double q_continue = rsp->second[1];
      if (q_continue == 0)
        continue;
      for (std::size_t a = 0; a < actions_.size(); ++a)
        action_sum[a] += deal.weight * q_continue * leaf_responder(deal, a);
      double profile_leaf = 0;
      for (std::size_t a = 0; a < actions_.size(); ++a)
        profile_leaf += hero_row->second[a] * leaf_responder(deal, a);
      profile_weighted += deal.weight * q_continue * profile_leaf;
      arrival_mass += deal.weight * q_continue;
    }
    if (arrival_mass == 0)
      continue;  // off-path hero infoset has no deviation value under profile
    const double best_action_sum = *std::min_element(action_sum.begin(), action_sum.end());
    // Hero maximizes -responder utility: gain = profile - best-action responder sum.
    report.hero_gain += (profile_weighted - best_action_sum) / total_mass_;
  }

  report.nash_conv = report.responder_gain + report.hero_gain;
  return report;
}

}  // namespace bs::resolver_test
