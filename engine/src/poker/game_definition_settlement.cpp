// Unified N-seat terminal settlement for `GameState` (RFC 0008 §L1).
//
// This is the one settlement path both rule profiles share: it generalizes
// `HeadsUpState::award` and `MultiwayState::award` to the fixed seat array, and
// for two seats returns element-for-element what both compute today. Scoring
// stays outside: callers hand in one optional score per SEAT, and seats that
// did not reach the showdown carry no score, so settlement never reads one.

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "poker_detail.hpp"

namespace bs::poker {
namespace {

using detail::require;
using detail::use_card;

}  // namespace

ContributionSettlement GameState::award(
    std::span<const std::optional<std::uint32_t>> scores) const {
  require(scores.size() == def_.player_count, "one score slot per seat required");
  SettlementInput input;
  input.seat_count = def_.player_count;
  input.button = def_.button;
  input.odd_chip_rule = OddChipRule::ClockwiseLeftOfButton;
  input.rake = {RakeRule::PotPercentageFloor, 0, 0, true};
  input.flop_dealt = board_size_ >= 3;
  for (std::size_t p = 0; p < def_.player_count; ++p) {
    PlayerChips chips;
    chips.stack = players_[p].stack;
    chips.street_committed = players_[p].street_committed;
    chips.contributed = players_[p].contributed;
    chips.refunded = players_[p].refunded;
    chips.folded = players_[p].folded;
    input.players.push_back({p, chips, scores[p]});
  }
  return settle_contributions(input);
}

ContributionSettlement GameState::settle_fold() const {
  require(phase_ == Phase::Folded, "hand has not ended by folding");
  require(live_size_ == 1, "fold settlement requires a single live player");
  // Settlement consults a score only for a contested layer, and one live seat
  // can never contest a layer, so no folded seat needs one.
  std::vector<std::optional<std::uint32_t>> scores(def_.player_count);
  scores[live_[0]] = 1U;
  return award(scores);
}

ContributionSettlement GameState::settle_showdown(std::span<const std::array<int, 2>> holes) const {
  require(phase_ == Phase::Showdown && board_size_ == 5, "showdown is not ready");
  require(holes.size() == live_size_, "one hole-card pair per live player required");
  std::array<bool, 52> used{};
  for (int card : board_)
    use_card(card, used);
  // `holes` is packed by live position while pot layers are built over seats;
  // reading holes[i] and writing scores[live_[i]] is what maps one onto the
  // other, so neither the deck check nor the write may use i directly.
  std::vector<std::optional<std::uint32_t>> scores(def_.player_count);
  for (std::size_t i = 0; i < live_size_; ++i) {
    for (int card : holes[i])
      use_card(card, used);
    std::array<int, 7> cards{};
    std::copy(board_.begin(), board_.end(), cards.begin());
    std::copy(holes[i].begin(), holes[i].end(), cards.begin() + 5);
    scores[live_[i]] = bs::evaluate(cards.data(), static_cast<int>(cards.size())).score;
  }
  return award(scores);
}

}  // namespace bs::poker
