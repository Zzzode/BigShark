// Unified N-seat game definition (RFC 0008 §L1).
//
// One game definition for 2..10 seats with ONE legal-transition machine,
// replacing the two parallel rule types (`HeadsUpState` in bs/heads_up.hpp and
// `MultiwayState` in bs/multiway.hpp); they remain as adapters during the
// transition. Two seats are the heads-up rules profile and 3..10 seats are
// the multiway profile, selected by seat count inside the machine, not by a
// second type: four rules genuinely differ between the profiles (the blind
// seats, the all-in flag across a refund, the big-blind preflop option, and
// when a street closes) and each branch is pinned by an exhaustive equivalence
// oracle.
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
//  * ONE ALL-IN FIELD PER SEAT, AND THE TWO PROFILES DISAGREE ON REFUNDS.
//    At two seats `all_in`, once set by a capped blind, STAYS set when the
//    unmatched part is refunded; at 3+ seats it is re-derived from the stack,
//    which is the `MultiwayState` rule. Both are correct for their own
//    profile and the seat-count branch in the transition code selects each.
//    The two-seat form was found the hard way: re-deriving after a refund
//    revived a short big blind and opened a betting round the shipped rules
//    never enter (`engine/tests/test_heads_up_preflop.cpp:229`: "The board
//    runs out with no action at any street"). The multiway form is the
//    inverse rule and RFC 0008 §L1 records it: a refunded all-in seat in a
//    3+ game gets its unmatched chips AND its ability to act back.
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
inline constexpr std::size_t kMaxUnifiedSeats = kMaxContributionSeats;
static_assert(kMaxUnifiedSeats == 10, "RFC 0008 serves 2..10 seats");

// The declared rules variant. One value today; it is part of the definition
// because RFC 0008 §L1 names it, and adding it later would move a frozen
// identity surface.
enum class RulesVariant { NoLimitHoldem };

// RFC 0007's terminal-depth rule. A game that ends when a completed flop would
// open action is a DIFFERENT GAME, not a different tree: the depth selects the
// settlement path. It therefore belongs to the definition rather than to the
// abstraction above it (RFC 0008 §L1). `Flop` is accepted for heads-up
// preflop roots only; the frontier evaluator is a two-player contract.
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

  // The root pot the definition declares. The constructor validates it rather
  // than trusting it, and the exact contract is profile-specific:
  //   - two-seat preflop root:   contributions equal and their sum <= pot;
  //   - two-seat rooted board:   contributions equal and their sum == pot;
  //   - 3..10 seats:             pot must equal the dead money, antes, and
  //                              capped posted blinds exactly, and may be
  //                              zero (the multiway profile admits an empty pot).
  // `GameState::pot()` never returns this value; it sums live contributions,
  // which is the formula both existing types already share.
  Chips pot = 0;

  // Public cards at the root, as an ordered prefix. In the two-seat profile
  // an empty board means preflop and must be paired with `preflop == true`; a
  // rooted board carries exactly the flop. In the 3..10 profile an empty board
  // IS the preflop profile (there is no separate flag in `MultiwayRoot`), and
  // a rooted board may carry three, four, or five cards.
  std::array<int, 5> board{};
  std::uint8_t board_size = 0;

  // Blinds actually posted, indexed by seat. Zero means "not posted", which is
  // unambiguous because `big_blind > 0` forces every posted blind above zero.
  // Declared rather than derived: at TWO seats the blinds use the heads-up
  // convention (the BUTTON posts the small blind) while 3+ seats post them
  // clockwise of the button, and a two-seat big blind posts the full nominal
  // amount while a 3+ blind may be capped by its poster's stack after the ante.
  std::array<Chips, kMaxUnifiedSeats> blinds_posted{};

  // Selects the two-seat preflop shape explicitly: blinds posted, no board,
  // the button acts first, and the big blind keeps its option after a limp.
  // For 3..10 seats the profile is implied by board emptiness instead, matching
  // `MultiwayRoot`, which has no such flag.
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

// Blind seats and the preflop opener.
//
// Heads-up is NOT the three-handed rule with the seat count turned down. With
// two seats the BUTTON posts the small blind and the other seat posts the big
// blind, which is what lets the button act first preflop; with three or more
// the blinds sit clockwise of the button and the opener is the seat after the
// big blind. These helpers are the single source of truth for the convention,
// shared by GameDef validation, the GameState constructor, and the artifact
// reader/writer (RFC 0010).
std::size_t small_blind_seat(const GameDef& def);
std::size_t big_blind_seat(const GameDef& def);
std::size_t preflop_first_actor(const GameDef& def);

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
  // `validate(def)` first, then the profile construction rules: posts blinds
  // and antes, derives the initial actor and pending set, and closes the
  // street when nobody can act. Throws `std::invalid_argument` for a malformed
  // definition and `std::overflow_error` for a checked chip sum that leaves the
  // exact numeric profile. The two-seat rules follow `HeadsUpState` and the
  // 3..10 rules follow `MultiwayState`; the seat count selects the profile.
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
  // reached. At two seats a standing `all_in` is deliberately preserved
  // across the refund; at 3+ seats `all_in` is re-derived from the stack. The
  // seat-count branch in this function is the only place that rule differs.
  void refund_unmatched();
  void close_street();
  // First seat clockwise from `from` (exclusive) that still owes an action.
  std::optional<std::size_t> next_actor(std::size_t from) const;
  // Whether any seat still owes an action. The 3..10 profile closes a street
  // when this is false. The two-seat profile cannot use it alone, because the
  // big blind's preflop option keeps the street open with nobody pending, so
  // `after_action` reconstructs the heads-up close from the option flag.
  bool any_pending() const;
  // Seats that are neither folded nor all in. Retained as a query; the
  // transition code does NOT gate the 3+ profile on this being at least two.
  // At 3+ seats a betting round opens whenever ONE seat still owes a decision
  // (the last live seat facing only all-in or folded opponents still owes its
  // call/fold), and closes only when `any_pending()` is false; the two-seat
  // profile instead closes the moment EITHER seat is all in. The battery pins
  // the distinction with a mutant that wrongly closes here when this count is
  // below two.
  std::size_t actionable_count() const;
  void refresh_live();
  ContributionSettlement award(std::span<const std::optional<std::uint32_t>> scores) const;

  GameDef def_;
  // Selects the rules profile: true reproduces `HeadsUpState` exactly; false
  // reproduces `MultiwayState`. Fixed for the life of the state because seat
  // count is root identity.
  bool heads_up_profile_ = false;
  // Two-seat preflop only: the big blind still holds its option to raise or
  // check after a limp, exactly as `HeadsUpState::big_blind_option_` tracks
  // it. No analogue exists in the 3..10 profile and it stays false there.
  bool big_blind_option_ = false;
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

// One observed public action on the path from the root to a decision node.
// GameState is historyless (its design notes above), so the resolver and the
// resident mapper carry this observation log explicitly: it is the only record
// of which seat did what on which street to reach the current node. `seat` is
// the acting occupied-seat index; `street` is the street the action closed or
// occurred on, matching the BettingEvent grammar the information key encodes.
struct PublicAction {
  Street street;
  std::size_t seat;
  Action action;
};

}  // namespace bs::poker
