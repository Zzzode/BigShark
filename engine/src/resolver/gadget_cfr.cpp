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
  const std::size_t seat_count;

  std::map<GadgetKey, std::vector<double>> gadget_regret;
  std::map<GadgetKey, std::vector<double>> gadget_sum;
  std::map<InformationKey, std::vector<double>> hero_regret;
  std::map<InformationKey, std::vector<double>> hero_sum;
  std::map<GadgetKey, std::size_t> baseline_index;

  std::set<GadgetKey> averaged_gadget;
  std::set<InformationKey> averaged_hero;

  GadgetTrainer(const ReachModel& m, Budget& b)
      : model(m), budget(b), action_count(m.node_actions.size()), seat_count(m.seat_count) {
    for (std::size_t i = 0; i < model.infosets.size(); ++i) {
      const SeatInfoset& infoset = model.infosets[i];
      const GadgetKey key{infoset.seat, infoset.cards};
      baseline_index[key] = i;
      gadget_regret[key].assign(2, 0.0);
      gadget_sum[key].assign(2, 0.0);
    }
    std::set<std::array<int, 2>> heroes;
    for (const GadgetDeal& deal : model.deals)
      heroes.insert(deal.hands[model.hero]);
    for (const auto& hero : heroes) {
      InformationKey key =
          solver::make_information_key(model.hero, hero, model.node.board(), model.history);
      hero_regret[key].assign(action_count, 0.0);
      hero_sum[key].assign(action_count, 0.0);
    }
  }

  InformationKey hero_key(const std::array<int, 2>& cards) const {
    return solver::make_information_key(model.hero, cards, model.node.board(), model.history);
  }

  // One traverser sweep over every joint deal. The gadget is hero-vs-field:
  // the field is the separable sum of the non-hero seats, each owning one -x
  // infoset. For a deal the hero's strategy sigma_h and every non-hero seat's
  // -x strategy sigma_s are read first; then C_s = sum_a sigma_h[a] * U_s(leaf
  // a) is each seat's locked-continuation value. The hero's regret is the SUM
  // over non-hero seats of cont_s * (C_s - U_s(a)); a non-hero seat's regret is
  // its own TERMINATE/CONTINUE margin against b_s. The kSimple average is
  // updated once per infoset per sweep with own reach 1. See the header comment
  // for the normalization and the per-seat non-regression semantics.
  void sweep(std::size_t traverser, double iter_weight) {
    const bool hero_traverser = (traverser == model.hero);
    for (std::size_t deal_index = 0; deal_index < model.deals.size(); ++deal_index) {
      const GadgetDeal& deal = model.deals[deal_index];
      for (std::size_t n = 0; n < seat_count; ++n)
        budget.visit();
      const double chance = deal.weight / model.total_mass;
      const InformationKey hkey = hero_key(deal.hands[model.hero]);
      const std::vector<double> sigma_h = regret_matching(hero_regret[hkey]);

      // Per-seat -x strategy and locked-continuation value C_s under sigma_h,
      // computed for every non-hero seat so the hero's regret (a sum over
      // seats) and any seat's regret share the identical values.
      std::array<std::vector<double>, poker::kMaxUnifiedSeats> sigma_s{};
      std::array<double, poker::kMaxUnifiedSeats> C_s{};
      for (std::size_t s = 0; s < seat_count; ++s) {
        if (s == model.hero)
          continue;
        const GadgetKey gkey{s, deal.hands[s]};
        sigma_s[s] = regret_matching(gadget_regret[gkey]);
        double c = 0;
        for (std::size_t a = 0; a < action_count; ++a)
          c += sigma_h[a] * model.leaf_values[deal_index][a][s];
        C_s[s] = c;
      }

      if (hero_traverser) {
        auto& regret = hero_regret[hkey];
        for (std::size_t s = 0; s < seat_count; ++s) {
          if (s == model.hero)
            continue;
          const double cont_s = sigma_s[s][1];
          for (std::size_t a = 0; a < action_count; ++a)
            regret[a] +=
                ((iter_weight * chance) * cont_s) * (C_s[s] - model.leaf_values[deal_index][a][s]);
        }
        if (averaged_hero.insert(hkey).second) {
          auto& sum = hero_sum[hkey];
          for (std::size_t a = 0; a < action_count; ++a)
            sum[a] += iter_weight * 1.0 * sigma_h[a];
        }
      } else {
        const std::size_t t = traverser;
        const GadgetKey gkey{t, deal.hands[t]};
        const SeatInfoset& infoset = model.infosets[baseline_index.at(gkey)];
        const double b_t = infoset.baseline;
        const double v_t = sigma_s[t][0] * b_t + sigma_s[t][1] * C_s[t];
        auto& regret = gadget_regret[gkey];
        regret[0] += (iter_weight * chance) * 1.0 * (b_t - v_t);
        regret[1] += (iter_weight * chance) * 1.0 * (C_s[t] - v_t);
        if (averaged_gadget.insert(gkey).second) {
          auto& sum = gadget_sum[gkey];
          sum[0] += iter_weight * 1.0 * sigma_s[t][0];
          sum[1] += iter_weight * 1.0 * sigma_s[t][1];
        }
      }
    }
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
    for (const auto& [key, sum] : gadget_sum) {
      double total = sum[0] + sum[1];
      output.terminate[key] = {sum[0] / total, sum[1] / total};
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
      for (std::size_t traverser = 0; traverser < model.seat_count; ++traverser) {
        trainer.averaged_gadget.clear();
        trainer.averaged_hero.clear();
        trainer.sweep(traverser, iter_weight);
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
