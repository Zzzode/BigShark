// request_mapper.cpp — RFC 0022 wire HandState -> heuristic bs::Ctx.
//
// Maps the DecisionRequest into the heuristic context consumed by
// bs::decideSourced. The wire format uses seat-based identity (not
// player_id strings), a TransitionEvent path with explicit BoardDeal
// boundaries (not a flat ActionEvent history), and the G/F contribution
// ledger. This mapper extracts the public action history from the transition
// path and builds the same Ctx knobs the mapper produces.
#include <algorithm>
#include <array>
#include <bs/decision.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "mappers.hpp"

namespace bs {
namespace {

namespace pe = ::bigshark::engine;

[[noreturn]] void rejectMapping(const std::string& field, const std::string& description) {
  throw MappingError(pe::ERROR_CODE_INVALID_REQUEST, description + " (" + field + ")", false);
}

const char* streetToken(pe::Street street) {
  switch (street) {
    case pe::STREET_PREFLOP:
      return "preflop";
    case pe::STREET_FLOP:
      return "flop";
    case pe::STREET_TURN:
      return "turn";
    case pe::STREET_RIVER:
      return "river";
    default:
      rejectMapping("state.street", "unknown decision street in mapper");
  }
}

// evaluatePolicy parses cards as a rank character from "23456789TJQKA"
// followed by a suit character from "shdc".
std::string cardToken(const pe::Card& card) {
  static const char* kRanks = "23456789TJQKA";
  std::string token;
  token.push_back(kRanks[static_cast<int>(card.rank()) - 1]);
  switch (card.suit()) {
    case pe::SUIT_SPADES:
      token.push_back('s');
      break;
    case pe::SUIT_HEARTS:
      token.push_back('h');
      break;
    case pe::SUIT_DIAMONDS:
      token.push_back('d');
      break;
    case pe::SUIT_CLUBS:
      token.push_back('c');
      break;
    default:
      rejectMapping("card.suit", "unknown card suit in mapper");
  }
  return token;
}

// Position labels derived from ordered occupied seats relative to the button.
std::string positionLabel(int indexFromButton, int playerCount) {
  if (playerCount <= 2)
    return indexFromButton == 0 ? "BTN" : "BB";
  switch (indexFromButton) {
    case 0:
      return "BTN";
    case 1:
      return "SB";
    case 2:
      return "BB";
    default: {
      const int distanceFromButton = playerCount - indexFromButton;
      if (distanceFromButton == 1)
        return "CO";
      if (distanceFromButton == 2)
        return "HJ";
      return "UTG";
    }
  }
}

char actionCode(pe::ActionType action) {
  switch (action) {
    case pe::ACTION_TYPE_FOLD:
      return 'f';
    case pe::ACTION_TYPE_CHECK:
      return 'x';
    case pe::ACTION_TYPE_CALL:
      return 'c';
    case pe::ACTION_TYPE_BET:
      return 'b';
    case pe::ACTION_TYPE_RAISE:
      return 'r';
    default:
      rejectMapping("action", "unknown action type in mapper");
  }
}

// River line state machine mirrored from the River normalizer.
std::string transitionRiverLine(const std::string& line, pe::ActionType action) {
  auto step =
      [&](std::initializer_list<std::pair<pe::ActionType, const char*>> table) -> std::string {
    for (const auto& [candidate, next] : table)
      if (action == candidate)
        return next;
    return "";
  };
  if (line.empty())
    return step({{pe::ACTION_TYPE_CHECK, "c"}, {pe::ACTION_TYPE_BET, "b"}});
  if (line == "c")
    return step({{pe::ACTION_TYPE_CHECK, "cc"}, {pe::ACTION_TYPE_BET, "cb"}});
  if (line == "b")
    return step({{pe::ACTION_TYPE_FOLD, "bf"},
                 {pe::ACTION_TYPE_CALL, "bc"},
                 {pe::ACTION_TYPE_RAISE, "br"}});
  if (line == "cb")
    return step({{pe::ACTION_TYPE_FOLD, "cbf"},
                 {pe::ACTION_TYPE_CALL, "cbc"},
                 {pe::ACTION_TYPE_RAISE, "cbr"}});
  if (line == "br")
    return step({{pe::ACTION_TYPE_FOLD, "brf"}, {pe::ACTION_TYPE_CALL, "brc"}});
  if (line == "cbr")
    return step({{pe::ACTION_TYPE_FOLD, "cbrf"}, {pe::ACTION_TYPE_CALL, "cbrc"}});
  return "";
}

bool isRiverTerminal(const std::string& line) {
  static const std::array<const char*, 9> kTerminals = {"cc",  "bf",  "bc",   "cbf", "cbc",
                                                        "brf", "brc", "cbrf", "cbrc"};
  return std::any_of(kTerminals.begin(), kTerminals.end(),
                     [&](const char* terminal) { return line == terminal; });
}

int asInt(std::uint64_t value) {
  return static_cast<int>(value);
}

}  // namespace

pe::ErrorCode validateAndMap(const pe::DecisionRequest& request, ValidationReport& report,
                             bs::Ctx& context) {
  const pe::HandState& state = request.state();
  const pe::DecisionOptions& options = request.options();

  context = bs::Ctx{};
  context.handId = state.hand_id();
  context.revision = static_cast<long long>(state.decision_index());
  context.seed = options.seed();
  context.style = options.strategy_profile();

  context.street = streetToken(state.street());

  context.sb = asInt(state.game().small_blind());
  context.bb = asInt(state.game().big_blind());

  // Players sorted by seat for position derivation.
  std::vector<const pe::PlayerState*> bySeat;
  for (const pe::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pe::PlayerState* a, const pe::PlayerState* b) {
    return a->seat() < b->seat();
  });
  const int playerCount = static_cast<int>(bySeat.size());
  int buttonOffset = 0;
  for (int i = 0; i < playerCount; ++i)
    if (bySeat[static_cast<std::size_t>(i)]->seat() == state.button_seat()) {
      buttonOffset = i;
      break;
    }

  // Seat-based position map and hero lookup.
  std::unordered_map<uint32_t, std::string> positions;
  const pe::PlayerState* hero = nullptr;
  for (int i = 0; i < playerCount; ++i) {
    const pe::PlayerState* player =
        bySeat[static_cast<std::size_t>((buttonOffset + i) % playerCount)];
    positions.emplace(player->seat(), positionLabel(i, playerCount));
    if (player->seat() == state.hero_seat())
      hero = player;
  }
  if (hero == nullptr) {
    report.add("state.hero_seat", "hero seat not found in players");
    return pe::ERROR_CODE_INVALID_REQUEST;
  }
  context.position = positions.at(state.hero_seat());
  context.playersInHand = playerCount;

  // Effective stack computation.
  std::int64_t shortestOpponentTotal = std::numeric_limits<std::int64_t>::max();
  int liveOpponents = 0;
  for (const pe::PlayerState* player : bySeat) {
    if (player->seat() == state.hero_seat())
      continue;
    if (player->status() == pe::PLAYER_STATUS_FOLDED)
      continue;
    ++liveOpponents;
    const std::int64_t total = static_cast<std::int64_t>(player->stack()) +
                               static_cast<std::int64_t>(player->street_committed());
    shortestOpponentTotal = std::min(shortestOpponentTotal, total);
  }

  if (options.has_preflop_effective_stack_bb()) {
    context.effectiveStackBb = options.preflop_effective_stack_bb();
  } else {
    const std::int64_t heroBehind = static_cast<std::int64_t>(hero->stack());
    const std::int64_t heroTotalChips =
        heroBehind + static_cast<std::int64_t>(hero->street_committed());
    const std::int64_t effectiveChips =
        (liveOpponents == 1) ? std::min(heroTotalChips, shortestOpponentTotal) : heroBehind;
    context.effectiveStackBb =
        static_cast<double>(effectiveChips) / static_cast<double>(context.bb);
  }

  for (const pe::Card& card : state.hero_hole_cards())
    context.hole.push_back(cardToken(card));
  for (const pe::Card& card : state.board())
    context.board.push_back(cardToken(card));

  context.pot = asInt(state.pot().pot_total());

  const int toCall = asInt(state.to_call());
  if (static_cast<std::uint64_t>(context.pot) + static_cast<std::uint64_t>(toCall) >
      static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw MappingError(pe::ERROR_CODE_INVALID_REQUEST,
                       "pot_total plus to_call exceeds the heuristic integer range (mapper)",
                       false);
  }
  context.legal.call = toCall;
  const double potPlusCall = static_cast<double>(context.pot) + static_cast<double>(toCall);
  if (potPlusCall > 0.0) {
    const double odds = static_cast<double>(toCall) / potPlusCall;
    context.legal.potOdds = std::round(odds * 10000.0) / 10000.0;
  }
  for (const pe::LegalAction& action : state.legal_actions()) {
    const char* token;
    switch (action.type()) {
      case pe::ACTION_TYPE_FOLD:
        token = "fold";
        break;
      case pe::ACTION_TYPE_CHECK:
        token = "check";
        break;
      case pe::ACTION_TYPE_CALL:
        token = "call";
        break;
      case pe::ACTION_TYPE_BET:
        token = "bet";
        break;
      case pe::ACTION_TYPE_RAISE:
        token = "raise";
        break;
      default:
        rejectMapping("legal_actions.type", "unknown legal action type in mapper");
    }
    context.legal.actions.emplace_back(token);
    if (action.type() == pe::ACTION_TYPE_BET || action.type() == pe::ACTION_TYPE_RAISE) {
      context.legal.raiseMin = asInt(action.min_target_total());
      context.legal.raiseMax = asInt(action.max_target_total());
    }
  }

  // Extract ActionEvents from the TransitionEvent path for the heuristic
  // action history. BoardDeal events are skipped (the board is already in
  // state.board).
  struct HistoryEntry {
    pe::Street street;
    uint32_t actor_seat;
    pe::ActionType action;
  };
  std::vector<HistoryEntry> history;
  for (const pe::TransitionEvent& event : state.transition_path()) {
    if (event.has_action())
      history.push_back(
          {event.action().street(), event.action().actor_seat(), event.action().action()});
  }

  // Voluntary action history reconstructed into the preflop heuristic knobs
  // and the actor-tagged per-street line strings the river solver reads.
  int preflopRaises = 0;
  int preflopCalls = 0;
  uint32_t openerSeat = 0;
  uint32_t lastRaiserSeat = 0;
  bool hasOpener = false;
  bool hasLastRaiser = false;

  struct RoundToken {
    bool hero;
    char code;
  };
  std::vector<std::vector<RoundToken>> rounds(1);
  bool roundRaised = false;
  int passiveActions = 0;
  bool handEnded = false;

  for (const HistoryEntry& entry : history) {
    const bool isHero = entry.actor_seat == state.hero_seat();
    const char code = actionCode(entry.action);

    if (entry.street == pe::STREET_PREFLOP) {
      if (entry.action == pe::ACTION_TYPE_RAISE) {
        ++preflopRaises;
        if (!hasOpener) {
          openerSeat = entry.actor_seat;
          hasOpener = true;
        }
        lastRaiserSeat = entry.actor_seat;
        hasLastRaiser = true;
      } else if (entry.action == pe::ACTION_TYPE_CALL) {
        ++preflopCalls;
      }
    } else if (entry.action == pe::ACTION_TYPE_RAISE) {
      lastRaiserSeat = entry.actor_seat;
      hasLastRaiser = true;
    }

    if (handEnded)
      continue;
    rounds.back().push_back({isHero, code});
    if (code == 'f') {
      handEnded = true;
      continue;
    }
    if (code == 'b' || code == 'r') {
      roundRaised = true;
      passiveActions = 0;
      continue;
    }
    if (code == 'c' && (roundRaised || rounds.size() > 1)) {
      rounds.emplace_back();
      roundRaised = false;
      passiveActions = 0;
      continue;
    }
    ++passiveActions;
    if (passiveActions >= 2) {
      rounds.emplace_back();
      roundRaised = false;
      passiveActions = 0;
    }
  }

  if (state.street() == pe::STREET_PREFLOP) {
    context.raises = preflopRaises;
    context.limpers = preflopCalls;
    if (hasOpener) {
      auto found = positions.find(openerSeat);
      if (found != positions.end())
        context.openerPosition = found->second;
    }
  }
  context.heroWasRaiser = hasLastRaiser && lastRaiserSeat == state.hero_seat();
  context.heroPreflopAggressor = context.heroWasRaiser;

  // River equilibrium: heads-up, river, complete fold-free action.
  if (state.street() == pe::STREET_RIVER && state.board_size() == 5 && playerCount == 2 &&
      !handEnded && rounds.size() >= 4) {
    auto lineForRound = [](const std::vector<RoundToken>& round) {
      std::string line;
      for (const RoundToken& token : round) {
        if (!line.empty())
          line.push_back(',');
        line.push_back(token.hero ? 'H' : 'O');
        line.push_back(token.code);
      }
      return line;
    };
    const std::string flopLine = lineForRound(rounds[1]);
    const std::string turnLine = lineForRound(rounds[2]);

    std::string riverLine;
    bool riverValid = true;
    for (const RoundToken& token : rounds[3]) {
      pe::ActionType action = pe::ACTION_TYPE_UNSPECIFIED;
      switch (token.code) {
        case 'x':
          action = pe::ACTION_TYPE_CHECK;
          break;
        case 'c':
          action = pe::ACTION_TYPE_CALL;
          break;
        case 'b':
          action = pe::ACTION_TYPE_BET;
          break;
        case 'r':
          action = pe::ACTION_TYPE_RAISE;
          break;
        case 'f':
          action = pe::ACTION_TYPE_FOLD;
          break;
        default:
          break;
      }
      riverLine = transitionRiverLine(riverLine, action);
      if (riverLine.empty() || isRiverTerminal(riverLine)) {
        riverValid = false;
        break;
      }
    }

    if (riverValid) {
      context.riverGtoOn = true;
      context.riverLine = riverLine;
      context.flopLine = flopLine;
      context.turnLine = turnLine;
      if (toCall > 0 && context.pot > 0) {
        context.riverBetFrac =
            std::round(static_cast<double>(toCall) / context.pot * 1000.0) / 1000.0;
      }
    }
  }

  return pe::ERROR_CODE_UNSPECIFIED;
}

}  // namespace bs
