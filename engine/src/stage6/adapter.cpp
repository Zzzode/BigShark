#include <bs/eval.hpp>
#include <bs/stage6/adapter.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace bs::stage6 {

namespace {

std::size_t button_relative_offset(std::size_t player_count, std::size_t button, std::size_t seat) {
  return (seat + player_count - button) % player_count;
}

bool is_aggressive(poker::ActionType type) {
  return type == poker::ActionType::Bet || type == poker::ActionType::Raise;
}

}  // namespace

std::string position_vocabulary(std::size_t player_count, std::size_t offset) {
  if (player_count == 2) {
    // Heads-up: the button posts the small blind and acts first preflop; the
    // other seat is the big blind.
    return offset == 0 ? "BTN" : "BB";
  }
  switch (offset) {
    case 0:
      return "BTN";
    case 1:
      return "SB";
    case 2:
      return "BB";
    case 3:
      return "UTG";
    default:
      if (offset + 1 == player_count)
        return "CO";  // n-1
      if (offset + 2 == player_count)
        return "HJ";  // n-2
      return "MP";    // 4..n-3 interior
  }
}

Ctx adapt_to_ctx(const poker::GameState& state, std::size_t seat, const std::array<int, 2>& hole,
                 const HandLog& log, std::uint64_t decision_seed, const AdapterConfig& config) {
  if (state.phase() != poker::Phase::Action)
    throw std::invalid_argument("adapt_to_ctx requires an action phase");
  const auto actor = state.actor();
  if (!actor || *actor != seat)
    throw std::invalid_argument("adapt_to_ctx requires the acting seat");

  const poker::GameDef& def = state.def();
  const std::size_t n = def.player_count;
  const poker::Chips bb = def.big_blind;
  if (bb == 0)
    throw std::invalid_argument("stage 6 fixture requires a posted big blind");

  Ctx c;
  c.handId = "stage6";
  c.revision = 0;
  c.seed = decision_seed;  // nonzero: postflop Monte Carlo uses it verbatim
  c.style = config.style;

  switch (state.street()) {
    case poker::Street::Preflop:
      c.street = "preflop";
      break;
    case poker::Street::Flop:
      c.street = "flop";
      break;
    case poker::Street::Turn:
      c.street = "turn";
      break;
    case poker::Street::River:
      c.street = "river";
      break;
  }

  c.sb = static_cast<int>(bb / 2);
  c.bb = static_cast<int>(bb);
  const std::size_t offset = button_relative_offset(n, def.button, seat);
  c.position = position_vocabulary(n, offset);

  int players_in_hand = 0;
  for (std::size_t p = 0; p < n; ++p)
    if (!state.players()[p].folded)
      ++players_in_hand;
  c.playersInHand = players_in_hand;

  const poker::Chips stack_behind = state.players()[seat].stack;
  c.effectiveStackBb = static_cast<double>(stack_behind) / static_cast<double>(bb);

  c.hole = {bs::cardName(hole[0]), bs::cardName(hole[1])};
  c.board.clear();
  for (int card : state.board())
    c.board.push_back(bs::cardName(card));

  c.pot = static_cast<int>(state.pot());

  const poker::LegalActions legal = state.legal();
  if (legal.fold)
    c.legal.actions.push_back("fold");
  if (legal.check)
    c.legal.actions.push_back("check");
  if (legal.call)
    c.legal.actions.push_back("call");
  if (legal.aggressive)
    // The aggressive verb follows the live server convention (pinned from
    // session journals): postflop, opening the betting is "bet" and wagering
    // over an existing bet is "raise"; preflop the only no-wager-owed state is
    // the big blind's option after a limp, which the server still calls
    // "raise", and the deployed chart's preflop branches gate on that token.
    c.legal.actions.push_back(
        state.street() == poker::Street::Preflop
            ? "raise"
            : (legal.aggressive->type == poker::ActionType::Bet ? "bet" : "raise"));
  c.legal.call = static_cast<int>(legal.call_amount);
  c.legal.potOdds = legal.call_amount > 0 ? static_cast<double>(legal.call_amount) /
                                                static_cast<double>(state.pot() + legal.call_amount)
                                          : 0.0;
  if (legal.aggressive) {
    c.legal.raiseMin = static_cast<int>(legal.aggressive->minimum);
    c.legal.raiseMax = static_cast<int>(legal.aggressive->maximum);
  }

  // Observation fields. These mirror the platform normalizer
  // (buildContext in the River Club mapper): the raises count, limpers, and
  // opener position are PREFLOP-ONLY fields (the platform's preflopRaises
  // list is empty on later streets); heroWasRaiser/heroPreflopAggressor are
  // the same boolean — whether the last raise event across every street so
  // far was this seat's.
  int preflop_raises = 0;
  int preflop_calls = 0;
  bool first_aggressor_seen = false;
  std::size_t opener_seat = 0;
  for (const LoggedAction& logged : log.preflop) {
    if (is_aggressive(logged.action.type)) {
      if (!first_aggressor_seen) {
        first_aggressor_seen = true;
        opener_seat = logged.seat;
      }
      ++preflop_raises;
    } else if (logged.action.type == poker::ActionType::Call) {
      ++preflop_calls;
    }
  }
  const bool on_preflop = state.street() == poker::Street::Preflop;
  c.raises = on_preflop ? preflop_raises : 0;
  c.limpers = on_preflop ? preflop_calls : 0;
  if (on_preflop && first_aggressor_seen) {
    const std::size_t opener_offset = button_relative_offset(n, def.button, opener_seat);
    c.openerPosition = position_vocabulary(n, opener_offset);
  }

  // Last raise in chronological order: later streets supersede earlier ones.
  const std::vector<LoggedAction>* street_logs[] = {&log.preflop, &log.flop, &log.turn, &log.river};
  bool last_raise_is_hero = false;
  for (const auto* street_log : street_logs)
    for (const LoggedAction& logged : *street_log)
      if (is_aggressive(logged.action.type))
        last_raise_is_hero = (logged.seat == seat);
  c.heroWasRaiser = last_raise_is_hero;
  c.heroPreflopAggressor = last_raise_is_hero;

  // The harness baseline never enters the river LP/DCFR solver, so no figure
  // depends on a HiGHS-present build; the per-street line strings stay empty
  // because only riverAnswer reads them.
  c.riverGtoOn = false;
  c.riverLine.clear();
  c.flopLine.clear();
  c.turnLine.clear();
  c.riverBetFrac = 0.75;
  c.riverRaiseFrac = 1.0;

  return c;
}

poker::Action map_deployed_decision(const poker::GameState& state, const Decision& decision) {
  poker::Action action;
  if (decision.action == "fold")
    action = poker::Action{poker::ActionType::Fold, 0};
  else if (decision.action == "check")
    action = poker::Action{poker::ActionType::Check, 0};
  else if (decision.action == "call")
    // Domain convention: a call carries target 0; after_action derives the
    // amount from the state.
    action = poker::Action{poker::ActionType::Call, 0};
  else if (decision.action == "bet" || decision.action == "raise") {
    const poker::LegalActions probe = state.legal();
    if (!probe.aggressive)
      throw std::runtime_error(
          "deployed decision wagered into a state with no legal aggressive action: " +
          decision.action + " " + std::to_string(decision.amount));
    const poker::ActionType state_type = probe.aggressive->type;
    const bool preflop = state.street() == poker::Street::Preflop;
    const bool token_accepted =
        preflop ? decision.action == "raise"
                : ((decision.action == "bet" && state_type == poker::ActionType::Bet) ||
                   (decision.action == "raise" && state_type == poker::ActionType::Raise));
    if (!token_accepted)
      throw std::runtime_error("deployed aggressive verb does not match the unified state: " +
                               decision.action + " " + std::to_string(decision.amount));
    action = poker::Action{state_type, static_cast<poker::Chips>(decision.amount)};
  } else
    throw std::runtime_error("deployed decision returned an unknown action: " + decision.action);

  const poker::LegalActions legal = state.legal();
  if (!legal.contains(action))
    throw std::runtime_error("deployed decision is not legal in the unified state: " +
                             decision.action + " " + std::to_string(decision.amount) +
                             " (mapped to target " + std::to_string(action.target_total) + ")");
  return action;
}

}  // namespace bs::stage6
