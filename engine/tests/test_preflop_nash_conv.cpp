// RFC 0015: independent fixtures for information-set pooling and held-out
// confirmation. The tiny native games use declared diagnostic frontier values.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "../benchmarks/preflop_nash_conv_detail.hpp"

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
      return 1;                                                                           \
    }                                                                                     \
  } while (false)

namespace {

namespace measurement = bs::benchmarks::preflop;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::GameDef;
using bs::solver::InformationKey;
using bs::solver::SeatPolicyRow;
using bs::tree::AbstractTree;
using bs::tree::TerminalPayload;

bool near(double a, double b) {
  return std::abs(a - b) < 1e-10;
}

template <class F>
bool throws(F&& run) {
  try {
    run();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

GameDef heads_up(bs::poker::Chips stack = 4) {
  GameDef def;
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {stack, stack};
  def.blinds_posted = {1, 2};
  def.pot = 2;
  def.preflop = true;
  def.terminal = bs::poker::TerminalDepth::Flop;
  return def;
}

measurement::World world(std::uint64_t seed, bool hidden_a = true,
                         std::array<int, 2> own = {0, 1}) {
  return {
      seed, {own, hidden_a ? std::array<int, 2>{2, 3} : std::array<int, 2>{4, 5}}, {{30, 31, 32}}};
}

SeatPolicyRow point(std::span<const Action> menu, std::size_t chosen) {
  SeatPolicyRow row;
  row.actions.assign(menu.begin(), menu.end());
  row.probabilities.assign(menu.size(), 0.0);
  row.probabilities.at(chosen) = 1.0;
  return row;
}

std::size_t find_action(std::span<const Action> menu, ActionType type) {
  for (std::size_t i = 0; i < menu.size(); ++i)
    if (menu[i].type == type)
      return i;
  throw std::runtime_error("fixture action is missing");
}

measurement::ProfileQuery passive() {
  return [](const InformationKey&, std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
    for (const auto type : {ActionType::Check, ActionType::Call, ActionType::Fold})
      for (std::size_t i = 0; i < menu.size(); ++i)
        if (menu[i].type == type)
          return point(menu, i);
    throw std::runtime_error("fixture has no passive action");
  };
}

class ConstantFrontier final : public bs::gto::FrontierEvaluator {
 public:
  explicit ConstantFrontier(std::vector<double> values) : values_(std::move(values)) {}
  std::vector<double> evaluate(std::span<const int>, std::span<const std::array<int, 2>>,
                               const TerminalPayload&) const override {
    return values_;
  }

 private:
  std::vector<double> values_;
};

class HiddenConflictFrontier final : public bs::gto::FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int> flop, std::span<const std::array<int, 2>> hands,
                               const TerminalPayload& ledger) const override {
    // The two public lines reverse their hidden-world payoffs. An omniscient
    // max gets +4 in both worlds; a single observable root action averages 0.
    if (!std::equal(flop.begin(), flop.end(), expected_flop.begin(), expected_flop.end()))
      throw std::runtime_error("alternative actions did not reuse the world's fixed flop");
    const double hidden_sign = hands[1][0] == 2 ? 1.0 : -1.0;
    const double line_sign = ledger.seats[0].contributed <= 2 ? 1.0 : -1.0;
    const double value = 4.0 * hidden_sign * line_sign;
    return {value, -value};
  }

 private:
  const std::array<int, 3> expected_flop{30, 31, 32};
};

int hidden_information_and_normalization() {
  const AbstractTree tree(heads_up(), bs::abstraction::ActionAbstraction::identity());
  const HiddenConflictFrontier frontier;
  const std::vector<measurement::World> learn{world(1), world(2, false)};
  const std::vector<measurement::World> confirm{world(10), world(11, false)};
  const auto profile = passive();
  const auto frozen = measurement::learn_response(tree, profile, frontier, 0, learn);
  const auto root_key = bs::solver::make_information_key(0, {0, 1}, {}, {});
  CHECK(frozen.actions.contains(root_key));
  CHECK(frozen.actions.at(root_key).menu[frozen.actions.at(root_key).chosen].type ==
        ActionType::Call);
  const auto result = measurement::confirm_response(tree, profile, frontier, frozen, confirm);
  CHECK(near(result.profile_value, 0.0));
  CHECK(near(result.deviation_value, 0.0));
  CHECK(result.deviation_value < 4.0);
  CHECK(near(result.gain.mean, 0.0));
  CHECK(near(result.gain.standard_error, 0.0));
  CHECK(result.counts.response_misses == 0);

  const measurement::ProfileQuery opening_raise =
      [&](const InformationKey& key, std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
    if (key[0] == 0 && key.size() == 4)
      return point(menu, find_action(menu, ActionType::Raise));
    return profile(key, menu);
  };
  const auto raised_response = measurement::learn_response(tree, opening_raise, frontier, 0, learn);
  const auto uncertain =
      measurement::confirm_response(tree, opening_raise, frontier, raised_response, confirm);
  CHECK(near(uncertain.profile_value, 0.0));
  CHECK(near(uncertain.deviation_value, 0.0));
  CHECK(near(uncertain.gain.mean, 0.0));
  CHECK(near(uncertain.gain.standard_error, 8.0));
  CHECK(uncertain.gain.ci95_low < 0.0 && uncertain.gain.ci95_high > 0.0);

  // Empty frozen maps must reproduce the profile for every seat, while each
  // baseline value is divided by its own two worlds, never by four seat-deals.
  const ConstantFrontier constant({6.0, -6.0});
  for (std::size_t seat = 0; seat < 2; ++seat) {
    measurement::LearnedResponse empty;
    empty.seat = seat;
    const auto value = measurement::confirm_response(tree, profile, constant, empty, confirm);
    CHECK(near(value.profile_value, seat == 0 ? 6.0 : -6.0));
    CHECK(near(value.deviation_value, value.profile_value));
    CHECK(near(value.gain.mean, 0.0));
    CHECK(value.counts.response_queries > 0);
    CHECK(value.counts.response_misses == value.counts.response_queries);
  }
  return 0;
}

int nested_actions() {
  const AbstractTree tree(heads_up(), bs::abstraction::ActionAbstraction::identity());
  const ConstantFrontier frontier({5.0, -5.0});
  const auto check_call = passive();
  const measurement::ProfileQuery profile =
      [&](const InformationKey& key, std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
    if (key[0] == 0)
      return point(menu, find_action(menu, ActionType::Fold));
    // After a limp, the big blind bets all in. Facing an opening raise it
    // folds instead, so the ancestor must use its learned descendant call.
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == ActionType::Bet)
        return point(menu, i);
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == ActionType::Fold)
        return point(menu, i);
    return check_call(key, menu);
  };
  const std::vector<measurement::World> learn{world(1), world(2, false)};
  const std::vector<measurement::World> confirm{world(10), world(11, false)};
  const auto frozen = measurement::learn_response(tree, profile, frontier, 0, learn);
  const auto root_key = bs::solver::make_information_key(0, {0, 1}, {}, {});
  const auto& root = frozen.actions.at(root_key);
  CHECK(root.menu[root.chosen].type == ActionType::Call);
  CHECK(frozen.actions.size() >= 2);
  bool descendant_call = false;
  for (const auto& [key, action] : frozen.actions)
    if (key.size() > root_key.size() && action.menu[action.chosen].type == ActionType::Call)
      descendant_call = true;
  CHECK(descendant_call);
  const auto result = measurement::confirm_response(tree, profile, frontier, frozen, confirm);
  CHECK(near(result.profile_value, -1.0));
  CHECK(near(result.deviation_value, 5.0));
  CHECK(near(result.gain.mean, 6.0));
  return 0;
}

int confirmation_is_frozen() {
  const AbstractTree tree(heads_up(), bs::abstraction::ActionAbstraction::identity());
  const HiddenConflictFrontier frontier;
  const auto check_call = passive();
  const measurement::ProfileQuery profile =
      [&](const InformationKey& key, std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
    if (key[0] == 0 && key.size() == 4)
      return point(menu, find_action(menu, ActionType::Fold));
    return check_call(key, menu);
  };
  const std::vector<measurement::World> learn{world(1), world(2)};
  const std::vector<measurement::World> confirm{world(10, false), world(11, false)};
  const auto frozen = measurement::learn_response(tree, profile, frontier, 0, learn);
  const auto actions_before = frozen.actions;
  const auto result = measurement::confirm_response(tree, profile, frontier, frozen, confirm);
  CHECK(frozen.actions == actions_before);
  CHECK(near(result.profile_value, -1.0));
  CHECK(near(result.deviation_value, -4.0));
  CHECK(near(result.gain.mean, -3.0));
  CHECK(near(result.gain.ci95_high, -3.0));
  CHECK(throws([&] { measurement::confirm_response(tree, profile, frontier, frozen, learn); }));

  const std::vector<measurement::World> unknown{world(20, false, {6, 7}), world(21, false, {6, 7})};
  const auto missing = measurement::confirm_response(tree, profile, frontier, frozen, unknown);
  CHECK(near(missing.gain.mean, 0.0));
  CHECK(near(missing.deviation_value, -1.0));
  CHECK(missing.counts.response_misses == 2);
  return 0;
}

int opponent_reach_conditions_descendant_pool() {
  const AbstractTree tree(heads_up(), bs::abstraction::ActionAbstraction::identity());
  class ConditionalFrontier final : public bs::gto::FrontierEvaluator {
   public:
    std::vector<double> evaluate(std::span<const int>, std::span<const std::array<int, 2>> hands,
                                 const TerminalPayload& ledger) const override {
      const bool hidden_a = hands[0][0] == 0;
      const bool checked_line = ledger.seats[1].contributed <= 2;
      const double responder_value = hidden_a == checked_line ? 4.0 : -4.0;
      return {-responder_value, responder_value};
    }
  } frontier;
  const auto check_call = passive();
  const measurement::ProfileQuery profile =
      [&](const InformationKey& key, std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
    if (key[0] == 0 && key.size() == 4)
      return point(menu, find_action(menu, key[1] == 0 ? ActionType::Call : ActionType::Raise));
    if (key[0] == 1)
      for (std::size_t i = 0; i < menu.size(); ++i)
        if (menu[i].type == ActionType::Fold)
          return point(menu, i);
    return check_call(key, menu);
  };
  const auto conditional_world = [](std::uint64_t seed, bool hidden_a) {
    return measurement::World{
        seed,
        {hidden_a ? std::array<int, 2>{0, 1} : std::array<int, 2>{2, 3}, {8, 9}},
        {{30, 31, 32}}};
  };
  // Hidden B dominates the unconditional sample, but only A reaches the limp
  // node. Pooling all worlds at that descendant would incorrectly prefer a
  // bet. After the public raise only B reaches the responder's call node.
  const std::vector<measurement::World> learn{
      conditional_world(1, true), conditional_world(2, false), conditional_world(3, false),
      conditional_world(4, false)};
  const std::vector<measurement::World> confirm{
      conditional_world(11, true), conditional_world(12, false), conditional_world(13, false),
      conditional_world(14, false)};
  const auto frozen = measurement::learn_response(tree, profile, frontier, 1, learn);
  const std::vector<bs::poker::PublicAction> limp_path{
      {bs::poker::Street::Preflop, 0, {ActionType::Call, 0}}};
  const auto limp_key = bs::solver::make_information_key(1, {8, 9}, {}, limp_path);
  const auto& limp_response = frozen.actions.at(limp_key);
  CHECK(limp_response.menu[limp_response.chosen].type == ActionType::Check);
  const auto raise_index = find_action(tree.node(0).actions, ActionType::Raise);
  const std::vector<bs::poker::PublicAction> raise_path{
      {bs::poker::Street::Preflop, 0, tree.node(0).actions[raise_index]}};
  const auto raise_key = bs::solver::make_information_key(1, {8, 9}, {}, raise_path);
  const auto& raise_response = frozen.actions.at(raise_key);
  CHECK(raise_response.menu[raise_response.chosen].type == ActionType::Call);
  const auto result = measurement::confirm_response(tree, profile, frontier, frozen, confirm);
  CHECK(near(result.profile_value, -0.5));
  CHECK(near(result.deviation_value, 4.0));
  CHECK(near(result.gain.mean, 4.5));
  CHECK(result.counts.response_misses == 0);
  return 0;
}

int statistics_and_sampling() {
  const std::array<double, 2> gains{-2.0, 2.0};
  const auto stats = measurement::estimate(gains);
  CHECK(near(stats.mean, 0.0));
  CHECK(near(stats.standard_error, 2.0));
  CHECK(near(stats.ci95_low, -3.92));
  CHECK(near(stats.ci95_high, 3.92));
  const std::array<double, 1> single{4.0};
  CHECK(std::isnan(measurement::estimate(single).standard_error));
  CHECK(throws([&] { measurement::estimate({}); }));
  CHECK(throws([&] { measurement::sample_worlds(2, 0, 1, 42); }));
  CHECK(throws([&] { measurement::sample_worlds(11, 1, 1, 42); }));

  const AbstractTree tree(heads_up(2), bs::abstraction::ActionAbstraction::identity());
  const ConstantFrontier frontier({3.0, -3.0});
  const auto first = measurement::measure(tree, passive(), frontier, 16, 2, 42);
  const auto second = measurement::measure(tree, passive(), frontier, 16, 2, 42);
  CHECK(first.learn_seed != first.confirm_seed);
  CHECK(first.learned.size() == 2 && first.seats.size() == 2);
  std::vector<double> summed(16);
  for (std::size_t seat = 0; seat < 2; ++seat) {
    CHECK(first.learned[seat].actions == second.learned[seat].actions);
    CHECK(first.seats[seat].paired_gains == second.seats[seat].paired_gains);
    for (std::size_t i = 0; i < summed.size(); ++i)
      summed[i] += first.seats[seat].paired_gains[i];
  }
  const auto reference = measurement::estimate(summed);
  CHECK(near(first.summed_gain.mean, reference.mean));
  CHECK(near(first.summed_gain.standard_error, reference.standard_error));
  return 0;
}

int multiway_and_fallback() {
  for (std::size_t seats = 2; seats <= 10; ++seats) {
    GameDef def = heads_up(2);
    if (seats > 2) {
      def.player_count = seats;
      def.preflop = true;
      def.ante = 1;
      def.stacks.fill(0);
      for (std::size_t seat = 0; seat < seats; ++seat)
        def.stacks[seat] = 2;
      def.blinds_posted = {};
      def.blinds_posted[1] = 1;
      def.blinds_posted[2] = 1;
      def.pot = static_cast<bs::poker::Chips>(seats + 2);
    }
    const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    std::vector<double> values(seats);
    double total = 0.0;
    for (std::size_t seat = 0; seat + 1 < seats; ++seat)
      total += values[seat] = static_cast<double>(seat + 1);
    values.back() = -total;
    const ConstantFrontier frontier(values);
    const auto learn_worlds = measurement::sample_worlds(seats, 2, 1, 88);
    const auto worlds = measurement::sample_worlds(seats, 2, 1, 99);
    const measurement::ProfileQuery missing =
        [](const InformationKey&, std::span<const Action>) -> std::optional<SeatPolicyRow> {
      return std::nullopt;
    };
    for (std::size_t seat = 0; seat < seats; ++seat) {
      measurement::LearnedResponse frozen;
      frozen.seat = seat;
      const auto result = measurement::confirm_response(tree, missing, frontier, frozen, worlds);
      CHECK(near(result.profile_value, values[seat]));
      CHECK(near(result.deviation_value, values[seat]));
      CHECK(near(result.gain.mean, 0.0));
      CHECK(result.counts.profile_queries == result.counts.profile_fallbacks);
      const auto learned = measurement::learn_response(tree, missing, frontier, seat, learn_worlds);
      const auto held_out = measurement::confirm_response(tree, missing, frontier, learned, worlds);
      CHECK(std::isfinite(held_out.gain.mean));
      CHECK(held_out.paired_gains.size() == worlds.size());
    }
  }
  return 0;
}

int invalid_inputs_fail_whole_measurement() {
  const AbstractTree tree(heads_up(), bs::abstraction::ActionAbstraction::identity());
  const ConstantFrontier frontier({4.0, -4.0});
  const std::vector<measurement::World> worlds{world(1), world(2, false)};
  for (int kind = 0; kind < 5; ++kind) {
    const measurement::ProfileQuery bad =
        [kind](const InformationKey&,
               std::span<const Action> menu) -> std::optional<SeatPolicyRow> {
      auto row = point(menu, 0);
      if (kind == 0)
        row.probabilities[0] = std::numeric_limits<double>::quiet_NaN();
      else if (kind == 1)
        row.probabilities[0] = -1.0;
      else if (kind == 2)
        row.probabilities[0] = 0.5;
      else if (kind == 3)
        row.probabilities.pop_back();
      else
        row.actions[0].target_total += 1;
      return row;
    };
    CHECK(throws([&] { measurement::learn_response(tree, bad, frontier, 0, worlds); }));
  }
  const auto profile = passive();
  const auto frozen = measurement::learn_response(tree, profile, frontier, 0, worlds);
  const std::vector<measurement::World> confirmation{world(10), world(11)};
  const ConstantFrontier wrong_count({4.0});
  const ConstantFrontier nonfinite({4.0, std::numeric_limits<double>::infinity()});
  CHECK(throws(
      [&] { measurement::confirm_response(tree, profile, wrong_count, frozen, confirmation); }));
  CHECK(throws(
      [&] { measurement::confirm_response(tree, profile, nonfinite, frozen, confirmation); }));
  class MissingFrontier final : public bs::gto::FrontierEvaluator {
    std::vector<double> evaluate(std::span<const int>, std::span<const std::array<int, 2>>,
                                 const TerminalPayload&) const override {
      throw std::runtime_error("declared frontier coverage missing");
    }
  } missing_frontier;
  CHECK(throws([&] { measurement::measure(tree, profile, missing_frontier, 8, 1, 42); }));
  auto blocked = worlds;
  blocked[0].flops[0][0] = blocked[0].holes[1][0];
  CHECK(throws([&] { measurement::learn_response(tree, profile, frontier, 0, blocked); }));
  auto duplicate = worlds;
  duplicate[1].seed = duplicate[0].seed;
  CHECK(throws([&] { measurement::learn_response(tree, profile, frontier, 0, duplicate); }));
  auto drift = frozen;
  drift.actions.begin()->second.chosen = 999;
  CHECK(
      throws([&] { measurement::confirm_response(tree, profile, frontier, drift, confirmation); }));
  return 0;
}

}  // namespace

int main() {
  try {
    if (hidden_information_and_normalization() || nested_actions() || confirmation_is_frozen() ||
        opponent_reach_conditions_descendant_pool() || statistics_and_sampling() ||
        multiway_and_fallback() || invalid_inputs_fail_whole_measurement())
      return 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "unexpected exception: %s\n", error.what());
    return 1;
  }
  std::printf("preflop frozen deviation measurement PASS\n");
  return 0;
}
