#include "gadget_cfr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace bs::resolver::detail {
namespace {

// RFC 0004 regret matching: uniform only when total positive regret is zero.
std::vector<double> regret_matching(const std::vector<double>& regrets) {
  std::vector<double> strategy(regrets.size(), 1.0 / regrets.size());
  double sum = 0;
  for (double regret : regrets)
    sum += std::max(0.0, regret);
  if (sum > 0)
    for (std::size_t i = 0; i < regrets.size(); ++i)
      strategy[i] = std::max(0.0, regrets[i]) / sum;
  return strategy;
}

struct GadgetTrainer {
  const ReachModel& model;
  Budget& budget;
  const std::size_t action_count;

  std::map<std::array<int, 2>, std::vector<double>> gadget_regret;
  std::map<std::array<int, 2>, std::vector<double>> gadget_sum;
  std::map<InformationKey, std::vector<double>> hero_regret;
  std::map<InformationKey, std::vector<double>> hero_sum;
  std::map<std::array<int, 2>, std::size_t> baseline_index;

  std::set<std::array<int, 2>> averaged_gadget;
  std::set<InformationKey> averaged_hero;

  GadgetTrainer(const ReachModel& m, Budget& b)
      : model(m), budget(b), action_count(m.node_actions.size()) {
    for (std::size_t i = 0; i < model.infosets.size(); ++i) {
      const auto& cards = model.infosets[i].cards;
      baseline_index[cards] = i;
      gadget_regret[cards].assign(2, 0.0);
      gadget_sum[cards].assign(2, 0.0);
    }
    std::set<std::array<int, 2>> heroes;
    for (const GadgetDeal& deal : model.deals)
      heroes.insert(deal.hero);
    for (const auto& hero : heroes) {
      InformationKey key = solver::information_key(model.node, hero);
      hero_regret[key].assign(action_count, 0.0);
      hero_sum[key].assign(action_count, 0.0);
    }
  }

  InformationKey hero_key(const std::array<int, 2>& cards) const {
    return solver::information_key(model.node, cards);
  }

  std::array<std::array<int, 2>, 2> holes(const GadgetDeal& deal) const {
    std::array<std::array<int, 2>, 2> hands{};
    hands[model.hero] = deal.hero;
    hands[model.responder] = deal.responder;
    return hands;
  }

  // RESPONDER-chip leaf utility for one hero action from the constant payoff
  // matrix (terminal-only continuation has no in-subtree decision).
  double leaf(std::size_t deal_index, std::size_t a) const {
    return model.leaf_values[deal_index][a];
  }

  double walk_hero(std::size_t deal_index, std::size_t traverser, std::array<double, 2> reach,
                   double chance, double iter_weight) {
    const GadgetDeal& deal = model.deals[deal_index];
    budget.visit();
    const InformationKey key = hero_key(deal.hero);
    const std::vector<double> sigma = regret_matching(hero_regret[key]);
    std::vector<double> child(action_count);
    double value = 0;
    const double sign = traverser == model.responder ? 1.0 : -1.0;
    for (std::size_t a = 0; a < action_count; ++a) {
      child[a] = sign * leaf(deal_index, a);
      value += sigma[a] * child[a];
    }
    if (traverser == model.hero) {
      auto& regret = hero_regret[key];
      for (std::size_t a = 0; a < action_count; ++a)
        regret[a] += iter_weight * chance * reach[model.responder] * (child[a] - value);
      if (averaged_hero.insert(key).second) {
        auto& sum = hero_sum[key];
        for (std::size_t a = 0; a < action_count; ++a)
          sum[a] += iter_weight * reach[model.hero] * sigma[a];
      }
    }
    return value;
  }

  double walk_gadget(std::size_t deal_index, std::size_t traverser, std::array<double, 2> reach,
                     double chance, double iter_weight) {
    const GadgetDeal& deal = model.deals[deal_index];
    budget.visit();
    const auto& cards = deal.responder;
    const double baseline = model.infosets[baseline_index.at(cards)].baseline;
    const std::vector<double> sigma = regret_matching(gadget_regret[cards]);
    const double sign = traverser == model.responder ? 1.0 : -1.0;
    std::array<double, 2> child{};
    // a=0 TERMINATE (constant centered margin), a=1 CONTINUE into hero node.
    child[0] = sign * baseline;
    auto continued_reach = reach;
    continued_reach[model.responder] *= sigma[1];
    child[1] = walk_hero(deal_index, traverser, continued_reach, chance, iter_weight);
    const double value = sigma[0] * child[0] + sigma[1] * child[1];
    if (traverser == model.responder) {
      auto& regret = gadget_regret[cards];
      for (std::size_t a = 0; a < 2; ++a)
        regret[a] += iter_weight * chance * reach[model.hero] * (child[a] - value);
      if (averaged_gadget.insert(cards).second) {
        auto& sum = gadget_sum[cards];
        for (std::size_t a = 0; a < 2; ++a)
          sum[a] += iter_weight * reach[model.responder] * sigma[a];
      }
    }
    return value;
  }

  GadgetOutput finish(std::uint64_t completed) {
    GadgetOutput output;
    output.status = ResolveStatus::Certified;
    output.completed_iterations = completed;
    output.nodes = budget.nodes;
    output.information_sets = gadget_regret.size() + hero_regret.size();
    for (const auto& [key, sum] : hero_sum) {
      PolicyRow row;
      row.actions = model.node_actions;
      row.probabilities = regret_matching(sum);
      output.candidate.emplace(key, std::move(row));
    }
    for (const auto& [cards, sum] : gadget_sum) {
      double total = sum[0] + sum[1];
      output.terminate[cards] = {sum[0] / total, sum[1] / total};
    }
    return output;
  }
};

}  // namespace

GadgetOutput run_gadget_cfr(const ReachModel& model, const ResolveLimits& limits, Budget& budget) {
  SplitMix64 rng(limits.public_seed);
  (void)rng;
  GadgetTrainer trainer(model, budget);
  std::uint64_t completed = 0;
  try {
    for (std::uint64_t iteration = 0; iteration < limits.iterations; ++iteration) {
      // Linear CFR weight: 1-based iteration index t.
      const double iter_weight = static_cast<double>(iteration + 1);
      for (std::size_t traverser = 0; traverser < 2; ++traverser) {
        trainer.averaged_gadget.clear();
        trainer.averaged_hero.clear();
        for (std::size_t deal_index = 0; deal_index < model.deals.size(); ++deal_index) {
          const double chance = model.deals[deal_index].weight / model.total_mass;
          (void)trainer.walk_gadget(deal_index, traverser, {1.0, 1.0}, chance, iter_weight);
        }
      }
      ++completed;
    }
  } catch (const Exhausted&) {
    GadgetOutput timed_out;
    timed_out.status = ResolveStatus::SolveDeadline;
    timed_out.completed_iterations = completed;
    timed_out.nodes = budget.nodes;
    return timed_out;
  }
  return trainer.finish(completed);
}

}  // namespace bs::resolver::detail
