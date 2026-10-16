// v2_semantic_validator.cpp — RFC 0022 v2 semantic validation.
//
// Validates the v2 DecisionRequest: game definition, rule profile, G/F
// contribution ledger, phase-tagged pot layers, transition path, observation
// provenance, legal actions, and decision options. Returns ERROR_CODE_UNSPECIFIED
// when valid; otherwise the applicable failure class with all discovered
// violations appended to the report.
//
// v2-specific checks beyond v1:
// - RuleProfile: settlement, reopening, runout, dealt_seat_count
// - G/F ledger invariant: starting_stack = stack + gross_contributed - refunded
// - Phase-tagged pot layers: PENDING sole-contributor OK, FINALIZED >= 2
// - TransitionEvent path with explicit BoardDeal boundaries
// - ObservationProvenance: source, coverage, controller_epoch, event count
// - UtilityProfile: CASH_CHIP only
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "v1_mappers.hpp"
#include "v2_mappers.hpp"

namespace bs::v2 {
namespace {

namespace pv2 = ::bigshark::engine::v2;

constexpr std::uint64_t kIntMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());

constexpr double kMaxEffectiveStackBbHint = 10000.0;

// --- Closed-set predicates (proto3 enums are open on the wire) ---

bool isKnownRank(pv2::Rank value) {
  return value >= pv2::RANK_TWO && value <= pv2::RANK_ACE;
}

bool isKnownSuit(pv2::Suit value) {
  return value >= pv2::SUIT_SPADES && value <= pv2::SUIT_CLUBS;
}

bool isKnownStreet(pv2::Street value) {
  return value >= pv2::STREET_PREFLOP && value <= pv2::STREET_SHOWDOWN;
}

bool isKnownPlayerStatus(pv2::PlayerStatus value) {
  return value >= pv2::PLAYER_STATUS_ACTIVE && value <= pv2::PLAYER_STATUS_ALL_IN;
}

bool isKnownActionType(pv2::ActionType value) {
  return value >= pv2::ACTION_TYPE_FOLD && value <= pv2::ACTION_TYPE_RAISE;
}

bool isKnownForcedType(pv2::ForcedContributionType value) {
  return value >= pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
         value <= pv2::FORCED_CONTRIBUTION_TYPE_STRADDLE;
}

bool isKnownGameVariant(pv2::GameVariant value) {
  return value == pv2::GAME_VARIANT_NLHE;
}

bool isKnownBettingStructure(pv2::BettingStructure value) {
  return value == pv2::BETTING_STRUCTURE_NO_LIMIT;
}

bool isKnownGameType(pv2::GameType value) {
  return value == pv2::GAME_TYPE_CASH || value == pv2::GAME_TYPE_TOURNAMENT;
}

bool isKnownSolverMode(pv2::SolverMode value) {
  return value >= pv2::SOLVER_MODE_AUTOMATIC && value <= pv2::SOLVER_MODE_RESOLVING;
}

bool isKnownPotPhase(pv2::PotPhase value) {
  return value >= pv2::POT_PHASE_ACTION && value <= pv2::POT_PHASE_SETTLED;
}

bool isKnownLayerStatus(pv2::LayerStatus value) {
  return value == pv2::LAYER_STATUS_PENDING || value == pv2::LAYER_STATUS_FINALIZED;
}

bool isKnownObservationSource(pv2::ObservationSource value) {
  return value >= pv2::OBSERVATION_SOURCE_OFFICIAL_RESPONSE &&
         value <= pv2::OBSERVATION_SOURCE_SUPPLIED;
}

bool isKnownObservationCoverage(pv2::ObservationCoverage value) {
  return value >= pv2::OBSERVATION_COVERAGE_FULL && value <= pv2::OBSERVATION_COVERAGE_GAP;
}

bool isKnownSettlementRule(pv2::SettlementRule value) {
  return value == pv2::SETTLEMENT_RULE_CLOCKWISE_LEFT_OF_BUTTON;
}

bool isKnownReopeningRule(pv2::ReopeningRule value) {
  return value == pv2::REOPENING_RULE_NEVER_CUMULATIVE || value == pv2::REOPENING_RULE_TDA_2026;
}

bool isKnownRunoutRule(pv2::RunoutRule value) {
  return value == pv2::RUNOUT_RULE_SINGLE;
}

bool isKnownUtilityProfile(pv2::UtilityProfile value) {
  return value == pv2::UTILITY_PROFILE_CASH_CHIP || value == pv2::UTILITY_PROFILE_TOURNAMENT_ICM;
}

bool isKnownGuaranteeLevel(pv2::GuaranteeLevel value) {
  return value >= pv2::GUARANTEE_LEVEL_OPERATIONAL_FALLBACK &&
         value <= pv2::GUARANTEE_LEVEL_CERTIFIED_BOUND;
}

// --- Validator ---

class SemanticValidator {
 public:
  explicit SemanticValidator(const pv2::DecisionRequest& request) : request_(request) {}

  pv2::ErrorCode run() {
    checkRequestPresence();
    if (request_.has_state())
      checkHandState(request_.state(), "state");
    if (request_.has_options())
      checkOptions(request_.options(), "options");
    return resultCode();
  }

  const ValidationReport& report() const { return report_; }

 private:
  const pv2::DecisionRequest& request_;
  ValidationReport report_;
  bool unsupportedGame_ = false;
  bool unsupportedFeature_ = false;
  bool rulesUnconfirmed_ = false;
  bool historyIncomplete_ = false;
  bool unknownUtility_ = false;

  void reject(const std::string& field, const std::string& description) {
    report_.add(field, description);
  }

  void rejectGame(const std::string& field, const std::string& description) {
    unsupportedGame_ = true;
    reject(field, description);
  }

  void rejectFeature(const std::string& field, const std::string& description) {
    unsupportedFeature_ = true;
    reject(field, description);
  }

  void rejectRulesUnconfirmed(const std::string& field, const std::string& description) {
    rulesUnconfirmed_ = true;
    reject(field, description);
  }

  void rejectHistoryIncomplete(const std::string& field, const std::string& description) {
    historyIncomplete_ = true;
    reject(field, description);
  }

  void rejectUnknownUtility(const std::string& field, const std::string& description) {
    unknownUtility_ = true;
    reject(field, description);
  }

  pv2::ErrorCode resultCode() const {
    if (rulesUnconfirmed_)
      return pv2::ERROR_CODE_RULES_UNCONFIRMED;
    if (unknownUtility_)
      return pv2::ERROR_CODE_UNKNOWN_UTILITY;
    if (historyIncomplete_)
      return pv2::ERROR_CODE_HISTORY_INCOMPLETE;
    if (unsupportedGame_)
      return pv2::ERROR_CODE_UNSUPPORTED_GAME;
    if (unsupportedFeature_)
      return pv2::ERROR_CODE_UNSUPPORTED_FEATURE;
    return report_.ok() ? pv2::ERROR_CODE_UNSPECIFIED : pv2::ERROR_CODE_INVALID_REQUEST;
  }

  bool stringField(const std::string& value, unsigned minLength, unsigned maxLength,
                   const std::string& field) {
    if (value.size() < minLength || value.size() > maxLength) {
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

  void checkCard(const pv2::Card& card, const std::string& field) {
    if (!isKnownRank(card.rank()))
      reject(field + ".rank", "card rank is outside the closed rank set");
    if (!isKnownSuit(card.suit()))
      reject(field + ".suit", "card suit is outside the closed suit set");
  }

  void checkGameDefinition(const pv2::GameDefinition& game, const std::string& field) {
    if (!isKnownGameVariant(game.variant())) {
      reject(field + ".variant", "game variant is outside the closed variant set");
    } else if (game.variant() != pv2::GAME_VARIANT_NLHE) {
      rejectGame(field + ".variant", "only NLHE is supported");
    }
    if (!isKnownBettingStructure(game.betting_structure())) {
      reject(field + ".betting_structure", "betting structure is outside the closed set");
    } else if (game.betting_structure() != pv2::BETTING_STRUCTURE_NO_LIMIT) {
      rejectGame(field + ".betting_structure", "only no-limit is supported");
    }
    if (!isKnownGameType(game.game_type())) {
      reject(field + ".game_type", "game type is outside the closed set");
    } else if (game.game_type() != pv2::GAME_TYPE_CASH) {
      rejectGame(field + ".game_type", "tournament/ICM decisions are unsupported");
    }
    if (game.table_capacity() < 2 || game.table_capacity() > 10)
      reject(field + ".table_capacity", "table capacity must be in 2..10");
    if (!game.has_amount_unit()) {
      reject(field + ".amount_unit", "amount unit is required");
    } else {
      stringField(game.amount_unit().name(), 1, 32, field + ".amount_unit.name");
      if (game.amount_unit().decimal_places() > 9)
        reject(field + ".amount_unit.decimal_places",
               "amount unit decimal_places must be at most 9");
      if (game.amount_unit().name() != "chip" || game.amount_unit().decimal_places() != 0)
        rejectFeature(field + ".amount_unit",
                      "only chip units with decimal_places=0 are supported");
    }
    chipFitsInt(game.small_blind(), field + ".small_blind", "small blind");
    chipFitsInt(game.big_blind(), field + ".big_blind", "big blind");
    if (game.big_blind() == 0)
      reject(field + ".big_blind", "big blind must be positive");
    if (game.small_blind() == 0 || game.small_blind() >= game.big_blind())
      reject(field + ".small_blind", "small blind must be positive and smaller than the big blind");
    // Antes are accepted: the native GameDef supports them.
    chipFitsInt(game.ante(), field + ".ante", "ante");
    if (game.button_ante() != 0) {
      chipFitsInt(game.button_ante(), field + ".button_ante", "button ante");
      rejectFeature(field + ".button_ante", "button ante decisions are unsupported");
    }
    if (game.has_rake()) {
      if (game.rake().basis_points() > 10000)
        reject(field + ".rake.basis_points", "rake basis points must be at most 10000");
      chipFitsInt(game.rake().cap(), field + ".rake.cap", "rake cap");
      // Native terminal rake is hardcoded to zero; nonzero rake is unsupported.
      if (game.rake().basis_points() > 0 || game.rake().cap() > 0)
        rejectFeature(field + ".rake", "rake decisions are unsupported");
    }
    if (game.has_straddle() && game.straddle().enabled()) {
      if (game.straddle().has_seat() && game.straddle().seat() > 9)
        reject(field + ".straddle.seat", "straddle seat must be at most 9");
      chipFitsInt(game.straddle().amount(), field + ".straddle.amount", "straddle amount");
      rejectFeature(field + ".straddle", "straddle decisions are unsupported");
    }
    // v2 RuleProfile validation.
    if (!game.has_rule_profile()) {
      reject(field + ".rule_profile", "rule profile is required for v2");
    } else {
      checkRuleProfile(game.rule_profile(), field + ".rule_profile");
    }
  }

  void checkRuleProfile(const pv2::RuleProfile& rules, const std::string& field) {
    if (!isKnownSettlementRule(rules.settlement())) {
      reject(field + ".settlement", "settlement rule is outside the closed set");
    } else if (rules.settlement() != pv2::SETTLEMENT_RULE_CLOCKWISE_LEFT_OF_BUTTON) {
      rejectFeature(field + ".settlement", "only clockwise-left-of-button settlement is supported");
    }
    if (!isKnownReopeningRule(rules.reopening())) {
      reject(field + ".reopening", "reopening rule is outside the closed set");
    } else if (rules.reopening() == pv2::REOPENING_RULE_TDA_2026) {
      // TDA 2026 reopening is declared but unimplemented.
      rejectRulesUnconfirmed(field + ".reopening",
                             "TDA 2026 reopening rule has not been confirmed");
    }
    if (!isKnownRunoutRule(rules.runout())) {
      reject(field + ".runout", "runout rule is outside the closed set");
    } else if (rules.runout() != pv2::RUNOUT_RULE_SINGLE) {
      rejectFeature(field + ".runout", "only single runout is supported");
    }
    if (rules.dealt_seat_count() < 2 || rules.dealt_seat_count() > 10)
      reject(field + ".dealt_seat_count", "dealt seat count must be in 2..10");
  }

  void checkPlayer(const pv2::PlayerState& player, const std::string& field,
                   std::uint32_t tableCapacity) {
    if (player.seat() > 9 || player.seat() >= tableCapacity)
      reject(field + ".seat", "seat must be in 0..table_capacity-1");
    if (player.has_display_name())
      stringField(player.display_name(), 0, 128, field + ".display_name");
    chipInProfile(player.starting_stack(), field + ".starting_stack", "starting stack");
    chipFitsInt(player.stack(), field + ".stack", "stack");
    chipFitsInt(player.street_committed(), field + ".street_committed", "street commitment");
    chipInProfile(player.gross_contributed(), field + ".gross_contributed", "gross contributed");
    chipInProfile(player.refunded(), field + ".refunded", "refunded");
    if (!isKnownPlayerStatus(player.status()))
      reject(field + ".status", "player status is outside the closed status set");

    // G/F ledger invariant: starting_stack = stack + gross_contributed - refunded.
    // Check with checked arithmetic to avoid overflow.
    const std::uint64_t g = player.gross_contributed();
    const std::uint64_t f = player.refunded();
    if (f > g) {
      reject(field + ".refunded", "refunded cannot exceed gross_contributed");
    } else {
      const std::uint64_t net = g - f;
      if (player.stack() > kMaxChipAmount - net) {
        reject(field, "stack + net contribution exceeds the chip profile");
      } else {
        const std::uint64_t effective = player.stack() + net;
        if (effective != player.starting_stack()) {
          reject(field,
                 "G/F invariant violated: starting_stack != stack + gross_contributed - refunded");
        }
      }
    }
  }

  void checkPotState(const pv2::PotState& pot, const std::string& field, int playerCount,
                     const std::unordered_map<std::uint32_t, const pv2::PlayerState*>& players) {
    if (!isKnownPotPhase(pot.phase())) {
      reject(field + ".phase", "pot phase is outside the closed set");
      return;
    }
    chipFitsInt(pot.pot_total(), field + ".pot_total", "pot total");

    std::uint64_t layerSum = 0;
    std::uint64_t prevLevel = 0;
    bool hasPrevLevel = false;
    for (int i = 0; i < pot.layers_size(); ++i) {
      const pv2::PotLayer& layer = pot.layers(i);
      const std::string layerField = field + ".layers[" + std::to_string(i) + "]";

      // Layers must be ordered by contribution_level ascending.
      if (hasPrevLevel && layer.contribution_level() <= prevLevel)
        reject(layerField + ".contribution_level",
               "layers must be ordered by contribution_level ascending");
      prevLevel = layer.contribution_level();
      hasPrevLevel = true;

      if (layer.amount() == 0)
        reject(layerField + ".amount", "pot layer amount must be positive");
      chipFitsInt(layer.amount(), layerField + ".amount", "pot layer amount");
      if (layerSum > pot.pot_total() - layer.amount())
        reject(layerField + ".amount", "pot layers exceed the pot total");
      layerSum += layer.amount();

      if (!isKnownLayerStatus(layer.status())) {
        reject(layerField + ".status", "layer status is outside the closed set");
      } else {
        // Phase-aware layer validation.
        if (pot.phase() == pv2::POT_PHASE_ACTION && layer.status() != pv2::LAYER_STATUS_PENDING)
          reject(layerField + ".status", "ACTION-phase layers must be PENDING");
        if (pot.phase() == pv2::POT_PHASE_SETTLED && layer.status() != pv2::LAYER_STATUS_FINALIZED)
          reject(layerField + ".status", "SETTLED-phase layers must be FINALIZED");

        if (layer.status() == pv2::LAYER_STATUS_PENDING) {
          // PENDING layers must not carry rake or winners (provisional).
          if (layer.has_rake())
            reject(layerField + ".rake", "PENDING layer must not carry rake");
          if (layer.winner_seats_size() > 0)
            reject(layerField + ".winner_seats", "PENDING layer must not have winners");
        }

        if (layer.status() == pv2::LAYER_STATUS_FINALIZED) {
          // FINALIZED layers must have at least two contributors (eligible seats).
          // A sole-contributor FINALIZED layer is a refund, not a pot.
          if (layer.eligible_seats_size() < 2)
            reject(layerField + ".eligible_seats",
                   "FINALIZED layer must have at least two eligible seats");
          if (!layer.has_rake())
            reject(layerField + ".rake", "FINALIZED layer must carry rake");
          if (layer.winner_seats_size() == 0)
            reject(layerField + ".winner_seats", "FINALIZED layer must have at least one winner");
        }
      }

      // Eligible seats must reference known players.
      std::unordered_set<std::uint32_t> eligible;
      for (std::uint32_t seat : layer.eligible_seats()) {
        if (!players.contains(seat))
          reject(layerField + ".eligible_seats", "eligible seat references unknown player");
        if (!eligible.insert(seat).second)
          reject(layerField + ".eligible_seats", "duplicate eligible seat");
      }
      // Winner seats must be a subset of eligible seats.
      for (std::uint32_t seat : layer.winner_seats()) {
        if (!eligible.contains(seat))
          reject(layerField + ".winner_seats", "winner seat is not in eligible seats");
      }
    }
    if (layerSum != pot.pot_total())
      reject(field, "pot total must equal the sum of layer amounts");
  }

  void checkTransitionPath(
      const pv2::HandState& state, const std::string& field,
      const std::unordered_map<std::uint32_t, const pv2::PlayerState*>& players) {
    std::uint64_t previousSequence = 0;
    bool hasSequence = false;
    pv2::Street previousStreet = pv2::STREET_UNSPECIFIED;
    int boardCardsDealt = 0;
    // Track all cards dealt to detect duplicates.
    std::unordered_set<int> dealtCards;

    for (int i = 0; i < state.transition_path_size(); ++i) {
      const pv2::TransitionEvent& event = state.transition_path(i);
      const std::string eventField = field + ".transition_path[" + std::to_string(i) + "]";

      if (hasSequence && event.sequence() <= previousSequence)
        reject(eventField + ".sequence", "transition sequence numbers must be strictly increasing");
      previousSequence = event.sequence();
      hasSequence = true;

      switch (event.transition_case()) {
        case pv2::TransitionEvent::kAction: {
          const pv2::ActionEvent& action = event.action();
          if (!isKnownStreet(action.street())) {
            reject(eventField + ".action.street", "action street is outside the closed set");
          } else {
            if (previousStreet != pv2::STREET_UNSPECIFIED &&
                static_cast<int>(action.street()) < static_cast<int>(previousStreet))
              reject(eventField + ".action.street", "transition streets must be nondecreasing");
            // Transition streets must not exceed the current street.
            if (static_cast<int>(action.street()) > static_cast<int>(state.street()))
              reject(eventField + ".action.street",
                     "transition street exceeds the current decision street");
            previousStreet = action.street();
          }
          if (!players.contains(action.actor_seat()))
            reject(eventField + ".action.actor_seat", "actor seat references unknown player");
          if (!isKnownActionType(action.action())) {
            reject(eventField + ".action.action", "action type is outside the closed set");
          } else {
            // Chip-field shape per action kind.
            const bool aggressive = action.action() == pv2::ACTION_TYPE_BET ||
                                    action.action() == pv2::ACTION_TYPE_RAISE;
            if (action.action() == pv2::ACTION_TYPE_FOLD ||
                action.action() == pv2::ACTION_TYPE_CHECK) {
              if (action.has_incremental_amount() && action.incremental_amount() != 0)
                reject(eventField + ".action.incremental_amount",
                       "fold/check must not carry an incremental amount");
              if (action.has_target_total())
                reject(eventField + ".action.target_total",
                       "fold/check must not carry a target total");
            } else if (action.action() == pv2::ACTION_TYPE_CALL) {
              if (!action.has_incremental_amount() || action.incremental_amount() == 0)
                reject(eventField + ".action.incremental_amount",
                       "call must carry a positive incremental amount");
              if (action.has_target_total())
                reject(eventField + ".action.target_total", "call must not carry a target total");
            } else if (aggressive) {
              if (!action.has_target_total() || action.target_total() == 0)
                reject(eventField + ".action.target_total",
                       "bet/raise must carry a positive target total");
            }
          }
          break;
        }
        case pv2::TransitionEvent::kBoardDeal: {
          const pv2::BoardDeal& deal = event.board_deal();
          if (!isKnownStreet(deal.street())) {
            reject(eventField + ".board_deal.street",
                   "board deal street is outside the closed set");
          } else {
            if (previousStreet != pv2::STREET_UNSPECIFIED &&
                static_cast<int>(deal.street()) < static_cast<int>(previousStreet))
              reject(eventField + ".board_deal.street", "transition streets must be nondecreasing");
            // Transition streets must not exceed the current street.
            if (static_cast<int>(deal.street()) > static_cast<int>(state.street()))
              reject(eventField + ".board_deal.street",
                     "transition street exceeds the current decision street");
            // Card count per street: FLOP=3, TURN=1, RIVER=1.
            const int expectedCards = [&]() -> int {
              switch (deal.street()) {
                case pv2::STREET_FLOP:
                  return 3;
                case pv2::STREET_TURN:
                case pv2::STREET_RIVER:
                  return 1;
                default:
                  return 0;  // PREFLOP has no board deal
              }
            }();
            if (expectedCards > 0 && deal.cards_size() != expectedCards)
              reject(eventField + ".board_deal.cards",
                     "board deal card count does not match street");
            previousStreet = deal.street();
          }
          if (deal.cards_size() < 1 || deal.cards_size() > 3)
            reject(eventField + ".board_deal.cards", "board deal must carry 1..3 cards");
          for (int c = 0; c < deal.cards_size(); ++c) {
            const pv2::Card& card = deal.cards(c);
            checkCard(card, eventField + ".board_deal.cards[" + std::to_string(c) + "]");
            // Duplicate card detection.
            const int cardId = static_cast<int>(card.rank()) * 4 + static_cast<int>(card.suit());
            if (!dealtCards.insert(cardId).second)
              reject(eventField + ".board_deal.cards[" + std::to_string(c) + "]",
                     "duplicate board card");
            ++boardCardsDealt;
          }
          break;
        }
        case pv2::TransitionEvent::TRANSITION_NOT_SET:
          reject(eventField, "transition event requires exactly one transition");
          break;
      }
    }

    // Board cards from transition path must match the declared board.
    if (boardCardsDealt != state.board_size())
      reject(field + ".board", "board card count does not match transition path board deals");
  }

  void checkProvenance(const pv2::ObservationProvenance& provenance, const std::string& field,
                       int transitionPathSize) {
    if (!isKnownObservationSource(provenance.source()))
      reject(field + ".source", "observation source is outside the closed set");
    if (!isKnownObservationCoverage(provenance.coverage()))
      reject(field + ".coverage", "observation coverage is outside the closed set");
    stringField(provenance.controller_epoch(), 1, 128, field + ".controller_epoch");
    if (provenance.observed_event_count() != static_cast<std::uint32_t>(transitionPathSize))
      reject(field + ".observed_event_count",
             "observed event count does not match transition path length");
    // FULL coverage requires a non-empty transition path.
    if (provenance.coverage() == pv2::OBSERVATION_COVERAGE_FULL && transitionPathSize == 0)
      rejectHistoryIncomplete(field + ".coverage",
                              "FULL coverage requires a non-empty transition path");
  }

  void checkLegal(const pv2::HandState& state, const std::string& field,
                  const std::unordered_map<std::uint32_t, const pv2::PlayerState*>& players) {
    if (state.legal_actions_size() < 1 || state.legal_actions_size() > 5)
      reject(field + ".legal_actions", "legal actions must have 1..5 entries");

    const pv2::PlayerState* hero = nullptr;
    if (players.contains(state.hero_seat()))
      hero = players.at(state.hero_seat());
    if (!hero) {
      reject(field + ".hero_seat", "hero seat references unknown player");
      return;
    }

    const std::uint64_t heroCapacity = hero->stack() + hero->street_committed();
    // to_call must not exceed hero capacity.
    if (state.to_call() > heroCapacity)
      reject(field + ".to_call", "to_call exceeds hero capacity");

    std::unordered_set<int> seenKinds;
    bool hasBet = false;
    bool hasRaise = false;
    bool hasCheck = false;
    bool hasCall = false;
    for (int i = 0; i < state.legal_actions_size(); ++i) {
      const pv2::LegalAction& legal = state.legal_actions(i);
      const std::string legalField = field + ".legal_actions[" + std::to_string(i) + "]";
      if (!isKnownActionType(legal.type())) {
        reject(legalField + ".type", "legal action type is outside the closed set");
        continue;
      }
      if (!seenKinds.insert(static_cast<int>(legal.type())).second)
        reject(legalField + ".type", "duplicate legal action type");
      if (legal.type() == pv2::ACTION_TYPE_BET)
        hasBet = true;
      if (legal.type() == pv2::ACTION_TYPE_RAISE)
        hasRaise = true;
      if (legal.type() == pv2::ACTION_TYPE_CHECK)
        hasCheck = true;
      if (legal.type() == pv2::ACTION_TYPE_CALL)
        hasCall = true;
      if (legal.type() == pv2::ACTION_TYPE_BET || legal.type() == pv2::ACTION_TYPE_RAISE) {
        if (!legal.has_min_target_total() || !legal.has_max_target_total())
          reject(legalField, "bet/raise legal action requires min and max target");
        else {
          if (legal.min_target_total() == 0)
            reject(legalField + ".min_target_total", "min target must be positive");
          if (legal.min_target_total() > legal.max_target_total())
            reject(legalField, "min target cannot exceed max target");
          if (legal.max_target_total() > heroCapacity)
            reject(legalField + ".max_target_total",
                   "max target exceeds hero stack + street_committed");
        }
      }
    }
    if (hasBet && hasRaise)
      reject(field + ".legal_actions", "bet and raise are mutually exclusive legal actions");

    // to_call consistency: check is legal when nothing owed, call when owed.
    if (state.to_call() == 0) {
      if (hasCall)
        reject(field + ".legal_actions", "call is not legal when to_call is zero");
      if (!hasCheck)
        reject(field + ".legal_actions", "check must be legal when to_call is zero");
    } else {
      if (hasCheck)
        reject(field + ".legal_actions", "check is not legal when to_call is positive");
      if (!hasCall)
        reject(field + ".legal_actions", "call must be legal when to_call is positive");
    }
  }

  void checkOptions(const pv2::DecisionOptions& options, const std::string& field) {
    stringField(options.strategy_profile(), 1, 64, field + ".strategy_profile");
    const std::string& profile = options.strategy_profile();
    if (profile != "tag" && profile != "lag" && profile != "station-hunter")
      rejectFeature(field + ".strategy_profile", "unsupported strategy profile");
    if (options.solve_time_budget_ms() == 0 || options.solve_time_budget_ms() > 120000)
      reject(field + ".solve_time_budget_ms", "solve budget must be in 1..120000 ms");
    if (!isKnownSolverMode(options.solver_mode())) {
      reject(field + ".solver_mode", "solver mode is outside the closed set");
    } else {
      // Only AUTOMATIC, HEURISTIC, BLUEPRINT, and RESOLVING are selectable.
      // RIVER_LP, RIVER_DCFR, and MULTISTREET_CFR are not force-selectable.
      switch (options.solver_mode()) {
        case pv2::SOLVER_MODE_AUTOMATIC:
        case pv2::SOLVER_MODE_HEURISTIC:
        case pv2::SOLVER_MODE_BLUEPRINT:
        case pv2::SOLVER_MODE_RESOLVING:
          break;
        default:
          rejectFeature(field + ".solver_mode", "solver mode is not force-selectable");
          break;
      }
    }
    if (options.has_preflop_effective_stack_bb()) {
      const double es = options.preflop_effective_stack_bb();
      if (!std::isfinite(es) || es <= 0.0 || es > kMaxEffectiveStackBbHint)
        reject(field + ".preflop_effective_stack_bb",
               "preflop effective stack must be finite, positive, and <= 10000");
    }
    if (options.has_minimum_guarantee() && !isKnownGuaranteeLevel(options.minimum_guarantee()))
      reject(field + ".minimum_guarantee", "guarantee level is outside the closed set");
    if (!isKnownUtilityProfile(options.utility_profile())) {
      reject(field + ".utility_profile", "utility profile is outside the closed set");
    } else if (options.utility_profile() == pv2::UTILITY_PROFILE_TOURNAMENT_ICM) {
      rejectUnknownUtility(field + ".utility_profile", "tournament ICM utility is not supported");
    }
  }

  void checkHandState(const pv2::HandState& state, const std::string& field) {
    if (!state.has_game()) {
      reject(field + ".game", "hand state requires a game definition");
      return;
    }
    checkGameDefinition(state.game(), field + ".game");

    stringField(state.hand_id(), 1, 128, field + ".hand_id");
    if (!isKnownStreet(state.street()))
      reject(field + ".street", "street is outside the closed set");
    else if (state.street() == pv2::STREET_SHOWDOWN)
      reject(field + ".street", "SHOWDOWN street is not actionable");
    if (state.button_seat() > 9)
      reject(field + ".button_seat", "button seat must be at most 9");
    if (state.hero_seat() > 9)
      reject(field + ".hero_seat", "hero seat must be at most 9");

    // Players.
    if (state.players_size() < 2 || state.players_size() > 10)
      reject(field + ".players", "players must have 2..10 entries");

    std::unordered_map<std::uint32_t, const pv2::PlayerState*> players;
    for (int i = 0; i < state.players_size(); ++i) {
      const pv2::PlayerState& player = state.players(i);
      const std::string playerField = field + ".players[" + std::to_string(i) + "]";
      checkPlayer(player, playerField, state.game().table_capacity());
      if (players.contains(player.seat()))
        reject(playerField + ".seat", "duplicate player seat");
      players[player.seat()] = &player;
    }

    // Cross-field: dealt_seat_count must equal player count.
    if (state.game().has_rule_profile()) {
      const std::uint32_t dealt = state.game().rule_profile().dealt_seat_count();
      if (dealt != static_cast<std::uint32_t>(state.players_size()))
        reject(field + ".game.rule_profile.dealt_seat_count",
               "dealt_seat_count must equal the number of players");
    }

    // Hero must be present and ACTIVE.
    if (players.contains(state.hero_seat())) {
      const pv2::PlayerState* hero = players.at(state.hero_seat());
      if (hero->status() != pv2::PLAYER_STATUS_ACTIVE)
        reject(field + ".hero_seat", "hero must be ACTIVE");
    }

    // Hero hole cards.
    if (state.hero_hole_cards_size() != 2)
      reject(field + ".hero_hole_cards", "hero hole cards must have exactly 2 entries");
    for (int i = 0; i < state.hero_hole_cards_size(); ++i)
      checkCard(state.hero_hole_cards(i), field + ".hero_hole_cards[" + std::to_string(i) + "]");

    // Board.
    if (state.board_size() > 5)
      reject(field + ".board", "board must have at most 5 cards");
    for (int i = 0; i < state.board_size(); ++i)
      checkCard(state.board(i), field + ".board[" + std::to_string(i) + "]");

    // Pot state.
    if (!state.has_pot()) {
      reject(field + ".pot", "hand state requires a pot state");
    } else {
      checkPotState(state.pot(), field + ".pot", state.players_size(), players);
    }

    // Forced contributions.
    for (int i = 0; i < state.forced_contributions_size(); ++i) {
      const pv2::ForcedContribution& forced = state.forced_contributions(i);
      const std::string forcedField = field + ".forced_contributions[" + std::to_string(i) + "]";
      if (forced.seat() > 9)
        reject(forcedField + ".seat", "forced contribution seat must be at most 9");
      if (!isKnownForcedType(forced.type())) {
        reject(forcedField + ".type", "forced contribution type is outside the closed set");
      } else if (forced.type() != pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
                 forced.type() != pv2::FORCED_CONTRIBUTION_TYPE_BIG_BLIND) {
        rejectFeature(forcedField + ".type", "antes, dead blinds, and straddles are unsupported");
      }
      if (forced.amount() == 0)
        reject(forcedField + ".amount", "forced contribution amount must be positive");
      chipFitsInt(forced.amount(), forcedField + ".amount", "forced contribution amount");
      if (!players.contains(forced.seat()))
        reject(forcedField + ".seat", "forced contribution references unknown player");
    }

    // Transition path.
    checkTransitionPath(state, field, players);

    // Observation provenance.
    if (!state.has_provenance()) {
      reject(field + ".provenance", "hand state requires observation provenance");
    } else {
      checkProvenance(state.provenance(), field + ".provenance", state.transition_path_size());
    }

    // Legal actions.
    checkLegal(state, field, players);
  }
};

}  // namespace

pv2::ErrorCode validateDecisionRequest(const pv2::DecisionRequest& request,
                                       ValidationReport& report) {
  SemanticValidator validator(request);
  const pv2::ErrorCode code = validator.run();
  for (const pv2::FieldViolation& violation : validator.report().violations)
    report.violations.push_back(violation);
  return code;
}

// Delegates to the shared v1 implementation (RFC 3629 validation). The v2
// protocol library links v1 PUBLIC, so the symbol is available.
bool isValidUtf8(std::string_view value) {
  return bs::v1::isValidUtf8(value);
}

}  // namespace bs::v2
