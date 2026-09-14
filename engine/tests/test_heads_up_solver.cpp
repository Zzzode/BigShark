// Independent public-API regressions for RFC 0004 Stage 2 full traversal.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bs::poker;
using namespace bs::solver;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using Hand = std::array<int, 2>;
using Holes = std::array<Hand, 2>;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

bool near(double a, double b, double tolerance = 1e-12) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

template <typename Exception, typename F>
bool throws_as(F&& operation) {
  try {
    operation();
  } catch (const Exception&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

HeadsUpGame fixture(Chips stack = 1) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {stack, stack}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
  game.fixed_runout = {card("Js"), card("9c")};
  return game;
}

TrainingLimits training_budget() {
  TrainingLimits limits;
  // max_nodes is cumulative over the complete call, not per iteration.
  limits.max_nodes = 5000000;
  return limits;
}

struct JointDeal {
  Holes holes;
  double probability;
};

std::vector<JointDeal> enumerate_deals(const HeadsUpGame& game) {
  std::vector<JointDeal> result;
  long double total = 0;
  for (const auto& first : game.ranges[0]) {
    for (const auto& second : game.ranges[1]) {
      std::set<int> cards(game.root.flop.begin(), game.root.flop.end());
      for (auto fixed : game.fixed_runout)
        if (fixed)
          cards.insert(*fixed);
      bool compatible = true;
      for (int c : first.cards)
        compatible = cards.insert(c).second && compatible;
      for (int c : second.cards)
        compatible = cards.insert(c).second && compatible;
      if (!compatible)
        continue;
      const long double mass = static_cast<long double>(first.weight) * second.weight;
      total += mass;
      result.push_back({{first.cards, second.cards}, static_cast<double>(mass)});
    }
  }
  if (total <= 0)
    throw std::invalid_argument("oracle has no compatible deals");
  for (auto& deal : result)
    deal.probability = static_cast<double>(deal.probability / total);
  return result;
}

std::vector<int> enumerate_cards(const HeadsUpGame& game, const HeadsUpState& state,
                                 const Holes& holes) {
  const auto fixed = game.fixed_runout[state.board().size() - 3];
  if (fixed)
    return {*fixed};
  std::set<int> unavailable(state.board().begin(), state.board().end());
  for (const auto& hand : holes)
    unavailable.insert(hand.begin(), hand.end());
  for (auto reserved : game.fixed_runout)
    if (reserved)
      unavailable.insert(*reserved);
  std::vector<int> result;
  for (int c = 0; c < 52; ++c)
    if (!unavailable.contains(c))
      result.push_back(c);
  return result;
}

// In the equal one/two-chip fixtures every legal integer target belongs to the
// default abstraction. Enumerate them without consulting abstract_actions.
std::vector<Action> enumerate_actions(const HeadsUpState& state) {
  const auto legal = state.legal();
  std::vector<Action> actions;
  if (legal.fold)
    actions.push_back({ActionType::Fold});
  if (legal.check)
    actions.push_back({ActionType::Check});
  if (legal.call)
    actions.push_back({ActionType::Call});
  if (legal.aggressive) {
    if (legal.aggressive->maximum > 2)
      throw std::invalid_argument("oracle requires stacks at most two");
    for (Chips target = legal.aggressive->minimum; target <= legal.aggressive->maximum; ++target)
      actions.push_back({legal.aggressive->type, target});
  }
  return actions;
}

// This label is deliberately separate from the solver's InformationKey.
std::string observation(const HeadsUpState& state, Hand own) {
  std::sort(own.begin(), own.end());
  std::ostringstream out;
  out << *state.actor() << ':' << own[0] << ',' << own[1] << ';';
  for (int c : state.board())
    out << c << ',';
  out << ';';
  for (const auto& event : state.history())
    out << static_cast<int>(event.street) << ':' << event.actor << ':'
        << static_cast<int>(event.action.type) << ':' << event.action.target_total << ';';
  return out.str();
}

struct OracleNode {
  int actor = -1;
  std::size_t information_set = 0;
  std::vector<std::size_t> children;
  std::vector<double> probabilities;
  std::array<double, 2> utility{};
};

// Compile public-rule trees once, then enumerate complete pure response policies.
// A response selects the same action at every occurrence of its own observation.
// No native response evaluator, information key, or private solver hook is used.
class PureOracle {
 public:
  PureOracle(const HeadsUpGame& game, const HeadsUpPolicy& policy) {
    for (const auto& deal : enumerate_deals(game))
      roots_.push_back(
          {build(game, policy, HeadsUpState(game.root), deal.holes), deal.probability});
  }

  std::size_t information_sets(std::size_t player) const { return choices_[player].size(); }
  std::size_t coverage() const { return information_sets(0) + information_sets(1); }

  double profile(std::size_t player) const { return value(player, nullptr); }

  double best_response(std::size_t player) const {
    if (information_sets(player) > 16)
      throw std::invalid_argument("pure-policy oracle exceeds sixteen information sets");
    std::size_t count = 1;
    for (std::size_t radix : choices_[player])
      count *= radix;
    if (count > 65536)
      throw std::invalid_argument("pure-policy oracle exceeds 65536 policies");
    double best = -std::numeric_limits<double>::infinity();
    std::vector<std::size_t> pure(information_sets(player));
    for (std::size_t code = 0; code < count; ++code) {
      auto digits = code;
      for (std::size_t i = 0; i < pure.size(); ++i) {
        pure[i] = digits % choices_[player][i];
        digits /= choices_[player][i];
      }
      best = std::max(best, value(player, &pure));
    }
    return best;
  }

 private:
  std::vector<OracleNode> nodes_;
  std::vector<std::pair<std::size_t, double>> roots_;
  std::array<std::map<std::string, std::size_t>, 2> observations_;
  std::array<std::vector<std::size_t>, 2> choices_;

  std::size_t build(const HeadsUpGame& game, const HeadsUpPolicy& policy, const HeadsUpState& state,
                    const Holes& holes) {
    const auto id = nodes_.size();
    nodes_.emplace_back();
    OracleNode node;
    if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown) {
      const auto settlement =
          state.phase() == Phase::Folded ? state.settle_fold() : state.settle_showdown(holes);
      for (std::size_t p = 0; p < 2; ++p)
        node.utility[p] = static_cast<double>(settlement.net_utility[p]);
      if (settlement.net_utility[0] + settlement.net_utility[1] != 0)
        throw std::runtime_error("oracle terminal is not zero sum");
    } else if (state.phase() == Phase::Deal) {
      const auto cards = enumerate_cards(game, state, holes);
      for (int c : cards)
        node.children.push_back(build(game, policy, state.after_card(c), holes));
      node.probabilities.assign(cards.size(), 1.0 / static_cast<double>(cards.size()));
    } else {
      const auto actor = *state.actor();
      const auto actions = enumerate_actions(state);
      const auto* row = policy.lookup(state, holes[actor]);
      if (!row || row->actions != actions || row->probabilities.size() != actions.size())
        throw std::runtime_error("oracle encountered missing or incompatible policy");
      double sum = 0;
      for (double probability : row->probabilities) {
        if (!std::isfinite(probability) || probability < 0 || probability > 1)
          throw std::runtime_error("oracle encountered invalid probability");
        sum += probability;
      }
      if (!near(sum, 1))
        throw std::runtime_error("oracle encountered unnormalized policy");
      auto [it, inserted] =
          observations_[actor].emplace(observation(state, holes[actor]), choices_[actor].size());
      if (inserted)
        choices_[actor].push_back(actions.size());
      else if (choices_[actor][it->second] != actions.size())
        throw std::runtime_error("inconsistent oracle information set");
      node.actor = static_cast<int>(actor);
      node.information_set = it->second;
      node.probabilities = row->probabilities;
      for (Action action : actions)
        node.children.push_back(build(game, policy, state.after_action(actor, action), holes));
    }
    nodes_[id] = std::move(node);
    return id;
  }

  double walk(std::size_t id, std::size_t player, const std::vector<std::size_t>* pure) const {
    const auto& node = nodes_[id];
    if (node.children.empty())
      return node.utility[player];
    if (pure && node.actor == static_cast<int>(player))
      return walk(node.children[(*pure)[node.information_set]], player, pure);
    double result = 0;
    for (std::size_t a = 0; a < node.children.size(); ++a)
      result += node.probabilities[a] * walk(node.children[a], player, pure);
    return result;
  }

  double value(std::size_t player, const std::vector<std::size_t>* pure) const {
    double result = 0;
    for (const auto& [root, probability] : roots_)
      result += probability * walk(root, player, pure);
    return result;
  }
};

int compare_policies(const HeadsUpPolicy& first, const HeadsUpPolicy& second) {
  CHECK(first.rows().size() == second.rows().size());
  auto a = first.rows().begin();
  auto b = second.rows().begin();
  for (; a != first.rows().end(); ++a, ++b) {
    CHECK(a->first == b->first);
    CHECK(a->second.actions == b->second.actions);
    CHECK(a->second.probabilities == b->second.probabilities);
  }
  return 0;
}

int compare_evaluation(const HeadsUpGame& game, const HeadsUpPolicy& policy, bool pure = true) {
  const PureOracle oracle(game, policy);
  CHECK(oracle.coverage() == policy.rows().size());
  const auto exact = HeadsUpTrainer(game).evaluate(policy);
  CHECK(near(exact.profile_value[0] + exact.profile_value[1], 0));
  double gains = 0;
  for (std::size_t p = 0; p < 2; ++p) {
    CHECK(near(exact.profile_value[p], oracle.profile(p)));
    CHECK(exact.best_response_value[p] + 1e-12 >= exact.profile_value[p]);
    if (pure) {
      CHECK(oracle.information_sets(p) <= 16);
      CHECK(near(exact.best_response_value[p], oracle.best_response(p)));
    }
    gains += exact.best_response_value[p] - exact.profile_value[p];
  }
  CHECK(near(exact.nash_conv, gains));
  CHECK(near(exact.exploitability_pot, gains / static_cast<double>(game.root.pot)));
  return 0;
}

int test_sizing() {
  auto game = fixture();
  game.root.stacks = {100, 100};
  const HeadsUpState root(game.root);
  CHECK((abstract_actions(root, game.sizes) == std::vector<Action>{{ActionType::Check},
                                                                   {ActionType::Bet, 1},
                                                                   {ActionType::Bet, 2},
                                                                   {ActionType::Bet, 3},
                                                                   {ActionType::Bet, 100}}));
  const auto facing = root.after_action(0, {ActionType::Bet, 3});
  CHECK((abstract_actions(facing, game.sizes) == std::vector<Action>{{ActionType::Fold},
                                                                     {ActionType::Call},
                                                                     {ActionType::Raise, 6},
                                                                     {ActionType::Raise, 7},
                                                                     {ActionType::Raise, 11},
                                                                     {ActionType::Raise, 100}}));
  const auto reraised = facing.after_action(1, {ActionType::Raise, 7});
  CHECK((abstract_actions(reraised, game.sizes) == std::vector<Action>{{ActionType::Fold},
                                                                       {ActionType::Call},
                                                                       {ActionType::Raise, 11},
                                                                       {ActionType::Raise, 15},
                                                                       {ActionType::Raise, 23},
                                                                       {ActionType::Raise, 100}}));
  auto sizes = game.sizes;
  sizes[0].bets = {{1, 3}, {1, 3}, {1, 4}, {1000, 1}};
  CHECK((abstract_actions(root, sizes) ==
         std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 1}, {ActionType::Bet, 100}}));
  game.root.stacks = {100, 2};
  CHECK((abstract_actions(HeadsUpState(game.root), game.sizes) ==
         std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 1}, {ActionType::Bet, 2}}));
  game.root.big_blind = 3;
  CHECK((abstract_actions(HeadsUpState(game.root), game.sizes) ==
         std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 3}}));
  for (Fraction invalid : {Fraction{0, 1}, Fraction{1, 0}, Fraction{2, 4}, Fraction{2, 2}}) {
    sizes = fixture().sizes;
    sizes[0].bets = {invalid};
    CHECK(throws_as<std::invalid_argument>([&] { abstract_actions(root, sizes); }));
    for (std::size_t street = 0; street < 3; ++street) {
      for (bool raise : {false, true}) {
        auto bad = fixture();
        (raise ? bad.sizes[street].raises : bad.sizes[street].bets) = {invalid};
        CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
      }
    }
  }
  sizes = fixture().sizes;
  sizes[0].bets = {{std::numeric_limits<std::uint64_t>::max(), 1}};
  CHECK(throws_as<std::overflow_error>([&] { abstract_actions(root, sizes); }));
  sizes = fixture().sizes;
  sizes[0].raises = {{std::numeric_limits<std::uint64_t>::max(), 1}};
  CHECK(throws_as<std::overflow_error>([&] { abstract_actions(facing, sizes); }));
  const auto addition_overflow = root.after_action(0, {ActionType::Bet, 5});
  sizes[0].raises = {{std::numeric_limits<std::uint64_t>::max() / 12, 1}};
  CHECK(throws_as<std::overflow_error>([&] { abstract_actions(addition_overflow, sizes); }));
  auto bad = fixture();
  bad.sizes[2].raises.assign(28, {1, 1});
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));

  game = fixture();
  game.root.stacks = {20, 20};
  game.sizes[1].bets = {{1, 1}};
  game.sizes[2].bets = {{2, 1}};
  auto turn = HeadsUpState(game.root)
                  .after_action(0, {ActionType::Check})
                  .after_action(1, {ActionType::Check})
                  .after_card(card("Js"));
  CHECK((abstract_actions(turn, game.sizes) == std::vector<Action>{{ActionType::Check},
                                                                   {ActionType::Bet, 1},
                                                                   {ActionType::Bet, 2},
                                                                   {ActionType::Bet, 20}}));
  const auto river = turn.after_action(0, {ActionType::Check})
                         .after_action(1, {ActionType::Check})
                         .after_card(card("9c"));
  CHECK((abstract_actions(river, game.sizes) == std::vector<Action>{{ActionType::Check},
                                                                    {ActionType::Bet, 1},
                                                                    {ActionType::Bet, 4},
                                                                    {ActionType::Bet, 20}}));
  return 0;
}

int test_information_keys() {
  auto game = fixture(2);
  const HeadsUpState root(game.root);
  const Hand own = game.ranges[0][0].cards;
  const auto key = information_key(root, own);
  CHECK(key == information_key(root, {own[1], own[0]}));
  CHECK(key != information_key(root, game.ranges[0][1].cards));
  const auto bet = root.after_action(0, {ActionType::Bet, 1});
  const auto check_bet =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Bet, 1});
  CHECK(information_key(bet, own) != information_key(check_bet, own));
  const auto a = root.after_action(0, {ActionType::Bet, 1})
                     .after_action(1, {ActionType::Call})
                     .after_card(card("Js"));
  const auto b = root.after_action(0, {ActionType::Check})
                     .after_action(1, {ActionType::Bet, 1})
                     .after_action(0, {ActionType::Call})
                     .after_card(card("Js"));
  CHECK(a.actor() == b.actor());
  CHECK(a.pot() == b.pot());
  CHECK(a.players()[0].stack == b.players()[0].stack);
  CHECK(a.players()[1].stack == b.players()[1].stack);
  CHECK(a.board() == b.board());
  CHECK(information_key(a, own) != information_key(b, own));
  const auto deal = root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const auto first = deal.after_card(card("Js"))
                         .after_action(0, {ActionType::Check})
                         .after_action(1, {ActionType::Check})
                         .after_card(card("9c"));
  const auto second = deal.after_card(card("9c"))
                          .after_action(0, {ActionType::Check})
                          .after_action(1, {ActionType::Check})
                          .after_card(card("Js"));
  CHECK(information_key(first, own) != information_key(second, own));
  for (Hand invalid : {Hand{-1, 4}, Hand{4, 52}, Hand{4, 4}, Hand{game.root.flop[0], own[0]}})
    CHECK(throws_as<std::invalid_argument>([&] { information_key(root, invalid); }));
  CHECK(throws_as<std::invalid_argument>([&] { information_key(deal, own); }));

  // Target totals beyond 32 bits must remain distinct in exact history.
  const Chips high = (Chips{1} << 32) + 1;
  game.root.stacks = {high + 1, high + 1};
  const HeadsUpState large(game.root);
  CHECK(information_key(large.after_action(0, {ActionType::Bet, 1}), own) !=
        information_key(large.after_action(0, {ActionType::Bet, high}), own));
  return 0;
}

int test_joint_deals_and_validation() {
  const auto game = fixture();
  const auto deals = enumerate_deals(game);
  CHECK(deals.size() == 3);
  CHECK(near(deals[0].probability, 14.0 / 50));
  CHECK(near(deals[1].probability, 15.0 / 50));
  CHECK(near(deals[2].probability, 21.0 / 50));
  CHECK(deals[1].holes[0] == deals[2].holes[0]);
  CHECK(deals[1].holes[1] != deals[2].holes[1]);
  const HeadsUpState root(game.root);
  CHECK(information_key(root, deals[1].holes[0]) == information_key(root, deals[2].holes[0]));
  const auto trained = HeadsUpTrainer(game).train(1);
  CHECK(trained.status == TrainingStatus::Complete);
  CHECK(trained.completed_iterations == 1);
  CHECK(trained.information_sets == 24);
  CHECK(trained.policy.lookup(root, deals[1].holes[0]) ==
        trained.policy.lookup(root, deals[2].holes[0]));
  CHECK(compare_evaluation(game, trained.policy) == 0);

  for (std::size_t p = 0; p < 2; ++p) {
    for (double weight : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity()}) {
      auto bad = game;
      bad.ranges[p][0].weight = weight;
      CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
    }
    auto bad = game;
    bad.ranges[p].clear();
    CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
    bad = game;
    auto duplicate = bad.ranges[p][0];
    std::swap(duplicate.cards[0], duplicate.cards[1]);
    bad.ranges[p].push_back(duplicate);
    CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
    for (int invalid : {-1, 52, game.root.flop[0], *game.fixed_runout[0], *game.fixed_runout[1],
                        game.ranges[p][0].cards[1]}) {
      bad = game;
      bad.ranges[p][0].cards[0] = invalid;
      CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
    }
  }
  for (std::size_t slot = 0; slot < 2; ++slot)
    for (int invalid : {-1, 52, game.root.flop[0], *game.fixed_runout[1 - slot]}) {
      auto bad = game;
      bad.fixed_runout[slot] = invalid;
      CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(bad); }));
    }
  auto incompatible = game;
  incompatible.ranges[0].resize(1);
  incompatible.ranges[1].resize(1);
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer(incompatible).train(1); }));
  return 0;
}

int test_fixed_future_blocking() {
  auto game = fixture();
  game.fixed_runout[0].reset();
  const auto deals = enumerate_deals(game);
  const auto deal_state = HeadsUpState(game.root)
                              .after_action(0, {ActionType::Check})
                              .after_action(1, {ActionType::Check});
  for (const auto& deal : deals) {
    const auto cards = enumerate_cards(game, deal_state, deal.holes);
    CHECK(cards.size() == 44);
    CHECK(std::find(cards.begin(), cards.end(), *game.fixed_runout[1]) == cards.end());
  }
  const auto trained = HeadsUpTrainer(game).train(1);
  CHECK(trained.status == TrainingStatus::Complete);
  CHECK(compare_evaluation(game, trained.policy, false) == 0);
  auto impossible = game;
  impossible.ranges[0][0].cards[0] = *game.fixed_runout[1];
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer trainer(impossible); }));
  return 0;
}

int test_convergence_and_repeatability() {
  const auto game = fixture();
  const HeadsUpTrainer trainer(game);
  const auto early = trainer.train(1);
  const auto late = trainer.train(8192, training_budget());
  CHECK(early.status == TrainingStatus::Complete);
  CHECK(late.status == TrainingStatus::Complete);
  CHECK(late.completed_iterations == 8192);
  const auto before = trainer.evaluate(early.policy);
  const auto after = trainer.evaluate(late.policy);
  CHECK(after.exploitability_pot < before.exploitability_pot);
  CHECK(after.exploitability_pot <= 0.002);
  CHECK(compare_evaluation(game, late.policy) == 0);
  std::printf(
      "heads-up full traversal: iterations=%llu nodes=%zu infosets=%zu "
      "initial=%.9f final=%.9f\n",
      static_cast<unsigned long long>(late.completed_iterations), late.nodes, late.information_sets,
      before.exploitability_pot, after.exploitability_pot);
  const auto first = trainer.train(64);
  const auto second = trainer.train(64);
  CHECK(first.status == TrainingStatus::Complete);
  CHECK(second.status == TrainingStatus::Complete);
  CHECK(first.nodes == second.nodes);
  CHECK(first.accounted_bytes == second.accounted_bytes);
  CHECK(compare_policies(first.policy, second.policy) == 0);
  CHECK(compare_evaluation(game, first.policy) == 0);

  auto reordered = game;
  for (auto& range : reordered.ranges) {
    std::reverse(range.begin(), range.end());
    for (auto& hand : range) {
      std::swap(hand.cards[0], hand.cards[1]);
      hand.weight *= 8;
    }
  }
  const auto canonical = HeadsUpTrainer(reordered).train(64);
  CHECK(canonical.status == TrainingStatus::Complete);
  CHECK(compare_policies(first.policy, canonical.policy) == 0);

  for (std::size_t button : {std::size_t{0}, std::size_t{1}}) {
    auto near_all_in = fixture(2);
    near_all_in.root.button = button;
    const auto trained = HeadsUpTrainer(near_all_in).train(32);
    CHECK(trained.status == TrainingStatus::Complete);
    CHECK(compare_evaluation(near_all_in, trained.policy, false) == 0);
  }
  return 0;
}

int test_coverage_misses() {
  const auto game = fixture();
  const HeadsUpTrainer trainer(game);
  const HeadsUpState root(game.root);
  const HeadsUpPolicy empty;
  CHECK(empty.rows().empty());
  CHECK(empty.lookup(root, game.ranges[0][0].cards) == nullptr);
  CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(empty); }));
  const auto untrained = trainer.train(0);
  CHECK(untrained.status == TrainingStatus::Complete);
  CHECK(untrained.completed_iterations == 0);
  CHECK(untrained.policy.rows().empty());
  CHECK(untrained.policy.lookup(root, game.ranges[0][0].cards) == nullptr);
  CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(untrained.policy); }));
  const auto trained = trainer.train(1);
  CHECK(trained.status == TrainingStatus::Complete);
  CHECK(trained.policy.lookup(root, {card("Kh"), card("Kd")}) == nullptr);
  const auto* row = trained.policy.lookup(root, game.ranges[0][0].cards);
  CHECK(row != nullptr);
  CHECK(row ==
        trained.policy.lookup(root, {game.ranges[0][0].cards[1], game.ranges[0][0].cards[0]}));
  for (int field = 0; field < 5; ++field) {
    auto different = game;
    if (field == 0)
      ++different.root.stacks[0];
    else if (field == 1)
      different.root.button = 0;
    else if (field == 2)
      ++different.root.big_blind;
    else if (field == 3) {
      different.root.contributions = {2, 2};
      different.root.pot = 4;
    } else
      different.root.flop[0] = card("4c");
    CHECK(trained.policy.lookup(HeadsUpState(different.root), game.ranges[0][0].cards) == nullptr);
    CHECK(throws_as<std::invalid_argument>(
        [&] { HeadsUpTrainer(different).evaluate(trained.policy); }));
  }
  const auto waiting =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  CHECK(trained.policy.lookup(waiting, game.ranges[0][0].cards) == nullptr);
  const auto wrong_board = waiting.after_card(card("Ts"));
  CHECK(trained.policy.lookup(wrong_board, game.ranges[0][0].cards) == nullptr);
  return 0;
}

int test_resource_rollback() {
  const auto game = fixture();
  const HeadsUpTrainer trainer(game);
  const auto one = trainer.train(1);
  const auto two = trainer.train(2);
  CHECK(one.status == TrainingStatus::Complete);
  CHECK(two.status == TrainingStatus::Complete);
  CHECK(two.nodes > one.nodes + 1);
  for (std::size_t cap : {one.nodes, one.nodes + 1, two.nodes - 1}) {
    auto limits = training_budget();
    limits.max_nodes = cap;
    const auto partial = trainer.train(3, limits);
    CHECK(partial.status == TrainingStatus::ResourceLimit);
    CHECK(partial.completed_iterations == 1);
    CHECK(partial.nodes == cap);
    CHECK(partial.information_sets == one.information_sets);
    CHECK(partial.accounted_bytes >= one.accounted_bytes);
    CHECK(partial.accounted_bytes <= limits.max_bytes);
    CHECK(compare_policies(partial.policy, one.policy) == 0);
    CHECK(compare_evaluation(game, partial.policy) == 0);
  }
  auto limits = training_budget();
  limits.max_nodes = one.nodes;
  const auto exact_fit = trainer.train(1, limits);
  CHECK(exact_fit.status == TrainingStatus::Complete);
  CHECK(compare_policies(one.policy, exact_fit.policy) == 0);
  for (int resource = 0; resource < 3; ++resource) {
    limits = training_budget();
    if (resource == 0)
      limits.max_nodes = one.nodes - 1;
    else if (resource == 1)
      limits.max_information_sets = 1;
    else
      limits.max_bytes = 1;
    const auto failed = trainer.train(3, limits);
    CHECK(failed.status == TrainingStatus::ResourceLimit);
    CHECK(failed.completed_iterations == 0);
    CHECK(failed.information_sets == 0);
    CHECK(failed.policy.rows().empty());
    CHECK(failed.policy.lookup(HeadsUpState(game.root), game.ranges[0][0].cards) == nullptr);
    CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(failed.policy); }));
  }
  limits = training_budget();
  limits.max_nodes = 1;
  CHECK(throws_as<std::runtime_error>([&] { trainer.evaluate(one.policy, limits); }));
  for (int resource = 0; resource < 4; ++resource) {
    limits = training_budget();
    if (resource == 0)
      limits.max_nodes = 0;
    else if (resource == 1)
      limits.max_information_sets = 0;
    else if (resource == 2)
      limits.max_bytes = 0;
    else
      limits.time = std::chrono::milliseconds{0};
    CHECK(throws_as<std::invalid_argument>([&] { trainer.train(1, limits); }));
  }
  return 0;
}

int test_numeric_and_resource_findings() {
  const auto game = fixture();
  const HeadsUpTrainer trainer(game);
  const auto trained = trainer.train(1);
  CHECK(trained.status == TrainingStatus::Complete);
  CHECK(trained.information_sets > 0);
  CHECK(!trained.policy.rows().empty());

  auto limits = training_budget();
  limits.time = std::chrono::milliseconds::max();
  CHECK(throws_as<std::invalid_argument>([&] { trainer.train(1, limits); }));
  CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(trained.policy, limits); }));
  for (std::size_t depth :
       {std::size_t{0}, std::size_t{257}, std::numeric_limits<std::size_t>::max()}) {
    limits = training_budget();
    limits.max_depth = depth;
    CHECK(throws_as<std::invalid_argument>([&] { trainer.train(1, limits); }));
    CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(trained.policy, limits); }));
  }
  limits = training_budget();
  limits.max_depth = 1;
  const auto shallow = trainer.train(2, limits);
  CHECK(shallow.status == TrainingStatus::ResourceLimit);
  CHECK(shallow.completed_iterations == 0);
  CHECK(shallow.information_sets == 0);
  CHECK(shallow.policy.rows().empty());
  CHECK(shallow.policy.lookup(HeadsUpState(game.root), game.ranges[0][0].cards) == nullptr);
  CHECK(throws_as<std::invalid_argument>([&] { trainer.evaluate(shallow.policy); }));
  CHECK(throws_as<std::runtime_error>([&] { trainer.evaluate(trained.policy, limits); }));
  for (std::size_t bytes : {std::size_t{1}, std::size_t{4096}}) {
    limits = training_budget();
    limits.max_bytes = bytes;
    CHECK(throws_as<std::runtime_error>([&] { trainer.evaluate(trained.policy, limits); }));
  }

  const double tiny = std::numeric_limits<double>::denorm_min();
  CHECK(tiny > 0);
  for (std::size_t p = 0; p < 2; ++p) {
    auto unsupported = game;
    unsupported.ranges[p][0].weight = tiny;
    unsupported.ranges[p][1].weight = std::numeric_limits<double>::max();
    CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer(unsupported).train(1); }));
  }
  // Each individual range survives normalization, but the compatible joint
  // probability cannot be represented after division by the total mass.
  auto unsupported = game;
  unsupported.ranges[0][0].weight = tiny;
  unsupported.ranges[0][1].weight = 1;
  for (auto& hand : unsupported.ranges[1])
    hand.weight = 1;
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpTrainer(unsupported).train(1); }));
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_sizing() == 0);
    CHECK(test_information_keys() == 0);
    CHECK(test_joint_deals_and_validation() == 0);
    CHECK(test_fixed_future_blocking() == 0);
    CHECK(test_convergence_and_repeatability() == 0);
    CHECK(test_coverage_misses() == 0);
    CHECK(test_resource_rollback() == 0);
    CHECK(test_numeric_and_resource_findings() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected heads-up solver exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_heads_up_solver PASS\n");
  return 0;
}
