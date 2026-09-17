#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "v1_mappers.hpp"

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

[[noreturn]] void rejectMapping(const std::string& field, const std::string& description) {
  throw MappingError(pv::ERROR_CODE_INVALID_REQUEST, description + " (" + field + ")", false);
}

// Defense in depth: the validator rejects closed-set violations before the
// mapper runs, but the mappers themselves never silently turn an unknown enum
// into an empty street, a one-character card, or a dropped action.
const char* streetToken(pv::Street street) {
  switch (street) {
    case pv::STREET_PREFLOP:
      return "preflop";
    case pv::STREET_FLOP:
      return "flop";
    case pv::STREET_TURN:
      return "turn";
    case pv::STREET_RIVER:
      return "river";
    default:
      rejectMapping("state.street", "unknown decision street in mapper");
  }
}

// v0 evaluatePolicy parses cards as a rank character from "23456789TJQKA"
// followed by a suit character from "shdc" (spades, hearts, diamonds, clubs).
std::string cardToken(const pv::Card& card) {
  static const char* kRanks = "23456789TJQKA";
  if (!isKnownRank(card.rank()) || !isKnownSuit(card.suit()))
    rejectMapping("card", "unknown card rank or suit in mapper");
  std::string token;
  token.push_back(kRanks[static_cast<int>(card.rank()) - 1]);
  switch (card.suit()) {
    case pv::SUIT_SPADES:
      token.push_back('s');
      break;
    case pv::SUIT_HEARTS:
      token.push_back('h');
      break;
    case pv::SUIT_DIAMONDS:
      token.push_back('d');
      break;
    case pv::SUIT_CLUBS:
      token.push_back('c');
      break;
    default:
      rejectMapping("card.suit", "unknown card suit in mapper");
  }
  return token;
}

// Position labels derived from ordered occupied seats relative to the button.
// With more than two players the rotational order is BTN, SB, BB, then early
// to late positions counting back from the button: CO, HJ, UTG.
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

char actionCode(pv::ActionType action) {
  switch (action) {
    case pv::ACTION_TYPE_FOLD:
      return 'f';
    case pv::ACTION_TYPE_CHECK:
      return 'x';
    case pv::ACTION_TYPE_CALL:
      return 'c';
    case pv::ACTION_TYPE_BET:
      return 'b';
    case pv::ACTION_TYPE_RAISE:
      return 'r';
    default:
      rejectMapping("action_history.action", "unknown action type in mapper");
  }
}

// River line state machine mirrored from the v0 River normalizer: each public
// river action transitions a compact line token; terminals mean the decision
// is no longer on hero and disable the river solver.
std::string transitionRiverLine(const std::string& line, pv::ActionType action) {
  auto step =
      [&](std::initializer_list<std::pair<pv::ActionType, const char*>> table) -> std::string {
    for (const auto& [candidate, next] : table)
      if (action == candidate)
        return next;
    return "";
  };
  if (line.empty())
    return step({{pv::ACTION_TYPE_CHECK, "c"}, {pv::ACTION_TYPE_BET, "b"}});
  if (line == "c")
    return step({{pv::ACTION_TYPE_CHECK, "cc"}, {pv::ACTION_TYPE_BET, "cb"}});
  if (line == "b")
    return step({{pv::ACTION_TYPE_FOLD, "bf"},
                 {pv::ACTION_TYPE_CALL, "bc"},
                 {pv::ACTION_TYPE_RAISE, "br"}});
  if (line == "cb")
    return step({{pv::ACTION_TYPE_FOLD, "cbf"},
                 {pv::ACTION_TYPE_CALL, "cbc"},
                 {pv::ACTION_TYPE_RAISE, "cbr"}});
  if (line == "br")
    return step({{pv::ACTION_TYPE_FOLD, "brf"}, {pv::ACTION_TYPE_CALL, "brc"}});
  if (line == "cbr")
    return step({{pv::ACTION_TYPE_FOLD, "cbrf"}, {pv::ACTION_TYPE_CALL, "cbrc"}});
  return "";
}

bool isRiverTerminal(const std::string& line) {
  static const std::array<const char*, 9> kTerminals = {"cc",  "bf",  "bc",   "cbf", "cbc",
                                                        "brf", "brc", "cbrf", "cbrc"};
  return std::any_of(kTerminals.begin(), kTerminals.end(),
                     [&](const char* terminal) { return line == terminal; });
}

int asInt(std::uint64_t value) {
  return static_cast<int>(value);  // callers validated int fit
}

}  // namespace

pv::ErrorCode validateAndMap(const pv::DecisionRequest& request, ValidationReport& report,
                             bs::Ctx& context, unsigned negotiated_minor) {
  pv::ErrorCode code = validateDecisionRequest(request, report, negotiated_minor);
  if (code != pv::ERROR_CODE_UNSPECIFIED)
    return code;

  const pv::HandState& state = request.state();
  const pv::DecisionOptions& options = request.options();

  context = bs::Ctx{};
  context.handId = state.hand_id();
  if (state.decision_index() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    report.add("state.decision_index", "decision index exceeds the heuristic int range");
    return pv::ERROR_CODE_INVALID_REQUEST;
  }
  context.revision = static_cast<long long>(state.decision_index());
  context.seed = options.seed();
  context.style = options.strategy_profile();

  const char* street = streetToken(state.street());
  context.street = street;

  context.sb = asInt(state.game().small_blind());
  context.bb = asInt(state.game().big_blind());

  // Players rotated into post-button order for position derivation.
  std::vector<const pv::PlayerState*> bySeat;
  for (const pv::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pv::PlayerState* a, const pv::PlayerState* b) {
    return a->seat() < b->seat();
  });
  const int playerCount = static_cast<int>(bySeat.size());
  int buttonOffset = 0;
  for (int i = 0; i < playerCount; ++i)
    if (bySeat[static_cast<std::size_t>(i)]->seat() == state.button_seat()) {
      buttonOffset = i;
      break;
    }
  std::unordered_map<std::string, std::string> positions;
  const pv::PlayerState* hero = nullptr;
  for (int i = 0; i < playerCount; ++i) {
    const pv::PlayerState* player =
        bySeat[static_cast<std::size_t>((buttonOffset + i) % playerCount)];
    positions.emplace(player->player_id(), positionLabel(i, playerCount));
    if (player->player_id() == state.hero_player_id())
      hero = player;
  }
  context.position = positions.at(state.hero_player_id());
  // playersInHand mirrors the v0 solver field semantics: the count of players
  // dealt into the hand as carried in structured state (v0 passes the server
  // value, which includes folded/leaving seats on the frozen snapshots). The
  // effective-depth computation below filters folded opponents separately.
  context.playersInHand = playerCount;

  std::int64_t shortestOpponentTotal = std::numeric_limits<std::int64_t>::max();
  int liveOpponents = 0;
  for (const pv::PlayerState* player : bySeat) {
    if (player->player_id() == state.hero_player_id())
      continue;
    if (player->status() == pv::PLAYER_STATUS_FOLDED)
      continue;
    ++liveOpponents;
    const std::int64_t total = static_cast<std::int64_t>(player->stack()) +
                               static_cast<std::int64_t>(player->street_committed());
    shortestOpponentTotal = std::min(shortestOpponentTotal, total);
  }

  // Preflop effective-stack depth for the <=12bb short-stack jam heuristic.
  // A platform-computed hint (options.preflop_effective_stack_bb) gives exact
  // parity with the v0 path and wins when present; the validator guarantees it
  // is finite, positive, and bounded. Without a hint the host reconstructs a
  // physical approximation from structured chips:
  //   heads-up -> min(hero, opponent) of stack_behind + street_committed
  //   multiway -> hero stack_behind
  // then / big_blind. The fallback is a physical estimate (not the platform
  // solver value); it is decision-equivalent on the frozen and 156-room
  // corpora but can differ in the full input space, which is why the hint
  // exists.
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

  for (const pv::Card& card : state.hero_hole_cards())
    context.hole.push_back(cardToken(card));
  for (const pv::Card& card : state.board())
    context.board.push_back(cardToken(card));

  context.pot = asInt(state.pot().pot_total());

  // Defensive derived-sum boundary (the validator rejects first): pot and
  // to_call each fit int but their sum is a derived heuristic input, so it
  // must fit int as well.
  const int toCall = asInt(state.to_call());
  if (static_cast<std::uint64_t>(context.pot) + static_cast<std::uint64_t>(toCall) >
      static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw MappingError(pv::ERROR_CODE_INVALID_REQUEST,
                       "pot_total plus to_call exceeds the heuristic integer range (mapper)",
                       false);
  }
  context.legal.call = toCall;
  // The denominator is widened to double; pot and call are below 2^31 so any
  // exact sum is representable and potOdds matches the platform rounding.
  const double potPlusCall = static_cast<double>(context.pot) + static_cast<double>(toCall);
  if (potPlusCall > 0.0) {
    const double odds = static_cast<double>(toCall) / potPlusCall;
    context.legal.potOdds = std::round(odds * 10000.0) / 10000.0;
  }
  for (const pv::LegalAction& action : state.legal_actions()) {
    if (!isKnownActionType(action.type()))
      rejectMapping("legal_actions.type", "unknown legal action type in mapper");
    const char* token;
    switch (action.type()) {
      case pv::ACTION_TYPE_FOLD:
        token = "fold";
        break;
      case pv::ACTION_TYPE_CHECK:
        token = "check";
        break;
      case pv::ACTION_TYPE_CALL:
        token = "call";
        break;
      case pv::ACTION_TYPE_BET:
        token = "bet";
        break;
      case pv::ACTION_TYPE_RAISE:
        token = "raise";
        break;
      default:
        rejectMapping("legal_actions.type", "unknown legal action type in mapper");
    }
    context.legal.actions.emplace_back(token);
    if (action.type() == pv::ACTION_TYPE_BET || action.type() == pv::ACTION_TYPE_RAISE) {
      context.legal.raiseMin = asInt(action.min_target_total());
      context.legal.raiseMax = asInt(action.max_target_total());
    }
  }

  // Voluntary action history reconstructed into the exact preflop heuristic
  // knobs and the actor-tagged per-street line strings the river solver reads.
  int preflopRaises = 0;
  int preflopCalls = 0;
  std::string openerId;
  std::string lastRaiserId;

  // The river solver knobs were designed around the v0 normalizer's public
  // action-round grouping, which partitions the entire voluntary sequence by
  // betting-pattern tokens rather than by authoritative street tags. A call
  // closes an open round, two passive actions close an unchecked round, and a
  // fold ends the hand. Replaying the same algorithm is required for byte
  // parity with v0 decisions (e.g. a lone river check belongs to the round
  // that opened before it).
  struct RoundToken {
    bool hero;
    char code;
  };
  std::vector<std::vector<RoundToken>> rounds(1);
  bool roundRaised = false;
  int passiveActions = 0;
  bool handEnded = false;

  for (const pv::ActionEvent& event : state.action_history()) {
    const bool isHero = event.actor_player_id() == state.hero_player_id();
    // An empty actor id marks an opener the platform could not resolve to a
    // seat (v0 first-token name miss); raise/call counts still accrue, but
    // openerId/lastRaiser attribution is skipped.
    const bool actorResolved = !event.actor_player_id().empty();
    const char code = actionCode(event.action());

    if (event.street() == pv::STREET_PREFLOP) {
      if (event.action() == pv::ACTION_TYPE_RAISE) {
        ++preflopRaises;
        if (actorResolved) {
          if (openerId.empty())
            openerId = event.actor_player_id();
          lastRaiserId = event.actor_player_id();
        }
      } else if (event.action() == pv::ACTION_TYPE_CALL) {
        ++preflopCalls;
      }
    } else if (actorResolved && event.action() == pv::ACTION_TYPE_RAISE) {
      lastRaiserId = event.actor_player_id();
    }

    // Unknown action enums were rejected by the validator and actionCode
    // throws defensively; every replay token is therefore one of f/x/c/b/r.
    // v0 stops replaying rounds once a fold has ended the hand; preflop
    // counters above are still derived from the full structured history. An
    // unresolved actor cannot be attributed to hero or opponent and is
    // excluded from the line replay.
    if (handEnded || !actorResolved)
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

  // v0 only counted preflop raises/limpers while actually on the preflop
  // decision; on later streets those knobs stay zero.
  if (state.street() == pv::STREET_PREFLOP) {
    context.raises = preflopRaises;
    context.limpers = preflopCalls;
    if (!openerId.empty()) {
      auto found = positions.find(openerId);
      if (found != positions.end())
        context.openerPosition = found->second;
      // A missing id (unresolved opener) leaves openerPosition empty, which
      // the policy maps to its HJ default, matching v0.
    }
  }
  context.heroWasRaiser = !lastRaiserId.empty() && lastRaiserId == state.hero_player_id();
  context.heroPreflopAggressor = context.heroWasRaiser;

  // The river equilibrium runs only heads-up on the river with complete,
  // fold-free action matching the compact line grammar across four grouped
  // rounds.
  if (state.street() == pv::STREET_RIVER && state.board_size() == 5 && playerCount == 2 &&
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
      pv::ActionType action = pv::ACTION_TYPE_UNSPECIFIED;
      switch (token.code) {
        case 'x':
          action = pv::ACTION_TYPE_CHECK;
          break;
        case 'c':
          action = pv::ACTION_TYPE_CALL;
          break;
        case 'b':
          action = pv::ACTION_TYPE_BET;
          break;
        case 'r':
          action = pv::ACTION_TYPE_RAISE;
          break;
        case 'f':
          action = pv::ACTION_TYPE_FOLD;
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

  return pv::ERROR_CODE_UNSPECIFIED;
}

}  // namespace bs::v1
