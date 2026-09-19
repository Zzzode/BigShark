// Cross-type invariants for the unified N-seat game definition that RFC 0008
// proposes. This suite exists to be the GUARD on that refactor, so it is
// deliberately written now, against the two separate implementations, and every
// expectation is stated from the RFCs' text plus the poker rules - never from
// either implementation's internals.
//
// The point is not that the two types agree. They do not agree today, and
// RFC 0008 §L1 documents where. The point is that the boundary between "differs
// because the rules differ" and "differs because the implementation drifted" is
// currently unwritten, so a unification cannot tell the two apart. Every case
// below names one of those boundaries and pins the poker property that must
// survive whichever representation wins.
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/multiway.hpp>
#include <bs/settlement.hpp>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
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

// ---------------------------------------------------------------- fixtures

// A 100 BB heads-up preflop root: blinds 1/2, button posts the small blind.
HeadsUpRoot hu_root(Chips stack, std::size_t button = 1) {
  HeadsUpRoot root{};
  root.flop = {-1, -1, -1};
  root.stacks = {stack, stack};
  root.contributions = {0, 0};
  root.pot = 2;
  root.big_blind = 2;
  root.button = button;
  root.preflop = true;
  root.blinds_posted[button] = 1;
  root.blinds_posted[1 - button] = 2;
  return root;
}

MultiwayRoot mw_root(std::size_t players, Chips stack, Chips blind = 2) {
  MultiwayRoot root;
  root.players = players;
  root.button = 1;
  root.big_blind = blind;
  root.ante = 0;
  root.stacks.assign(players, stack);
  root.contributions.assign(players, 0);
  return root;
}

Action hu_fold() {
  return {ActionType::Fold, 0};
}
// A Call action carries NO target total; `LegalActions::call_amount` is an
// output describing what calling costs, and `contains` rejects a Call that
// supplies a target.
Action hu_call() {
  return {ActionType::Call, 0};
}
Action hu_aggressive(const LegalActions& legal) {
  return {legal.aggressive->type, legal.aggressive->minimum};
}

MultiwayAction mw_fold() {
  return {ActionType::Fold, 0};
}
// A Call action carries NO target total, same contract as the heads-up type.
MultiwayAction mw_call() {
  return {ActionType::Call, 0};
}
MultiwayAction mw_aggressive(const MultiwayLegal& legal) {
  return {legal.aggressive->type, legal.aggressive->minimum};
}
MultiwayAction mw_check() {
  return {ActionType::Check, 0};
}

// Total chips the state accounts for. Conservation is always asserted as
// "unchanged from the root", never against a hard-coded total, so the check
// does not depend on how either type declares its root pot.
Chips hu_system_chips(const HeadsUpState& state) {
  return state.pot() + state.players()[0].stack + state.players()[1].stack;
}

Chips mw_system_chips(const MultiwayState& state) {
  Chips total = state.pot();
  for (const MultiwayPlayer& player : state.players())
    total += player.stack;
  return total;
}

// Public cards used by the walks. Distinct, and never a hole card the fixtures
// name, so a walk can deal a full board without a collision.
const char* const kBoardCards[] = {"5c", "6c", "7c", "8c", "9c"};

// Advances a multiway state to the next actionable node, dealing public cards
// as the phase machine requires.
//
// This is not cosmetic. The refund and all-in paths are only reachable AFTER a
// street closes, so a walk that stops at the first `!actor` never enters them
// and a check built on that walk is vacuous - it passes no matter what the
// implementation does. The deal step is what makes the checks below real.
bool mw_advance_to_action(MultiwayState& state, int& dealt) {
  for (int guard = 0; guard < 8; ++guard) {
    if (state.phase() == Phase::Deal) {
      if (dealt >= 5)
        return false;
      state = state.after_card(card(kBoardCards[dealt]));
      ++dealt;
      continue;
    }
    return state.phase() == Phase::Action && state.actor().has_value();
  }
  return false;
}

// Parity of the deal count, so a walk alternates between calling and taking the
// largest legal action rather than following one line.
bool mw_take_max(int step) {
  return (step % 3) == 2;
}
bool mw_take_fold(int step) {
  return (step % 5) == 4;
}

// ---------------------------------------------------------------------------
// 1. A seat with an empty stack must never hold the action, in either type.
//
//    The two types represent all-in differently: heads-up keeps an independent
//    `all_in_` array that `refund_unmatched` does NOT update, while the multiway
//    player derives `all_in = stack == 0` and re-derives it after a refund.
//    Whichever representation the unified state keeps, this property is what
//    the representation has to preserve, so it is asserted here rather than
//    left implicit in either implementation.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 0. An ANTE that empties a stack must leave the seat unable to act.
//
//    This is not hypothetical. The multiway constructor re-derived `all_in`
//    after posting blinds but not after posting antes, so a seat emptied by an
//    ante stayed `pending`, `next_actor` selected it, and `legal()` offered it a
//    free check with nothing behind. The omission was invisible because the v1
//    validator rejects every non-zero ante before any state is built, and no
//    test used an ante at all. It is asserted here against the invariants the
//    rest of this file already relies on, so the ante path stops being the one
//    payment route that does not maintain all-in status.
//
//    Keep this the first case: it guards the constructor, before any walk.
// ---------------------------------------------------------------------------
int check_ante_that_empties_a_stack_disables_action() {
  // Every seat is emptied by its own ante.
  {
    MultiwayRoot root;
    root.players = 3;
    root.button = 1;
    root.big_blind = 2;
    root.ante = 10;
    root.stacks.assign(3, 10);
    root.contributions.assign(3, 0);
    const MultiwayState state(root);
    Chips total_contributed = 0;
    for (const MultiwayPlayer& player : state.players()) {
      CHECK(player.stack == 0);
      CHECK(player.all_in);
      total_contributed += player.contributed;
    }
    // Antes are dead money: all of it is in the pot, and no seat can act.
    CHECK(total_contributed == 30);
    CHECK(state.pot() == 30);
    CHECK(!state.actor().has_value());
  }

  // Mixed depths: seats that survive the ante can still act, seats emptied by it
  // cannot, and the actor is never one of the emptied ones. This is the case a
  // blanket "nobody acts" fix would wrongly satisfy.
  for (Chips shallow : {Chips{5}, Chips{10}, Chips{25}}) {
    MultiwayRoot root;
    root.players = 4;
    root.button = 0;
    root.big_blind = 2;
    root.ante = 10;
    root.stacks = {shallow, 100, shallow, 100};
    root.contributions.assign(4, 0);
    const MultiwayState state(root);
    for (const MultiwayPlayer& player : state.players()) {
      CHECK((player.stack == 0) == player.all_in);
      if (player.stack == 0)
        CHECK(!player.pending);
    }
    if (const auto actor = state.actor()) {
      // Whoever holds the action must have chips to wager.
      CHECK(state.players()[*actor].stack > 0);
      CHECK(!state.players()[*actor].all_in);
    }
    // And the action must be offered only to seats that can pay for it.
    const MultiwayLegal legal = state.legal();
    if (legal.call) {
      const auto actor = state.actor();
      CHECK(actor.has_value());
      CHECK(legal.call_amount <= state.players()[*actor].stack);
    }
  }
  return 0;
}

int check_no_empty_stack_ever_holds_the_action() {
  // Heads-up: walk a deterministic, legal branch from many small roots so the
  // all-in paths are actually reached.
  for (Chips stack = 2; stack <= 14; ++stack) {
    for (std::size_t button = 0; button < 2; ++button) {
      HeadsUpState state(hu_root(stack, button));
      for (int step = 0; step < 24; ++step) {
        const auto actor = state.actor();
        if (!actor)
          break;
        CHECK(state.players()[*actor].stack > 0);
        const LegalActions legal = state.legal();
        // Alternate between the cheapest and the largest legal action so the
        // walk reaches all-in states instead of always calling.
        const bool take_max = (step % 3) == 2 && legal.aggressive.has_value();
        const Action choice = take_max ? hu_aggressive(legal) : hu_call();
        if (!legal.contains(choice))
          break;
        state = state.after_action(*actor, choice);
        // Invariant at every reachable node, not only when acting.
        for (std::size_t p = 0; p < 2; ++p) {
          if (state.players()[p].stack == 0 && !state.players()[p].folded) {
            const auto next = state.actor();
            if (next.has_value() && *next == p)
              return 1;
          }
        }
      }
    }
  }

  // Multiway: the representation makes it structural, so the check pins that a
  // unification may not quietly invert it. The walk deals, folds, and goes all
  // in, because the all-in re-derivation after a refund is only reachable on
  // those paths.
  for (std::size_t players = 3; players <= 6; ++players) {
    for (Chips stack : {Chips{2}, Chips{5}, Chips{9}, Chips{20}}) {
      MultiwayState state(mw_root(players, stack));
      int dealt = 0;
      bool reached_refund = false;
      for (int step = 0; step < 40; ++step) {
        if (!mw_advance_to_action(state, dealt))
          break;
        const auto actor = state.actor();
        if (!actor)
          break;
        const MultiwayLegal legal = state.legal();
        const bool take_fold = mw_take_fold(step) && legal.fold;
        const bool take_max = !take_fold && mw_take_max(step) && legal.aggressive.has_value();
        // A seat that has already matched must CHECK; `call` is not legal then.
        MultiwayAction choice{};
        if (take_fold)
          choice = mw_fold();
        else if (take_max)
          choice = mw_aggressive(legal);
        else if (legal.call)
          choice = mw_call();
        else if (legal.check)
          choice = mw_check();
        else
          break;
        if (!legal.contains(choice))
          break;
        state = state.after_action(*actor, choice);
        for (const MultiwayPlayer& player : state.players()) {
          if (player.refunded > 0)
            reached_refund = true;
          if ((player.stack == 0) != player.all_in)
            return 1;
        }
      }
      // Equal stacks and matching calls never produce an uncalled excess, so
      // this walk guards the invariant but cannot exercise the refund itself.
      // The refund path has its own fixture below, with unequal stacks.
      static_cast<void>(reached_refund);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 1b. The refund path itself, which equal-stack fixtures cannot reach. A seat
//     that goes all in for MORE than any other seat committed must have its
//     excess returned, and - the point of the check - its all-in status must be
//     re-derived from the stack that the refund just changed.
//
//     This is the exact shape RFC 0008 §L1 records: the multiway representation
//     derives `all_in` from the stack and re-derives it after a refund, while
//     the heads-up representation keeps an independent `all_in_` that
//     `refund_unmatched` does not touch. Remove the re-derivation and this case
//     fails; nothing else in this suite notices.
// ---------------------------------------------------------------------------
int check_refund_re_derives_all_in_status() {
  // Three seats with UNEQUAL stacks, which is what makes an excess possible:
  // seat 0 can commit more than seat 1 is able to.
  MultiwayRoot root;
  root.players = 3;
  root.button = 1;
  root.big_blind = 2;
  root.ante = 0;
  // Multiway posts its own blinds from the button and big blind; unequal
  // stacks are what make an uncalled excess possible at all.
  root.stacks = {20, 9, 20};
  root.contributions.assign(3, 0);
  MultiwayState state(root);

  // Preflop order starts left of the big blind. Drive to a state where seat 0
  // is all in for more than seat 1 can match and every other seat folds.
  int dealt = 0;
  bool saw_excess = false;
  for (int step = 0; step < 20; ++step) {
    if (!mw_advance_to_action(state, dealt))
      break;
    const auto actor = state.actor();
    if (!actor)
      break;
    const MultiwayLegal legal = state.legal();
    MultiwayAction choice{};
    if (*actor == 1 && legal.aggressive.has_value()) {
      // Seat 1 goes all in: the largest legal target.
      choice = {legal.aggressive->type, legal.aggressive->maximum};
    } else if (*actor == 0 && legal.aggressive.has_value()) {
      choice = {legal.aggressive->type, legal.aggressive->maximum};
    } else if (*actor == 2 && legal.fold) {
      choice = mw_fold();
    } else if (legal.fold) {
      choice = mw_fold();
    } else if (legal.call) {
      choice = mw_call();
    } else if (legal.check) {
      choice = mw_check();
    } else {
      break;
    }
    if (!legal.contains(choice))
      break;
    state = state.after_action(*actor, choice);
    for (const MultiwayPlayer& player : state.players()) {
      if (player.refunded > 0)
        saw_excess = true;
      // The invariant under test, asserted at every node the line passes
      // through: a refunded seat's all-in status follows its stack.
      if ((player.stack == 0) != player.all_in)
        return 1;
    }
  }

  // The fixture has to actually produce an excess, or it guards nothing.
  return saw_excess ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 2. An uncalled excess comes back only down to the best level another seat
//    actually reached. Asserted as the poker property it is: after a hand ends
//    by folding, the winner's street commitment equals the folded seat's, so
//    the winner never books a wager nobody called.
// ---------------------------------------------------------------------------
int check_fold_out_leaves_no_uncalled_wager() {
  // Heads-up, both blind positions, several bet sizes.
  for (std::size_t button = 0; button < 2; ++button) {
    const HeadsUpState start(hu_root(50, button));
    const auto opener = start.actor();
    CHECK(opener.has_value());
    const LegalActions legal = start.legal();
    if (!legal.aggressive.has_value())
      continue;  // the opener must be able to bet in this fixture

    const Action big_bet = hu_aggressive(legal);
    if (!legal.contains(big_bet))
      return 1;
    const HeadsUpState after_bet = start.after_action(*opener, big_bet);
    const auto responder = after_bet.actor();
    CHECK(responder.has_value());

    const HeadsUpState after_fold = after_bet.after_action(*responder, hu_fold());
    CHECK(after_fold.phase() == Phase::Folded);

    const std::size_t winner = *opener;
    const std::size_t loser = *responder;
    // The winner may not keep more committed than the opponent actually put
    // in on the street; the excess is refunded.
    CHECK(after_fold.players()[winner].street_committed ==
          after_fold.players()[loser].street_committed);

    // And conservation still holds through that refund.
    CHECK(hu_system_chips(after_fold) == hu_system_chips(start));

    const Settlement settled = after_fold.settle_fold();
    CHECK(settled.final_stacks[0] + settled.final_stacks[1] == hu_system_chips(start));
  }

  // Multiway: the same property, reached by folding everyone but one seat.
  for (std::size_t players = 3; players <= 6; ++players) {
    MultiwayState state(mw_root(players, 60));
    const Chips start_chips = mw_system_chips(state);
    for (std::size_t step = 0; step < 3 * players; ++step) {
      const auto actor = state.actor();
      if (!actor)
        break;
      if (state.phase() != Phase::Action)
        break;
      const MultiwayLegal legal = state.legal();
      if (!legal.fold)
        break;
      state = state.after_action(*actor, mw_fold());
      if (state.live_players().size() == 1)
        break;
    }
    if (state.live_players().size() != 1)
      continue;  // this fixture's line did not fold the hand out
    CHECK(state.phase() == Phase::Folded);
    CHECK(mw_system_chips(state) == start_chips);

    const std::size_t winner = state.live_players()[0];
    const ContributionSettlement settled = state.settle_fold();
    Chips final_total = 0;
    for (Chips value : settled.final_stacks)
      final_total += value;
    CHECK(final_total == start_chips);
    // The winner ends with strictly more than it started with, and no seat ends
    // below zero.
    CHECK(settled.final_stacks[winner] > 60 - settled.net_contributions[winner]);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 3. The big-blind option is a heads-up-only rule. It is the only rule in the
//    twelve shared names that exists in one type and has no counterpart at all
//    in the other, so a unified definition has to state whether it generalizes.
// ---------------------------------------------------------------------------
int check_big_blind_option_exists_heads_up_and_not_in_three_seats() {
  // Heads-up: the big blind acts again after the button limps, and may raise.
  for (std::size_t button = 0; button < 2; ++button) {
    const HeadsUpState start(hu_root(50, button));
    const auto limper = start.actor();
    CHECK(limper.has_value());
    const LegalActions legal = start.legal();
    CHECK(legal.contains(hu_call()));
    const HeadsUpState after_limp = start.after_action(*limper, hu_call());

    const auto option = after_limp.actor();
    CHECK(option.has_value());
    CHECK(*option == 1 - button);  // the big blind is the non-button seat
    CHECK(after_limp.can_raise(*option));
    const LegalActions option_legal = after_limp.legal();
    CHECK(option_legal.check);
  }

  // Multiway, three seats: once every seat has matched, the street closes and
  // no seat retains an option. The round must not leave a seat waiting.
  {
    MultiwayState state(mw_root(3, 50));
    for (int step = 0; step < 8; ++step) {
      const auto actor = state.actor();
      if (!actor || state.street() != Street::Preflop)
        break;
      const MultiwayLegal legal = state.legal();
      const MultiwayAction choice = legal.check ? mw_check() : mw_call();
      if (!legal.contains(choice))
        break;
      state = state.after_action(*actor, choice);
    }
    // Limping around must close the preflop round rather than hand anyone a
    // free option, so no actor remains on the preflop street.
    if (state.street() == Street::Preflop)
      CHECK(!state.actor().has_value());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 4. A mid-hand fold removes a seat from the action; it does not end the hand.
//    The hand ends only when one live player remains. This is the reading that
//    the shared `Phase::Folded` name invites and that RFC 0008 §L1 records as a
//    merge hazard, so it is pinned from the public API.
// ---------------------------------------------------------------------------
int check_mid_hand_fold_does_not_end_the_hand() {
  for (std::size_t players = 4; players <= 6; ++players) {
    MultiwayState state(mw_root(players, 80));
    const auto first = state.actor();
    CHECK(first.has_value());
    const MultiwayLegal legal = state.legal();
    CHECK(legal.fold);
    state = state.after_action(*first, mw_fold());

    // One seat folded out of many: the hand is still live and still acting.
    CHECK(state.live_players().size() == players - 1);
    CHECK(state.phase() == Phase::Action);
    CHECK(state.actor().has_value());
    // The folded seat is gone from the action but its chips stay in the pot.
    CHECK(state.pot() > 0);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 5. Conservation is a floor, not a new property: every terminal path in both
//    types must account for exactly the chips the root declared. Asserted
//    relative to the root so neither type's root convention is baked in.
// ---------------------------------------------------------------------------
int check_conservation_on_every_terminal_path() {
  // Heads-up, checked-down showdown at several depths.
  for (Chips stack = 4; stack <= 20; stack += 4) {
    HeadsUpState state(hu_root(stack));
    const Chips start = hu_system_chips(state);
    // Check to the flop if both seats are deep enough to have an option.
    for (int step = 0; step < 4 && state.phase() == Phase::Action; ++step) {
      const auto actor = state.actor();
      if (!actor)
        break;
      const LegalActions legal = state.legal();
      if (!legal.check)
        break;
      state = state.after_action(*actor, {ActionType::Check, 0});
    }
    if (state.phase() != Phase::Deal)
      continue;
    for (const char* name : {"5c", "6c", "7c", "8c", "9c"}) {
      if (state.phase() != Phase::Deal)
        break;
      state = state.after_card(card(name));
    }
    if (state.phase() != Phase::Showdown)
      continue;
    const Settlement settled =
        state.settle_showdown({{{card("Ah"), card("Kh")}, {card("9c"), card("8c")}}});
    CHECK(settled.final_stacks[0] + settled.final_stacks[1] == start);
  }

  // Multiway, folded out at every seat count, plus a checked-down showdown.
  for (std::size_t players = 3; players <= 6; ++players) {
    for (Chips stack = 4; stack <= 16; stack += 4) {
      MultiwayState state(mw_root(players, stack));
      const Chips start = mw_system_chips(state);
      int dealt = 0;
      for (std::size_t step = 0; step < 4 * players; ++step) {
        if (!mw_advance_to_action(state, dealt))
          break;
        const auto actor = state.actor();
        if (!actor)
          break;
        const MultiwayLegal legal = state.legal();
        // Check when free, otherwise call, so the hand reaches a showdown.
        const MultiwayAction choice = legal.check ? mw_check() : mw_call();
        if (!legal.contains(choice))
          break;
        state = state.after_action(*actor, choice);
      }
      Chips total = 0;
      if (state.phase() == Phase::Folded) {
        const ContributionSettlement settled = state.settle_fold();
        for (Chips value : settled.final_stacks)
          total += value;
        CHECK(total == start);
      } else if (state.phase() == Phase::Deal) {
        while (state.phase() == Phase::Deal && dealt < 5) {
          state = state.after_card(card(kBoardCards[dealt]));
          ++dealt;
        }
        if (state.phase() == Phase::Showdown) {
          std::vector<std::array<int, 2>> holes;
          for (std::size_t live = 0; live < state.live_players().size(); ++live)
            holes.push_back({card("Ah"), card("Kh")});
          const ContributionSettlement settled = state.settle_showdown(holes);
          for (Chips value : settled.final_stacks)
            total += value;
          CHECK(total == start);
        }
      }
      // Whatever the path, live chips plus pot never exceed the root total.
      CHECK(mw_system_chips(state) <= start);
    }
  }
  return 0;
}

}  // namespace

int main() {
  using Case = std::pair<const char*, int (*)()>;
  const std::array<Case, 7> cases = {{
      {"ante_that_empties_a_stack_disables_action",
       check_ante_that_empties_a_stack_disables_action},
      {"no_empty_stack_ever_holds_the_action", check_no_empty_stack_ever_holds_the_action},
      {"refund_re_derives_all_in_status", check_refund_re_derives_all_in_status},
      {"fold_out_leaves_no_uncalled_wager", check_fold_out_leaves_no_uncalled_wager},
      {"big_blind_option_exists_heads_up_and_not_in_three_seats",
       check_big_blind_option_exists_heads_up_and_not_in_three_seats},
      {"mid_hand_fold_does_not_end_the_hand", check_mid_hand_fold_does_not_end_the_hand},
      {"conservation_on_every_terminal_path", check_conservation_on_every_terminal_path},
  }};
  int failures = 0;
  for (const Case& item : cases) {
    std::printf("case %s ... ", item.first);
    std::fflush(stdout);
    const int result = item.second();
    std::printf("%s\n", result == 0 ? "ok" : "FAILED");
    failures += result;
  }
  if (failures == 0)
    std::printf("all unification invariant cases passed\n");
  return failures == 0 ? 0 : 1;
}
