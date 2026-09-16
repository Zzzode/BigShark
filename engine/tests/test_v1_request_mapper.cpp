#include <bs/decision.hpp>
#include <bs/service.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

void near(double actual, double expected, const std::string& description) {
  const double tolerance = std::max(1e-9, 1e-12 * std::max(1.0, std::abs(expected)));
  if (std::abs(actual - expected) > tolerance) {
    std::fprintf(stderr, "FAIL: %s (actual %.12g expected %.12g)\n", description.c_str(), actual,
                 expected);
    ++failures;
  }
}

pv::Card card(pv::Rank rank, pv::Suit suit) {
  pv::Card value;
  value.set_rank(rank);
  value.set_suit(suit);
  return value;
}

// Builds a player; folded/seated-out players are still dealt-in members when
// the v1 adapter preserves ordered table positions.
void addPlayer(pv::HandState* state, const std::string& id, std::uint32_t seat, std::uint64_t stack,
               pv::PlayerStatus status, std::uint64_t committed = 0) {
  pv::PlayerState* player = state->add_players();
  player->set_player_id(id);
  player->set_seat(seat);
  player->set_stack(stack);
  player->set_street_committed(committed);
  player->set_status(status);
}

void addEvent(pv::HandState* state, std::uint64_t sequence, pv::Street street,
              const std::string& actor, pv::ActionType action) {
  pv::ActionEvent* event = state->add_action_history();
  event->set_sequence(sequence);
  event->set_street(street);
  event->set_actor_player_id(actor);
  event->set_action(action);
}

void addEventChips(pv::HandState* state, std::uint64_t sequence, pv::Street street,
                   const std::string& actor, pv::ActionType action, std::uint64_t incremental,
                   std::uint64_t target) {
  pv::ActionEvent* event = state->add_action_history();
  event->set_sequence(sequence);
  event->set_street(street);
  event->set_actor_player_id(actor);
  event->set_action(action);
  event->set_incremental_amount(incremental);
  event->set_target_total(target);
}

void addLegal(pv::HandState* state, pv::ActionType type) {
  state->add_legal_actions()->set_type(type);
}
void addLegalRange(pv::HandState* state, pv::ActionType type, std::uint64_t minimum,
                   std::uint64_t maximum) {
  pv::LegalAction* action = state->add_legal_actions();
  action->set_type(type);
  action->set_min_target_total(minimum);
  action->set_max_target_total(maximum);
}

void addBlind(pv::HandState* state, const std::string& id, pv::ForcedContributionType type,
              std::uint64_t amount) {
  pv::ForcedContribution* contribution = state->add_forced_contributions();
  contribution->set_player_id(id);
  contribution->set_type(type);
  contribution->set_amount(amount);
}

void baseGame(pv::DecisionRequest& request, std::uint64_t sb, std::uint64_t bb,
              double effectiveStackHint = -1.0) {
  pv::HandState* state = request.mutable_state();
  pv::GameDefinition* game = state->mutable_game();
  game->set_variant(pv::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv::GAME_TYPE_CASH);
  game->set_table_capacity(6);
  game->mutable_amount_unit()->set_name("chip");
  game->mutable_amount_unit()->set_decimal_places(0);
  game->set_small_blind(sb);
  game->set_big_blind(bb);

  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(2000);
  options->set_seed(0);
  options->set_include_sampled_action(true);
  options->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
  if (effectiveStackHint > 0.0)
    options->set_preflop_effective_stack_bb(effectiveStackHint);
}

void mapRequest(const pv::DecisionRequest& request, bs::Ctx& context) {
  bs::v1::ValidationReport report;
  pv::ErrorCode code = bs::v1::validateAndMap(request, report, context);
  if (code != pv::ERROR_CODE_UNSPECIFIED) {
    std::fprintf(stderr, "mapping rejected: %d\n", static_cast<int>(code));
    for (const pv::FieldViolation& violation : report.violations)
      std::fprintf(stderr, "  %s: %s\n", violation.field_path().c_str(),
                   violation.description().c_str());
    throw std::runtime_error("mapping rejected");
  }
}

// ---- Fixture 1: preflop-facing-reraise ----
pv::DecisionRequest fixture1() {
  pv::DecisionRequest request;
  baseGame(request, 10, 20, 96.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-preflop-facing-reraise");
  state->set_decision_index(34);
  state->set_street(pv::STREET_PREFLOP);
  state->set_button_seat(0);
  state->set_hero_player_id("seat4");
  addPlayer(state, "seat0", 0, 1920, pv::PLAYER_STATUS_ACTIVE, 60);
  addPlayer(state, "seat1", 1, 2030, pv::PLAYER_STATUS_FOLDED, 10);
  addPlayer(state, "seat3", 3, 1790, pv::PLAYER_STATUS_ACTIVE, 210);
  addPlayer(state, "seat4", 4, 1940, pv::PLAYER_STATUS_ACTIVE, 60);
  addPlayer(state, "seat5", 5, 1120, pv::PLAYER_STATUS_FOLDED, 0);
  *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_HEARTS);
  state->mutable_pot()->set_pot_total(340);
  state->mutable_pot()->set_main_pot(340);
  addBlind(state, "seat1", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
  addBlind(state, "seat3", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
  addEventChips(state, 0, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_RAISE, 60, 60);
  addEvent(state, 1, pv::STREET_PREFLOP, "seat5", pv::ACTION_TYPE_FOLD);
  addEvent(state, 2, pv::STREET_PREFLOP, "seat0", pv::ACTION_TYPE_CALL);
  addEvent(state, 3, pv::STREET_PREFLOP, "seat1", pv::ACTION_TYPE_FOLD);
  addEventChips(state, 4, pv::STREET_PREFLOP, "seat3", pv::ACTION_TYPE_RAISE, 150, 210);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CALL);
  addLegalRange(state, pv::ACTION_TYPE_RAISE, 360, 2000);
  state->set_to_call(150);
  return request;
}

// ---- Fixture 2: multiway-flop-check-option ----
pv::DecisionRequest fixture2() {
  pv::DecisionRequest request;
  baseGame(request, 10, 20, 99.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-multiway-flop-check-option");
  state->set_decision_index(854);
  state->set_street(pv::STREET_FLOP);
  state->set_button_seat(2);
  state->set_hero_player_id("seat4");
  const std::uint64_t stacks[6] = {3870, 830, 3620, 1370, 1980, 3360};
  for (std::uint32_t seat = 0; seat < 6; ++seat)
    addPlayer(state, "seat" + std::to_string(seat), seat, stacks[seat], pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_FOUR, pv::SUIT_CLUBS);
  *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_THREE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_SEVEN, pv::SUIT_CLUBS);
  *state->add_board() = card(pv::RANK_FIVE, pv::SUIT_CLUBS);
  state->mutable_pot()->set_pot_total(120);
  state->mutable_pot()->set_main_pot(120);
  addBlind(state, "seat3", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
  addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
  std::uint64_t sequence = 0;
  const std::uint32_t preflopCallers[5] = {5, 0, 1, 2, 3};
  for (std::uint32_t seat : preflopCallers)
    addEvent(state, sequence++, pv::STREET_PREFLOP, "seat" + std::to_string(seat),
             pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence, pv::STREET_FLOP, "seat3", pv::ACTION_TYPE_CHECK);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CHECK);
  addLegalRange(state, pv::ACTION_TYPE_BET, 20, 1980);
  state->set_to_call(0);
  return request;
}

// ---- Fixture 3: multiway-turn-facing-raise ----
pv::DecisionRequest fixture3() {
  pv::DecisionRequest request;
  baseGame(request, 10, 20, 89.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-multiway-turn-facing-raise");
  state->set_decision_index(984);
  state->set_street(pv::STREET_TURN);
  state->set_button_seat(2);
  state->set_hero_player_id("seat4");
  addPlayer(state, "seat0", 0, 3370, pv::PLAYER_STATUS_FOLDED);
  addPlayer(state, "seat1", 1, 785, pv::PLAYER_STATUS_ACTIVE, 60);
  addPlayer(state, "seat2", 2, 3555, pv::PLAYER_STATUS_ACTIVE, 80);
  addPlayer(state, "seat3", 3, 1750, pv::PLAYER_STATUS_FOLDED);
  addPlayer(state, "seat4", 4, 1780, pv::PLAYER_STATUS_ACTIVE, 40);
  addPlayer(state, "seat5", 5, 2000, pv::PLAYER_STATUS_FOLDED);
  *state->add_hero_hole_cards() = card(pv::RANK_FIVE, pv::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pv::RANK_TEN, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_NINE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_EIGHT, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_THREE, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_TWO, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(460);
  state->mutable_pot()->set_main_pot(460);
  addBlind(state, "seat3", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
  addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
  std::uint64_t sequence = 0;
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat3", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_BET);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat0", pv::ACTION_TYPE_FOLD);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat1", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat2", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat3", pv::ACTION_TYPE_FOLD);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat1", pv::ACTION_TYPE_BET);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat2", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat4", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_TURN, "seat1", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence, pv::STREET_TURN, "seat2", pv::ACTION_TYPE_RAISE);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CALL);
  addLegalRange(state, pv::ACTION_TYPE_RAISE, 100, 1820);
  state->set_to_call(40);
  return request;
}

// ---- Fixture 4: heads-up-river-check-option ----
pv::DecisionRequest fixture4() {
  pv::DecisionRequest request;
  baseGame(request, 10, 20, 72.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-heads-up-river-check-option");
  state->set_decision_index(1316);
  state->set_street(pv::STREET_RIVER);
  state->set_button_seat(5);
  state->set_hero_player_id("seat4");
  addPlayer(state, "seat4", 4, 1440, pv::PLAYER_STATUS_ACTIVE);
  addPlayer(state, "seat5", 5, 1440, pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_FOUR, pv::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pv::RANK_TEN, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_TEN, pv::SUIT_DIAMONDS);
  *state->add_board() = card(pv::RANK_TWO, pv::SUIT_DIAMONDS);
  *state->add_board() = card(pv::RANK_EIGHT, pv::SUIT_CLUBS);
  *state->add_board() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_NINE, pv::SUIT_CLUBS);
  state->mutable_pot()->set_pot_total(40);
  state->mutable_pot()->set_main_pot(40);
  addBlind(state, "seat5", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
  addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
  std::uint64_t sequence = 0;
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat5", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat5", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_TURN, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence, pv::STREET_TURN, "seat5", pv::ACTION_TYPE_CHECK);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CHECK);
  addLegalRange(state, pv::ACTION_TYPE_BET, 20, 1440);
  state->set_to_call(0);
  return request;
}

// ---- Fixture 5: multiway-river-facing-bet ----
pv::DecisionRequest fixture5() {
  pv::DecisionRequest request;
  baseGame(request, 10, 20, 86.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-multiway-river-facing-bet");
  state->set_decision_index(991);
  state->set_street(pv::STREET_RIVER);
  state->set_button_seat(2);
  state->set_hero_player_id("seat4");
  addPlayer(state, "seat0", 0, 3370, pv::PLAYER_STATUS_FOLDED);
  addPlayer(state, "seat1", 1, 725, pv::PLAYER_STATUS_ACTIVE, 20);
  addPlayer(state, "seat2", 2, 3515, pv::PLAYER_STATUS_ACTIVE, 20);
  addPlayer(state, "seat3", 3, 1750, pv::PLAYER_STATUS_FOLDED);
  addPlayer(state, "seat4", 4, 1720, pv::PLAYER_STATUS_ACTIVE);
  addPlayer(state, "seat5", 5, 2000, pv::PLAYER_STATUS_FOLDED);
  *state->add_hero_hole_cards() = card(pv::RANK_FIVE, pv::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pv::RANK_TEN, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_NINE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_EIGHT, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_THREE, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_TWO, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_FIVE, pv::SUIT_DIAMONDS);
  state->mutable_pot()->set_pot_total(620);
  state->mutable_pot()->set_main_pot(620);
  addBlind(state, "seat3", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
  addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
  std::uint64_t sequence = 0;
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat1", pv::ACTION_TYPE_BET);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat2", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat1", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat2", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat1", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat2", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_RIVER, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_RIVER, "seat1", pv::ACTION_TYPE_BET);
  addEvent(state, sequence, pv::STREET_RIVER, "seat2", pv::ACTION_TYPE_CALL);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CALL);
  addLegalRange(state, pv::ACTION_TYPE_RAISE, 40, 1720);
  state->set_to_call(20);
  return request;
}

// ---- Fixture 6: short-stack-heads-up-river ----
pv::DecisionRequest fixture6() {
  pv::DecisionRequest request;
  baseGame(request, 25, 50, 38.0);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("hand-short-stack-heads-up-river");
  state->set_decision_index(410);
  state->set_street(pv::STREET_RIVER);
  state->set_button_seat(5);
  state->set_hero_player_id("seat5");
  addPlayer(state, "seat4", 4, 1900, pv::PLAYER_STATUS_ACTIVE);
  addPlayer(state, "seat5", 5, 10125, pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_CLUBS);
  *state->add_hero_hole_cards() = card(pv::RANK_QUEEN, pv::SUIT_CLUBS);
  *state->add_board() = card(pv::RANK_SEVEN, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_KING, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_TEN, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_NINE, pv::SUIT_DIAMONDS);
  *state->add_board() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(300);
  state->mutable_pot()->set_main_pot(300);
  addBlind(state, "seat5", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 25);
  addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 50);
  std::uint64_t sequence = 0;
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat5", pv::ACTION_TYPE_RAISE);
  addEvent(state, sequence++, pv::STREET_PREFLOP, "seat4", pv::ACTION_TYPE_CALL);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_FLOP, "seat5", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_TURN, "seat4", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence++, pv::STREET_TURN, "seat5", pv::ACTION_TYPE_CHECK);
  addEvent(state, sequence, pv::STREET_RIVER, "seat4", pv::ACTION_TYPE_CHECK);
  addLegal(state, pv::ACTION_TYPE_FOLD);
  addLegal(state, pv::ACTION_TYPE_CHECK);
  addLegalRange(state, pv::ACTION_TYPE_BET, 50, 10125);
  state->set_to_call(0);
  return request;
}

void expectDecision(const bs::Ctx& context, const char* action, int amount,
                    const std::string& label) {
  const bs::Decision decision = bs::decide(context);
  check(decision.action == action, label + ": action " + decision.action);
  check(decision.amount == amount, label + ": amount " + std::to_string(decision.amount));
}

}  // namespace

int main() {
  {
    bs::Ctx context;
    mapRequest(fixture1(), context);
    check(context.handId == "hand-preflop-facing-reraise", "f1 hand id");
    check(context.revision == 34, "f1 revision");
    check(context.street == "preflop", "f1 street");
    check(context.sb == 10 && context.bb == 20, "f1 blinds");
    check(context.position == "HJ", "f1 position " + context.position);
    check(context.playersInHand == 5, "f1 players");
    near(context.effectiveStackBb, 96.0, "f1 effective stack (server hint)");
    check(context.hole.size() == 2 && context.hole[0] == "As" && context.hole[1] == "Ah",
          "f1 hole");
    check(context.board.empty(), "f1 board empty");
    check(context.pot == 340, "f1 pot");
    check(context.legal.call == 150, "f1 call");
    near(context.legal.potOdds, 0.3061, "f1 pot odds matches platform rounding");
    check(context.legal.raiseMin == 360 && context.legal.raiseMax == 2000, "f1 raise range");
    check(context.legal.actions.size() == 3, "f1 legal actions");
    check(context.raises == 2, "f1 raises");
    check(context.limpers == 1, "f1 limpers");
    check(context.openerPosition == "HJ", "f1 opener position");
    check(!context.heroWasRaiser && !context.heroPreflopAggressor, "f1 hero flags");
    check(!context.riverGtoOn, "f1 river solver off");
    expectDecision(context, "raise", 2000, "f1");
  }

  {
    bs::Ctx context;
    mapRequest(fixture2(), context);
    check(context.street == "flop", "f2 street");
    check(context.position == "BB", "f2 position");
    check(context.playersInHand == 6, "f2 players");
    near(context.effectiveStackBb, 99.0, "f2 effective stack");
    check(context.hole[0] == "4c" && context.hole[1] == "2h", "f2 hole");
    check(context.board.size() == 3 && context.board[0] == "3s" && context.board[2] == "5c",
          "f2 board");
    check(context.pot == 120 && context.legal.call == 0, "f2 pot/call");
    near(context.legal.potOdds, 0.0, "f2 pot odds");
    check(context.legal.raiseMin == 20 && context.legal.raiseMax == 1980, "f2 range");
    check(context.raises == 0 && context.limpers == 0, "f2 preflop knobs zero postflop");
    check(context.openerPosition.empty(), "f2 opener empty");
    expectDecision(context, "check", 0, "f2");
  }

  {
    bs::Ctx context;
    mapRequest(fixture3(), context);
    check(context.street == "turn", "f3 street");
    check(context.position == "BB", "f3 position");
    check(context.playersInHand == 6, "f3 players");
    near(context.effectiveStackBb, 89.0, "f3 effective stack");
    check(context.hole[0] == "5h" && context.hole[1] == "Th", "f3 hole");
    check(context.board.size() == 4 && context.board[3] == "2s", "f3 board");
    check(context.pot == 460 && context.legal.call == 40, "f3 pot/call");
    near(context.legal.potOdds, 0.08, "f3 pot odds");
    check(context.legal.raiseMin == 100 && context.legal.raiseMax == 1820, "f3 range");
    check(context.raises == 0 && context.limpers == 0, "f3 raises zero on turn");
    check(!context.heroWasRaiser, "f3 hero not raiser");
    expectDecision(context, "raise", 340, "f3");
  }

  {
    bs::Ctx context;
    mapRequest(fixture4(), context);
    check(context.street == "river", "f4 street");
    check(context.position == "BB", "f4 position");
    check(context.playersInHand == 2, "f4 players");
    near(context.effectiveStackBb, 72.0, "f4 effective stack");
    check(context.hole[0] == "4h" && context.hole[1] == "Ts", "f4 hole");
    check(context.board.size() == 5 && context.board[0] == "Td", "f4 board");
    check(context.riverGtoOn, "f4 river solver on");
    check(context.riverLine.empty(), "f4 river line root");
    check(context.flopLine == "Hx,Ox", "f4 flop line " + context.flopLine);
    check(context.turnLine == "Hx,Ox", "f4 turn line " + context.turnLine);
    near(context.riverBetFrac, 0.75, "f4 river bet frac");
    expectDecision(context, "check", 0, "f4");
  }

  {
    bs::Ctx context;
    mapRequest(fixture5(), context);
    check(context.street == "river", "f5 street");
    check(context.playersInHand == 6, "f5 players");
    near(context.effectiveStackBb, 86.0, "f5 effective stack");
    check(context.hole[0] == "5h" && context.board[4] == "5d", "f5 cards");
    check(context.pot == 620 && context.legal.call == 20, "f5 pot/call");
    near(context.legal.potOdds, 0.0313, "f5 pot odds matches platform rounding");
    check(context.legal.raiseMin == 40 && context.legal.raiseMax == 1720, "f5 range");
    check(!context.riverGtoOn, "f5 river solver off multiway");
    expectDecision(context, "fold", 0, "f5");
  }

  {
    bs::Ctx context;
    mapRequest(fixture6(), context);
    check(context.sb == 25 && context.bb == 50, "f6 blinds");
    check(context.position == "BTN", "f6 position " + context.position);
    check(context.playersInHand == 2, "f6 players");
    near(context.effectiveStackBb, 38.0, "f6 effective stack (short opponent)");
    check(context.hole[0] == "Ac" && context.hole[1] == "Qc", "f6 hole");
    check(context.riverGtoOn, "f6 river solver on");
    check(context.riverLine == "c", "f6 river line " + context.riverLine);
    check(context.flopLine == "Ox,Hx", "f6 flop line " + context.flopLine);
    check(context.turnLine == "Ox,Hx", "f6 turn line " + context.turnLine);
    check(context.heroWasRaiser && context.heroPreflopAggressor, "f6 hero aggressor");
    expectDecision(context, "bet", 250, "f6");
  }

  // Derived-sum overflow defense at the mapper boundary: pot and to_call each
  // fit int but their sum does not; the mapper must reject rather than derive
  // a negative potOdds and flip a fold into a call.
  {
    pv::DecisionRequest request = fixture5();
    constexpr std::uint64_t twoBillion = 2'000'000'000ull;
    request.mutable_state()->mutable_pot()->set_pot_total(twoBillion);
    request.mutable_state()->mutable_pot()->set_main_pot(twoBillion);
    request.mutable_state()->set_to_call(twoBillion);
    bs::Ctx overflowed;
    bs::v1::ValidationReport report;
    const pv::ErrorCode code = bs::v1::validateAndMap(request, report, overflowed);
    check(code == pv::ERROR_CODE_INVALID_REQUEST, "pot+to_call overflow rejected by mapper");
    check(!report.ok(), "overflow carries a violation");
  }

  // The platform hint overrides the physical fallback even when they straddle
  // the 12bb gate: the same deep structure maps to 10bb jam depth only when
  // the hint says so.
  {
    pv::DecisionRequest withHint = fixture2();  // 99bb deep multiway, hint 99
    bs::Ctx hinted;
    mapRequest(withHint, hinted);
    near(hinted.effectiveStackBb, 99.0, "hint 99 used");

    pv::DecisionRequest withoutHint = withHint;
    withoutHint.mutable_options()->clear_preflop_effective_stack_bb();
    bs::Ctx fallback;
    mapRequest(withoutHint, fallback);
    // Multiway fallback is hero stack_behind: 1980/20 = 99 here too, so use a
    // hint that explicitly crosses the gate to prove precedence.
    pv::DecisionRequest shortHint = withHint;
    shortHint.mutable_options()->set_preflop_effective_stack_bb(10.0);
    bs::Ctx shorted;
    mapRequest(shortHint, shorted);
    near(shorted.effectiveStackBb, 10.0, "10bb hint overrides deep structure");
    check(std::abs(hinted.effectiveStackBb - shorted.effectiveStackBb) > 50.0,
          "hint changes effective stack");
  }

  // Short effective stack heads-up: hero (BB, 1980 behind + 20 posted, AA)
  // faces a single short opponent (SB, 180 behind + 20 posted). The physical
  // effective stack is min(2000, 200)/20 = 10 bb, so the preflop jam heuristic
  // must jams AA to the full target even though hero alone is ~100 bb deep.
  {
    pv::DecisionRequest request;
    baseGame(request, 10, 20, 10.0);
    pv::HandState* state = request.mutable_state();
    state->set_hand_id("hand-short-eff");
    state->set_decision_index(2);
    state->set_street(pv::STREET_PREFLOP);
    state->set_button_seat(5);
    state->set_hero_player_id("seat4");
    addPlayer(state, "seat5", 5, 180, pv::PLAYER_STATUS_ACTIVE, 20);
    addPlayer(state, "seat4", 4, 1980, pv::PLAYER_STATUS_ACTIVE, 20);
    addBlind(state, "seat5", pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 10);
    addBlind(state, "seat4", pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 20);
    *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_SPADES);
    *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_HEARTS);
    state->mutable_pot()->set_pot_total(40);
    state->mutable_pot()->set_main_pot(40);
    // SB limped; BB faces a completed 20 with nothing more to call.
    state->set_to_call(0);
    state->clear_legal_actions();
    addLegal(state, pv::ACTION_TYPE_FOLD);
    addLegal(state, pv::ACTION_TYPE_CHECK);
    addLegalRange(state, pv::ACTION_TYPE_RAISE, 40, 200);
    bs::Ctx shortContext;
    mapRequest(request, shortContext);
    near(shortContext.effectiveStackBb, 10.0, "short effective stack is 10 bb");
    // AA at 10bb with an open raise path: preflop limp pot with a legal raise
    // jams the full 200 (short jam for premium).
    expectDecision(shortContext, "raise", 200, "short effective stack");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 REQUEST MAPPER TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 REQUEST MAPPER TESTS PASSED");
  return 0;
}
