#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "v1_mappers.hpp"

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

constexpr std::uint64_t kIntMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());

// Upper sanity bound for the optional effective-stack hint (10,000 bb is far
// beyond any real table depth while rejecting garbage).
constexpr double kMaxEffectiveStackBbHint = 10000.0;

bool inRangeInt(int value, int low, int high) {
  return value >= low && value <= high;
}

// Field-level checks mirror the buf.validate annotations on engine.proto
// (the protovalidate C++ runtime is intentionally not linked), followed by the
// RFC 0002 poker-semantic invariants. Every proto3 wire enum is checked
// against its closed set here; see the isKnown* predicates below.
class SemanticValidator {
 public:
  explicit SemanticValidator(const pv::DecisionRequest& request, unsigned negotiated_minor = 0)
      : request_(request), minor_(negotiated_minor) {}

  pv::ErrorCode run() {
    checkRequestPresence();
    if (request_.has_state())
      checkHandState(request_.state(), "state");
    if (request_.has_options())
      checkOptions(request_.options(), "options");
    return resultCode();
  }

  const ValidationReport& report() const { return report_; }

 private:
  const pv::DecisionRequest& request_;
  const unsigned minor_;
  ValidationReport report_;
  bool unsupportedGame_ = false;
  bool unsupportedFeature_ = false;

  void reject(const std::string& field, const std::string& description) {
    report_.add(field, description);
  }

  // Minor-0 diagnostic wording is pinned byte-for-byte; minor 1 uses a
  // minor-neutral note.
  const char* profileNote() const { return minor_ == 0 ? "in minor 0" : "by the engine"; }

  void rejectGame(const std::string& field, const std::string& description) {
    unsupportedGame_ = true;
    reject(field, description);
  }

  void rejectFeature(const std::string& field, const std::string& description) {
    unsupportedFeature_ = true;
    reject(field, description);
  }

  pv::ErrorCode resultCode() const {
    if (unsupportedGame_)
      return pv::ERROR_CODE_UNSUPPORTED_GAME;
    if (unsupportedFeature_)
      return pv::ERROR_CODE_UNSUPPORTED_FEATURE;
    return report_.ok() ? pv::ERROR_CODE_UNSPECIFIED : pv::ERROR_CODE_INVALID_REQUEST;
  }

  static bool stringLength(const std::string& value, unsigned minLength, unsigned maxLength) {
    return value.size() >= minLength && value.size() <= maxLength;
  }

  bool stringField(const std::string& value, unsigned minLength, unsigned maxLength,
                   const std::string& field) {
    if (!stringLength(value, minLength, maxLength)) {
      reject(field, "string length is outside the allowed range");
      return false;
    }
    if (!isValidUtf8(value)) {
      reject(field, "string field must be valid UTF-8");
      return false;
    }
    return true;
  }

  bool chipInProfile(std::uint64_t amount, const std::string& field, const char* what) {
    if (amount > kMaxChipAmount) {
      reject(field, std::string(what) + " exceeds the 2^53-1 integer chip profile");
      return false;
    }
    return true;
  }

  // Every chip field that reaches the integer heuristic Ctx (or is compared
  // against one) must fit the 2^53-1 profile and a signed int.
  bool chipFitsInt(std::uint64_t amount, const std::string& field, const char* what) {
    if (!chipInProfile(amount, field, what))
      return false;
    if (amount > kIntMax) {
      reject(field, std::string(what) + " exceeds the heuristic int range");
      return false;
    }
    return true;
  }

  void checkRequestPresence() {
    if (!request_.has_state())
      reject("state", "decision request requires state");
    if (!request_.has_options())
      reject("options", "decision request requires options");
  }

  void checkCard(const pv::Card& card, const std::string& field) {
    if (!isKnownRank(card.rank()))
      reject(field + ".rank", "card rank is outside the closed rank set");
    if (!isKnownSuit(card.suit()))
      reject(field + ".suit", "card suit is outside the closed suit set");
  }

  void checkAmountUnit(const pv::AmountUnit& unit, const std::string& field) {
    stringField(unit.name(), 1, 32, field + ".name");
    if (unit.decimal_places() > 9)
      reject(field + ".decimal_places", "amount unit decimal_places must be at most 9");
    // Minor-0 supports integral chips only; fractional units are an
    // unsupported feature rather than a malformed request.
    if (unit.name() != "chip" || unit.decimal_places() != 0)
      rejectFeature(field, std::string("only chip units with decimal_places=0 are supported ") +
                               profileNote());
  }

  void checkGame(const pv::GameDefinition& game, const std::string& field) {
    if (!isKnownGameVariant(game.variant())) {
      reject(field + ".variant", "game variant is outside the closed variant set");
    } else if (game.variant() != pv::GAME_VARIANT_NLHE) {
      rejectGame(field + ".variant", "only NLHE is supported");
    }
    if (!isKnownBettingStructure(game.betting_structure())) {
      reject(field + ".betting_structure", "betting structure is outside the closed set");
    } else if (game.betting_structure() != pv::BETTING_STRUCTURE_NO_LIMIT) {
      rejectGame(field + ".betting_structure", "only no-limit is supported");
    }
    if (!isKnownGameType(game.game_type())) {
      reject(field + ".game_type", "game type is outside the closed set");
    } else if (game.game_type() != pv::GAME_TYPE_CASH) {
      rejectGame(field + ".game_type", "tournament/ICM decisions are unsupported");
    }
    if (game.table_capacity() < 2 || game.table_capacity() > 10)
      reject(field + ".table_capacity", "table capacity must be in 2..10");
    if (!game.has_amount_unit()) {
      reject(field + ".amount_unit", "amount unit is required");
    } else {
      checkAmountUnit(game.amount_unit(), field + ".amount_unit");
    }
    chipFitsInt(game.small_blind(), field + ".small_blind", "small blind");
    chipFitsInt(game.big_blind(), field + ".big_blind", "big blind");
    if (game.big_blind() == 0)
      reject(field + ".big_blind", "big blind must be positive");
    if (game.small_blind() == 0 || game.small_blind() >= game.big_blind())
      reject(field + ".small_blind", "small blind must be positive and smaller than the big blind");
    if (game.ante() != 0) {
      chipFitsInt(game.ante(), field + ".ante", "ante");
      rejectFeature(field + ".ante", std::string("ante decisions are unsupported ") +
                                         (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    }
    if (game.button_ante() != 0) {
      chipFitsInt(game.button_ante(), field + ".button_ante", "button ante");
      rejectFeature(field + ".button_ante",
                    std::string("button ante decisions are unsupported ") +
                        (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    }
    if (game.has_rake()) {
      if (game.rake().basis_points() > 10000)
        reject(field + ".rake.basis_points", "rake basis points must be at most 10000");
      chipFitsInt(game.rake().cap(), field + ".rake.cap", "rake cap");
      rejectFeature(field + ".rake", std::string("rake-aware decisions are unsupported ") +
                                         (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    }
    if (game.has_straddle() && game.straddle().enabled()) {
      if (game.straddle().has_seat() && game.straddle().seat() > 9)
        reject(field + ".straddle.seat", "straddle seat must be at most 9");
      chipFitsInt(game.straddle().amount(), field + ".straddle.amount", "straddle amount");
      rejectFeature(field + ".straddle",
                    std::string("straddle decisions are unsupported ") +
                        (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    }
  }

  void checkPlayer(const pv::PlayerState& player, const std::string& field,
                   std::uint32_t capacity) {
    stringField(player.player_id(), 1, 128, field + ".player_id");
    if (player.seat() > 9 || player.seat() >= capacity)
      reject(field + ".seat", "seat must be in 0..table_capacity-1");
    chipFitsInt(player.stack(), field + ".stack", "stack");
    chipFitsInt(player.street_committed(), field + ".street_committed", "street commitment");
    if (!isKnownPlayerStatus(player.status()))
      reject(field + ".status", "player status is outside the closed status set");
  }

  void checkPot(const pv::PotState& pot, const std::string& field, int playerCount,
                const std::unordered_map<std::string, const pv::PlayerState*>& players) {
    chipFitsInt(pot.pot_total(), field + ".pot_total", "pot total");
    chipFitsInt(pot.main_pot(), field + ".main_pot", "main pot");
    if (pot.main_pot() > pot.pot_total())
      reject(field + ".main_pot", "main pot cannot exceed the pot total");

    const int maxSidePots = std::max(0, playerCount - 1);
    if (pot.side_pots_size() > maxSidePots)
      reject(field + ".side_pots", "more side pots than players");

    std::uint64_t sideSum = 0;
    for (int i = 0; i < pot.side_pots_size(); ++i) {
      const pv::SidePot& side = pot.side_pots(i);
      const std::string sideField = field + ".side_pots[" + std::to_string(i) + "]";
      if (side.amount() == 0)
        reject(sideField + ".amount", "side pot amount must be positive");
      if (!chipFitsInt(side.amount(), sideField + ".amount", "side pot amount"))
        continue;
      // Checked addition so a long repeated side-pot list cannot wrap.
      if (sideSum > pot.pot_total() - side.amount())
        reject(sideField + ".amount", "side pots exceed the pot total");
      sideSum += side.amount();
      if (side.eligible_player_ids_size() < 2)
        reject(sideField, "side pot requires at least two eligible players");
      std::unordered_set<std::string> eligible;
      for (const std::string& id : side.eligible_player_ids()) {
        if (id.empty() || id.size() > 128 || !isValidUtf8(id))
          reject(sideField, "side pot eligibility id must be 1..128 UTF-8 characters");
        if (!eligible.insert(id).second)
          reject(sideField, "duplicate side pot eligibility for " + id);
        if (!players.contains(id))
          reject(sideField, "side pot eligibility references unknown player " + id);
      }
    }
    if (pot.side_pots_size() > 0) {
      if (sideSum + pot.main_pot() != pot.pot_total())
        reject(field, "pot total must equal main pot plus side pots");
      // Side-pot-aware utility is an advertised capability gap; fail closed
      // rather than silently evaluating the main pot.
      rejectFeature(field + ".side_pots",
                    std::string("side-pot-aware decisions are unsupported ") +
                        (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    } else if (pot.main_pot() != pot.pot_total()) {
      reject(field, "with no side pots the pot total must equal the main pot");
    }
  }

  void checkForced(const pv::ForcedContribution& contribution, const std::string& field,
                   const std::unordered_map<std::string, const pv::PlayerState*>& players) {
    stringField(contribution.player_id(), 1, 128, field + ".player_id");
    if (!isKnownForcedType(contribution.type())) {
      reject(field + ".type", "forced contribution type is outside the closed set");
    } else if (contribution.type() != pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
               contribution.type() != pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND) {
      rejectFeature(field + ".type",
                    std::string("antes, dead blinds, and straddles are unsupported ") +
                        (minor_ == 0 ? "in minor 0" : "on the minor-1 profile"));
    }
    if (contribution.amount() == 0)
      reject(field + ".amount", "forced contribution amount must be positive");
    chipFitsInt(contribution.amount(), field + ".amount", "forced contribution amount");
    if (!contribution.player_id().empty() && !players.contains(contribution.player_id()))
      reject(field + ".player_id", "forced contribution references unknown player");
  }

  void checkHistory(const pv::HandState& state, const std::string& field,
                    const std::unordered_map<std::string, const pv::PlayerState*>& players) {
    std::uint64_t previousSequence = 0;
    bool hasSequence = false;
    pv::Street previousStreet = pv::STREET_UNSPECIFIED;
    for (int i = 0; i < state.action_history_size(); ++i) {
      const pv::ActionEvent& event = state.action_history(i);
      const std::string eventField = field + ".action_history[" + std::to_string(i) + "]";
      if (!isKnownHistoryStreet(event.street())) {
        reject(eventField + ".street", "action event street is outside the closed street set");
      } else {
        if (event.street() > state.street())
          reject(eventField + ".street", "action event street is after the current street");
        if (previousStreet != pv::STREET_UNSPECIFIED &&
            static_cast<int>(event.street()) < static_cast<int>(previousStreet))
          reject(eventField + ".street", "action history streets must be nondecreasing");
        previousStreet = event.street();
      }

      if (hasSequence && event.sequence() <= previousSequence)
        reject(eventField + ".sequence", "action sequence numbers must be strictly increasing");
      previousSequence = event.sequence();
      hasSequence = true;

      // An empty actor_player_id is the explicit marker of an opener the
      // platform could not resolve to a seat (the minor-0 adapter replicates
      // the v0 normalizer's first-token name matching); the mapper treats it
      // as "no opener". Every non-empty id must reference a seated player.
      if (!event.actor_player_id().empty()) {
        if (stringField(event.actor_player_id(), 1, 128, eventField + ".actor_player_id") &&
            !players.contains(event.actor_player_id()))
          reject(eventField + ".actor_player_id", "action actor is not a seated player");
      }
      if (!isKnownActionType(event.action())) {
        reject(eventField + ".action", "action type is outside the closed action set");
        continue;  // chip-field shape checks below require a known action
      }

      if (event.action() == pv::ACTION_TYPE_FOLD || event.action() == pv::ACTION_TYPE_CHECK) {
        if (event.has_incremental_amount())
          reject(eventField + ".incremental_amount", "fold/check move no chips");
        if (event.has_target_total())
          reject(eventField + ".target_total", "fold/check have no target total");
      } else if (event.action() == pv::ACTION_TYPE_CALL) {
        if (event.has_target_total())
          reject(eventField + ".target_total", "call has no target total");
        if (event.has_incremental_amount() && event.incremental_amount() == 0)
          reject(eventField + ".incremental_amount", "call increment must be positive");
      } else {
        if (event.has_incremental_amount() && event.incremental_amount() == 0)
          reject(eventField + ".incremental_amount", "bet/raise increment must be positive");
        if (event.has_target_total() && event.target_total() == 0)
          reject(eventField + ".target_total", "bet/raise target must be positive");
        if (event.has_incremental_amount() && event.has_target_total() &&
            event.incremental_amount() > event.target_total())
          reject(eventField, "incremental amount cannot exceed the target total");
      }
      if (event.has_incremental_amount())
        chipFitsInt(event.incremental_amount(), eventField + ".incremental_amount",
                    "incremental amount");
      if (event.has_target_total())
        chipFitsInt(event.target_total(), eventField + ".target_total", "history target total");
      chipFitsInt(event.pot_before(), eventField + ".pot_before", "pot before action");
      chipFitsInt(event.stack_after(), eventField + ".stack_after", "stack after action");
    }
  }

  // True exactly at the structured big-blind preflop option: the hero posted
  // the big blind, has exactly the big blind in this street with nothing more
  // to call, and no voluntary raise has happened on the preflop yet. This is
  // the one spot where check and raise coexist legally with to_call == 0;
  // every other no-bet spot is check/bet.
  bool bigBlindOptionOpen(const pv::HandState& state, const pv::PlayerState& hero) {
    if (state.street() != pv::STREET_PREFLOP || state.to_call() != 0)
      return false;
    if (!state.has_game() || hero.street_committed() != state.game().big_blind())
      return false;
    bool postedBigBlind = false;
    for (const pv::ForcedContribution& contribution : state.forced_contributions()) {
      if (contribution.player_id() == hero.player_id() &&
          contribution.type() == pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
        postedBigBlind = true;
    }
    if (!postedBigBlind)
      return false;
    for (const pv::ActionEvent& event : state.action_history()) {
      // Any preflop aggressive action (a bet or a raise) means the unopened
      // big-blind option is closed.
      if (event.street() == pv::STREET_PREFLOP &&
          (event.action() == pv::ACTION_TYPE_RAISE || event.action() == pv::ACTION_TYPE_BET))
        return false;
    }
    return true;
  }

  void checkLegal(const pv::HandState& state, const std::string& field,
                  const pv::PlayerState& hero) {
    const auto& legal = state.legal_actions();
    std::array<bool, 5> present{};
    const std::uint64_t heroCapacity = hero.stack() + hero.street_committed();
    const bool bbOption = bigBlindOptionOpen(state, hero);

    for (int i = 0; i < legal.size(); ++i) {
      const pv::LegalAction& action = legal.Get(i);
      const std::string actionField = field + ".legal_actions[" + std::to_string(i) + "]";
      if (!isKnownActionType(action.type())) {
        reject(actionField + ".type", "legal action type is outside the closed action set");
        continue;
      }
      const std::size_t index = static_cast<std::size_t>(static_cast<int>(action.type()) -
                                                         static_cast<int>(pv::ACTION_TYPE_FOLD));
      if (present[index])
        reject(actionField + ".type", "duplicate legal action");
      present[index] = true;

      const bool hasTargetRange =
          action.type() == pv::ACTION_TYPE_BET || action.type() == pv::ACTION_TYPE_RAISE;
      if (!hasTargetRange) {
        if (action.has_min_target_total() || action.has_max_target_total())
          reject(actionField, "fold/check/call carry no target range");
        continue;
      }
      if (!action.has_min_target_total() || !action.has_max_target_total()) {
        reject(actionField, "bet/raise require an inclusive target range");
        continue;
      }
      const std::uint64_t minimum = action.min_target_total();
      const std::uint64_t maximum = action.max_target_total();
      chipFitsInt(minimum, actionField + ".min_target_total", "minimum target");
      chipFitsInt(maximum, actionField + ".max_target_total", "maximum target");
      if (minimum == 0)
        reject(actionField + ".min_target_total", "target minimum must be positive");
      if (minimum > maximum)
        reject(actionField, ".target range minimum cannot exceed maximum");
      if (maximum > heroCapacity)
        reject(actionField + ".max_target_total",
               "target maximum exceeds the hero's available chips");
    }

    const auto has = [&](pv::ActionType type) {
      return present[static_cast<std::size_t>(static_cast<int>(type) -
                                              static_cast<int>(pv::ACTION_TYPE_FOLD))];
    };
    const bool hasBet = has(pv::ACTION_TYPE_BET);
    const bool hasRaise = has(pv::ACTION_TYPE_RAISE);
    const bool hasCheck = has(pv::ACTION_TYPE_CHECK);
    const bool hasCall = has(pv::ACTION_TYPE_CALL);
    if (hasBet && hasRaise)
      reject(field + ".legal_actions", "bet and raise are mutually exclusive at one decision");
    if (state.to_call() == 0) {
      if (!hasCheck)
        reject(field + ".legal_actions", "check must be legal when nothing is owed");
      if (hasRaise && !bbOption)
        reject(field + ".legal_actions",
               "raise with nothing owed is legal only at the unopened big blind option");
    } else {
      if (hasCheck)
        reject(field + ".legal_actions", "check is only legal when nothing is owed");
      if (hasBet)
        reject(field + ".legal_actions", "bet is only legal when nothing is owed");
      if (!hasCall)
        reject(field + ".legal_actions", "call must be legal when chips are owed");
    }
    chipFitsInt(state.to_call(), field + ".to_call", "to_call");
    if (state.to_call() > heroCapacity)
      reject(field + ".to_call", "to_call exceeds the hero's available chips");
  }

  void checkHandState(const pv::HandState& state, const std::string& field) {
    stringField(state.hand_id(), 1, 128, field + ".hand_id");
    if (!isKnownDecisionStreet(state.street()))
      reject(field + ".street",
             "decision street must be preflop, flop, turn, or river (showdown has no decision)");
    stringField(state.hero_player_id(), 1, 128, field + ".hero_player_id");

    if (!state.has_game()) {
      reject(field + ".game", "game definition is required");
    } else {
      checkGame(state.game(), field + ".game");
    }
    const std::uint32_t capacity = state.has_game() ? state.game().table_capacity() : 10;

    if (state.players_size() < 2 || state.players_size() > 10)
      reject(field + ".players", "hand state requires 2..10 players");

    std::unordered_map<std::string, const pv::PlayerState*> players;
    std::unordered_set<std::uint32_t> seats;
    const pv::PlayerState* hero = nullptr;
    bool buttonPresent = false;
    for (int i = 0; i < state.players_size(); ++i) {
      const pv::PlayerState& player = state.players(i);
      const std::string playerField = field + ".players[" + std::to_string(i) + "]";
      checkPlayer(player, playerField, capacity);
      if (!player.player_id().empty() && !players.emplace(player.player_id(), &player).second)
        reject(playerField + ".player_id", "duplicate player id");
      if (!seats.insert(player.seat()).second)
        reject(playerField + ".seat", "duplicate seat");
      if (player.seat() == state.button_seat())
        buttonPresent = true;
      if (player.player_id() == state.hero_player_id())
        hero = &player;
    }
    if (state.players_size() >= 2 && !buttonPresent)
      reject(field + ".button_seat", "button seat must reference a seated player");
    if (!state.hero_player_id().empty() && !hero)
      reject(field + ".hero_player_id", "hero must be a seated player");
    if (hero && hero->status() != pv::PLAYER_STATUS_ACTIVE)
      reject(field + ".hero_player_id", "hero must be active to decide");

    // Cards: exact two hole cards, board cap and street length, uniqueness.
    if (state.hero_hole_cards_size() != 2)
      reject(field + ".hero_hole_cards", "exactly two hero hole cards are required");
    for (int i = 0; i < state.hero_hole_cards_size(); ++i)
      checkCard(state.hero_hole_cards(i), field + ".hero_hole_cards[" + std::to_string(i) + "]");
    if (state.board_size() > 5)
      reject(field + ".board", "board can contain at most five cards");
    for (int i = 0; i < state.board_size(); ++i)
      checkCard(state.board(i), field + ".board[" + std::to_string(i) + "]");
    if (isKnownDecisionStreet(state.street())) {
      const int expected = state.street() == pv::STREET_PREFLOP ? 0
                           : state.street() == pv::STREET_FLOP  ? 3
                           : state.street() == pv::STREET_TURN  ? 4
                                                                : 5;
      if (state.board_size() != expected)
        reject(field + ".board", "board length does not match the current street");
    }
    auto cardKey = [](const pv::Card& card) {
      return static_cast<int>(card.rank()) * 16 + static_cast<int>(card.suit());
    };
    std::unordered_set<int> cardKeys;
    for (const pv::Card& card : state.hero_hole_cards()) {
      // Only unique closed-set cards participate; unknown cards were rejected
      // above and could otherwise share the 0 key.
      if (isKnownRank(card.rank()) && isKnownSuit(card.suit()) &&
          !cardKeys.insert(cardKey(card)).second)
        reject(field + ".hero_hole_cards", "duplicate hero hole card");
    }
    for (const pv::Card& card : state.board()) {
      if (isKnownRank(card.rank()) && isKnownSuit(card.suit()) &&
          !cardKeys.insert(cardKey(card)).second)
        reject(field + ".board", "board card duplicates a hero or board card");
    }

    if (!state.has_pot()) {
      reject(field + ".pot", "pot state is required");
    } else {
      checkPot(state.pot(), field + ".pot", state.players_size(), players);
      // Derived-sum boundary: pot_total and to_call each fit int, but the
      // heuristic computes pot+call (pot odds, MDF, bet geometry) in a widened
      // type; reject the combination before mapping when even that derived sum
      // cannot be represented by the integer heuristic. The uint64 add itself
      // cannot overflow for values bounded by 2^53-1.
      if (state.pot().pot_total() <= kIntMax && state.to_call() <= kIntMax &&
          state.pot().pot_total() + state.to_call() > kIntMax) {
        reject(field, "pot_total plus to_call exceeds the heuristic integer range");
      }
      chipFitsInt(state.to_call(), field + ".to_call", "to_call");
    }

    for (int i = 0; i < state.forced_contributions_size(); ++i)
      checkForced(state.forced_contributions(i),
                  field + ".forced_contributions[" + std::to_string(i) + "]", players);

    checkHistory(state, field, players);

    if (state.legal_actions_size() < 1 || state.legal_actions_size() > 5) {
      reject(field + ".legal_actions", "1..5 legal actions are required");
    } else if (hero) {
      checkLegal(state, field, *hero);
    }
  }

  void checkOptions(const pv::DecisionOptions& options, const std::string& field) {
    const bool profileLengthOk =
        stringField(options.strategy_profile(), 1, 64, field + ".strategy_profile");
    static const std::array<const char*, 3> kProfiles = {"tag", "lag", "station-hunter"};
    bool profileKnown = false;
    for (const char* profile : kProfiles)
      if (options.strategy_profile() == profile)
        profileKnown = true;
    if (profileLengthOk && !profileKnown)
      reject(field + ".strategy_profile", "unknown strategy profile");
    if (options.solve_time_budget_ms() == 0 || options.solve_time_budget_ms() > 120000)
      reject(field + ".solve_time_budget_ms", "solve time budget must be in 1..120000 ms");
    if (!isKnownSolverMode(options.solver_mode())) {
      reject(field + ".solver_mode", "solver mode is outside the closed solver mode set");
    } else if (options.solver_mode() == pv::SOLVER_MODE_RIVER_LP ||
               options.solver_mode() == pv::SOLVER_MODE_RIVER_DCFR ||
               options.solver_mode() == pv::SOLVER_MODE_MULTISTREET_CFR) {
      // The heuristic engine may internally select a river backend; callers
      // cannot force one on either negotiated minor.
      rejectFeature(
          field + ".solver_mode",
          std::string("forced solver backends (river LP/DCFR, multistreet) are unsupported") +
              (minor_ == 0 ? " in minor 0" : ""));
    } else if (options.solver_mode() == pv::SOLVER_MODE_RESOLVING) {
      // RFC 0005 Stage 9: terminal-only resolving is a negotiated minor-1
      // feature. Minor 0 keeps the exact Stage-8 rejection code and message;
      // only minor 1 passes validation through to the resolver.
      if (minor_ == 0)
        rejectFeature(field + ".solver_mode", "resolving is not supported by this engine");
    } else if (options.solver_mode() == pv::SOLVER_MODE_BLUEPRINT && minor_ == 0) {
      // Minor 0 never sees the new modes even when a resident root exists.
      rejectFeature(field + ".solver_mode",
                    "blueprint lookup requires negotiated protocol minor 1");
    }
    if (options.has_preflop_effective_stack_bb()) {
      const double hint = options.preflop_effective_stack_bb();
      if (!std::isfinite(hint) || hint <= 0.0 || hint > kMaxEffectiveStackBbHint)
        reject(field + ".preflop_effective_stack_bb",
               "preflop effective stack hint must be a finite positive value within table depth");
    }
    // RFC 0008 stage 5: minimum_guarantee is a negotiated minor-2 request
    // field. Presence on either older minor (including an explicit UNSPECIFIED)
    // is a feature negotiation failure; at minor 2 a present value must be one
    // of the five real levels, matching the buf in:[1..5] annotation.
    if (options.has_minimum_guarantee()) {
      if (minor_ < 2) {
        rejectFeature(field + ".minimum_guarantee",
                      "minimum_guarantee requires negotiated protocol minor 2");
      } else if (!isKnownGuaranteeLevel(options.minimum_guarantee())) {
        reject(field + ".minimum_guarantee",
               "minimum_guarantee is outside the closed guarantee level set");
      }
    }
  }
};

}  // namespace

bool isKnownActionType(pv::ActionType value) {
  return inRangeInt(static_cast<int>(value), static_cast<int>(pv::ACTION_TYPE_FOLD),
                    static_cast<int>(pv::ACTION_TYPE_RAISE));
}

bool isKnownRank(pv::Rank value) {
  return inRangeInt(static_cast<int>(value), static_cast<int>(pv::RANK_TWO),
                    static_cast<int>(pv::RANK_ACE));
}

bool isKnownSuit(pv::Suit value) {
  return inRangeInt(static_cast<int>(value), static_cast<int>(pv::SUIT_SPADES),
                    static_cast<int>(pv::SUIT_CLUBS));
}

bool isKnownDecisionStreet(pv::Street value) {
  return value == pv::STREET_PREFLOP || value == pv::STREET_FLOP || value == pv::STREET_TURN ||
         value == pv::STREET_RIVER;
}

bool isKnownHistoryStreet(pv::Street value) {
  return isKnownDecisionStreet(value);
}

bool isKnownPlayerStatus(pv::PlayerStatus value) {
  return value == pv::PLAYER_STATUS_ACTIVE || value == pv::PLAYER_STATUS_FOLDED ||
         value == pv::PLAYER_STATUS_ALL_IN;
}

bool isKnownForcedType(pv::ForcedContributionType value) {
  return inRangeInt(static_cast<int>(value),
                    static_cast<int>(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND),
                    static_cast<int>(pv::FORCED_CONTRIBUTION_TYPE_STRADDLE));
}

bool isKnownGameVariant(pv::GameVariant value) {
  return value == pv::GAME_VARIANT_NLHE;
}

bool isKnownBettingStructure(pv::BettingStructure value) {
  return value == pv::BETTING_STRUCTURE_NO_LIMIT;
}

bool isKnownGameType(pv::GameType value) {
  return value == pv::GAME_TYPE_CASH || value == pv::GAME_TYPE_TOURNAMENT;
}

bool isKnownSolverMode(pv::SolverMode value) {
  return value == pv::SOLVER_MODE_AUTOMATIC || value == pv::SOLVER_MODE_HEURISTIC ||
         value == pv::SOLVER_MODE_RIVER_LP || value == pv::SOLVER_MODE_RIVER_DCFR ||
         value == pv::SOLVER_MODE_MULTISTREET_CFR || value == pv::SOLVER_MODE_BLUEPRINT ||
         value == pv::SOLVER_MODE_RESOLVING;
}

bool isKnownGuaranteeLevel(pv::GuaranteeLevel value) {
  return value == pv::GUARANTEE_LEVEL_OPERATIONAL_FALLBACK ||
         value == pv::GUARANTEE_LEVEL_APPROXIMATE || value == pv::GUARANTEE_LEVEL_ABSTRACT_SOLVED ||
         value == pv::GUARANTEE_LEVEL_EXACT_SOLVED || value == pv::GUARANTEE_LEVEL_CERTIFIED_BOUND;
}

bool isValidUtf8(std::string_view value) {
  // RFC 3629: per-byte lead classification with strict continuation and
  // overlong/surrogate/range rejection.
  std::size_t index = 0;
  while (index < value.size()) {
    const unsigned char lead = static_cast<unsigned char>(value[index++]);
    int continuation = 0;
    unsigned int codePoint = 0;
    if (lead <= 0x7f)
      continue;
    if ((lead & 0xe0) == 0xc0) {
      continuation = 1;
      codePoint = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
      continuation = 2;
      codePoint = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
      continuation = 3;
      codePoint = lead & 0x07;
    } else {
      return false;  // 0xf5-0xff or bare continuation
    }
    if (index + static_cast<std::size_t>(continuation) > value.size())
      return false;
    for (int i = 0; i < continuation; ++i) {
      const unsigned char next = static_cast<unsigned char>(value[index++]);
      if ((next & 0xc0) != 0x80)
        return false;
      codePoint = (codePoint << 6) | (next & 0x3f);
    }
    const unsigned int minimum = continuation == 1 ? 0x80 : continuation == 2 ? 0x800 : 0x10000;
    if (codePoint < minimum)
      return false;  // overlong
    if (codePoint > 0x10ffff)
      return false;
    if (codePoint >= 0xd800 && codePoint <= 0xdfff)
      return false;  // UTF-16 surrogate
  }
  return true;
}

void ValidationReport::add(const std::string& fieldPath, const std::string& description) {
  pv::FieldViolation violation;
  violation.set_field_path(fieldPath);
  violation.set_description(description);
  violations.push_back(std::move(violation));
}

pv::ErrorCode validateDecisionRequest(const pv::DecisionRequest& request, ValidationReport& report,
                                      unsigned negotiated_minor) {
  SemanticValidator validator(request, negotiated_minor);
  pv::ErrorCode code = validator.run();
  for (const pv::FieldViolation& violation : validator.report().violations)
    report.violations.push_back(violation);
  return code;
}

}  // namespace bs::v1
