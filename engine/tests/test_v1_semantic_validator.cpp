#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "../src/protocol/v1_mappers.hpp"

namespace {

namespace pv = bs::v1::pv;

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

bool hasViolation(const bs::v1::ValidationReport& report, const std::string& fieldPath) {
  for (const pv::FieldViolation& violation : report.violations)
    if (violation.field_path() == fieldPath)
      return true;
  return false;
}

pv::Card card(pv::Rank rank, pv::Suit suit) {
  pv::Card value;
  value.set_rank(rank);
  value.set_suit(suit);
  return value;
}

pv::PlayerState player(const std::string& id, std::uint32_t seat, std::uint64_t stack,
                       pv::PlayerStatus status, std::uint64_t committed = 0) {
  pv::PlayerState value;
  value.set_player_id(id);
  value.set_seat(seat);
  value.set_stack(stack);
  value.set_street_committed(committed);
  value.set_status(status);
  return value;
}

pv::LegalAction legal(pv::ActionType type, std::uint64_t minimum = 0, std::uint64_t maximum = 0) {
  pv::LegalAction value;
  value.set_type(type);
  if (minimum || maximum) {
    value.set_min_target_total(minimum);
    value.set_max_target_total(maximum);
  }
  return value;
}

// Baseline: heads-up flop spot, check/bet option.
pv::DecisionRequest baseline() {
  pv::DecisionRequest request;
  pv::HandState* state = request.mutable_state();
  pv::GameDefinition* game = state->mutable_game();
  game->set_variant(pv::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv::GAME_TYPE_CASH);
  game->set_table_capacity(6);
  game->mutable_amount_unit()->set_name("chip");
  game->mutable_amount_unit()->set_decimal_places(0);
  game->set_small_blind(10);
  game->set_big_blind(20);

  state->set_hand_id("hand-1");
  state->set_decision_index(7);
  state->set_street(pv::STREET_FLOP);
  state->set_button_seat(5);
  state->set_hero_player_id("hero");
  *state->add_players() = player("villain", 5, 1980, pv::PLAYER_STATUS_ACTIVE);
  *state->add_players() = player("hero", 4, 1980, pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_FOUR, pv::SUIT_CLUBS);
  *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_THREE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_SEVEN, pv::SUIT_CLUBS);
  *state->add_board() = card(pv::RANK_FIVE, pv::SUIT_CLUBS);
  state->mutable_pot()->set_pot_total(120);
  state->mutable_pot()->set_main_pot(120);
  *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
  *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
  *state->add_legal_actions() = legal(pv::ACTION_TYPE_BET, 20, 1980);
  state->set_to_call(0);

  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(42);
  options->set_include_sampled_action(true);
  options->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
  return request;
}

pv::ErrorCode validate(const pv::DecisionRequest& request, bs::v1::ValidationReport& report) {
  report = {};
  return bs::v1::validateDecisionRequest(request, report);
}

}  // namespace

int main() {
  {
    pv::DecisionRequest request = baseline();
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED, "baseline is valid");
    check(report.ok(), "baseline has no violations");
  }

  // Unspecified enums.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_street(pv::STREET_UNSPECIFIED);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unspecified street");
    check(hasViolation(report, "state.street"), "street violation recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->set_variant(pv::GAME_VARIANT_UNSPECIFIED);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unspecified variant");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_UNSPECIFIED);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unspecified solver mode rejected");
  }

  // Card uniqueness and hero/board overlap.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_hero_hole_cards(1)->CopyFrom(
        card(pv::RANK_FOUR, pv::SUIT_CLUBS));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "duplicate hole card");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_board(0)->CopyFrom(card(pv::RANK_FOUR, pv::SUIT_CLUBS));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "hero/board overlap");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->set_street(pv::STREET_TURN);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "three-card board rejected on turn");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->clear_hero_hole_cards();
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "missing hole cards");
  }

  // Player, seat, and button consistency.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_button_seat(9);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "button not seated");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_hero_player_id("ghost");
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "hero not seated");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_players(1)->set_seat(5);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "duplicate seat");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_players(1)->set_status(pv::PLAYER_STATUS_FOLDED);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "folded hero rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_players()->RemoveLast();
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "single player rejected");
  }

  // Pot coherence.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_pot()->set_main_pot(121);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "main pot over total");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::SidePot* side = request.mutable_state()->mutable_pot()->add_side_pots();
    side->set_amount(40);
    side->add_eligible_player_ids("hero");
    side->add_eligible_player_ids("ghost");
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "side pots require an unsupported feature");
    check(hasViolation(report, "state.pot.side_pots[0]"), "unknown side pot eligibility recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::PotState* pot = request.mutable_state()->mutable_pot();
    pot->set_main_pot(80);
    pv::SidePot* side = pot->add_side_pots();
    side->set_amount(50);
    side->add_eligible_player_ids("hero");
    side->add_eligible_player_ids("villain");
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "feature rejection dominates pot arithmetic");
    check(hasViolation(report, "state.pot"), "pot total must equal main plus sides");
  }

  // Action history order and actor existence.
  {
    pv::DecisionRequest request = baseline();
    pv::ActionEvent* event = request.mutable_state()->add_action_history();
    event->set_sequence(1);
    event->set_street(pv::STREET_PREFLOP);
    event->set_actor_player_id("ghost");
    event->set_action(pv::ACTION_TYPE_RAISE);
    event->set_target_total(60);
    event->set_incremental_amount(60);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unknown history actor");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    pv::ActionEvent* a = state->add_action_history();
    a->set_sequence(2);
    a->set_street(pv::STREET_TURN);
    a->set_actor_player_id("hero");
    a->set_action(pv::ACTION_TYPE_CHECK);
    pv::ActionEvent* b = state->add_action_history();
    b->set_sequence(3);
    b->set_street(pv::STREET_FLOP);
    b->set_actor_player_id("hero");
    b->set_action(pv::ACTION_TYPE_CHECK);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "decreasing street rejected");
    check(hasViolation(report, "state.action_history[1].street"),
          "street violation on second event");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::ActionEvent* event = request.mutable_state()->add_action_history();
    event->set_sequence(1);
    event->set_street(pv::STREET_FLOP);
    event->set_actor_player_id("hero");
    event->set_action(pv::ACTION_TYPE_CHECK);
    event->set_incremental_amount(10);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "check with chips rejected");
  }

  // Legal action coherence.
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_BET, 20, 100);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 40, 200);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "bet and raise are mutually exclusive");
  }

  // Open-enum guard: action type 99 must never index the fixed action array.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_legal_actions(0)->set_type(static_cast<pv::ActionType>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown legal action 99 rejected before indexing");
    check(hasViolation(report, "state.legal_actions[0].type"),
          "unknown legal action violation recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_legal_actions(0)->set_type(static_cast<pv::ActionType>(-1));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "negative wire action value rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::ActionEvent* event = request.mutable_state()->add_action_history();
    event->set_sequence(1);
    event->set_street(pv::STREET_FLOP);
    event->set_actor_player_id("hero");
    event->set_action(static_cast<pv::ActionType>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown history action rejected");
  }

  // Big blind option: to_call 0 with fold/check/raise is valid when the hero
  // is the unopened big blind.
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->set_street(pv::STREET_PREFLOP);
    state->clear_board();
    state->set_button_seat(5);
    state->clear_players();
    *state->add_players() = player("villain", 5, 1980, pv::PLAYER_STATUS_ACTIVE, 20);
    *state->add_players() = player("hero", 4, 1980, pv::PLAYER_STATUS_ACTIVE, 20);
    pv::ForcedContribution* villainBlind = state->add_forced_contributions();
    villainBlind->set_player_id("villain");
    villainBlind->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    villainBlind->set_amount(10);
    pv::ForcedContribution* heroBlind = state->add_forced_contributions();
    heroBlind->set_player_id("hero");
    heroBlind->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    heroBlind->set_amount(20);
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 40, 2000);
    state->set_to_call(0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED,
          "unopened big blind option accepts check plus raise");
    check(report.ok(), "big blind option has no violations");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->set_street(pv::STREET_PREFLOP);
    state->clear_board();
    state->set_button_seat(5);
    state->clear_players();
    *state->add_players() = player("villain", 5, 1980, pv::PLAYER_STATUS_ACTIVE, 60);
    *state->add_players() = player("hero", 4, 1960, pv::PLAYER_STATUS_ACTIVE, 40);
    pv::ForcedContribution* villainBlind = state->add_forced_contributions();
    villainBlind->set_player_id("villain");
    villainBlind->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    villainBlind->set_amount(10);
    pv::ForcedContribution* heroBlind = state->add_forced_contributions();
    heroBlind->set_player_id("hero");
    heroBlind->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    heroBlind->set_amount(20);
    pv::ActionEvent* open = state->add_action_history();
    open->set_sequence(1);
    open->set_street(pv::STREET_PREFLOP);
    open->set_actor_player_id("villain");
    open->set_action(pv::ACTION_TYPE_RAISE);
    open->set_target_total(60);
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 80, 2000);
    state->set_to_call(0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "raise without a bet is rejected after the option closes");
  }
  // Defense in depth: a preflop BET (not just RAISE) also closes the option.
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->set_street(pv::STREET_PREFLOP);
    state->clear_board();
    state->set_button_seat(5);
    state->clear_players();
    *state->add_players() = player("villain", 5, 1960, pv::PLAYER_STATUS_ACTIVE, 40);
    *state->add_players() = player("hero", 4, 1960, pv::PLAYER_STATUS_ACTIVE, 40);
    pv::ForcedContribution* sb = state->add_forced_contributions();
    sb->set_player_id("villain");
    sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    sb->set_amount(10);
    pv::ForcedContribution* bb = state->add_forced_contributions();
    bb->set_player_id("hero");
    bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    bb->set_amount(20);
    pv::ActionEvent* open = state->add_action_history();
    open->set_sequence(1);
    open->set_street(pv::STREET_PREFLOP);
    open->set_actor_player_id("villain");
    open->set_action(pv::ACTION_TYPE_BET);
    open->set_target_total(40);
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 80, 2000);
    state->set_to_call(0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "a preflop bet also closes the big blind option");
  }

  // Integer chip profile: pot fields reaching the heuristic Ctx fit in int.
  {
    pv::DecisionRequest request = baseline();
    const std::uint64_t tooLarge =
        static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1ull;
    request.mutable_state()->mutable_pot()->set_pot_total(tooLarge);
    request.mutable_state()->mutable_pot()->set_main_pot(tooLarge);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "oversize pot rejected");
  }
  // Derived-sum overflow: each field fits int but pot_total + to_call does
  // not. This flipped a fold into a call through a negative potOdds.
  {
    pv::DecisionRequest request = baseline();
    constexpr std::uint64_t twoBillion = 2'000'000'000ull;
    request.mutable_state()->mutable_pot()->set_pot_total(twoBillion);
    request.mutable_state()->mutable_pot()->set_main_pot(twoBillion);
    request.mutable_state()->set_to_call(twoBillion);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "pot plus to_call int overflow rejected");
    check(hasViolation(report, "state"), "derived-sum violation recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    const std::uint64_t intMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    request.mutable_state()->mutable_pot()->set_pot_total(intMax);
    request.mutable_state()->mutable_pot()->set_main_pot(intMax);
    request.mutable_state()->set_to_call(1);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "INT_MAX pot plus 1 to_call rejected");
  }
  // The derived-sum boundary is inclusive at INT_MAX (to_call 0 here).
  {
    pv::DecisionRequest request = baseline();
    const std::uint64_t intMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    request.mutable_state()->mutable_pot()->set_pot_total(intMax);
    request.mutable_state()->mutable_pot()->set_main_pot(intMax);
    request.mutable_state()->set_to_call(0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED,
          "pot equal INT_MAX with zero to_call accepted");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_pot()->set_pot_total(120);
    request.mutable_state()->mutable_pot()->set_main_pot(100);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "total/main mismatch without side pots rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_BET, 20, 5000);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "bet max above hero chips rejected");
  }

  // Forced contribution and player status closed sets.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->clear_forced_contributions();
    pv::ForcedContribution* contribution = request.mutable_state()->add_forced_contributions();
    contribution->set_player_id("hero");
    contribution->set_type(static_cast<pv::ForcedContributionType>(99));
    contribution->set_amount(10);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown forced contribution type rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_players(1)->set_status(static_cast<pv::PlayerStatus>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown player status rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CALL);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 40, 1980);
    state->set_to_call(20);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED,
          "facing a bet: fold/call/raise is valid");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_to_call(20);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "check with chips owed rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_BET, 100, 20);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "inverted range rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_CHECK);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_BET);  // missing range
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "bet without range rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_FOLD);
    *state->add_legal_actions() = legal(pv::ACTION_TYPE_RAISE, 40, 1980);
    state->set_to_call(20);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "raise without call rejected");
  }

  // Amount profile and blind coherence.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->set_big_blind(0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "zero big blind rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->set_small_blind(20);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "small blind >= big blind rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_to_call(5000);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "to_call beyond hero chips rejected");
  }

  // Unsupported games and features.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->set_game_type(pv::GAME_TYPE_TOURNAMENT);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_GAME, "tournament rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->mutable_amount_unit()->set_decimal_places(2);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "fractional units rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->set_ante(5);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE, "ante rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_game()->mutable_straddle()->set_enabled(true);
    request.mutable_state()->mutable_game()->mutable_straddle()->set_amount(40);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE, "straddle rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    pv::ActionEvent* event = request.mutable_state()->add_action_history();
    event->set_sequence(1);
    event->set_street(static_cast<pv::Street>(99));
    event->set_actor_player_id("hero");
    event->set_action(pv::ACTION_TYPE_CHECK);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown history street rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_street(static_cast<pv::Street>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown decision street rejected");
    check(hasViolation(report, "state.street"), "unknown street violation recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_street(pv::STREET_SHOWDOWN);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "showdown decision rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_hero_hole_cards(0)->set_rank(static_cast<pv::Rank>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unknown rank rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_board(0)->set_suit(static_cast<pv::Suit>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unknown suit rejected");
  }

  // Forced river backends are features, not silently ignored modes.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_RIVER_LP);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced river LP rejected as a feature");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_RIVER_DCFR);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced river DCFR rejected as a feature");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(static_cast<pv::SolverMode>(99));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "unknown solver mode rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_HEURISTIC);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED,
          "explicit heuristic mode accepted");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_MULTISTREET_CFR);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced multistreet mode rejected");
  }

  // Effective stack hint validation.
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_preflop_effective_stack_bb(12.0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_UNSPECIFIED,
          "valid 12 bb effective stack hint accepted");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_preflop_effective_stack_bb(0.0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "zero effective stack hint rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_preflop_effective_stack_bb(-5.0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "negative effective stack hint rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_preflop_effective_stack_bb(
        std::numeric_limits<double>::infinity());
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "infinite effective stack hint rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_preflop_effective_stack_bb(50000.0);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "out-of-bounds effective stack hint rejected");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_options()->set_strategy_profile("nit");
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "unknown profile rejected");
  }

  // Forced contributions reference real players.
  {
    pv::DecisionRequest request = baseline();
    pv::ForcedContribution* contribution = request.mutable_state()->add_forced_contributions();
    contribution->set_player_id("ghost");
    contribution->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    contribution->set_amount(10);
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "blind by unknown player rejected");
  }

  // Proto3 string fields must be valid UTF-8 (the C++ parser is lenient; the
  // strict TypeScript decoder would reject the echo).
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->set_hand_id(std::string("bad-\xff-id"));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST, "invalid UTF-8 rejected");
    check(hasViolation(report, "state.hand_id"), "UTF-8 violation recorded");
  }
  {
    pv::DecisionRequest request = baseline();
    request.mutable_state()->mutable_players(1)->set_player_id(std::string("h\xff"));
    bs::v1::ValidationReport report;
    check(validate(request, report) == pv::ERROR_CODE_INVALID_REQUEST,
          "invalid UTF-8 player id rejected");
  }
  {
    bs::v1::ValidationReport none;
    check(bs::v1::isValidUtf8(""), "empty string is valid UTF-8");
    check(bs::v1::isValidUtf8("ascii-123"), "ascii is valid UTF-8");
    check(bs::v1::isValidUtf8("\xe4\xb8\xad"), "three-byte CJK is valid UTF-8");
    check(!bs::v1::isValidUtf8("\xed\xa0\x80"), "UTF-16 surrogate rejected");
    check(!bs::v1::isValidUtf8("\xc0\xaf"), "overlong encoding rejected");
    check(!bs::v1::isValidUtf8("\xf4\x90\x80\x80"), "code point above U+10FFFF rejected");
    check(!bs::v1::isValidUtf8("\xe4\xb8"), "truncated sequence rejected");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 SEMANTIC VALIDATOR TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 SEMANTIC VALIDATOR TESTS PASSED");
  return 0;
}
