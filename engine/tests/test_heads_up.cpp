// Independent public-API regressions for RFC 0004 Stage 1.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace bs::poker;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using Holes = std::array<std::array<int, 2>, 2>;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

HeadsUpRoot root(Chips first = 20, Chips second = 20, Chips blind = 2, std::size_t button = 1) {
  return {{card("2h"), card("3h"), card("4h")}, {first, second}, {3, 3}, 6, blind, button};
}

Holes winning_holes() {
  return {{{card("Ah"), card("Kh")}, {card("9c"), card("8c")}}};
}

Holes tying_holes() {
  return {{{card("As"), card("Kd")}, {card("Ac"), card("Ks")}}};
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

template <typename F>
bool rejects_amount(F&& operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (const std::overflow_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

// Capture every public state field, including legal behavior and raise rights.
std::string snapshot(const HeadsUpState& state) {
  std::ostringstream out;
  const auto& r = state.root();
  for (int c : r.flop)
    out << c << ',';
  for (Chips s : r.stacks)
    out << s << ',';
  for (Chips c : r.contributions)
    out << c << ',';
  out << r.pot << ',' << r.big_blind << ',' << r.button << ';';
  out << static_cast<int>(state.phase()) << ',' << static_cast<int>(state.street()) << ','
      << (state.actor() ? static_cast<int>(*state.actor()) : -1) << ',' << state.pot() << ','
      << state.last_full_raise() << ';';
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& p = state.players()[i];
    out << p.stack << ',' << p.street_committed << ',' << p.contributed << ',' << p.refunded << ','
        << p.folded << ',' << p.net_contributed() << ',' << state.can_raise(i) << ';';
  }
  for (int c : state.board())
    out << c << ',';
  out << ';';
  for (const auto& event : state.history())
    out << static_cast<int>(event.street) << ',' << event.actor << ','
        << static_cast<int>(event.action.type) << ',' << event.action.target_total << ','
        << event.paid << ';';
  const auto legal = state.legal();
  out << legal.fold << ',' << legal.check << ',' << legal.call << ',' << legal.call_amount;
  if (legal.aggressive)
    out << ',' << static_cast<int>(legal.aggressive->type) << ',' << legal.aggressive->minimum
        << ',' << legal.aggressive->maximum << ',' << legal.aggressive->all_in_only;
  return out.str();
}

HeadsUpState checked_down(HeadsUpState state) {
  while (state.phase() == Phase::Action || state.phase() == Phase::Deal) {
    if (state.phase() == Phase::Deal) {
      state = state.after_card(state.board().size() == 3 ? card("Qc") : card("Jd"));
    } else {
      state = state.after_action(*state.actor(), {ActionType::Check});
    }
  }
  return state;
}

int check_range(const HeadsUpState& state, ActionType type, Chips minimum, Chips maximum) {
  const auto legal = state.legal();
  CHECK(legal.aggressive.has_value());
  CHECK(legal.aggressive->type == type);
  CHECK(legal.aggressive->minimum == minimum);
  CHECK(legal.aggressive->maximum == maximum);
  CHECK(legal.aggressive->all_in_only == (minimum == maximum));
  CHECK(legal.contains({type, minimum}));
  CHECK(legal.contains({type, maximum}));
  CHECK(!legal.contains({type, minimum - 1}));
  CHECK(!legal.contains({type, maximum + 1}));
  return 0;
}

int test_streets_and_order() {
  for (std::size_t button : {std::size_t{0}, std::size_t{1}}) {
    const HeadsUpState initial(root(20, 20, 2, button));
    const auto original = snapshot(initial);
    CHECK(initial.actor() == 1 - button);
    CHECK(initial.phase() == Phase::Action);
    CHECK(initial.street() == Street::Flop);
    CHECK(initial.board().size() == 3);
    CHECK(initial.history().empty());
    CHECK(initial.pot() == 6);
    CHECK(initial.last_full_raise() == 2);
    CHECK(initial.can_raise(0));
    CHECK(initial.can_raise(1));
    CHECK(initial.legal().check);
    CHECK(!initial.legal().call);
    CHECK(!initial.legal().fold);
    CHECK(initial.legal().call_amount == 0);
    CHECK(check_range(initial, ActionType::Bet, 2, 20) == 0);
    auto state = initial;
    for (Street street : {Street::Flop, Street::Turn, Street::River}) {
      CHECK(state.street() == street);
      CHECK(state.actor() == 1 - button);
      CHECK(state.players()[0].street_committed == 0);
      CHECK(state.players()[1].street_committed == 0);
      state = state.after_action(1 - button, {ActionType::Check});
      CHECK(state.phase() == Phase::Action);
      CHECK(state.actor() == button);
      state = state.after_action(button, {ActionType::Check});
      CHECK(!state.actor());
      if (street == Street::River) {
        CHECK(state.phase() == Phase::Showdown);
      } else {
        CHECK(state.phase() == Phase::Deal);
        state = state.after_card(street == Street::Flop ? card("Qc") : card("Jd"));
        CHECK(state.phase() == Phase::Action);
        CHECK(state.last_full_raise() == 2);
      }
    }
    CHECK(snapshot(initial) == original);
    CHECK(state.history().size() == 6);
    CHECK(state.settle_showdown(winning_holes()).awards[0] == 6);
    CHECK(state.settle_showdown(tying_holes()).awards[0] == 3);
    CHECK(state.settle_showdown(tying_holes()).awards[1] == 3);
  }
  return 0;
}

int test_raises_and_short_all_ins() {
  const HeadsUpState initial(root(100, 100));
  auto state = initial.after_action(0, {ActionType::Bet, 7});
  CHECK(state.last_full_raise() == 7);
  CHECK(!state.can_raise(0));
  CHECK(state.legal().call_amount == 7);
  CHECK(!state.legal().check);
  CHECK(check_range(state, ActionType::Raise, 14, 100) == 0);
  CHECK(throws_as<std::invalid_argument>([&] { state.after_action(1, {ActionType::Raise, 13}); }));
  state = state.after_action(1, {ActionType::Raise, 14});
  CHECK(state.last_full_raise() == 7);
  CHECK(state.can_raise(0));
  CHECK(check_range(state, ActionType::Raise, 21, 100) == 0);
  state = state.after_action(0, {ActionType::Raise, 23});
  CHECK(state.last_full_raise() == 9);
  CHECK(state.legal().call_amount == 9);
  CHECK(check_range(state, ActionType::Raise, 32, 100) == 0);
  state = state.after_action(1, {ActionType::Raise, 32});
  CHECK(state.players()[1].stack == 68);
  CHECK(state.players()[1].contributed == 35);
  CHECK(state.history().back().paid == 18);
  CHECK(check_range(state, ActionType::Raise, 41, 100) == 0);
  state = state.after_action(0, {ActionType::Call});
  CHECK(state.phase() == Phase::Deal);
  CHECK(state.pot() == 70);
  CHECK(state.history().back().paid == 9);
  state = state.after_card(card("Qc"));
  CHECK(state.actor() == 0);
  CHECK(state.last_full_raise() == 2);
  CHECK(check_range(state, ActionType::Bet, 2, 68) == 0);

  // Checking preserves the right to raise when the opponent subsequently bets.
  state = initial.after_action(0, {ActionType::Check});
  state = state.after_action(1, {ActionType::Bet, 3});
  CHECK(check_range(state, ActionType::Raise, 6, 100) == 0);
  state = state.after_action(0, {ActionType::Raise, 6});
  CHECK(state.actor() == 1);

  state = HeadsUpState(root(100, 13)).after_action(0, {ActionType::Bet, 7});
  CHECK(check_range(state, ActionType::Raise, 13, 13) == 0);
  CHECK(throws_as<std::invalid_argument>([&] { state.after_action(1, {ActionType::Raise, 12}); }));
  state = state.after_action(1, {ActionType::Raise, 13});
  CHECK(state.last_full_raise() == 7);
  CHECK(state.players()[1].stack == 0);
  CHECK(!state.can_raise(0));
  CHECK(!state.legal().aggressive);
  CHECK(state.legal().call_amount == 6);
  CHECK(throws_as<std::invalid_argument>([&] { state.after_action(0, {ActionType::Raise, 20}); }));
  state = state.after_action(0, {ActionType::Call});
  CHECK(state.phase() == Phase::Deal);
  state = state.after_card(card("Qc"));
  CHECK(state.phase() == Phase::Deal);
  CHECK(!state.actor());
  state = state.after_card(card("Jd"));
  CHECK(state.phase() == Phase::Showdown);

  state = HeadsUpState(root(1, 20));
  CHECK(check_range(state, ActionType::Bet, 1, 1) == 0);
  state = state.after_action(0, {ActionType::Bet, 1});
  CHECK(state.last_full_raise() == 2);
  CHECK(!state.legal().aggressive);
  CHECK(state.legal().call_amount == 1);

  state = HeadsUpState(root(20, 1)).after_action(0, {ActionType::Check});
  CHECK(check_range(state, ActionType::Bet, 1, 1) == 0);
  state = state.after_action(1, {ActionType::Bet, 1});
  CHECK(!state.legal().aggressive);
  CHECK(state.legal().call);

  state = HeadsUpState(root(2, 20));
  CHECK(check_range(state, ActionType::Bet, 2, 2) == 0);
  state = state.after_action(0, {ActionType::Bet, 2});
  CHECK(state.last_full_raise() == 2);
  CHECK(state.players()[0].stack == 0);
  CHECK(!state.legal().aggressive);

  state = HeadsUpState(root(100, 14)).after_action(0, {ActionType::Bet, 7});
  CHECK(check_range(state, ActionType::Raise, 14, 14) == 0);
  state = state.after_action(1, {ActionType::Raise, 14});
  CHECK(state.last_full_raise() == 7);
  CHECK(state.players()[1].stack == 0);
  CHECK(state.can_raise(0));
  CHECK(!state.legal().aggressive);
  return 0;
}

int test_refunds_and_payouts() {
  auto state = HeadsUpState(root(100, 4));
  CHECK(check_range(state, ActionType::Bet, 2, 100) == 0);
  state = state.after_action(0, {ActionType::Bet, 100});
  CHECK(state.pot() == 106);
  CHECK(state.legal().call_amount == 4);
  CHECK(!state.legal().aggressive);
  state = state.after_action(1, {ActionType::Call});
  CHECK(state.phase() == Phase::Deal);
  CHECK(state.players()[0].stack == 96);
  CHECK(state.players()[0].contributed == 103);
  CHECK(state.players()[0].refunded == 96);
  CHECK(state.players()[0].net_contributed() == 7);
  CHECK(state.pot() == 14);
  state = checked_down(state);
  const auto before = snapshot(state);
  auto payout = state.settle_showdown(winning_holes());
  CHECK((payout.awards == std::array<Chips, 2>{14, 0}));
  CHECK((payout.refunds == std::array<Chips, 2>{96, 0}));
  CHECK((payout.final_stacks == std::array<Chips, 2>{110, 0}));
  CHECK((payout.net_utility == std::array<std::int64_t, 2>{7, -7}));
  auto reversed = winning_holes();
  std::swap(reversed[0], reversed[1]);
  payout = state.settle_showdown(reversed);
  CHECK((payout.final_stacks == std::array<Chips, 2>{96, 14}));
  CHECK((payout.net_utility == std::array<std::int64_t, 2>{-7, 7}));
  payout = state.settle_showdown(tying_holes());
  CHECK((payout.final_stacks == std::array<Chips, 2>{103, 7}));
  CHECK((payout.net_utility == std::array<std::int64_t, 2>{0, 0}));
  CHECK(snapshot(state) == before);

  state = HeadsUpState(root(100, 4)).after_action(0, {ActionType::Bet, 100});
  state = state.after_action(1, {ActionType::Fold});
  CHECK(state.phase() == Phase::Folded);
  CHECK(!state.actor());
  CHECK(state.pot() == 6);
  CHECK(state.players()[1].folded);
  CHECK(state.players()[0].contributed == 103);
  CHECK(state.players()[0].refunded == 100);
  CHECK(state.players()[0].stack == 100);
  payout = state.settle_fold();
  CHECK((payout.awards == std::array<Chips, 2>{6, 0}));
  CHECK((payout.refunds == std::array<Chips, 2>{100, 0}));
  CHECK((payout.final_stacks == std::array<Chips, 2>{106, 4}));
  CHECK((payout.net_utility == std::array<std::int64_t, 2>{3, -3}));

  // A fold after a raise refunds only the unmatched part, including on later streets.
  state = HeadsUpState(root(100, 100)).after_action(0, {ActionType::Bet, 5});
  state = state.after_action(1, {ActionType::Call}).after_card(card("Qc"));
  state = state.after_action(0, {ActionType::Bet, 7});
  state = state.after_action(1, {ActionType::Raise, 20});
  state = state.after_action(0, {ActionType::Fold});
  payout = state.settle_fold();
  CHECK(state.pot() == 30);
  CHECK((payout.refunds == std::array<Chips, 2>{0, 13}));
  CHECK((payout.final_stacks == std::array<Chips, 2>{88, 118}));
  CHECK((payout.net_utility == std::array<std::int64_t, 2>{-15, 15}));
  return 0;
}

int test_rejections() {
  const HeadsUpState initial(root());
  const auto original = snapshot(initial);
  for (std::size_t wrong :
       {std::size_t{1}, std::size_t{2}, std::numeric_limits<std::size_t>::max()})
    CHECK(throws_as<std::invalid_argument>(
        [&] { initial.after_action(wrong, {ActionType::Check}); }));
  const std::vector<Action> invalid = {{ActionType::Fold},     {ActionType::Call},
                                       {ActionType::Check, 1}, {ActionType::Call, 1},
                                       {ActionType::Fold, 1},  {ActionType::Bet, 0},
                                       {ActionType::Bet, 1},   {ActionType::Bet, 21},
                                       {ActionType::Raise, 4}, {static_cast<ActionType>(99), 0}};
  for (Action action : invalid) {
    CHECK(!initial.legal().contains(action));
    CHECK(throws_as<std::invalid_argument>([&] { initial.after_action(0, action); }));
    CHECK(snapshot(initial) == original);
  }
  CHECK(throws_as<std::invalid_argument>([&] { initial.after_card(card("Qc")); }));
  CHECK(throws_as<std::invalid_argument>([&] { initial.settle_fold(); }));
  CHECK(throws_as<std::invalid_argument>([&] { initial.settle_showdown(winning_holes()); }));
  CHECK(snapshot(initial) == original);

  const auto facing = initial.after_action(0, {ActionType::Bet, 5});
  const auto facing_before = snapshot(facing);
  for (Action action :
       {Action{ActionType::Check}, Action{ActionType::Call, 5}, Action{ActionType::Fold, 1},
        Action{ActionType::Bet, 10}, Action{ActionType::Raise, 9}, Action{ActionType::Raise, 21}}) {
    CHECK(!facing.legal().contains(action));
    CHECK(throws_as<std::invalid_argument>([&] { facing.after_action(1, action); }));
    CHECK(snapshot(facing) == facing_before);
  }
  const auto deal = facing.after_action(1, {ActionType::Call});
  const auto deal_before = snapshot(deal);
  for (int bad_card :
       {-1, 52, std::numeric_limits<int>::max(), card("2h"), card("3h"), card("4h")}) {
    CHECK(throws_as<std::invalid_argument>([&] { deal.after_card(bad_card); }));
    CHECK(snapshot(deal) == deal_before);
  }
  const auto turn_deal = deal.after_card(card("Qc"))
                             .after_action(0, {ActionType::Check})
                             .after_action(1, {ActionType::Check});
  CHECK(throws_as<std::invalid_argument>([&] { turn_deal.after_card(card("Qc")); }));

  const auto showdown = checked_down(deal);
  const auto folded = facing.after_action(1, {ActionType::Fold});
  for (const auto* terminal : {&deal, &showdown, &folded}) {
    const auto saved = snapshot(*terminal);
    CHECK(!terminal->actor());
    CHECK(!terminal->legal().fold);
    CHECK(!terminal->legal().check);
    CHECK(!terminal->legal().call);
    CHECK(!terminal->legal().aggressive);
    for (Action action : {Action{ActionType::Check}, Action{ActionType::Fold},
                          Action{ActionType::Call}, Action{ActionType::Bet, 2}})
      CHECK(throws_as<std::invalid_argument>([&] { terminal->after_action(0, action); }));
    CHECK(snapshot(*terminal) == saved);
  }
  CHECK(throws_as<std::invalid_argument>([&] { deal.settle_fold(); }));
  CHECK(throws_as<std::invalid_argument>([&] { deal.settle_showdown(winning_holes()); }));
  CHECK(throws_as<std::invalid_argument>([&] { showdown.settle_fold(); }));
  CHECK(throws_as<std::invalid_argument>([&] { folded.settle_showdown(winning_holes()); }));
  CHECK(throws_as<std::invalid_argument>([&] { showdown.after_card(card("Ts")); }));
  CHECK(throws_as<std::invalid_argument>([&] { folded.after_card(card("Ts")); }));
  const auto showdown_before = snapshot(showdown);
  for (std::size_t player = 0; player < 2; ++player) {
    for (std::size_t slot = 0; slot < 2; ++slot) {
      for (int bad : {-1, 52, card("2h"), card("Qc"), card("Jd")}) {
        auto holes = winning_holes();
        holes[player][slot] = bad;
        CHECK(throws_as<std::invalid_argument>([&] { showdown.settle_showdown(holes); }));
      }
      auto holes = winning_holes();
      holes[player][slot] = holes[player][1 - slot];
      CHECK(throws_as<std::invalid_argument>([&] { showdown.settle_showdown(holes); }));
      for (std::size_t other_slot = 0; other_slot < 2; ++other_slot) {
        holes = winning_holes();
        holes[player][slot] = holes[1 - player][other_slot];
        CHECK(throws_as<std::invalid_argument>([&] { showdown.settle_showdown(holes); }));
      }
    }
  }
  CHECK(snapshot(showdown) == showdown_before);

  // Public chance accepts a card that a caller may hold privately.
  const auto public_only = deal.after_card(card("Ah"))
                               .after_action(0, {ActionType::Check})
                               .after_action(1, {ActionType::Check})
                               .after_card(card("Jd"))
                               .after_action(0, {ActionType::Check})
                               .after_action(1, {ActionType::Check});
  CHECK(public_only.board()[3] == card("Ah"));
  CHECK(throws_as<std::invalid_argument>([&] { public_only.settle_showdown(winning_holes()); }));
  return 0;
}

int test_roots_and_large_amounts() {
  for (int bad_card : {-1, 52, std::numeric_limits<int>::max()}) {
    for (std::size_t slot = 0; slot < 3; ++slot) {
      auto bad = root();
      bad.flop[slot] = bad_card;
      CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));
    }
  }
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = i + 1; j < 3; ++j) {
      auto bad = root();
      bad.flop[j] = bad.flop[i];
      CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));
    }
  auto bad = root();
  bad.button = 2;
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));
  bad = root();
  bad.big_blind = 0;
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));
  bad = root();
  bad.pot = 7;
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));
  bad = root();
  bad.contributions = {2, 4};
  CHECK(throws_as<std::invalid_argument>([&] { HeadsUpState state(bad); }));

  for (std::size_t button : {std::size_t{0}, std::size_t{1}}) {
    for (auto stacks :
         {std::array<Chips, 2>{0, 9}, std::array<Chips, 2>{9, 0}, std::array<Chips, 2>{0, 0}}) {
      auto r = root(stacks[0], stacks[1], 2, button);
      auto state = HeadsUpState(r);
      CHECK(state.phase() == Phase::Deal);
      CHECK(!state.actor());
      CHECK(!state.legal().aggressive);
      state = state.after_card(card("Qc"));
      CHECK(state.phase() == Phase::Deal);
      CHECK(state.street() == Street::Turn);
      state = state.after_card(card("Jd"));
      CHECK(state.phase() == Phase::Showdown);
      CHECK(state.street() == Street::River);
      CHECK(state.history().empty());
      const auto result = state.settle_showdown(winning_holes());
      CHECK(result.final_stacks[0] == stacks[0] + 6);
      CHECK(result.final_stacks[1] == stacks[1]);
      CHECK((result.net_utility == std::array<std::int64_t, 2>{3, -3}));
    }
  }

  static_assert(std::is_same_v<Chips, std::uint64_t>);
  static_assert(std::is_same_v<decltype(Settlement{}.net_utility)::value_type, std::int64_t>);
  CHECK(kMaxHeadsUpChips == 9007199254740991ULL);
  const Chips huge = (Chips{1} << 32) + 17;
  auto large = root(huge + 10, huge + 20, huge);
  auto state = HeadsUpState(large);
  CHECK(check_range(state, ActionType::Bet, huge, huge + 10) == 0);
  state = state.after_action(0, {ActionType::Bet, huge + 1});
  CHECK(state.last_full_raise() == huge + 1);
  CHECK(state.history().back().paid == huge + 1);
  CHECK(state.legal().call_amount == huge + 1);
  state = checked_down(state.after_action(1, {ActionType::Call}));
  auto result = state.settle_showdown(winning_holes());
  CHECK(result.awards[0] == 2 * huge + 8);
  CHECK(result.final_stacks[0] == 2 * huge + 17);
  CHECK(result.final_stacks[1] == 19);
  CHECK(result.net_utility[0] == static_cast<std::int64_t>(huge + 4));

  auto boundary = root(kMaxHeadsUpChips - 7, 1, 1);
  state = HeadsUpState(boundary);
  CHECK(check_range(state, ActionType::Bet, 1, kMaxHeadsUpChips - 7) == 0);
  state = state.after_action(0, {ActionType::Bet, kMaxHeadsUpChips - 7});
  CHECK(state.pot() == kMaxHeadsUpChips - 1);
  const auto saved = snapshot(state);
  CHECK(rejects_amount(
      [&] { state.after_action(1, {ActionType::Raise, std::numeric_limits<Chips>::max()}); }));
  CHECK(snapshot(state) == saved);
  state = checked_down(state.after_action(1, {ActionType::Call}));
  result = state.settle_showdown(winning_holes());
  CHECK(result.final_stacks[0] == kMaxHeadsUpChips);
  CHECK(result.final_stacks[1] == 0);
  CHECK(result.refunds[0] == kMaxHeadsUpChips - 8);
  CHECK((result.net_utility == std::array<std::int64_t, 2>{4, -4}));

  // Nearly the entire chip bound may also be in the root pot, not just stacks.
  boundary = root(0, 1, 1);
  boundary.contributions = {kMaxHeadsUpChips / 2, kMaxHeadsUpChips / 2};
  boundary.pot = kMaxHeadsUpChips - 1;
  state = checked_down(HeadsUpState(boundary));
  result = state.settle_showdown(winning_holes());
  CHECK(result.awards[0] == kMaxHeadsUpChips - 1);
  CHECK(result.net_utility[0] == static_cast<std::int64_t>(kMaxHeadsUpChips / 2));
  CHECK(result.net_utility[1] == -static_cast<std::int64_t>(kMaxHeadsUpChips / 2));
  for (Chips excessive : {kMaxHeadsUpChips + 1, std::numeric_limits<Chips>::max()}) {
    for (int field = 0; field < 6; ++field) {
      bad = root();
      if (field < 2)
        bad.stacks[static_cast<std::size_t>(field)] = excessive;
      else if (field < 4)
        bad.contributions[static_cast<std::size_t>(field - 2)] = excessive;
      else if (field == 4)
        bad.pot = excessive;
      else
        bad.big_blind = excessive;
      CHECK(rejects_amount([&] { HeadsUpState rejected(bad); }));
    }
  }
  bad = root(kMaxHeadsUpChips - 6, 1, 1);
  CHECK(rejects_amount([&] { HeadsUpState rejected(bad); }));
  bad = root(std::numeric_limits<Chips>::max(), std::numeric_limits<Chips>::max());
  bad.contributions = {std::numeric_limits<Chips>::max(), std::numeric_limits<Chips>::max()};
  bad.pot = std::numeric_limits<Chips>::max() - 1;
  CHECK(rejects_amount([&] { HeadsUpState rejected(bad); }));
  return 0;
}

// The oracle owns its ledger and derives turn closure from checks/facing a wager.
// It never asks native legality or settlement which branches or payouts to expect.
struct Reference {
  HeadsUpRoot initial;
  std::array<Chips, 2> available;
  std::array<Chips, 2> gross;
  std::array<Chips, 2> returned{};
  std::array<Chips, 2> wager{};
  std::vector<BettingEvent> events;
  int street = 0;
  Phase phase;
  std::size_t next;
  Chips increment;
  bool checked = false;
  std::optional<std::size_t> folded;

  explicit Reference(const HeadsUpRoot& r)
      : initial(r),
        available(r.stacks),
        gross(r.contributions),
        phase(r.stacks[0] == 0 || r.stacks[1] == 0 ? Phase::Deal : Phase::Action),
        next(1 - r.button),
        increment(r.big_blind) {}

  Chips pool() const { return gross[0] + gross[1] - returned[0] - returned[1]; }

  std::vector<Action> choices() const {
    std::vector<Action> result;
    if (phase != Phase::Action)
      return result;
    const auto other = 1 - next;
    const Chips debt = wager[other] - wager[next];
    if (debt != 0) {
      result.push_back({ActionType::Fold});
      result.push_back({ActionType::Call});
    } else {
      result.push_back({ActionType::Check});
    }
    if (available[other] == 0 || available[next] <= debt)
      return result;
    const Chips ceiling = wager[next] + available[next];
    const Chips full = wager[other] == 0 ? initial.big_blind : wager[other] + increment;
    for (Chips target = wager[other] + 1; target <= ceiling; ++target)
      if (target >= full || target == ceiling)
        result.push_back({wager[other] == 0 ? ActionType::Bet : ActionType::Raise, target});
    return result;
  }

  void close(bool fold) {
    const std::size_t high = wager[0] > wager[1] ? 0 : 1;
    const Chips excess = wager[high] - wager[1 - high];
    returned[high] += excess;
    available[high] += excess;
    wager[high] -= excess;
    phase = fold ? Phase::Folded : street == 2 ? Phase::Showdown : Phase::Deal;
  }

  Reference act(Action action) const {
    Reference child = *this;
    const auto other = 1 - next;
    Chips paid = 0;
    if (action.type == ActionType::Call)
      paid = std::min(available[next], wager[other] - wager[next]);
    else if (action.type == ActionType::Bet || action.type == ActionType::Raise)
      paid = action.target_total - wager[next];
    child.available[next] -= paid;
    child.gross[next] += paid;
    child.wager[next] += paid;
    child.events.push_back({static_cast<Street>(street), next, action, paid});
    if (action.type == ActionType::Fold) {
      child.folded = next;
      child.close(true);
    } else if (action.type == ActionType::Call || (action.type == ActionType::Check && checked)) {
      child.close(false);
    } else if (action.type == ActionType::Check) {
      child.checked = true;
      child.next = other;
    } else {
      const Chips increase = action.target_total - wager[other];
      if (increase >= increment)
        child.increment = increase;
      child.next = other;
    }
    return child;
  }

  Reference deal() const {
    Reference child = *this;
    ++child.street;
    child.wager = {};
    child.increment = initial.big_blind;
    child.checked = false;
    child.next = 1 - initial.button;
    const bool all_in = available[0] == 0 || available[1] == 0;
    child.phase = all_in ? (child.street == 2 ? Phase::Showdown : Phase::Deal) : Phase::Action;
    return child;
  }
};

int compare_settlement(const HeadsUpState& state, const Reference& ref,
                       std::optional<std::size_t> winner, const Holes& holes) {
  const auto before = snapshot(state);
  const auto actual = ref.folded ? state.settle_fold() : state.settle_showdown(holes);
  std::array<Chips, 2> awards{};
  if (winner) {
    awards[*winner] = ref.pool();
  } else {
    awards = {ref.pool() / 2, ref.pool() / 2};
    awards[1 - ref.initial.button] += ref.pool() % 2;
  }
  CHECK(actual.awards == awards);
  CHECK(actual.refunds == ref.returned);
  for (std::size_t i = 0; i < 2; ++i) {
    const Chips hand_start = ref.initial.stacks[i] + ref.initial.contributions[i];
    const Chips final = hand_start - ref.gross[i] + ref.returned[i] + awards[i];
    CHECK(actual.final_stacks[i] == final);
    CHECK(actual.net_utility[i] ==
          static_cast<std::int64_t>(final) - static_cast<std::int64_t>(hand_start));
  }
  CHECK(actual.final_stacks[0] + actual.final_stacks[1] ==
        ref.initial.stacks[0] + ref.initial.stacks[1] + ref.initial.pot);
  CHECK(actual.net_utility[0] + actual.net_utility[1] == 0);
  CHECK(snapshot(state) == before);
  return 0;
}

struct Counts {
  std::size_t nodes = 0;
  std::size_t folds = 0;
  std::size_t showdowns = 0;
  std::size_t refunds = 0;
  std::size_t short_raises = 0;
};

int enumerate(const HeadsUpState& state, const Reference& ref, Counts& counts,
              std::size_t depth = 0) {
  CHECK(++counts.nodes < 1000000);
  CHECK(depth < 64);
  CHECK(state.phase() == ref.phase);
  CHECK(static_cast<int>(state.street()) == ref.street);
  CHECK(state.board().size() == static_cast<std::size_t>(3 + ref.street));
  for (std::size_t i = 0; i < 3; ++i)
    CHECK(state.board()[i] == ref.initial.flop[i]);
  if (ref.street >= 1)
    CHECK(state.board()[3] == card("Qc"));
  if (ref.street == 2)
    CHECK(state.board()[4] == card("Jd"));
  CHECK(state.pot() == ref.pool());
  Chips accounted = state.pot();
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& player = state.players()[i];
    CHECK(player.stack == ref.available[i]);
    CHECK(player.contributed == ref.gross[i]);
    CHECK(player.refunded == ref.returned[i]);
    CHECK(player.net_contributed() == ref.gross[i] - ref.returned[i]);
    CHECK(player.folded == (ref.folded && *ref.folded == i));
    CHECK(player.stack + player.net_contributed() ==
          ref.initial.stacks[i] + ref.initial.contributions[i]);
    if (ref.phase == Phase::Action)
      CHECK(player.street_committed == ref.wager[i]);
    accounted += player.stack;
  }
  CHECK(accounted == ref.initial.stacks[0] + ref.initial.stacks[1] + ref.initial.pot);
  CHECK(state.history().size() == ref.events.size());
  for (std::size_t i = 0; i < ref.events.size(); ++i) {
    CHECK(state.history()[i].street == ref.events[i].street);
    CHECK(state.history()[i].actor == ref.events[i].actor);
    CHECK(state.history()[i].action == ref.events[i].action);
    CHECK(state.history()[i].paid == ref.events[i].paid);
  }
  const auto before = snapshot(state);
  if (ref.phase == Phase::Folded || ref.phase == Phase::Showdown) {
    CHECK(!state.actor());
    if (ref.returned[0] != 0 || ref.returned[1] != 0)
      ++counts.refunds;
    if (ref.folded) {
      ++counts.folds;
      CHECK(compare_settlement(state, ref, 1 - *ref.folded, winning_holes()) == 0);
    } else {
      ++counts.showdowns;
      CHECK(compare_settlement(state, ref, 0, winning_holes()) == 0);
      auto swapped = winning_holes();
      std::swap(swapped[0], swapped[1]);
      CHECK(compare_settlement(state, ref, 1, swapped) == 0);
      CHECK(compare_settlement(state, ref, std::nullopt, tying_holes()) == 0);
    }
  } else if (ref.phase == Phase::Deal) {
    CHECK(!state.actor());
    CHECK(enumerate(state.after_card(ref.street == 0 ? card("Qc") : card("Jd")), ref.deal(), counts,
                    depth + 1) == 0);
  } else {
    CHECK(state.actor() == ref.next);
    CHECK(state.last_full_raise() == ref.increment);
    const auto choices = ref.choices();
    const auto legal = state.legal();
    const auto expected = [&](Action action) {
      return std::find(choices.begin(), choices.end(), action) != choices.end();
    };
    CHECK(legal.fold == expected({ActionType::Fold}));
    CHECK(legal.check == expected({ActionType::Check}));
    CHECK(legal.call == expected({ActionType::Call}));
    CHECK(legal.call_amount ==
          std::min(ref.available[ref.next], ref.wager[1 - ref.next] - ref.wager[ref.next]));
    std::vector<Action> aggressive;
    for (Action action : choices)
      if (action.type == ActionType::Bet || action.type == ActionType::Raise)
        aggressive.push_back(action);
    CHECK(legal.aggressive.has_value() == !aggressive.empty());
    if (!aggressive.empty()) {
      const Chips full = ref.wager[1 - ref.next] == 0 ? ref.initial.big_blind
                                                      : ref.wager[1 - ref.next] + ref.increment;
      CHECK(check_range(state, aggressive.front().type, aggressive.front().target_total,
                        aggressive.back().target_total) == 0);
      if (aggressive.front().type == ActionType::Raise && aggressive.back().target_total < full)
        ++counts.short_raises;
    }
    const Chips ceiling = ref.available[ref.next] + ref.wager[ref.next];
    for (ActionType type : {ActionType::Fold, ActionType::Check, ActionType::Call, ActionType::Bet,
                            ActionType::Raise}) {
      for (Chips target = 0; target <= ceiling + 1; ++target) {
        const Action action{type, target};
        CHECK(legal.contains(action) == expected(action));
        if (!expected(action))
          CHECK(throws_as<std::invalid_argument>([&] { state.after_action(ref.next, action); }));
      }
    }
    CHECK(throws_as<std::invalid_argument>(
        [&] { state.after_action(1 - ref.next, choices.front()); }));
    CHECK(snapshot(state) == before);
    for (Action action : choices) {
      CHECK(enumerate(state.after_action(ref.next, action), ref.act(action), counts, depth + 1) ==
            0);
      CHECK(snapshot(state) == before);
    }
  }
  CHECK(snapshot(state) == before);
  return 0;
}

int test_exhaustive_small_stacks() {
  Counts counts;
  for (std::size_t button : {std::size_t{0}, std::size_t{1}})
    for (Chips blind = 1; blind <= 3; ++blind)
      for (Chips first = 0; first <= 4; ++first)
        for (Chips second = 0; second <= 4; ++second) {
          const auto r = root(first, second, blind, button);
          CHECK(enumerate(HeadsUpState(r), Reference(r), counts) == 0);
        }
  CHECK(counts.nodes > 1000);
  CHECK(counts.folds > 100);
  CHECK(counts.showdowns > 100);
  CHECK(counts.refunds > 100);
  CHECK(counts.short_raises > 0);
  std::printf(
      "heads-up exhaustive: nodes=%zu folds=%zu showdowns=%zu refunds=%zu "
      "short-raises=%zu\n",
      counts.nodes, counts.folds, counts.showdowns, counts.refunds, counts.short_raises);
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_streets_and_order() == 0);
    CHECK(test_raises_and_short_all_ins() == 0);
    CHECK(test_refunds_and_payouts() == 0);
    CHECK(test_rejections() == 0);
    CHECK(test_roots_and_large_amounts() == 0);
    CHECK(test_exhaustive_small_stacks() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected heads-up exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_heads_up PASS\n");
  return 0;
}
