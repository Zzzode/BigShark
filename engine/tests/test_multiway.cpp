// Independent public-API regressions for the RFC 0006 multiway (3..6 player)
// rules. Expectations are derived from the RFC text and the poker rules
// themselves (preflop order starts left of the big blind; postflop starts left
// of the button skipping players who cannot act; only a full raise reopens
// raise rights), not from the implementation.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/multiway.hpp>
#include <bs/settlement.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
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

int card(const char* name) {
  return bs::cardId(std::string(name));
}

MultiwayRoot seat_root(std::size_t players, std::size_t button, Chips stack = 200,
                       Chips blind = 2) {
  MultiwayRoot root;
  root.players = players;
  root.button = button;
  root.big_blind = blind;
  root.ante = 0;
  root.stacks.assign(players, stack);
  root.contributions.assign(players, 0);
  return root;
}

Chips total_chips(const MultiwayState& state) {
  Chips total = state.pot();
  for (const MultiwayPlayer& player : state.players())
    total += player.stack;
  return total;
}

// ------------------------------------------------------------- actor order

// Preflop action starts left of the big blind; the button, small blind, and big
// blind are posted clockwise from the button.
int test_preflop_order_and_blinds() {
  for (std::size_t players : {std::size_t{3}, std::size_t{6}}) {
    for (std::size_t button = 0; button < players; ++button) {
      MultiwayState state(seat_root(players, button));
      CHECK(state.street() == Street::Preflop);
      const std::size_t small = (button + 1) % players;
      const std::size_t big = (button + 2) % players;
      const std::size_t opener = (button + 3) % players;
      CHECK(state.players()[small].street_committed == 1);
      CHECK(state.players()[big].street_committed == 2);
      CHECK(state.players()[button].street_committed == 0);
      // Everyone who is not a blind has nothing committed.
      for (std::size_t p = 0; p < players; ++p)
        if (p != small && p != big)
          CHECK(state.players()[p].street_committed == 0);
      CHECK(state.pot() == 3);
      // The opener is left of the big blind, unless that seat cannot act.
      CHECK(state.phase() == Phase::Action);
      CHECK(state.actor() == opener);
    }
  }
  return 0;
}

// Postflop action starts left of the button, skipping players who cannot act.
int test_postflop_order_skips_non_actors() {
  MultiwayRoot root = seat_root(4, 0);
  root.board = {card("2c"), card("3d"), card("7h")};
  MultiwayState state(root);
  CHECK(state.street() == Street::Flop);
  CHECK(state.phase() == Phase::Action);
  CHECK(state.actor() == 1);  // left of the button
  return 0;
}

// A folded seat is skipped: after a fold the action moves past it.
int test_fold_skips_seat() {
  MultiwayState state(seat_root(4, 0));
  CHECK(state.actor() == 3);  // left of the big blind (seat 2)
  MultiwayState folded = state.after_action(3, {ActionType::Fold});
  CHECK(folded.phase() == Phase::Action);
  CHECK(folded.players()[3].folded);
  // Seat 0 is next in order; it must not be skipped.
  CHECK(folded.actor() == 0);
  return 0;
}

// ------------------------------------------------------- raise-reopening rule

// A full raise reopens raise rights for players who already acted.
int test_full_raise_reopens() {
  MultiwayState state(seat_root(3, 0));
  // Preflop order with button 0: SB=1, BB=2, opener=0.
  CHECK(state.actor() == 0);
  MultiwayState opened = state.after_action(0, {ActionType::Raise, 6});
  CHECK(opened.actor() == 1);
  CHECK(opened.can_raise(1));
  // The small blind calls, so it has acted.
  MultiwayState called = opened.after_action(1, {ActionType::Call});
  CHECK(called.actor() == 2);
  CHECK(!called.can_raise(1));  // seat 1 already acted and lost its rights
  // The big blind makes a FULL raise, which reopens raise rights for the
  // players who had already acted.
  MultiwayState reraised = called.after_action(2, {ActionType::Raise, 12});
  CHECK(reraised.can_raise(0));
  CHECK(reraised.can_raise(1));
  // The reopened action returns to seat 0 first (clockwise from the raiser).
  CHECK(reraised.actor() == 0);
  MultiwayState again = reraised.after_action(0, {ActionType::Raise, 20});
  CHECK(again.players()[0].street_committed == 20);
  // Seat 1 was reopened too, so it may still raise over the new wager.
  CHECK(again.can_raise(1));
  return 0;
}

// A short all-in does NOT reopen raise rights for players who already acted.
int test_short_all_in_does_not_reopen() {
  MultiwayRoot root = seat_root(3, 0);
  root.stacks = {200, 200, 9};  // the big blind is short
  MultiwayState state(root);
  CHECK(state.actor() == 0);
  MultiwayState opened = state.after_action(0, {ActionType::Raise, 8});
  // The small blind calls and has now acted.
  MultiwayState called = opened.after_action(1, {ActionType::Call});
  CHECK(!called.can_raise(1));
  // The big blind shoves its remaining 7 over the 8: a SHORT all-in.
  CHECK(called.players()[2].stack == 7);
  MultiwayState shoved = called.after_action(2, {ActionType::Raise, 9});
  CHECK(shoved.players()[2].all_in);
  // Seat 1 acted already and the increment was short, so it may not re-raise.
  CHECK(!shoved.can_raise(1));
  // Seat 0 acted already too and is likewise not reopened.
  CHECK(!shoved.can_raise(0));
  return 0;
}

// ------------------------------------------------------------ conservation

int test_chip_conservation_across_a_street() {
  const Chips total = 4 * 200;
  MultiwayState state(seat_root(4, 0));
  CHECK(total_chips(state) == total);
  MultiwayState a = state.after_action(state.actor().value(), {ActionType::Call});
  CHECK(total_chips(a) == total);
  MultiwayState b = a.after_action(a.actor().value(), {ActionType::Call});
  CHECK(total_chips(b) == total);
  MultiwayState c = b.after_action(b.actor().value(), {ActionType::Call});
  CHECK(total_chips(c) == total);
  // The big blind checks its option, closing preflop.
  MultiwayState d = c.after_action(c.actor().value(), {ActionType::Check});
  CHECK(total_chips(d) == total);
  CHECK(d.pot() == 8);  // four players, two each
  return 0;
}

// Every player folding but one ends the hand with a single live player.
int test_folds_end_the_hand() {
  MultiwayState state(seat_root(3, 0));
  CHECK(state.actor() == 0);
  MultiwayState f1 = state.after_action(0, {ActionType::Fold});
  CHECK(f1.phase() == Phase::Action);
  MultiwayState f2 = f1.after_action(1, {ActionType::Fold});
  // The big blind remains: only one live player, the hand ends.
  CHECK(f2.phase() == Phase::Folded);
  CHECK(f2.live_players().size() == 1);
  const ContributionSettlement settled = f2.settle_fold();
  Chips total = 0;
  for (Chips stack : settled.final_stacks)
    total += stack;
  CHECK(total == 3 * 200);
  return 0;
}

// A three-way showdown splits by hand strength and conserves chips exactly.
int test_three_way_showdown_conserves() {
  // A rooted showdown carries the dead money each player already put in.
  MultiwayRoot root = seat_root(3, 0);
  root.stacks.assign(3, 198);
  root.contributions = {2, 2, 2};
  root.board = {card("2c"), card("3d"), card("7h"), card("9s"), card("Jd")};
  MultiwayState state(root);
  CHECK(state.pot() == 6);
  CHECK(state.phase() == Phase::Action);
  MultiwayState done = state;
  while (done.phase() == Phase::Action) {
    const MultiwayLegal legal = done.legal();
    if (legal.check)
      done = done.after_action(done.actor().value(), {ActionType::Check});
    else
      done = done.after_action(done.actor().value(), {ActionType::Call});
  }
  CHECK(done.phase() == Phase::Showdown);
  // Seat 0 has aces, seat 1 kings, seat 2 queens: a clear single winner.
  const std::vector<std::array<int, 2>> holes{
      {card("Ah"), card("Ad")}, {card("Kh"), card("Kd")}, {card("Qh"), card("Qd")}};
  const ContributionSettlement settled = done.settle_showdown(holes);
  Chips total = 0;
  for (Chips stack : settled.final_stacks)
    total += stack;
  CHECK(total == 3 * 200);  // exact conservation
  CHECK(settled.chip_utility[0] + settled.chip_utility[1] + settled.chip_utility[2] == 0);
  CHECK(settled.chip_utility[0] == 4);  // wins the other two's 2 each
  CHECK(settled.chip_utility[1] == -2);
  CHECK(settled.chip_utility[2] == -2);
  return 0;
}

// A preflop fold ends the hand for the blinds' sake: the winner takes the
// posted blinds.
int test_preflop_walk_takes_the_blinds() {
  MultiwayState state(seat_root(3, 0));
  // Seats: button 0, SB 1, BB 2, opener 0.
  MultiwayState f1 = state.after_action(0, {ActionType::Fold});
  MultiwayState f2 = f1.after_action(1, {ActionType::Fold});
  CHECK(f2.phase() == Phase::Folded);
  const ContributionSettlement settled = f2.settle_fold();
  // The big blind wins the small blind's 1 plus its own 2 back.
  CHECK(settled.chip_utility[2] == 1);
  CHECK(settled.chip_utility[1] == -1);
  CHECK(settled.chip_utility[0] == 0);
  return 0;
}

// --------------------------------------------------------------- validation

int test_root_validation() {
  // Too few or too many players.
  for (std::size_t players : {std::size_t{0}, std::size_t{2}, std::size_t{7}}) {
    bool threw = false;
    try {
      MultiwayState state(seat_root(players, 0));
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Button outside the seated range.
  {
    bool threw = false;
    try {
      MultiwayState state(seat_root(3, 3));
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  // A partial board is not a root.
  {
    MultiwayRoot root = seat_root(3, 0);
    root.board = {card("2c")};
    bool threw = false;
    try {
      MultiwayState state(root);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  return 0;
}

// A NON-blind seat with zero stack and no ante must start the hand all in and be
// skipped for the action, even though it posts neither ante nor blind. The
// preflop constructor has to derive all-in for it independently of the two
// blind-posting loops (RFC 0008 stage 2 differential review: the rooted path
// derived it, the ante path derived it, the plain no-ante non-blind path did
// not and handed the empty seat a phantom check).
int test_preflop_zero_stack_nonblind_seat_is_skipped() {
  MultiwayRoot root = seat_root(3, 0, 4);
  root.stacks = {0, 4, 26};  // seat 0 is the opener, posts nothing
  const MultiwayState state(root);
  CHECK(state.players()[0].all_in);
  CHECK(!state.players()[0].pending);
  // Opener skipped; the small blind acts first.
  CHECK(state.actor() == 1);
  return 0;
}

// A zero-stack seat on a ROOTED board is all in from an earlier street and
// must never hold the action. The rooted construction path derives all-in
// exactly as the ante path does; pending alone cannot mark it, or the seat
// would be handed a free check with no chips behind and the street would
// never open. (RFC 0008 stage 2: the omission lived on the rooted path
// because no shipped fixture rooted an already-all-in seat.)
int test_rooted_zero_stack_seat_is_skipped() {
  MultiwayRoot root = seat_root(3, 0);
  root.board = {card("2c"), card("3d"), card("7h")};
  // Seat 0 committed its whole stack on an earlier street; everyone else
  // already matched that contribution, so the rooted pot is three-way.
  root.stacks = {0, 200, 200};
  root.contributions = {12, 12, 12};
  const MultiwayState state(root);
  CHECK(state.players()[0].all_in);
  CHECK(!state.players()[0].pending);
  // Postflop opens left of button 0: seat 0 is skipped for the all-in seat 1.
  CHECK(state.phase() == Phase::Action);
  CHECK(state.actor() == 1);
  CHECK(state.players()[1].pending);
  // Seat 0 cannot be handed the action even after seat 1 checks.
  const MultiwayState checked = state.after_action(1, {ActionType::Check});
  CHECK(checked.actor() == 2);
  return 0;
}

// A short all-in call is capped by the stack and conserves chips.
int test_short_call_is_capped() {
  // Seat 1 is the small blind with only 5 behind, so after posting 1 it holds 4.
  MultiwayRoot root = seat_root(3, 0);
  root.stacks = {200, 5, 200};
  MultiwayState state(root);
  CHECK(state.players()[1].stack == 4);
  CHECK(state.players()[1].street_committed == 1);
  // Seat 0 opens to 6: seat 1 owes 5 but can only pay 4.
  MultiwayState opened = state.after_action(0, {ActionType::Raise, 6});
  CHECK(opened.actor() == 1);
  const MultiwayLegal legal = opened.legal();
  CHECK(legal.call_amount == 4);  // capped by the stack, not the full 5 due
  MultiwayState called = opened.after_action(1, {ActionType::Call});
  CHECK(called.players()[1].all_in);
  CHECK(called.players()[1].stack == 0);
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_preflop_order_and_blinds() == 0);
    CHECK(test_postflop_order_skips_non_actors() == 0);
    CHECK(test_fold_skips_seat() == 0);
    CHECK(test_full_raise_reopens() == 0);
    CHECK(test_short_all_in_does_not_reopen() == 0);
    CHECK(test_chip_conservation_across_a_street() == 0);
    CHECK(test_folds_end_the_hand() == 0);
    CHECK(test_three_way_showdown_conserves() == 0);
    CHECK(test_preflop_walk_takes_the_blinds() == 0);
    CHECK(test_root_validation() == 0);
    CHECK(test_rooted_zero_stack_seat_is_skipped() == 0);
    CHECK(test_preflop_zero_stack_nonblind_seat_is_skipped() == 0);
    CHECK(test_short_call_is_capped() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected multiway exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_multiway PASS\n");
  return 0;
}
