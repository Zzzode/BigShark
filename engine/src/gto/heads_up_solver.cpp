#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace bs::solver {
namespace {
using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::HeadsUpState;
using poker::Phase;
using Clock = std::chrono::steady_clock;

void require(bool ok, const char* message) {
  if (!ok)
    throw std::invalid_argument(message);
}

Chips add(Chips a, Chips b) {
  if (b > std::numeric_limits<Chips>::max() - a)
    throw std::overflow_error("size sum overflow");
  return a + b;
}

Chips ceil_fraction(Chips amount, Fraction f) {
  require(f.numerator > 0 && f.denominator > 0 && std::gcd(f.numerator, f.denominator) == 1,
          "sizes must be positive reduced fractions");
  if (amount > std::numeric_limits<Chips>::max() / f.numerator)
    throw std::overflow_error("size product overflow");
  const Chips product = amount * f.numerator;
  return product / f.denominator + (product % f.denominator != 0);
}

void reserve_card(int card, std::array<bool, 52>& used) {
  require(card >= 0 && card < 52, "invalid game card");
  require(!used[card], "duplicate game card");
  used[card] = true;
}

bool same_root(const poker::HeadsUpRoot& a, const poker::HeadsUpRoot& b) {
  return a.flop == b.flop && a.stacks == b.stacks && a.contributions == b.contributions &&
         a.pot == b.pot && a.big_blind == b.big_blind && a.button == b.button;
}

struct Exhausted {};

struct Budget {
  TrainingLimits limits;
  Clock::time_point end;
  std::size_t nodes = 0;
  std::size_t bytes = 0;
  std::size_t peak_bytes = 0;
  explicit Budget(TrainingLimits value) : limits(value) {
    require(value.time.count() > 0 && value.max_nodes > 0 && value.max_information_sets > 0 &&
                value.max_bytes > 0 && value.max_depth > 0 && value.max_depth <= 256,
            "positive resource limits required");
    const auto now = Clock::now();
    const auto available =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    require(value.time <= available, "deadline outside clock range");
    end = now + std::chrono::duration_cast<Clock::duration>(value.time);
  }
  void visit() {
    if (nodes == limits.max_nodes || Clock::now() >= end)
      throw Exhausted{};
    ++nodes;
  }
  void allocate(std::size_t count) {
    if (bytes > limits.max_bytes || count > limits.max_bytes - bytes)
      throw Exhausted{};
    bytes += count;
    peak_bytes = std::max(peak_bytes, bytes);
  }
};

struct Frame {
  Budget& budget;
  std::size_t size;
  Frame(Budget& b, std::size_t depth, std::size_t count) : budget(b), size(count) {
    if (depth > b.limits.max_depth)
      throw Exhausted{};
    budget.allocate(size);
  }
  ~Frame() { budget.bytes -= size; }
};

double probability_product(double a, double b) {
  const double value = a * b;
  require(std::isfinite(value) && (a == 0 || b == 0 || value > 0),
          "probability below supported numeric precision");
  return value;
}

struct Deal {
  std::array<std::array<int, 2>, 2> hands;
  double probability;
};

std::vector<Deal> joint_deals(const HeadsUpGame& game, Budget& budget) {
  std::vector<Deal> deals;
  double mass = 0;
  // One bounded reservation avoids geometric vector growth and simultaneous
  // old/new backing stores. Include incompatible pairs in the capacity charge.
  const auto capacity = game.ranges[0].size() * game.ranges[1].size();
  budget.allocate(capacity * sizeof(Deal));
  deals.reserve(capacity);
  for (const auto& a : game.ranges[0]) {
    for (const auto& b : game.ranges[1]) {
      budget.visit();
      if (a.cards[0] == b.cards[0] || a.cards[0] == b.cards[1] || a.cards[1] == b.cards[0] ||
          a.cards[1] == b.cards[1])
        continue;
      // Range weights were scaled by their individual maxima before multiplying.
      const double p = a.weight * b.weight;
      require(p > 0 && std::isfinite(p), "joint range mass underflow");
      deals.push_back({{a.cards, b.cards}, p});
      mass += p;
    }
  }
  require(mass > 0 && std::isfinite(mass), "no compatible positive joint range");
  for (auto& deal : deals) {
    deal.probability /= mass;
    require(deal.probability > 0, "normalized joint probability underflow");
  }
  return deals;
}

std::vector<int> public_cards(const HeadsUpGame& game, const HeadsUpState& state,
                              const Deal& deal) {
  const std::size_t slot = state.board().size() - 3;
  if (game.fixed_runout[slot])
    return {*game.fixed_runout[slot]};
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const auto& hand : deal.hands)
    for (int card : hand)
      used[card] = true;
  for (auto card : game.fixed_runout)
    if (card)
      used[*card] = true;
  std::vector<int> cards;
  for (int card = 0; card < 52; ++card)
    if (!used[card])
      cards.push_back(card);
  return cards;
}

struct Info {
  std::vector<Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
};
using Table = std::map<InformationKey, Info>;

std::vector<double> normalize(const std::vector<double>& weights) {
  double sum = 0;
  for (double weight : weights) {
    if (!std::isfinite(weight))
      throw std::runtime_error("non-finite training weight");
    sum += std::max(0.0, weight);
  }
  std::vector<double> result(weights.size(), 1.0 / static_cast<double>(weights.size()));
  if (sum > 0)
    for (std::size_t i = 0; i < weights.size(); ++i)
      result[i] = std::max(0.0, weights[i]) / sum;
  return result;
}

std::size_t row_bytes(const InformationKey& key, std::size_t actions) {
  // Conservative accounting includes map links, vector capacities, delta and
  // export copies. A failed iteration never replaces the committed table.
  return 512 + key.size() * sizeof(std::uint64_t) * 8 +
         actions * (sizeof(Action) + sizeof(double) * 8) * 4;
}

struct FullTraversal {
  const HeadsUpGame& game;
  Table& table;
  Budget& budget;
  std::map<InformationKey, std::vector<double>> deltas;
  std::set<InformationKey> averaged;

  double walk(const HeadsUpState& state, const Deal& deal, std::size_t traverser,
              std::array<double, 2> reach, double chance, std::size_t depth = 0) {
    budget.visit();
    Frame frame(budget, depth, 8192 + state.history().size() * sizeof(poker::BettingEvent) * 4);
    if (state.phase() == Phase::Folded)
      return static_cast<double>(state.settle_fold().net_utility[traverser]);
    if (state.phase() == Phase::Showdown)
      return static_cast<double>(state.settle_showdown(deal.hands).net_utility[traverser]);
    if (state.phase() == Phase::Deal) {
      const auto cards = public_cards(game, state, deal);
      double value = 0;
      const double p = 1.0 / static_cast<double>(cards.size());
      for (int card : cards)
        value += p * walk(state.after_card(card), deal, traverser, reach,
                          probability_product(chance, p), depth + 1);
      return value;
    }
    const std::size_t actor = *state.actor();
    const auto key = information_key(state, deal.hands[actor]);
    auto it = table.find(key);
    if (it == table.end()) {
      if (table.size() == budget.limits.max_information_sets)
        throw Exhausted{};
      auto actions = abstract_actions(state, game.sizes);
      budget.allocate(row_bytes(key, actions.size()));
      const auto count = actions.size();
      it = table
               .emplace(key, Info{std::move(actions), std::vector<double>(count),
                                  std::vector<double>(count)})
               .first;
    }
    auto& info = it->second;
    const auto sigma = normalize(info.regrets);
    // Own reach is identical across hidden histories at this exact perfect-
    // recall information set. Count the average once per traverser sweep.
    if (actor == traverser && averaged.insert(key).second)
      for (std::size_t a = 0; a < sigma.size(); ++a)
        info.sums[a] += reach[actor] * sigma[a];

    std::vector<double> children(sigma.size());
    double value = 0;
    for (std::size_t a = 0; a < sigma.size(); ++a) {
      auto child_reach = reach;
      child_reach[actor] = probability_product(child_reach[actor], sigma[a]);
      children[a] = walk(state.after_action(actor, info.actions[a]), deal, traverser, child_reach,
                         chance, depth + 1);
      value += sigma[a] * children[a];
    }
    if (actor == traverser) {
      auto& delta = deltas.try_emplace(key, sigma.size(), 0.0).first->second;
      for (std::size_t a = 0; a < sigma.size(); ++a)
        delta[a] += probability_product(chance, reach[1 - actor]) * (children[a] - value);
    }
    return value;
  }

  void finish() {
    for (const auto& [key, values] : deltas)
      for (std::size_t a = 0; a < values.size(); ++a)
        table.at(key).regrets[a] += values[a];
  }
};

double response(const HeadsUpGame& game, const HeadsUpPolicy& policy, const HeadsUpState& state,
                const std::vector<Deal>& deals, const std::vector<double>& reach,
                std::size_t player, std::array<int, 2> own, bool best, Budget& budget,
                std::size_t depth = 0) {
  budget.visit();
  Frame frame(budget, depth, 8192 + state.history().size() * sizeof(poker::BettingEvent) * 4);
  if (std::accumulate(reach.begin(), reach.end(), 0.0) == 0)
    return 0;
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown) {
    double value = 0;
    for (std::size_t d = 0; d < deals.size(); ++d) {
      if (reach[d] == 0)
        continue;
      const auto result = state.phase() == Phase::Folded ? state.settle_fold()
                                                         : state.settle_showdown(deals[d].hands);
      value += reach[d] * static_cast<double>(result.net_utility[player]);
    }
    return value;
  }
  if (state.phase() == Phase::Deal) {
    Frame chance_memory(budget, depth, 52 * deals.size() * sizeof(double));
    std::array<std::vector<double>, 52> card_reach;
    for (std::size_t d = 0; d < deals.size(); ++d) {
      if (reach[d] == 0)
        continue;
      const auto cards = public_cards(game, state, deals[d]);
      for (int card : cards) {
        if (card_reach[card].empty())
          card_reach[card].resize(deals.size());
        card_reach[card][d] =
            probability_product(reach[d], 1.0 / static_cast<double>(cards.size()));
      }
    }
    double value = 0;
    for (int card = 0; card < 52; ++card)
      if (!card_reach[card].empty())
        value += response(game, policy, state.after_card(card), deals, card_reach[card], player,
                          own, best, budget, depth + 1);
    return value;
  }
  const auto actor = *state.actor();
  const auto actions = abstract_actions(state, game.sizes);
  std::vector<double> values(actions.size());
  Frame action_memory(budget, depth, deals.size() * sizeof(double));
  for (std::size_t a = 0; a < actions.size(); ++a) {
    auto child = reach;
    if (actor != player) {
      for (std::size_t d = 0; d < deals.size(); ++d) {
        if (reach[d] == 0)
          continue;
        const auto* row = policy.lookup(state, deals[d].hands[actor]);
        require(row && row->actions == actions, "incomplete or incompatible policy");
        child[d] = probability_product(child[d], row->probabilities[a]);
      }
    }
    values[a] = response(game, policy, state.after_action(actor, actions[a]), deals, child, player,
                         own, best, budget, depth + 1);
  }
  if (actor != player)
    return std::accumulate(values.begin(), values.end(), 0.0);
  if (best)
    return *std::max_element(values.begin(), values.end());
  const auto* row = policy.lookup(state, own);
  require(row && row->actions == actions, "incomplete profile policy");
  return std::inner_product(values.begin(), values.end(), row->probabilities.begin(), 0.0);
}
}  // namespace

std::vector<Action> abstract_actions(const HeadsUpState& state, const SizeSchedule& sizes) {
  const auto legal = state.legal();
  std::vector<Action> result;
  if (legal.fold)
    result.push_back({ActionType::Fold});
  if (legal.check)
    result.push_back({ActionType::Check});
  if (legal.call)
    result.push_back({ActionType::Call});
  if (!legal.aggressive)
    return result;
  const auto actor = *state.actor();
  const auto& hero = state.players()[actor];
  const auto& other = state.players()[1 - actor];
  const auto bounds = *legal.aggressive;
  const auto effective = std::min(bounds.maximum, add(other.street_committed, other.stack));
  // If the opponent cannot match a minimum full raise, the rules still require
  // that minimum (unless the actor itself is short); excess is refunded later.
  const auto cap = std::max(bounds.minimum, effective);
  const Chips base = add(hero.street_committed, legal.call_amount);
  const Chips pot_after_call = add(state.pot(), legal.call_amount);
  std::vector<Chips> targets{bounds.minimum, cap};
  const auto& schedule = sizes[static_cast<std::size_t>(state.street())];
  const auto& fractions = bounds.type == ActionType::Bet ? schedule.bets : schedule.raises;
  for (auto f : fractions) {
    const auto target = add(base, ceil_fraction(pot_after_call, f));
    targets.push_back(std::clamp(target, bounds.minimum, cap));
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  for (auto target : targets)
    result.push_back({bounds.type, target});
  require(result.size() <= 32, "abstract action count exceeds 32");
  return result;
}

InformationKey information_key(const HeadsUpState& state, std::array<int, 2> own) {
  require(state.actor().has_value(), "information key requires action phase");
  std::sort(own.begin(), own.end());
  require(own[0] >= 0 && own[1] < 52 && own[0] != own[1], "invalid private hand");
  for (int card : state.board())
    require(card != own[0] && card != own[1], "private card on public board");
  InformationKey key{*state.actor(), static_cast<std::uint64_t>(own[0]),
                     static_cast<std::uint64_t>(own[1]), state.board().size()};
  for (int card : state.board())
    key.push_back(static_cast<std::uint64_t>(card));
  for (const auto& event : state.history()) {
    key.push_back(static_cast<std::uint64_t>(event.street));
    key.push_back(event.actor);
    key.push_back(static_cast<std::uint64_t>(event.action.type));
    key.push_back(event.action.target_total);
  }
  return key;
}

const PolicyRow* HeadsUpPolicy::lookup(const HeadsUpState& state, std::array<int, 2> own) const {
  if (!same_root(game_.root, state.root()) || !state.actor())
    return nullptr;
  for (std::size_t i = 3; i < state.board().size(); ++i)
    if (game_.fixed_runout[i - 3] && state.board()[i] != *game_.fixed_runout[i - 3])
      return nullptr;
  const auto it = rows_.find(information_key(state, own));
  return it == rows_.end() ? nullptr : &it->second;
}

HeadsUpTrainer::HeadsUpTrainer(HeadsUpGame game) : game_(std::move(game)) {
  const HeadsUpState root(game_.root);
  std::array<bool, 52> blocked{};
  for (int card : game_.root.flop)
    reserve_card(card, blocked);
  for (auto card : game_.fixed_runout)
    if (card)
      reserve_card(*card, blocked);
  for (auto& range : game_.ranges) {
    require(!range.empty() && range.size() <= 1326, "invalid range size");
    double maximum = 0;
    std::set<std::array<int, 2>> seen;
    for (auto& hand : range) {
      std::sort(hand.cards.begin(), hand.cards.end());
      require(hand.cards[0] >= 0 && hand.cards[1] < 52 && hand.cards[0] != hand.cards[1],
              "invalid range cards");
      require(!blocked[hand.cards[0]] && !blocked[hand.cards[1]], "blocked range hand");
      require(seen.insert(hand.cards).second, "duplicate range combination");
      require(std::isfinite(hand.weight) && hand.weight > 0, "invalid range weight");
      maximum = std::max(maximum, hand.weight);
    }
    for (auto& hand : range)
      hand.weight /= maximum;
    std::sort(range.begin(), range.end(),
              [](const auto& a, const auto& b) { return a.cards < b.cards; });
  }
  for (const auto& street : game_.sizes)
    for (const auto* fractions : {&street.bets, &street.raises}) {
      require(fractions->size() <= 27, "too many size fractions");
      for (auto fraction : *fractions)
        (void)ceil_fraction(1, fraction);
    }
}

TrainingResult HeadsUpTrainer::train(std::uint64_t iterations, TrainingLimits limits) const {
  Budget budget(limits);
  TrainingResult result;
  Table committed;
  try {
    // Charge two game copies for the committed and prospective publications.
    std::size_t game_bytes = 2 * sizeof(HeadsUpGame);
    for (const auto& range : game_.ranges)
      game_bytes += 2 * range.size() * sizeof(WeightedHand);
    for (const auto& street : game_.sizes)
      game_bytes += 2 * (street.bets.size() + street.raises.size()) * sizeof(Fraction);
    budget.allocate(game_bytes);
    result.policy.game_ = game_;
    auto deals = joint_deals(game_, budget);
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
      Table pending = committed;
      const auto bytes_before = budget.bytes;
      try {
        for (std::size_t traverser = 0; traverser < 2; ++traverser) {
          FullTraversal traversal{game_, pending, budget, {}, {}};
          for (const auto& deal : deals)
            (void)traversal.walk(HeadsUpState(game_.root), deal, traverser, {1, 1},
                                 deal.probability);
          traversal.finish();
        }
        HeadsUpPolicy publication;
        publication.game_ = game_;
        for (const auto& [key, info] : pending)
          publication.rows_.emplace(key, PolicyRow{info.actions, normalize(info.sums)});
        result.policy = std::move(publication);
      } catch (...) {
        budget.bytes = bytes_before;
        throw;
      }
      committed.swap(pending);
      ++result.completed_iterations;
    }
    result.status = TrainingStatus::Complete;
  } catch (const Exhausted&) {
    result.status = TrainingStatus::ResourceLimit;
  } catch (const std::bad_alloc&) {
    result.status = TrainingStatus::ResourceLimit;
  }
  result.nodes = budget.nodes;
  result.information_sets = committed.size();
  result.accounted_bytes = budget.peak_bytes;
  return result;
}

ExactEvaluation HeadsUpTrainer::evaluate(const HeadsUpPolicy& policy, TrainingLimits limits) const {
  // Evaluation uses the policy's declared game, never an unverified caller
  // replacement range or abstraction. Full key identity lives inside the policy.
  require(same_root(game_.root, policy.game().root), "evaluation root mismatch");
  Budget budget(limits);
  ExactEvaluation result;
  try {
    const auto deals = joint_deals(policy.game(), budget);
    for (std::size_t p = 0; p < 2; ++p) {
      for (const auto& hand : policy.game().ranges[p]) {
        Frame root_memory(budget, 0, deals.size() * sizeof(double));
        std::vector<double> reach(deals.size());
        for (std::size_t d = 0; d < deals.size(); ++d)
          if (deals[d].hands[p] == hand.cards)
            reach[d] = deals[d].probability;
        result.profile_value[p] += response(policy.game(), policy, HeadsUpState(game_.root), deals,
                                            reach, p, hand.cards, false, budget);
        result.best_response_value[p] += response(policy.game(), policy, HeadsUpState(game_.root),
                                                  deals, reach, p, hand.cards, true, budget);
      }
      result.nash_conv += result.best_response_value[p] - result.profile_value[p];
    }
    result.exploitability_pot = result.nash_conv / static_cast<double>(game_.root.pot);
  } catch (const Exhausted&) {
    throw std::runtime_error("exact evaluation resource limit");
  } catch (const std::bad_alloc&) {
    throw std::runtime_error("exact evaluation allocation failure");
  }
  return result;
}

}  // namespace bs::solver
