// Unified N-seat game definition (RFC 0008 §L1) -- construction, validation,
// identity, and the queries both rule profiles share.
//
// ONE type serves 2..10 seats. The two-seat profile is NOT the three-handed
// profile with the seat count turned down: four rules differ at two seats
// (the button posts the small blind, a refund preserves an all-in flag,
// the big blind holds a preflop option no multiway rule has, and a street
// closes the moment either seat is all in rather than when no seat can act).
// They are not folded into one general formula; the seat-count branches
// below state each decision, and the exhaustive oracles pin that the shared
// transition code collapses onto `HeadsUpState` at two seats and onto
// `MultiwayState` at 3..10. The heads-up rules supply the stricter two-seat
// forms; the multiway rules supply the general clockwise forms.

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/multiway.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "poker_detail.hpp"

namespace bs::poker {
namespace {

using detail::add;
using detail::clockwise;
using detail::require;
using detail::use_card;

}  // namespace

// Blind seats and the preflop opener.
//
// Heads-up is NOT the three-handed rule with the seat count turned down. With
// two seats the BUTTON posts the small blind and the other seat posts the big
// blind, which is what lets the button act first preflop; with three or more the
// blinds sit clockwise of the button and the opener is the seat after the big
// blind. Deriving the two-seat case as `clockwise(count, button, 2)` returns the
// button itself, so the distinction has to be written down rather than folded
// into one formula. Exported (RFC 0010) so the artifact reader/writer derives
// N-way blinds from the same source of truth as GameDef validation.
std::size_t small_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? def.button : detail::clockwise(def.player_count, def.button);
}
std::size_t big_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? detail::clockwise(def.player_count, def.button)
                               : detail::clockwise(def.player_count, def.button, 2);
}
std::size_t preflop_first_actor(const GameDef& def) {
  return def.player_count == 2 ? def.button
                               : detail::clockwise(def.player_count, big_blind_seat(def));
}

void validate(const GameDef& def) {
  require(def.player_count >= kMinUnifiedSeats, "a game needs at least two seats");
  require(def.player_count <= kMaxUnifiedSeats, "the unified profile serves at most ten seats");
  require(def.button < def.player_count, "button outside the seated range");
  require(def.big_blind > 0 && def.big_blind <= kMaxHeadsUpChips, "invalid big blind");
  require(def.variant == RulesVariant::NoLimitHoldem, "unsupported rules variant");

  // RFC 0007 (scope extended by RFC 0010): a flop-terminal game ends at the
  // flop; the frontier evaluator supplies leaf values. The root may be the
  // preflop root (board_size == 0, the W4b preflop profile) or the flop
  // (board_size == 3, the flop class library). The frontier evaluator takes a
  // 3-card flop in both cases. Flop-terminal games support 2..10 seats and
  // require equal stacks: the frontier settles a single pot with no side pots,
  // and side pots form only with unequal stacks.
  if (def.terminal == TerminalDepth::Flop) {
    require(def.player_count >= 2 && def.player_count <= kMaxUnifiedSeats,
            "flop-terminal games support 2..10 seats");
    const bool valid_root =
        (def.preflop && def.board_size == 0) || (!def.preflop && def.board_size == 3);
    require(valid_root, "a flop-terminal game must start at the preflop root or on the flop");
    const Chips stack0 = def.stacks[0];
    for (std::size_t p = 1; p < def.player_count; ++p)
      require(def.stacks[p] == stack0,
              "flop-terminal games require equal stacks (single-pot settlement)");
  }

  const bool heads_up = def.player_count == 2;
  // A blind never exceeds the stack posting it. An ante MAY exceed a stack:
  // like a blind it is capped to the stack when posted, so no upper bound is
  // validated here (the 2-seat profile additionally rejects every ante).
  for (std::size_t p = 0; p < def.player_count; ++p)
    require(def.blinds_posted[p] <= def.stacks[p], "blind exceeds stack");

  if (heads_up) {
    // The two-seat profile. It is not the three-handed profile turned down:
    // the BUTTON posts the small blind, no ante exists, and both blinds post
    // their full nominal amounts (only the big blind may be short, by posting
    // its whole stack for the nominal amount). Each is a rule the 3+ profile
    // does not share, so they are pinned here rather than folded into one
    // general formula. A rooted board carries a complete street (3/4/5), the
    // same allowance the 3+ profile makes; the flop-only restriction lives at
    // the heads-up solve route (HeadsUpRoot is structurally flop-rooted), not
    // at the game definition, so the seat-parameterized trainer -- which
    // consumes the L3 tree directly and supports any postflop root -- can
    // solve a two-seat turn- or river-rooted game.
    require(def.ante == 0, "the two-seat profile has no ante");

    if (def.preflop) {
      require(def.board_size == 0, "a preflop root must not carry a board");
      const Chips small_blind = def.big_blind / 2;
      require(small_blind > 0, "big blind too small to post a small blind");
      require(def.blinds_posted[small_blind_seat(def)] == small_blind,
              "the small blind seat must post the small blind");
      require(def.blinds_posted[big_blind_seat(def)] == def.big_blind,
              "the big blind seat must post the big blind");
      // The pot covers the posted blinds, which are live street commitments
      // and not dead money, so equal contributions need only FIT inside it.
      require(def.contributions[0] == def.contributions[1],
              "unmatched closed-street contributions at preflop root");
      require(add(def.contributions[0], def.contributions[1]) <= def.pot,
              "preflop root contributions exceed the pot");
    } else {
      require(def.board_size == 3 || def.board_size == 4 || def.board_size == 5,
              "a rooted board must carry a complete street");
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
  } else {
    // The 3..10-seat profile, whose behavior `MultiwayState` used to own.
    // With three or more seats an empty board IS the preflop profile: there is
    // no separate flag in `MultiwayRoot` (board emptiness selects it), so a
    // rooted (non-posting) definition cannot carry an empty board and a
    // preflop definition cannot carry one.
    require(def.preflop == (def.board_size == 0),
            "for three or more seats an empty board is the preflop profile, and only it");
    if (def.board_size > 0)
      require(def.board_size == 3 || def.board_size == 4 || def.board_size == 5,
              "a rooted board must carry a complete street");
    std::array<bool, 52> used{};
    for (std::uint8_t i = 0; i < def.board_size; ++i)
      use_card(def.board[i], used);

    // Blinds sit on the two seats clockwise of the button, and every OTHER
    // seat posts nothing. The posted amount is capped by what that seat still
    // holds AFTER its ante, because antes are collected first and a blind is a
    // live wager nobody can be forced to post without chips. Requiring exact
    // equality here (rather than "<= nominal") is what lets an adapter map
    // `MultiwayRoot` onto this definition without deciding anything itself.
    const std::size_t small_seat = small_blind_seat(def);
    const std::size_t big_seat = big_blind_seat(def);
    const Chips small_blind = def.big_blind / 2;
    require(small_blind > 0, "big blind too small to post a small blind");
    Chips posted_total = 0;
    for (std::size_t p = 0; p < def.player_count; ++p) {
      const bool is_blind_seat = p == small_seat || p == big_seat;
      if (def.preflop) {
        require(is_blind_seat || def.blinds_posted[p] == 0,
                "only the two seats clockwise of the button post a blind");
        if (is_blind_seat)
          posted_total = add(posted_total, def.blinds_posted[p]);
      } else {
        require(def.blinds_posted[p] == 0,
                "a rooted board is past the blinds, which are already in the pot");
      }
    }
    const auto stack_after_ante = [&](std::size_t p) {
      return def.stacks[p] - std::min(def.ante, def.stacks[p]);
    };
    if (def.preflop) {
      require(def.blinds_posted[small_seat] == std::min(small_blind, stack_after_ante(small_seat)),
              "the small blind seat posts the small blind, capped after its ante");
      require(def.blinds_posted[big_seat] == std::min(def.big_blind, stack_after_ante(big_seat)),
              "the big blind seat posts the big blind, capped after its ante");
    }

    // The declared pot reconciles with every chip the root accounts for:
    // arbitrary per-seat dead money on a rooted board (that profile is past the
    // blinds), plus the antes and capped blinds on a preflop root. Unlike the
    // two-seat profile this may be zero: `MultiwayState` accepts a root with no
    // money in it, and rejecting what the old type built would be a hidden
    // divergence.
    Chips declared_pot = 0;
    for (std::size_t p = 0; p < def.player_count; ++p) {
      declared_pot = add(declared_pot, def.contributions[p]);
      if (def.preflop)
        declared_pot = add(declared_pot, std::min(def.ante, def.stacks[p]));
    }
    if (def.preflop)
      declared_pot = add(declared_pot, posted_total);
    require(def.pot == declared_pot,
            "root pot must equal the dead money, antes, and posted blinds");
  }

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
  heads_up_profile_ = def_.player_count == 2;

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
    // The two-seat profile rejects every nonzero ante, so this loop posts
    // nothing there; the branch exists for the 3..10 profile.
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
    // Every seat that can act still owes one. The big blind keeps its option at
    // an unraised preflop root in the TWO-seat profile only; the state expresses
    // that through `big_blind_option_`, exactly as `HeadsUpState` does, because
    // expressing it through pending alone would also hold in a 3+ handed game
    // (the multiway profile has no option: once every seat has matched, the
    // street closes).
    for (std::size_t p = 0; p < def_.player_count; ++p) {
      players_[p].pending = !players_[p].folded && !players_[p].all_in;
      players_[p].raise_rights = true;
    }
    big_blind_option_ = heads_up_profile_;
    refresh_live();
    if (heads_up_profile_) {
      // The shipped heads-up rule closes the root the moment EITHER seat is
      // all in, not when no seat can act: with two seats one player cannot
      // wager against nobody, and the refunded-capped-blind case rides that
      // flag. At 3+ seats the rule is different: a single seat facing only
      // all-in or folded opponents still OWES its call/fold decision, so the
      // street opens whenever an actionable seat exists -- the opener search
      // below closes it only when none does.
      if (players_[0].all_in || players_[1].all_in) {
        close_street();
        return;
      }
    }
    actor_ = preflop_first_actor(def_);
    if (!players_[actor_].pending || players_[actor_].all_in) {
      const auto next = next_actor(actor_);
      if (next) {
        actor_ = *next;
      } else {
        close_street();
        return;
      }
    }
  } else {
    board_size_ = def_.board_size;
    std::copy_n(def_.board.begin(), board_size_, board_.begin());
    street_ = board_size_ == 3 ? Street::Flop : (board_size_ == 4 ? Street::Turn : Street::River);
    last_full_raise_ = def_.big_blind;
    // A rooted board carries chips behind, so an empty stack here marks an
    // all-in seat from an earlier street. `GameDef` has no folded field, so no
    // seat can start a rooted street folded; pending therefore keys off the
    // stack alone.
    for (std::size_t p = 0; p < def_.player_count; ++p) {
      players_[p].all_in = players_[p].stack == 0;
      players_[p].pending = players_[p].stack > 0 && !players_[p].folded;
      players_[p].raise_rights = true;
    }
    refresh_live();
    if (heads_up_profile_) {
      // Two seats: either seat all in closes the street, because the other
      // seat cannot wager alone. 3+ seats: do nothing yet -- the opener
      // search below is the close test, and one actionable seat facing only
      // all-in opponents must still get its call/fold decision.
      if (players_[0].all_in || players_[1].all_in) {
        close_street();
        return;
      }
    }
    // The postflop opener is the first seat clockwise of the button that can
    // still act; `next_actor` skips a folded or all-in seat.
    const auto first = next_actor(def_.button);
    if (!first) {
      close_street();
      return;
    }
    actor_ = *first;
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
  // respond, and the hero must be able to cover the call first. The responder
  // test skips folded seats and seats already all in; a folded seat's chips do
  // not make a raise contestable.
  bool opponent_can_respond = false;
  for (std::size_t p = 0; p < def_.player_count; ++p)
    if (p != actor_ && !players_[p].folded && !players_[p].all_in && players_[p].stack > 0)
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
    // The two-seat profile does NOT skip a folded seat when locating the
    // high commitment: with two players the fold itself refunds the excess,
    // and the folded seat's commitment IS the level the winner actually
    // reached. The 3+ profile skips it, because a folded player's chips are
    // dead money the remaining players never had to call.
    if (players_[p].folded && !heads_up_profile_)
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
  if (!heads_up_profile_)
    // The multiway rule: the returned amount is what leaves this seat's
    // commitment equal to the next level, so a refunded all-in is no longer all
    // in and can act again if the street continues. The two-seat profile does
    // the opposite ON PURPOSE: the flag, once set, stays set until a new
    // street is dealt. A capped big blind posts its stack, the unmatched part
    // comes back at street close, and the shipped heads-up rule still treats
    // that seat as all in for the rest of the hand
    // (`engine/tests/test_heads_up_preflop.cpp:229`: "The board runs out with
    // no action at any street"). Re-deriving produced a real divergence: one
    // chip of stack and an entire extra betting round.
    players_[high].all_in = players_[high].stack == 0;
}

void GameState::close_street() {
  refund_unmatched();
  for (std::size_t p = 0; p < def_.player_count; ++p)
    players_[p].pending = false;
  // The two-seat profile clears raise rights on every street close (its
  // `close_street` zeroes them); the multiway profile leaves them as the
  // street's actions set them, because it re-establishes rights only when a
  // later street opens or a full raise reopens. At a close with no pending
  // seat the two readings agree until the next `after_card`, which resets both.
  if (heads_up_profile_)
    for (std::size_t p = 0; p < def_.player_count; ++p)
      players_[p].raise_rights = false;
  big_blind_option_ = false;
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
  // Heads-up preflop only: the big blind ACTING (check, call, raise, or fold)
  // spends its option, exactly as `HeadsUpState::big_blind_acting` clears it
  // before the switch. This is the missing side: a raise or fold clears it
  // in their branches; a check here is the common case that needs it too.
  if (heads_up_profile_ && street_ == Street::Preflop && player == big_blind_seat(def_))
    next.big_blind_option_ = false;
  Chips paid = 0;
  switch (action.type) {
    case ActionType::Fold:
      hero.folded = true;
      // The two-seat profile clears a folded seat's all-in flag AND settles
      // the fold immediately: with two players a fold ends the hand, so the
      // uncalled excess comes back at once. In the 3+ profile a fold is
      // mid-hand: no refund here, no phase change -- the unmatched excess
      // returns only when the street actually closes, exactly as
      // `MultiwayState` orders it. Refunding at the fold instead held one
      // chip of a still-open blind on the ledger while the hand continued.
      if (heads_up_profile_) {
        hero.all_in = false;
        next.refund_unmatched();
      }
      if (heads_up_profile_) {
        // Heads-up: a fold ends the hand immediately, and the shipped type
        // also clears every pending/raise/option flag and enters Folded right
        // here. There is no "mid-hand folded seat" state with two players.
        for (GamePlayer& other : next.players_) {
          other.pending = false;
          other.raise_rights = false;
        }
        next.big_blind_option_ = false;
        next.refresh_live();
        next.phase_ = Phase::Folded;
        return next;
      }
      break;
    case ActionType::Check:
      break;
    case ActionType::Call:
      paid = std::min(high - hero.street_committed, hero.stack);
      next.pay(player, paid);
      // A call does not necessarily close the street: at a two-seat preflop
      // root the big blind still holds its option, which it holds precisely
      // while it remains the one seat that still owes an action. In a 3+ game
      // there is no option and the generic pending test closes below.
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
  // A single remaining participant ends the hand immediately. This is the
  // only way the multiway profile reaches Folded: a MID-HAND fold merely sets
  // the folded flag and the action continues past it, which is the distinction
  // RFC 0008 §L1 records under the shared `Phase::Folded` name.
  if (next.live_size_ <= 1) {
    next.phase_ = Phase::Folded;
    next.refund_unmatched();
    for (GamePlayer& other : next.players_)
      other.pending = false;
    next.big_blind_option_ = false;
    return next;
  }
  if (heads_up_profile_) {
    // The shipped heads-up close: with the big blind option the street stays
    // open exactly while the non-button seat's option holds.
    const std::size_t opponent = 1 - player;
    const bool option_holds = next.big_blind_option_ && street_ == Street::Preflop &&
                              !next.players_[opponent].folded && !next.players_[opponent].all_in &&
                              !next.players_[opponent].pending;
    if (!next.players_[opponent].pending && !option_holds) {
      next.close_street();
      return next;
    }
    next.actor_ = opponent;
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
  // partially dealt flop is already `Street::Flop`.
  const std::size_t dealt = next.board_size_;
  next.street_ = dealt <= 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
  if (dealt < 3)
    return next;

  // RFC 0007: a flop-terminal game ends when a completed flop would open
  // action. The traversal reaches a frontier leaf whose value is supplied
  // by a declared frontier evaluator rather than by the rules' showdown.
  if (def_.terminal == TerminalDepth::Flop) {
    next.refresh_live();
    next.phase_ = Phase::Frontier;
    return next;
  }

  for (std::size_t p = 0; p < def_.player_count; ++p) {
    next.players_[p].street_committed = 0;
    next.players_[p].raise_rights = true;
    next.players_[p].pending = !next.players_[p].folded && !next.players_[p].all_in;
  }
  next.last_full_raise_ = def_.big_blind;
  next.refresh_live();
  // Two seats: EITHER seat all in closes the betting round, because there is
  // nobody left to bet against. 3+ seats: the round closes only when NO
  // actionable seat remains; the last actionable seat among all-in or folded
  // opponents still acts (it can call or fold; `legal()` denies it a raise).
  bool nobody_can_act;
  if (heads_up_profile_) {
    nobody_can_act = next.players_[0].all_in || next.players_[1].all_in;
  } else {
    const auto opener = next.next_actor(def_.button);
    nobody_can_act = !opener;
    if (!nobody_can_act) {
      next.phase_ = Phase::Action;
      next.actor_ = *opener;
    }
  }
  if (nobody_can_act) {
    next.phase_ = next.street_ == Street::River ? Phase::Showdown : Phase::Deal;
    return next;
  }
  if (heads_up_profile_) {
    next.phase_ = Phase::Action;
    next.actor_ = clockwise(def_.player_count, def_.button);
    if (!next.players_[next.actor_].pending)
      next.actor_ = *next.next_actor(next.actor_);
  }
  return next;
}

}  // namespace bs::poker
