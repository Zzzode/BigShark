// Public postflop rules for the offline heads-up trainer.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bs::poker {

using Chips = std::uint64_t;
inline constexpr Chips kMaxHeadsUpChips = (Chips{1} << 53) - 1;

enum class Street { Flop, Turn, River };
enum class Phase { Action, Deal, Showdown, Folded };
enum class ActionType { Fold, Check, Call, Bet, Raise };

struct Action {
  ActionType type;
  // Only bets and raises carry a target total for the current street.
  Chips target_total = 0;
  bool operator==(const Action&) const = default;
};

struct TargetRange {
  ActionType type;
  Chips minimum;
  Chips maximum;
  bool all_in_only;
};

struct LegalActions {
  bool fold = false;
  bool check = false;
  bool call = false;
  Chips call_amount = 0;
  std::optional<TargetRange> aggressive;
  bool contains(Action action) const;
};

struct HeadsUpRoot {
  std::array<int, 3> flop;
  std::array<Chips, 2> stacks;
  // Closed-street contributions must be equal in this no-ante, two-player profile.
  std::array<Chips, 2> contributions;
  Chips pot;
  Chips big_blind;
  std::size_t button;
};

struct PlayerChips {
  Chips stack = 0;
  Chips street_committed = 0;
  Chips contributed = 0;  // Gross contributions, including the root pot.
  Chips refunded = 0;
  bool folded = false;
  Chips net_contributed() const { return contributed - refunded; }
};

struct BettingEvent {
  Street street;
  std::size_t actor;
  Action action;
  Chips paid;
};

struct Settlement {
  std::array<Chips, 2> awards{};
  std::array<Chips, 2> refunds{};
  std::array<Chips, 2> final_stacks{};
  std::array<std::int64_t, 2> net_utility{};
};

class HeadsUpState {
 public:
  // Invalid roots/actions/cards throw std::invalid_argument; checked amount
  // overflow throws std::overflow_error. All transitions leave the source unchanged.
  explicit HeadsUpState(const HeadsUpRoot& root);

  const HeadsUpRoot& root() const { return root_; }
  const std::array<PlayerChips, 2>& players() const { return players_; }
  const std::vector<int>& board() const { return board_; }
  const std::vector<BettingEvent>& history() const { return history_; }
  Street street() const { return street_; }
  Phase phase() const { return phase_; }
  std::optional<std::size_t> actor() const;
  Chips pot() const;
  Chips last_full_raise() const { return last_full_raise_; }
  bool can_raise(std::size_t player) const;
  LegalActions legal() const;

  HeadsUpState after_action(std::size_t player, Action action) const;
  // Public-card uniqueness is checked here. Training must additionally exclude
  // both sampled private hands from chance; they are intentionally not stored.
  HeadsUpState after_card(int card) const;

  Settlement settle_fold() const;
  Settlement settle_showdown(const std::array<std::array<int, 2>, 2>& hole_cards) const;

 private:
  void pay(std::size_t player, Chips amount);
  void close_street();
  void refund_unmatched();
  Settlement award(std::optional<std::size_t> winner) const;

  HeadsUpRoot root_;
  std::array<PlayerChips, 2> players_{};
  std::vector<int> board_;
  std::vector<BettingEvent> history_;
  Street street_ = Street::Flop;
  Phase phase_ = Phase::Action;
  std::size_t actor_ = 0;
  Chips last_full_raise_ = 0;
  std::array<bool, 2> pending_{true, true};
  std::array<bool, 2> raise_rights_{true, true};
};

}  // namespace bs::poker
