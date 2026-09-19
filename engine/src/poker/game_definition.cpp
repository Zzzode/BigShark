// Unified N-seat game definition (RFC 0008 §L1) -- construction, validation,
// identity, and the queries both rule profiles share.
//
// Behavior bar: for two seats this must reproduce `HeadsUpState` exactly. The
// heads-up rules remain the reference for every two-seat decision; the multiway
// rules supply the general forms (clockwise next-actor search, folded-aware
// refund, seat-generic settlement) that must collapse onto the heads-up result
// at two seats.

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "poker_detail.hpp"

namespace bs::poker {
namespace {

using detail::add;
using detail::clockwise;
using detail::require;
using detail::use_card;

// Blind seats and the preflop opener.
//
// Heads-up is NOT the three-handed rule with the seat count turned down. With
// two seats the BUTTON posts the small blind and the other seat posts the big
// blind, which is what lets the button act first preflop; with three or more the
// blinds sit clockwise of the button and the opener is the seat after the big
// blind. Deriving the two-seat case as `clockwise(count, button, 2)` returns the
// button itself, so the distinction has to be written down rather than folded
// into one formula.
std::size_t small_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? def.button : clockwise(def.player_count, def.button);
}
std::size_t big_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? clockwise(def.player_count, def.button)
                               : clockwise(def.player_count, def.button, 2);
}
std::size_t preflop_first_actor(const GameDef& def) {
  return def.player_count == 2 ? def.button : clockwise(def.player_count, big_blind_seat(def));
}

}  // namespace

void validate(const GameDef& def) {
  require(def.player_count >= kMinUnifiedSeats, "a game needs at least two seats");
  require(def.player_count <= 2, "this stage serves two seats");
  require(def.button < def.player_count, "button outside the seated range");
  require(def.big_blind > 0 && def.big_blind <= kMaxHeadsUpChips, "invalid big blind");
  require(def.terminal == TerminalDepth::River,
          "flop-terminal games are RFC 0007's stage, not this one");
  require(def.variant == RulesVariant::NoLimitHoldem, "unsupported rules variant");

  for (std::size_t p = 0; p < def.player_count; ++p) {
    require(def.blinds_posted[p] <= def.stacks[p], "blind exceeds stack");
    require(def.ante <= def.stacks[p], "ante exceeds stack");
  }

  require(def.ante == 0, "the two-seat profile has no ante");

  if (def.preflop) {
    require(def.board_size == 0, "a preflop root must not carry a board");
    const Chips small_blind = def.big_blind / 2;
    require(small_blind > 0, "big blind too small to post a small blind");
    require(def.blinds_posted[small_blind_seat(def)] == small_blind,
            "the small blind seat must post the small blind");
    require(def.blinds_posted[big_blind_seat(def)] == def.big_blind,
            "the big blind seat must post the big blind");
    // The pot covers the posted blinds, which are live street commitments and
    // not dead money, so equal contributions need only FIT inside it.
    require(def.contributions[0] == def.contributions[1],
            "unmatched closed-street contributions at preflop root");
    require(add(def.contributions[0], def.contributions[1]) <= def.pot,
            "preflop root contributions exceed the pot");
  } else {
    require(def.board_size == 3, "a rooted board must carry a complete flop");
    // A rooted board implies the blinds already completed, so the pot IS the
    // dead money and the two must reconcile exactly.
    require(def.contributions[0] == def.contributions[1],
            "unmatched contributions at closed-street root");
    require(add(def.contributions[0], def.contributions[1]) == def.pot,
            "root pot differs from contributions");
    std::array<bool, 52> used{};
    for (std::uint8_t i = 0; i < def.board_size; ++i)
      use_card(def.board[i], used);
  }
  require(def.pot > 0, "empty root pot");

  Chips total = def.pot;
  for (std::size_t p = 0; p < def.player_count; ++p)
    total = add(total, def.stacks[p]);
  require(total <= kMaxHeadsUpChips, "root exceeds exact numeric profile");
}

bool same_game_def(const GameDef& a, const GameDef& b) {
  // Every field that changes WHICH game this is. Range weights, sizing
  // schedule, and fixed runout are not root identity; their owners compose them
  // on top of this predicate rather than folding them in here.
  if (a.player_count != b.player_count || a.button != b.button || a.big_blind != b.big_blind ||
      a.ante != b.ante || a.pot != b.pot || a.preflop != b.preflop || a.variant != b.variant ||
      a.terminal != b.terminal)
    return false;
  if (a.board_size != b.board_size)
    return false;
  for (std::uint8_t i = 0; i < a.board_size; ++i)
    if (a.board[i] != b.board[i])
      return false;
  for (std::size_t p = 0; p < a.player_count; ++p)
    if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p] ||
        a.blinds_posted[p] != b.blinds_posted[p])
      return false;
  return true;
}

void GameState::require_seat(std::size_t player, const char* message) const {
  require(player < def_.player_count, message);
}

GameState::GameState(const GameDef& def) : def_(def) {
  validate(def);

  // Seat the declared stacks and dead money before anything moves them. The
  // chip-movement paths below subtract from `stack`, so a seat left at its
  // default zero would underflow on the first subtraction.
  for (std::size_t p = 0; p < def_.player_count; ++p) {
    players_[p].stack = def_.stacks[p];
    players_[p].contributed = def_.contributions[p];
  }

  if (def_.preflop) {
    // Antes are dead money from every seat and never become street
    // commitments. They still move chips, so they MUST re-derive `all_in`:
    // omitting that is exactly the defect the multiway constructor carried, and
    // it let a seat emptied by an ante stay pending and be handed a free check.
    if (def_.ante > 0) {
      for (std::size_t p = 0; p < def_.player_count; ++p) {
        const Chips posted = std::min(def_.ante, players_[p].stack);
        players_[p].stack -= posted;
        players_[p].contributed = add(players_[p].contributed, posted);
        players_[p].all_in = players_[p].stack == 0;
      }
    }
    for (std::size_t p = 0; p < def_.player_count; ++p) {
      const Chips posted = def_.blinds_posted[p];
      players_[p].stack -= posted;
      players_[p].street_committed = add(players_[p].street_committed, posted);
      players_[p].contributed = add(players_[p].contributed, posted);
      players_[p].all_in = players_[p].stack == 0;
    }
    street_ = Street::Preflop;
    last_full_raise_ = def_.big_blind;
    // The big blind keeps its option at an unraised preflop root. The state
    // expresses that through pending: the big blind is the one seat that still
    // owes an action even after everyone has matched, so the street cannot
    // close until it acts. That IS the option, and it needs no separate flag.
    for (std::size_t p = 0; p < def_.player_count; ++p)
      players_[p].pending = !players_[p].folded && !players_[p].all_in;
    refresh_live();
    if (actionable_count() < 2) {
      close_street();
      return;
    }
    actor_ = preflop_first_actor(def_);
    if (!players_[actor_].pending)
      actor_ = *next_actor(actor_);
  } else {
    board_size_ = def_.board_size;
    std::copy_n(def_.board.begin(), board_size_, board_.begin());
    street_ = Street::Flop;
    last_full_raise_ = def_.big_blind;
    // A rooted board carries chips behind, so an empty stack here is an all-in
    // seat. Whether the seat is FOLDED is not a waffle: only `all_in` is. A
    // root with a folded seat is constructible, and if `pending` keyed off the
    // folded flag alone that seat would be handed the action.
    for (std::size_t p = 0; p < def_.player_count; ++p) {
      players_[p].all_in = players_[p].stack == 0;
      players_[p].pending = players_[p].stack > 0;
    }
    refresh_live();
    if (actionable_count() < 2) {
      close_street();
      return;
    }
    // The postflop opener is the seat left of the button, and no seat has acted
    // on this street yet, so every actionable seat still owes an action. That is
    // the heads-up flop-root contract: both seats act once and the street then
    // closes. Clearing `pending` here would instead open a round with the actor
    // not owing anything, which reads as a free check.
    actor_ = clockwise(def_.player_count, def_.button);
  }
}

std::size_t GameState::actionable_count() const {
  std::size_t count = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    if (!players_[p].folded && !players_[p].all_in)
      ++count;
  return count;
}

std::optional<std::size_t> GameState::actor() const {
  if (phase_ != Phase::Action)
    return std::nullopt;
  return actor_;
}

Chips GameState::pot() const {
  Chips total = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    total = add(total, players_[p].net_contributed());
  return total;
}

bool GameState::can_raise(std::size_t player) const {
  require(player < def_.player_count, "player outside the seated range");
  return players_[player].raise_rights && !players_[player].all_in;
}

LegalActions GameState::legal() const {
  LegalActions result;
  if (phase_ != Phase::Action)
    return result;
  const GamePlayer& hero = players_[actor_];
  Chips high = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    high = std::max(high, players_[p].street_committed);
  const Chips due = high - hero.street_committed;
  result.fold = due > 0;
  result.check = due == 0;
  result.call = due > 0;
  result.call_amount = std::min(due, hero.stack);

  // A seat with nothing behind it cannot raise, and neither can one whose
  // rights a full raise has not restored. A raise also needs someone able to
  // respond, and the hero must be able to cover the call first.
  bool opponent_can_respond = false;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    if (p != actor_ && !players_[p].folded && players_[p].stack > 0)
      opponent_can_respond = true;
  if (!hero.raise_rights || !opponent_can_respond || hero.stack <= due)
    return result;

  const Chips maximum = add(hero.street_committed, hero.stack);
  const Chips full_minimum = add(high, last_full_raise_);
  result.aggressive = TargetRange{
      due == 0 ? ActionType::Bet : ActionType::Raise,
      std::min(full_minimum, maximum),
      maximum,
      maximum <= full_minimum,
  };
  return result;
}

void GameState::refresh_live() {
  live_size_ = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    if (!players_[p].folded)
      live_[live_size_++] = p;
}

std::optional<std::size_t> GameState::next_actor(std::size_t from) const {
  for (std::size_t step = 1; step <= def_.player_count; ++step) {
    const std::size_t seat = clockwise(def_.player_count, from, step);
    const GamePlayer& player = players_[seat];
    if (player.pending && !player.folded && !player.all_in)
      return seat;
  }
  return std::nullopt;
}

bool GameState::any_pending() const {
  for (std::size_t p = 0; p < def_.player_count; ++p) {
    const GamePlayer& player = players_[p];
    if (player.pending && !player.folded && !player.all_in)
      return true;
  }
  return false;
}

void GameState::pay(std::size_t player, Chips amount) {
  GamePlayer& chips = players_[player];
  require(amount <= chips.stack, "payment exceeds stack");
  chips.stack -= amount;
  chips.street_committed = add(chips.street_committed, amount);
  chips.contributed = add(chips.contributed, amount);
  chips.all_in = chips.stack == 0;
}

void GameState::refund_unmatched() {
  // Only the highest contributor can hold an unmatched amount, and only when it
  // is strictly above every other commitment. A folded seat keeps no live
  // commitment to refund, but its chips stay in the pot as dead money, so it
  // still establishes a level the pot already reached: folding does not create
  // an uncalled excess for the winner.
  std::size_t high = 0;
  Chips high_amount = 0;
  std::size_t high_count = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p) {
    if (players_[p].folded)
      continue;
    if (players_[p].street_committed > high_amount) {
      high_amount = players_[p].street_committed;
      high = p;
      high_count = 1;
    } else if (players_[p].street_committed == high_amount) {
      ++high_count;
    }
  }
  if (high_count != 1)
    return;
  Chips other_level = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p) {
    if (p == high)
      continue;
    other_level = std::max(other_level, players_[p].street_committed);
  }
  const Chips excess = high_amount - other_level;
  if (excess == 0)
    return;
  players_[high].stack = add(players_[high].stack, excess);
  players_[high].refunded = add(players_[high].refunded, excess);
  players_[high].street_committed -= excess;
  // `all_in` is NOT cleared here. A refund returns an amount nobody matched,
  // and a seat that was all in had no such amount: it committed every chip it
  // had. Re-deriving from the stack keeps the flag honest without ever turning
  // an all-in seat back into one that can act on the same street.
  players_[high].all_in = players_[high].stack == 0;
}

void GameState::close_street() {
  refund_unmatched();
  for (std::size_t p = 0; p < def_.player_count; ++p)
    players_[p].pending = false;
  phase_ = street_ == Street::River ? Phase::Showdown : Phase::Deal;
}

GameState GameState::after_action(std::size_t player, Action action) const {
  // Preconditions first: a rejected action must not have copied the state.
  require(phase_ == Phase::Action && player == actor_, "not the acting player");
  require(legal().contains(action), "illegal action or target total");

  GameState next = *this;
  GamePlayer& hero = next.players_[player];
  Chips high = 0;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    high = std::max(high, next.players_[p].street_committed);

  hero.pending = false;
  hero.raise_rights = false;
  Chips paid = 0;
  switch (action.type) {
    case ActionType::Fold:
      hero.folded = true;
      hero.all_in = false;
      next.refund_unmatched();
      for (GamePlayer& other : next.players_) {
        other.pending = false;
        other.raise_rights = false;
      }
      next.refresh_live();
      next.phase_ = Phase::Folded;
      return next;
    case ActionType::Check:
      break;
    case ActionType::Call:
      paid = std::min(high - hero.street_committed, hero.stack);
      next.pay(player, paid);
      // A call does not necessarily close the street: at a preflop root the big
      // blind still holds its option, which it holds precisely while it remains
      // the one seat that still owes an action.
      break;
    case ActionType::Bet:
    case ActionType::Raise: {
      paid = action.target_total - hero.street_committed;
      const Chips increment = action.target_total - high;
      next.pay(player, paid);
      // Every other seat that can still act owes a response.
      for (std::size_t p = 0; p < def_.player_count; ++p) {
        if (p == player || next.players_[p].folded || next.players_[p].all_in)
          continue;
        next.players_[p].pending = true;
      }
      // Reopening is a FULL-raise rule: a short all-in does not restore raise
      // rights, and cumulative short all-ins never do.
      if (increment >= next.last_full_raise_) {
        next.last_full_raise_ = increment;
        for (std::size_t p = 0; p < def_.player_count; ++p) {
          if (p == player || next.players_[p].folded || next.players_[p].all_in)
            continue;
          next.players_[p].raise_rights = true;
        }
      }
      break;
    }
  }

  next.refresh_live();
  if (next.phase_ != Phase::Action)
    return next;
  // A single remaining participant ends the hand immediately, which is also
  // what makes the heads-up fold path identical to the multiway one.
  if (next.live_size_ <= 1) {
    next.phase_ = Phase::Folded;
    next.refund_unmatched();
    for (GamePlayer& other : next.players_)
      other.pending = false;
    return next;
  }
  if (!next.any_pending()) {
    next.close_street();
    return next;
  }
  const std::optional<std::size_t> following = next.next_actor(player);
  if (!following) {
    next.close_street();
    return next;
  }
  next.actor_ = *following;
  return next;
}

GameState GameState::after_card(int card) const {
  require(phase_ == Phase::Deal, "no public card pending");
  std::array<bool, 52> used{};
  for (std::uint8_t i = 0; i < board_size_; ++i)
    use_card(board_[i], used);
  use_card(card, used);

  GameState next = *this;
  next.board_[next.board_size_++] = card;
  // The street advances with the FIRST public card of that street, so a
  // partially dealt flop is already `Street::Flop` with fewer than three cards.
  const std::size_t dealt = next.board_size_;
  next.street_ = dealt <= 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
  if (dealt < 3)
    return next;

  for (std::size_t p = 0; p < def_.player_count; ++p) {
    next.players_[p].street_committed = 0;
    next.players_[p].raise_rights = true;
    next.players_[p].pending = !next.players_[p].folded && !next.players_[p].all_in;
  }
  next.last_full_raise_ = def_.big_blind;
  next.refresh_live();
  if (next.actionable_count() < 2) {
    // Fewer than two seats can act, so no betting round has participants and
    // the board simply runs out. This is the general form of the heads-up rule
    // that either seat being all in closes the street.
    next.phase_ = next.street_ == Street::River ? Phase::Showdown : Phase::Deal;
    return next;
  }
  next.phase_ = Phase::Action;
  next.actor_ = clockwise(def_.player_count, def_.button);
  if (!next.players_[next.actor_].pending)
    next.actor_ = *next.next_actor(next.actor_);
  return next;
}

}  // namespace bs::poker
