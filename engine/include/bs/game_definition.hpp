// Unified N-seat game definition (RFC 0008 §L1).
//
// One game definition for 2..10 seats with ONE legal-transition implementation,
// replacing the two parallel rule types (`HeadsUpState` in bs/heads_up.hpp and
// `MultiwayState` in bs/multiway.hpp). This stage constructs two seats only;
// the capacity, the field set, and the identity surface are fixed now so the
// seat-count stage widens the same type instead of re-typing it.
//
// Design constraints, each forced by something measured rather than chosen:
//
//  * FIXED CAPACITY, NO HEAP. `engine/tests/test_heads_up_allocations.cpp`
//    asserts an exact `::operator new` ordinal under fault injection, and a
//    per-node allocation would move it. Every member here is an array or a
//    scalar; the type is trivially copyable.
//
//  * NO HISTORY. History is an observation log, not transition state: neither
//    existing implementation ever READS its own `history_`, and its bound is
//    not provable (a full-raise ladder doubles `last_full_raise_` until the
//    chip cap, so a fixed inline array would need ~400 entries). The
//    `HeadsUpState` adapter keeps owning and exposing its own vector, exactly
//    as it does today; the seat-count stage revisits this when the multiway
//    type unifies. `board` stays here because transitions DO read it.
//
//  * ONE ALL-IN FIELD PER SEAT. The multiway representation
//    (`all_in == stack == 0`, re-derived on every chip movement) is adopted
//    because 10 seats need a general form and because the maintained alternative
//    was measured to be observationally identical across 1,990,808 reachable
//    nodes. The rule that makes it total is stated at `refund_unmatched`.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bs::poker {

inline constexpr std::size_t kMinUnifiedSeats = 2;
inline constexpr std::size_t kMaxUnifiedSeats = 10;

// The declared rules variant. One value today; it is part of the definition
// because RFC 0008 §L1 names it, and adding it later would move a frozen
// identity surface.
enum class RulesVariant { NoLimitHoldem };

// RFC 0007's terminal-depth rule. A game that ends when a completed flop would
// open action is a DIFFERENT GAME, not a different tree: the depth selects the
// settlement path. It therefore belongs to the definition rather than to the
// abstraction above it (RFC 0008 §L1). Only `River` is accepted in this stage;
// `Flop` is declared so the identity surface does not grow a field later.
enum class TerminalDepth { River, Flop };

// A hand's complete declared description.
//
// Field order is deliberate. Four sites construct `HeadsUpRoot` with positional
// aggregate initialization, and `apps/engine-host/main.cpp` hashes the RAW BYTES
// of a root into a deterministic resolver seed, so the adapter that maps this
// definition onto a `HeadsUpRoot` must reproduce its values exactly. This
// struct is not the thing that is byte-hashed; it is the source the adapter
// copies FROM.
struct GameDef {
  std::size_t player_count = 0;
  std::size_t button = 0;
  Chips big_blind = 0;

  // Dead money posted by every seat before the blinds. Always 0 in the
  // two-seat profile, which has no ante; the field exists because the
  // seat-count stage needs it and because `MultiwayRoot` already carries it.
  Chips ante = 0;

  // Chips behind, BEFORE any blind or ante is posted.
  std::array<Chips, kMaxUnifiedSeats> stacks{};
  // Dead money already contributed at the root, excluding the posted blinds.
  std::array<Chips, kMaxUnifiedSeats> contributions{};

  // The root pot the definition declares. Its exact contract depends on the
  // root shape, and the constructor validates it rather than trusting it:
  //   - preflop root:  contributions[0..n) must all be equal and their sum
  //                    must be <= pot (the pot covers the posted blinds);
  //   - rooted board:  contributions[0..n) must all be equal and their sum
  //                    must EQUAL pot (the board implies the blinds completed).
  // `GameState::pot()` never returns this value; it sums live contributions,
  // which is the formula both existing types already share.
  Chips pot = 0;

  // Public cards at the root, as an ordered prefix. Empty starts preflop; three
  // starts on the flop. The multiway type also admits four and five; the
  // two-seat profile admits only zero or three, and the constructor rejects
  // anything else.
  std::array<int, 5> board{};
  std::uint8_t board_size = 0;

  // Blinds actually posted, indexed by seat. Zero means "not posted", which is
  // unambiguous because `big_blind > 0` forces every posted blind above zero.
  // Declared rather than derived: the two-seat profile posts the small blind
  // from the BUTTON, which is the heads-up convention and not the "seat left of
  // the button" rule that 3+ handed games use.
  std::array<Chips, kMaxUnifiedSeats> blinds_posted{};

  // True when this definition is the preflop root: blinds posted, no board, and
  // the seat left of the big blind acts first. False is the rooted-board
  // profile, where the seat left of the button acts first and no blinds are
  // posted. This mirrors `HeadsUpRoot::preflop` exactly.
  bool preflop = false;

  RulesVariant variant = RulesVariant::NoLimitHoldem;
  TerminalDepth terminal = TerminalDepth::River;
};

// Validates a definition without building a state, so identity and persistence
// sites can ask whether a definition is well formed. Throws
// `std::invalid_argument` for a malformed definition and `std::overflow_error`
// for a checked chip sum that leaves the exact numeric profile.
void validate(const GameDef& def);

// The ONE root/game identity predicate. Replaces four implementations that
// disagreed on their field sets: `heads_up_solver.cpp` compared all eight root
// fields, while `resident_policy.cpp`, `counterfactual_reach.cpp` and
// `strategy_artifact.cpp` each compared a different subset. Compares every
// field that changes the game. Storage that is NOT root identity (range
// weights, sizing schedule, fixed runout) is composed on top by its owner.
bool same_game_def(const GameDef& a, const GameDef& b);

// One seat's chip and action state.
//
// `PlayerChips` in the heads-up header is this record's strict prefix at the
// same offsets, which is why the adapter's projection is a field copy and why
// this struct is exactly one padded word group.
struct GamePlayer {
  Chips stack = 0;
  Chips street_committed = 0;
  Chips contributed = 0;
  Chips refunded = 0;
  bool folded = false;
  bool all_in = false;
  // Whether this seat may still raise. Cleared by acting on a wager and
  // restored only by a FULL raise; a short all-in never restores it.
  bool raise_rights = true;
  // Whether this seat still owes an action on the current bet.
  bool pending = true;
  Chips net_contributed() const { return contributed - refunded; }
};
static_assert(sizeof(GamePlayer) == 40,
              "the unified player record must stay one padded word group, so the "
              "HeadsUpState adapter's projection costs no size");

class GameState {
 public:
  // `validate(def)` first, then the game-specific construction rules: posts
  // blinds and antes, derives the initial actor and pending set, and closes the
  // street when nobody can act. Throws `std::invalid_argument` for a malformed
  // definition, `std::overflow_error` for a chip sum that leaves the exact
  // profile, and rejects a seat count outside [kMinUnifiedSeats, 2] until the
  // seat-count stage widens the bound.
  explicit GameState(const GameDef& def);

  const GameDef& def() const { return def_; }
  std::size_t player_count() const { return def_.player_count; }
  const std::array<GamePlayer, kMaxUnifiedSeats>& players() const { return players_; }
  std::span<const int> board() const { return {board_.data(), board_size_}; }
  Street street() const { return street_; }
  Phase phase() const { return phase_; }
  // The acting seat, or nullopt unless the phase is Action.
  std::optional<std::size_t> actor() const;
  // Sum of live contributions. Never `def().pot`.
  Chips pot() const;
  Chips last_full_raise() const { return last_full_raise_; }
  bool can_raise(std::size_t player) const;
  LegalActions legal() const;
  // Seats that have not folded, in seat order.
  std::span<const std::size_t> live_players() const { return {live_.data(), live_size_}; }
  bool all_in(std::size_t player) const { return players_[player].all_in; }

  // Legal transition for the acting seat. Returns a new state by value; no
  // allocation, and every precondition is checked BEFORE the copy so a throwing
  // call performs no work.
  GameState after_action(std::size_t player, Action action) const;
  // Deals one public card. Advances the street with the FIRST card of that
  // street, so a partially dealt flop is already `Street::Flop`.
  GameState after_card(int card) const;

  // Terminal settlement, generalized to any seat count. Round-trips through
  // `settle_contributions`, so for two seats it is element-for-element
  // identical to what `HeadsUpState::award` computes today.
  //
  // `holes` is indexed by LIVE SEAT IN ASCENDING ORDER, which is the multiway
  // convention; the two-seat adapter packs its seat-ordered pair into it.
  ContributionSettlement settle_fold() const;
  ContributionSettlement settle_showdown(std::span<const std::array<int, 2>> holes) const;

 private:
  void require_seat(std::size_t player, const char* message) const;
  void pay(std::size_t player, Chips amount);
  // Returns an unmatched excess only down to the best level any OTHER seat
  // reached, counting folded seats' commitments as levels the pot already
  // reached. Deliberately does NOT clear a standing `all_in`: a refund restores
  // an amount nobody matched, and a seat that was all in had no such amount, so
  // a refund can never make an all-in seat able to act again.
  void refund_unmatched();
  void close_street();
  // First seat clockwise from `from` (exclusive) that still owes an action.
  std::optional<std::size_t> next_actor(std::size_t from) const;
  bool any_pending() const;
  // Seats that are neither folded nor all in. Betting needs at least two of
  // them; with fewer the board runs out instead of opening a round nobody can
  // contest, which is the general form of the heads-up all-in rule.
  std::size_t actionable_count() const;
  void refresh_live();
  ContributionSettlement award(std::span<const std::optional<std::uint32_t>> scores) const;

  GameDef def_;
  std::array<GamePlayer, kMaxUnifiedSeats> players_{};
  std::array<int, 5> board_{};
  std::array<std::size_t, kMaxUnifiedSeats> live_{};
  std::size_t actor_ = 0;
  std::uint8_t board_size_ = 0;
  std::uint8_t live_size_ = 0;
  Street street_ = Street::Flop;
  Phase phase_ = Phase::Action;
  Chips last_full_raise_ = 0;
};

}  // namespace bs::poker
