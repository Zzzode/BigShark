// Public multiway (3..6 player) no-limit Hold'em rules for the RFC 0006
// experimental multiplayer profile. This is a sibling of the heads-up rules,
// not a generalization of them: the heads-up state keeps its exact contract and
// its flop-rooted and preflop profiles are untouched.
//
// The contract follows RFC 0006 "Multiway training and range semantics":
// player-specific stacks, raise rights, observations, and full public history
// are retained; preflop action starts left of the big blind; postflop action
// starts left of the button skipping players who cannot act; and the
// provider-independent full-raise reopening rule decides each player's raise
// rights, including cumulative short all-ins.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "bs/heads_up.hpp"
#include "bs/settlement.hpp"

namespace bs::poker {

inline constexpr std::size_t kMinMultiwayPlayers = 3;
inline constexpr std::size_t kMaxMultiwayPlayers = 6;

// One dealt multiway hand. `button` seats the dealer; blinds are posted from
// the seats clockwise of the button. `stacks` are the chips behind BEFORE the
// blinds are posted, and `contributions` is dead money already in the pot
// (zero for a fresh hand).
struct MultiwayRoot {
  std::size_t players = 0;
  std::size_t button = 0;
  Chips big_blind = 0;
  // Ante posted by every player, in addition to the blinds. Zero when the
  // profile is a no-ante game.
  Chips ante = 0;
  std::vector<Chips> stacks;
  std::vector<Chips> contributions;
  // Cards on the board at the root. Empty starts preflop; three starts on the
  // flop, four on the turn, five on the river. A non-empty board must carry
  // distinct, valid card ids.
  std::vector<int> board;
};

struct MultiwayPlayer {
  Chips stack = 0;
  Chips street_committed = 0;
  Chips contributed = 0;
  Chips refunded = 0;
  bool folded = false;
  bool all_in = false;
  // Whether this player may still raise. Cleared by a full raise or by acting
  // on a wager, and restored only by a full raise (never by a short all-in).
  bool raise_rights = true;
  // Whether this player still owes an action on the current bet.
  bool pending = true;
  Chips net_contributed() const { return contributed - refunded; }
};

// RFC 0008 measured these two as field-identical to `Action` and
// `LegalActions`, with a byte-identical `contains` body. They are aliases now,
// not parallel declarations: a multiway action IS an action, and the multiway
// legal set IS the legal set. The names are kept so call sites read in the
// multiway vocabulary and so the eventual unified game definition can retire
// them deliberately rather than by accident.
using MultiwayAction = Action;
using MultiwayLegal = LegalActions;

class MultiwayState {
 public:
  // Invalid roots, actions, or cards throw std::invalid_argument; checked
  // amount overflow throws std::overflow_error. All transitions leave the
  // source unchanged.
  explicit MultiwayState(const MultiwayRoot& root);

  const MultiwayRoot& root() const { return root_; }
  const std::vector<MultiwayPlayer>& players() const { return players_; }
  const std::vector<int>& board() const { return board_; }
  const std::vector<BettingEvent>& history() const { return history_; }
  Street street() const { return street_; }
  Phase phase() const { return phase_; }
  std::optional<std::size_t> actor() const;
  Chips pot() const;
  Chips last_full_raise() const { return last_full_raise_; }
  bool can_raise(std::size_t player) const;
  MultiwayLegal legal() const;
  // Players who have not folded, in seat order. Used by settlement and by the
  // showdown evaluator.
  std::vector<std::size_t> live_players() const;

  MultiwayState after_action(std::size_t player, MultiwayAction action) const;
  // Public-card uniqueness is checked here. Training must additionally exclude
  // every sampled private hand from chance.
  MultiwayState after_card(int card) const;

  // Exact chip settlement, returning the general contribution-layer ledger
  // (the fixed two-seat `Settlement` type does not fit 3..6 players). Fold
  // requires every other player to have folded; otherwise use settle_showdown
  // with exactly the live players' hole cards, in ascending seat order.
  ContributionSettlement settle_fold() const;
  ContributionSettlement settle_showdown(const std::vector<std::array<int, 2>>& hole_cards) const;

 private:
  void pay(std::size_t player, Chips amount);
  void close_street();
  void refund_unmatched();
  // First seat clockwise from `from` that still owes an action, or nullopt.
  std::optional<std::size_t> next_actor(std::size_t from) const;
  bool any_pending() const;
  ContributionSettlement award(const std::vector<std::uint32_t>* scores) const;

  MultiwayRoot root_;
  std::vector<MultiwayPlayer> players_;
  std::vector<int> board_;
  std::vector<BettingEvent> history_;
  Street street_ = Street::Preflop;
  Phase phase_ = Phase::Action;
  std::size_t actor_ = 0;
  Chips last_full_raise_ = 0;
};

}  // namespace bs::poker
