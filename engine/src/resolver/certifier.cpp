#include "certifier.hpp"

#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <cmath>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <vector>

namespace bs::resolver::detail {
namespace {

// Independently written public-runout expectation for the responder, with its
// own uniform 1/N chance factors and exact settlement. Mirrors no resolver
// traversal function.
double expected_resp_value(const HeadsUpGame& game, const HeadsUpState& state,
                           const std::array<std::array<int, 2>, 2>& hands, std::size_t responder,
                           Budget& budget) {
  budget.visit();
  if (state.phase() == Phase::Folded)
    return static_cast<double>(state.settle_fold().net_utility[responder]);
  if (state.phase() == Phase::Showdown)
    return static_cast<double>(state.settle_showdown(hands).net_utility[responder]);
  if (state.phase() != Phase::Deal)
    throw RequireFailure("certifier reached an unexpected action node");

  const std::size_t slot = state.board().size() - 3;
  std::vector<int> cards;
  if (game.fixed_runout[slot]) {
    cards.push_back(*game.fixed_runout[slot]);
  } else {
    std::array<bool, 52> blocked{};
    for (int card : state.board())
      blocked[card] = true;
    for (const auto& hand : hands)
      for (int card : hand)
        blocked[card] = true;
    for (auto fixed : game.fixed_runout)
      if (fixed)
        blocked[*fixed] = true;
    for (int card = 0; card < 52; ++card)
      if (!blocked[card])
        cards.push_back(card);
  }
  if (cards.empty())
    throw RequireFailure("certifier runout has no support");
  double total = 0;
  const double each = 1.0 / static_cast<double>(cards.size());
  for (int card : cards)
    total += each * expected_resp_value(game, state.after_card(card), hands, responder, budget);
  return total;
}

}  // namespace

Certification certify_candidate(const ReachModel& model,
                                const std::map<InformationKey, PolicyRow>& candidate,
                                const ResolveLimits& limits, Budget& budget) {
  Certification result;
  try {
    // Whole-range completeness and distribution validity.
    for (const auto& [key, row] : candidate) {
      (void)key;
      if (row.actions != model.node_actions || row.probabilities.size() != row.actions.size()) {
        result.status = ResolveStatus::CoverageMiss;
        return result;
      }
      double sum = 0;
      for (double p : row.probabilities) {
        if (!std::isfinite(p) || p < 0) {
          result.status = ResolveStatus::CoverageMiss;
          return result;
        }
        sum += p;
      }
      if (std::abs(sum - 1.0) > 1e-12) {
        result.status = ResolveStatus::CoverageMiss;
        result.margins.clear();
        return result;
      }
    }
    // Whole-range completeness over every hero combo that participates in a
    // positive-weight deal, i.e. that has at least one compatible opponent
    // combination. A board-unblocked hero combo whose every opponent combo is
    // blocked carries zero counterfactual mass, enters no responder infoset,
    // and can never appear in any opponent best response; requiring a row for
    // it would discard an otherwise certifiable whole-range candidate. Such
    // combos stay recorded in model.live_hero but are not certified here.
    std::set<std::array<int, 2>> dealt_heroes;
    for (const GadgetDeal& deal : model.deals)
      dealt_heroes.insert(deal.hero);
    std::set<std::array<int, 2>> covered;
    for (const auto& hero : dealt_heroes) {
      const InformationKey key = solver::information_key(model.node, hero);
      if (!candidate.contains(key)) {
        result.status = ResolveStatus::CoverageMiss;
        return result;
      }
      covered.insert(hero);
    }
    if (covered.size() != dealt_heroes.size()) {
      result.status = ResolveStatus::CoverageMiss;
      return result;
    }

    const double pot = static_cast<double>(model.node.root().pot);
    const double tolerance = kCertPotTolerance * pot;

    for (const ResponderInfoset& infoset : model.infosets) {
      double weighted = 0;
      for (std::size_t di : infoset.deals) {
        const GadgetDeal& deal = model.deals[di];
        const InformationKey key = solver::information_key(model.node, deal.hero);
        const PolicyRow& row = candidate.at(key);
        std::array<std::array<int, 2>, 2> hands{};
        hands[model.hero] = deal.hero;
        hands[model.responder] = deal.responder;
        double deal_value = 0;
        for (std::size_t a = 0; a < model.node_actions.size(); ++a) {
          const HeadsUpState after = model.node.after_action(model.hero, model.node_actions[a]);
          deal_value += row.probabilities[a] *
                        expected_resp_value(*model.game, after, hands, model.responder, budget);
        }
        weighted += deal.weight * deal_value;
      }
      const double candidate_bound = weighted / infoset.mass;
      const double best_response = std::max(infoset.baseline, candidate_bound);
      MarginRecord record;
      record.responder_cards = infoset.cards;
      record.mass = infoset.mass;
      record.baseline = infoset.baseline;
      record.candidate = candidate_bound;
      record.best_response = best_response;
      record.slack = best_response - infoset.baseline - tolerance;
      record.positive_mass = true;
      result.margins.push_back(record);
    }

    // Zero-mass infosets provide no gadget chance; the whole-range candidate is
    // keyed by hero holdings and cannot route probability through them.
    for (const auto& cards : model.zero_mass_responder) {
      for (const GadgetDeal& deal : model.deals)
        if (deal.responder == cards && deal.weight > 0) {
          result.status = ResolveStatus::CoverageMiss;
          return result;
        }
      MarginRecord record;
      record.responder_cards = cards;
      record.mass = 0;
      record.positive_mass = false;
      result.margins.push_back(record);
    }

    result.certified = true;
    for (const MarginRecord& margin : result.margins)
      if (margin.positive_mass && margin.slack > 0.0)
        result.certified = false;
    result.status =
        result.certified ? ResolveStatus::Certified : ResolveStatus::CertificationRejected;
    result.nodes = budget.nodes;
  } catch (const Exhausted&) {
    Certification timed_out;
    timed_out.status = ResolveStatus::CertifyDeadline;
    timed_out.nodes = budget.nodes;
    return timed_out;
  } catch (const std::exception&) {
    result.status = ResolveStatus::CoverageMiss;
    result.certified = false;
    return result;
  }
  return result;
}

}  // namespace bs::resolver::detail
