#include "certifier.hpp"

#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <cmath>
#include <cstddef>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs::resolver::detail {
namespace {

// Independently written public-runout expectation for one seat, with its own
// uniform 1/N chance factors and exact settlement. Mirrors no resolver
// traversal function. At a showdown the holes span is indexed by seat, but the
// settlement input is indexed by LIVE POSITION (live_players ascending), so the
// live holes are repacked before settle_showdown; the returned chip_utility is
// indexed by seat, so the seat's value is read back by its seat index.
double expected_seat_value(const UnifiedGame& game, const GameState& state,
                           std::span<const std::array<int, 2>> hands, std::size_t seat,
                           Budget& budget) {
  budget.visit();
  if (state.phase() == Phase::Folded)
    return static_cast<double>(state.settle_fold().chip_utility[seat]);
  if (state.phase() == Phase::Showdown) {
    std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> live_holes{};
    std::size_t live_count = 0;
    for (std::size_t live_seat : state.live_players())
      live_holes[live_count++] = hands[live_seat];
    return static_cast<double>(
        state.settle_showdown(std::span<const std::array<int, 2>>(live_holes.data(), live_count))
            .chip_utility[seat]);
  }
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
    total += each * expected_seat_value(game, state.after_card(card), hands, seat, budget);
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
    // blocked carries zero counterfactual mass, enters no non-hero infoset,
    // and can never appear in any opponent best response; requiring a row for
    // it would discard an otherwise certifiable whole-range candidate. Such
    // combos stay recorded in model.live_hero but are not certified here.
    std::set<std::array<int, 2>> dealt_heroes;
    for (const GadgetDeal& deal : model.deals)
      dealt_heroes.insert(deal.hands[model.hero]);
    std::set<std::array<int, 2>> covered;
    for (const auto& hero : dealt_heroes) {
      const InformationKey key =
          solver::make_information_key(model.hero, hero, model.node.board(), model.history);
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

    const double pot = static_cast<double>(model.node.def().pot);
    const double tolerance = kCertPotTolerance * pot;

    // Per-seat unilateral non-regression: for every non-hero seat s and every
    // positive-mass infoset (s, c), the candidate locked-continuation value
    // must not exceed the baseline by more than the tolerance. This is NOT a
    // multi-player equilibrium claim and NOT a two-player bound; each seat is
    // certified independently against the unchanged prefix weights.
    for (const SeatInfoset& infoset : model.infosets) {
      const std::size_t s = infoset.seat;
      double weighted = 0;
      for (std::size_t di : infoset.deals) {
        const GadgetDeal& deal = model.deals[di];
        const InformationKey key = solver::make_information_key(model.hero, deal.hands[model.hero],
                                                                model.node.board(), model.history);
        const PolicyRow& row = candidate.at(key);
        double deal_value = 0;
        for (std::size_t a = 0; a < model.node_actions.size(); ++a) {
          const GameState after = model.node.after_action(model.hero, model.node_actions[a]);
          deal_value +=
              row.probabilities[a] * expected_seat_value(*model.game, after,
                                                         std::span<const std::array<int, 2>>(
                                                             deal.hands.data(), model.seat_count),
                                                         s, budget);
        }
        weighted += deal.weight * deal_value;
      }
      const double candidate_bound = weighted / infoset.mass;
      const double best_response = std::max(infoset.baseline, candidate_bound);
      MarginRecord record;
      record.seat = s;
      record.cards = infoset.cards;
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
    for (std::size_t s = 0; s < model.seat_count; ++s) {
      if (s == model.hero)
        continue;
      for (const auto& cards : model.zero_mass[s]) {
        for (const GadgetDeal& deal : model.deals)
          if (deal.hands[s] == cards && deal.weight > 0) {
            result.status = ResolveStatus::CoverageMiss;
            return result;
          }
        MarginRecord record;
        record.seat = s;
        record.cards = cards;
        record.mass = 0;
        record.positive_mass = false;
        result.margins.push_back(record);
      }
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
