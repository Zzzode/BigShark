#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace bs::poker {
namespace {

Chips add(Chips left, Chips right) {
  if (right > std::numeric_limits<Chips>::max() - left)
    throw std::overflow_error("chip sum overflow");
  return left + right;
}

void require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

void use_card(int card, std::array<bool, 52>& used) {
  require(card >= 0 && card < 52, "card ID outside deck");
  require(!used[card], "duplicate card");
  used[card] = true;
}

}  // namespace

bool LegalActions::contains(Action action) const {
  switch (action.type) {
    case ActionType::Fold:
      return fold && action.target_total == 0;
    case ActionType::Check:
      return check && action.target_total == 0;
    case ActionType::Call:
      return call && action.target_total == 0;
    case ActionType::Bet:
    case ActionType::Raise:
      return aggressive && action.type == aggressive->type &&
             action.target_total >= aggressive->minimum &&
             action.target_total <= aggressive->maximum;
  }
  return false;
}

HeadsUpState::HeadsUpState(const HeadsUpRoot& root) : root_(root) {
  require(root.button < 2, "button outside heads-up seats");
  require(root.big_blind > 0 && root.big_blind <= kMaxHeadsUpChips, "invalid big blind");
  require(root.pot > 0, "empty root pot");
  require(root.contributions[0] == root.contributions[1],
          "unmatched contributions at closed-street root");
  require(add(root.contributions[0], root.contributions[1]) == root.pot,
          "root pot differs from contributions");
  const Chips total = add(add(root.pot, root.stacks[0]), root.stacks[1]);
  require(total <= kMaxHeadsUpChips, "root exceeds exact numeric profile");

  std::array<bool, 52> used{};
  for (int card : root.flop)
    use_card(card, used);
  board_.assign(root.flop.begin(), root.flop.end());
  for (std::size_t p = 0; p < 2; ++p) {
    players_[p].stack = root.stacks[p];
    players_[p].contributed = root.contributions[p];
  }
  actor_ = 1 - root.button;
  last_full_raise_ = root.big_blind;
  if (root.stacks[0] == 0 || root.stacks[1] == 0)
    close_street();
}

std::optional<std::size_t> HeadsUpState::actor() const {
  if (phase_ != Phase::Action)
    return std::nullopt;
  return actor_;
}

Chips HeadsUpState::pot() const {
  return add(players_[0].net_contributed(), players_[1].net_contributed());
}

bool HeadsUpState::can_raise(std::size_t player) const {
  require(player < 2, "player outside heads-up seats");
  return raise_rights_[player];
}

LegalActions HeadsUpState::legal() const {
  LegalActions result;
  if (phase_ != Phase::Action)
    return result;
  const auto& hero = players_[actor_];
  const auto& opponent = players_[1 - actor_];
  const Chips high = std::max(hero.street_committed, opponent.street_committed);
  const Chips due = high - hero.street_committed;
  result.fold = due > 0;
  result.check = due == 0;
  result.call = due > 0;
  result.call_amount = std::min(due, hero.stack);
  // An opponent who cannot respond cannot contest any additional raise.
  if (!raise_rights_[actor_] || opponent.stack == 0 || hero.stack <= due)
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

void HeadsUpState::pay(std::size_t player, Chips amount) {
  auto& chips = players_[player];
  require(amount <= chips.stack, "payment exceeds stack");
  chips.stack -= amount;
  chips.street_committed = add(chips.street_committed, amount);
  chips.contributed = add(chips.contributed, amount);
}

void HeadsUpState::refund_unmatched() {
  const std::size_t high = players_[0].street_committed > players_[1].street_committed ? 0 : 1;
  const Chips excess = players_[high].street_committed - players_[1 - high].street_committed;
  players_[high].stack = add(players_[high].stack, excess);
  players_[high].refunded = add(players_[high].refunded, excess);
  players_[high].street_committed -= excess;
}

void HeadsUpState::close_street() {
  refund_unmatched();
  pending_ = {false, false};
  raise_rights_ = {false, false};
  phase_ = street_ == Street::River ? Phase::Showdown : Phase::Deal;
}

HeadsUpState HeadsUpState::after_action(std::size_t player, Action action) const {
  require(phase_ == Phase::Action && player == actor_, "not the acting player");
  require(legal().contains(action), "illegal action or target total");
  HeadsUpState next = *this;
  auto& hero = next.players_[player];
  const std::size_t opponent = 1 - player;
  const Chips high = std::max(hero.street_committed, next.players_[opponent].street_committed);
  Chips paid = 0;

  next.pending_[player] = false;
  next.raise_rights_[player] = false;
  switch (action.type) {
    case ActionType::Fold:
      hero.folded = true;
      next.refund_unmatched();
      next.pending_ = {false, false};
      next.raise_rights_ = {false, false};
      next.phase_ = Phase::Folded;
      break;
    case ActionType::Check:
      break;
    case ActionType::Call:
      paid = std::min(high - hero.street_committed, hero.stack);
      next.pay(player, paid);
      next.close_street();
      break;
    case ActionType::Bet:
    case ActionType::Raise: {
      paid = action.target_total - hero.street_committed;
      const Chips increment = action.target_total - high;
      next.pay(player, paid);
      next.pending_[opponent] = true;
      if (increment >= next.last_full_raise_) {
        next.last_full_raise_ = increment;
        next.raise_rights_[opponent] = true;
      }
      break;
    }
  }
  next.history_.push_back({street_, player, action, paid});
  if (next.phase_ == Phase::Action) {
    if (!next.pending_[opponent])
      next.close_street();
    else
      next.actor_ = opponent;
  }
  return next;
}

HeadsUpState HeadsUpState::after_card(int card) const {
  require(phase_ == Phase::Deal, "no public card pending");
  std::array<bool, 52> used{};
  for (int public_card : board_)
    use_card(public_card, used);
  use_card(card, used);
  HeadsUpState next = *this;
  next.board_.push_back(card);
  next.street_ = next.board_.size() == 4 ? Street::Turn : Street::River;
  for (auto& player : next.players_)
    player.street_committed = 0;
  next.last_full_raise_ = root_.big_blind;
  if (next.players_[0].stack == 0 || next.players_[1].stack == 0) {
    next.phase_ = next.street_ == Street::River ? Phase::Showdown : Phase::Deal;
  } else {
    next.phase_ = Phase::Action;
    next.actor_ = 1 - root_.button;
    next.pending_ = {true, true};
    next.raise_rights_ = {true, true};
  }
  return next;
}

Settlement HeadsUpState::award(std::optional<std::size_t> winner) const {
  SettlementInput input;
  input.seat_count = 2;
  input.button = root_.button;
  input.odd_chip_rule = OddChipRule::ClockwiseLeftOfButton;
  input.rake = {RakeRule::PotPercentageFloor, 0, 0, true};
  input.flop_dealt = true;
  for (std::size_t p = 0; p < 2; ++p)
    input.players.push_back({p, players_[p], winner && *winner == p ? 1U : 0U});
  const auto settled = settle_contributions(input);
  Settlement result;
  for (std::size_t p = 0; p < 2; ++p) {
    result.awards[p] = settled.awards[p];
    result.refunds[p] = settled.refunds[p];
    result.final_stacks[p] = settled.final_stacks[p];
    result.net_utility[p] = settled.chip_utility[p];
  }
  return result;
}

Settlement HeadsUpState::settle_fold() const {
  require(phase_ == Phase::Folded, "hand has not ended by folding");
  return award(players_[0].folded ? 1 : 0);
}

Settlement HeadsUpState::settle_showdown(
    const std::array<std::array<int, 2>, 2>& hole_cards) const {
  require(phase_ == Phase::Showdown && board_.size() == 5, "showdown is not ready");
  std::array<bool, 52> used{};
  for (int card : board_)
    use_card(card, used);
  for (const auto& hand : hole_cards)
    for (int card : hand)
      use_card(card, used);
  std::array<std::uint32_t, 2> scores{};
  for (std::size_t p = 0; p < 2; ++p) {
    std::array<int, 7> cards{};
    std::copy(board_.begin(), board_.end(), cards.begin());
    std::copy(hole_cards[p].begin(), hole_cards[p].end(), cards.begin() + 5);
    scores[p] = bs::evaluate(cards.data(), static_cast<int>(cards.size())).score;
  }
  if (scores[0] == scores[1])
    return award(std::nullopt);
  return award(scores[0] > scores[1] ? 0 : 1);
}

}  // namespace bs::poker
