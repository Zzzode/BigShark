#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/multiway.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#include "poker_detail.hpp"

namespace bs::poker {
namespace {

using detail::add;
using detail::clockwise;
using detail::require;
using detail::use_card;

}  // namespace

MultiwayState::MultiwayState(const MultiwayRoot& root) : root_(root) {
  require(root.players >= kMinMultiwayPlayers && root.players <= kMaxMultiwayPlayers,
          "multiway profile supports 3..6 players");
  require(root.button < root.players, "button outside the seated range");
  require(root.big_blind > 0 && root.big_blind <= kMaxHeadsUpChips, "invalid big blind");
  require(root.stacks.size() == root.players, "one stack per player required");
  require(root.contributions.size() == root.players, "one contribution per player required");
  require(root.board.size() <= 5, "root board exceeds five cards");
  require(root.board.size() != 1 && root.board.size() != 2,
          "a root board must be empty or carry a complete street");

  std::array<bool, 52> used{};
  for (int card : root.board)
    use_card(card, used);

  const Chips small_blind = root.big_blind / 2;
  require(small_blind > 0, "big blind too small to post a small blind");

  players_.assign(root.players, MultiwayPlayer{});
  for (std::size_t p = 0; p < root.players; ++p) {
    players_[p].stack = root.stacks[p];
    players_[p].contributed = root.contributions[p];
  }

  // A fresh preflop root posts the blinds and antes from the seats clockwise of
  // the button: the seat immediately left of the button posts the small blind,
  // the next one the big blind. A rooted board is already past the blinds.
  if (root.board.empty()) {
    // Antes are dead money from every player, including folded-out seats. They
    // are contributed but never street-committed.
    //
    // `all_in` is re-derived here exactly as the blind loop and `pay` do it. An
    // ante that empties a stack must leave the seat unable to act: without this
    // the seat stays pending, `next_actor` can select it, and `legal()` offers a
    // zero-stack seat a free check. The omission was unreachable only because
    // the v1 validator rejects every non-zero ante before a state is built, so
    // it never appeared in production or in a test.
    if (root.ante > 0) {
      for (std::size_t p = 0; p < root.players; ++p) {
        const Chips posted = std::min(root.ante, players_[p].stack);
        players_[p].stack -= posted;
        players_[p].contributed = add(players_[p].contributed, posted);
        players_[p].all_in = players_[p].stack == 0;
      }
    }
    struct Blind {
      std::size_t seat;
      Chips amount;
    };
    const std::array<Blind, 2> blinds{{{clockwise(root.players, root.button), small_blind},
                                       {clockwise(root.players, root.button, 2), root.big_blind}}};
    for (const Blind& blind : blinds) {
      const Chips posted = std::min(blind.amount, players_[blind.seat].stack);
      players_[blind.seat].stack -= posted;
      players_[blind.seat].street_committed = add(players_[blind.seat].street_committed, posted);
      players_[blind.seat].contributed = add(players_[blind.seat].contributed, posted);
      players_[blind.seat].all_in = players_[blind.seat].stack == 0;
    }
    street_ = Street::Preflop;
    last_full_raise_ = root.big_blind;
    // Preflop action starts LEFT OF THE BIG BLIND, i.e. the seat after the big
    // blind, which is the button when only three seats are dealt.
    const std::size_t opener = clockwise(root.players, root.button, 3);
    // Everyone who can act still owes an action, including the blinds.
    for (MultiwayPlayer& player : players_)
      player.pending = !player.folded && !player.all_in;
    actor_ = opener;
    // If the opener cannot act (all in), advance to the next seat that can.
    if (players_[actor_].all_in) {
      const auto next = next_actor(actor_);
      if (next) {
        actor_ = *next;
      } else {
        close_street();
        return;
      }
    }
  } else {
    // A rooted board opens a postflop street with no blinds to post.
    const std::size_t dealt = root.board.size();
    street_ = dealt == 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
    board_ = root.board;
    last_full_raise_ = root.big_blind;
    // Postflop action starts LEFT OF THE BUTTON, skipping players who cannot
    // act (folded or all in).
    for (MultiwayPlayer& player : players_)
      player.pending = !player.folded && !player.all_in;
    // `next_actor(from)` starts looking at the seat AFTER `from`, so passing
    // the button itself yields the seat immediately clockwise of it.
    const auto next = next_actor(root.button);
    if (!next) {
      // Nobody can act: the board runs out to a showdown.
      phase_ = street_ == Street::River ? Phase::Showdown : Phase::Deal;
      return;
    }
    actor_ = *next;
  }

  // The dead contribution must cover the blinds and antes actually posted; any
  // excess is rejected rather than silently dropped.
  for (std::size_t p = 0; p < root.players; ++p)
    require(players_[p].contributed >= root.contributions[p],
            "posted blinds and ante are below the declared contribution");

  const Chips total = pot();
  for (const MultiwayPlayer& player : players_)
    require(add(total, player.stack) <= kMaxHeadsUpChips, "root exceeds exact numeric profile");
}

std::optional<std::size_t> MultiwayState::actor() const {
  if (phase_ != Phase::Action)
    return std::nullopt;
  return actor_;
}

Chips MultiwayState::pot() const {
  Chips total = 0;
  for (const MultiwayPlayer& player : players_)
    total = add(total, player.net_contributed());
  return total;
}

bool MultiwayState::can_raise(std::size_t player) const {
  require(player < players_.size(), "player outside the seated range");
  return players_[player].raise_rights && !players_[player].all_in;
}

std::vector<std::size_t> MultiwayState::live_players() const {
  std::vector<std::size_t> live;
  for (std::size_t p = 0; p < players_.size(); ++p)
    if (!players_[p].folded)
      live.push_back(p);
  return live;
}

bool MultiwayState::any_pending() const {
  for (const MultiwayPlayer& player : players_)
    if (player.pending && !player.folded && !player.all_in)
      return true;
  return false;
}

std::optional<std::size_t> MultiwayState::next_actor(std::size_t from) const {
  const std::size_t count = players_.size();
  for (std::size_t step = 1; step <= count; ++step) {
    const std::size_t seat = clockwise(count, from, step);
    const MultiwayPlayer& player = players_[seat];
    if (player.pending && !player.folded && !player.all_in)
      return seat;
  }
  return std::nullopt;
}

MultiwayLegal MultiwayState::legal() const {
  MultiwayLegal result;
  if (phase_ != Phase::Action)
    return result;
  const MultiwayPlayer& hero = players_[actor_];
  const Chips high = [&] {
    Chips value = 0;
    for (const MultiwayPlayer& player : players_)
      value = std::max(value, player.street_committed);
    return value;
  }();
  const Chips due = high - hero.street_committed;
  result.fold = due > 0;
  result.check = due == 0;
  result.call = due > 0;
  result.call_amount = std::min(due, hero.stack);
  // A player facing a wager may re-raise only with raise rights, and only when
  // somebody else can still respond; a player with no chips behind cannot raise.
  if (!hero.raise_rights || hero.stack <= due)
    return result;
  const bool opponent_can_respond = [&] {
    for (std::size_t p = 0; p < players_.size(); ++p) {
      if (p == actor_ || players_[p].folded)
        continue;
      if (!players_[p].all_in)
        return true;
    }
    return false;
  }();
  if (!opponent_can_respond)
    return result;

  const Chips maximum = add(hero.street_committed, hero.stack);
  const Chips full_minimum = add(high, last_full_raise_);
  // A short all-in that cannot reach a full raise is still a legal aggressive
  // action; the interval collapses onto the stack.
  result.aggressive = TargetRange{
      due == 0 ? ActionType::Bet : ActionType::Raise,
      std::min(full_minimum, maximum),
      maximum,
      maximum <= full_minimum,
  };
  return result;
}

void MultiwayState::pay(std::size_t player, Chips amount) {
  MultiwayPlayer& chips = players_[player];
  require(amount <= chips.stack, "payment exceeds stack");
  chips.stack -= amount;
  chips.street_committed = add(chips.street_committed, amount);
  chips.contributed = add(chips.contributed, amount);
  chips.all_in = chips.stack == 0;
}

void MultiwayState::refund_unmatched() {
  // A single highest STREET commitment is returned only down to the best
  // commitment any OTHER player actually made. A folded player's chips stay in
  // the pot as dead money, so they still count as a level somebody reached:
  // folding does not create an uncalled excess for the winner.
  //
  // Only the highest contributor can hold an unmatched amount, and only when it
  // is strictly above every other player's street commitment. Distinct levels
  // below that are handled by settlement's pot layering, not here.
  std::size_t high = 0;
  Chips high_amount = 0;
  std::size_t high_count = 0;
  for (std::size_t p = 0; p < players_.size(); ++p) {
    // A folded player keeps no live commitment to refund, but its contribution
    // still establishes a level; settlement handles it.
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
  for (std::size_t p = 0; p < players_.size(); ++p) {
    if (p == high)
      continue;
    // Every other player's commitment counts, folded or not: it is a level the
    // pot already reached.
    other_level = std::max(other_level, players_[p].street_committed);
  }
  const Chips excess = high_amount - other_level;
  if (excess == 0)
    return;
  players_[high].stack = add(players_[high].stack, excess);
  players_[high].refunded = add(players_[high].refunded, excess);
  players_[high].street_committed -= excess;
  players_[high].all_in = players_[high].stack == 0;
}

void MultiwayState::close_street() {
  refund_unmatched();
  for (MultiwayPlayer& player : players_)
    player.pending = false;
  phase_ = street_ == Street::River ? Phase::Showdown : Phase::Deal;
}

MultiwayState MultiwayState::after_action(std::size_t player, MultiwayAction action) const {
  require(phase_ == Phase::Action && player == actor_, "not the acting player");
  require(legal().contains(action), "illegal action or target total");
  MultiwayState next = *this;
  MultiwayPlayer& hero = next.players_[player];
  const Chips high = [&] {
    Chips value = 0;
    for (const MultiwayPlayer& other : next.players_)
      value = std::max(value, other.street_committed);
    return value;
  }();

  hero.pending = false;
  hero.raise_rights = false;
  Chips paid = 0;
  switch (action.type) {
    case ActionType::Fold:
      hero.folded = true;
      break;
    case ActionType::Check:
      break;
    case ActionType::Call:
      paid = std::min(high - hero.street_committed, hero.stack);
      next.pay(player, paid);
      break;
    case ActionType::Bet:
    case ActionType::Raise: {
      paid = action.target_total - hero.street_committed;
      const Chips increment = action.target_total - high;
      next.pay(player, paid);
      // Every other live player who can still act owes a response.
      for (std::size_t p = 0; p < next.players_.size(); ++p) {
        if (p == player || next.players_[p].folded || next.players_[p].all_in)
          continue;
        next.players_[p].pending = true;
      }
      // The provider-independent reopening rule: only a FULL raise restores
      // raise rights for players who already acted. A short all-in does not,
      // and cumulative short all-ins never do.
      if (increment >= next.last_full_raise_) {
        next.last_full_raise_ = increment;
        for (std::size_t p = 0; p < next.players_.size(); ++p) {
          if (p == player || next.players_[p].folded || next.players_[p].all_in)
            continue;
          next.players_[p].raise_rights = true;
        }
      }
      break;
    }
  }
  next.history_.push_back({street_, player, Action{action.type, action.target_total}, paid});

  if (next.phase_ != Phase::Action)
    return next;
  // A single remaining live player ends the hand immediately.
  if (next.live_players().size() <= 1) {
    next.phase_ = Phase::Folded;
    next.refund_unmatched();
    for (MultiwayPlayer& other : next.players_)
      other.pending = false;
    return next;
  }
  if (!next.any_pending()) {
    next.close_street();
    return next;
  }
  const auto following = next.next_actor(player);
  if (!following) {
    next.close_street();
    return next;
  }
  next.actor_ = *following;
  return next;
}

MultiwayState MultiwayState::after_card(int card) const {
  require(phase_ == Phase::Deal, "no public card pending");
  std::array<bool, 52> used{};
  for (int public_card : board_)
    use_card(public_card, used);
  use_card(card, used);
  MultiwayState next = *this;
  next.board_.push_back(card);
  const std::size_t dealt = next.board_.size();
  next.street_ = dealt <= 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
  if (dealt < 3)
    return next;  // the flop is still being filled
  for (MultiwayPlayer& player : next.players_) {
    player.street_committed = 0;
    player.pending = !player.folded && !player.all_in;
    player.raise_rights = true;
  }
  next.last_full_raise_ = root_.big_blind;
  // Postflop order starts left of the button; `next_actor` begins at the seat
  // after its argument.
  const auto opener = next.next_actor(root_.button);
  if (!opener) {
    next.phase_ = next.street_ == Street::River ? Phase::Showdown : Phase::Deal;
    return next;
  }
  next.phase_ = Phase::Action;
  next.actor_ = *opener;
  return next;
}

ContributionSettlement MultiwayState::award(const std::vector<std::uint32_t>* scores) const {
  SettlementInput input;
  input.seat_count = players_.size();
  input.button = root_.button;
  input.odd_chip_rule = OddChipRule::ClockwiseLeftOfButton;
  input.rake = {RakeRule::PotPercentageFloor, 0, 0, true};
  input.flop_dealt = board_.size() >= 3;
  for (std::size_t p = 0; p < players_.size(); ++p) {
    PlayerChips chips;
    chips.stack = players_[p].stack;
    chips.street_committed = players_[p].street_committed;
    chips.contributed = players_[p].contributed;
    chips.refunded = players_[p].refunded;
    chips.folded = players_[p].folded;
    std::optional<std::uint32_t> score;
    if (scores != nullptr && !players_[p].folded)
      score = (*scores)[p];
    input.players.push_back({p, chips, score});
  }
  return settle_contributions(input);
}

ContributionSettlement MultiwayState::settle_fold() const {
  require(phase_ == Phase::Folded, "hand has not ended by folding");
  require(live_players().size() == 1, "fold settlement requires a single live player");
  return award(nullptr);
}

ContributionSettlement MultiwayState::settle_showdown(
    const std::vector<std::array<int, 2>>& hole_cards) const {
  require(phase_ == Phase::Showdown && board_.size() == 5, "showdown is not ready");
  const std::vector<std::size_t> live = live_players();
  require(hole_cards.size() == live.size(), "one hole-card pair per live player required");
  std::array<bool, 52> used{};
  for (int card : board_)
    use_card(card, used);
  std::vector<std::uint32_t> scores(players_.size(), 0);
  for (std::size_t i = 0; i < live.size(); ++i) {
    for (int card : hole_cards[i])
      use_card(card, used);
    std::array<int, 7> cards{};
    std::copy(board_.begin(), board_.end(), cards.begin());
    std::copy(hole_cards[i].begin(), hole_cards[i].end(), cards.begin() + 5);
    scores[live[i]] = bs::evaluate(cards.data(), static_cast<int>(cards.size())).score;
  }
  return award(&scores);
}

}  // namespace bs::poker
