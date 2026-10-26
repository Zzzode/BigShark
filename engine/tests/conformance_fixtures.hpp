// conformance_fixtures.hpp — shared current-schema DecisionRequest builders.
//
// RFC 0034: the legal engine envelope builders used to live anonymously in
// test_conformance.cpp. They are shared here so the protocol fuzz target
// reuses the SAME current-schema legal fixture instead of carrying a second,
// rot-prone copy. Header-only; both the conformance executable and the fuzz
// harness include it.
#pragma once

#include <bigshark/engine/engine.pb.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace bs::conformance {

namespace pe = ::bigshark::engine;

inline pe::Card card(pe::Rank rank, pe::Suit suit) {
  pe::Card value;
  value.set_rank(rank);
  value.set_suit(suit);
  return value;
}

inline void addPlayer(pe::HandState* state, uint32_t seat, uint64_t starting, uint64_t stack,
                      uint64_t street_committed, uint64_t gross, uint64_t refunded,
                      pe::PlayerStatus status) {
  pe::PlayerState* player = state->add_players();
  player->set_seat(seat);
  player->set_starting_stack(starting);
  player->set_stack(stack);
  player->set_street_committed(street_committed);
  player->set_gross_contributed(gross);
  player->set_refunded(refunded);
  player->set_status(status);
}

inline void addForced(pe::HandState* state, uint32_t seat, pe::ForcedContributionType type,
                      uint64_t amount) {
  pe::ForcedContribution* forced = state->add_forced_contributions();
  forced->set_seat(seat);
  forced->set_type(type);
  forced->set_amount(amount);
}

inline void addAction(pe::HandState* state, uint64_t seq, pe::Street street, uint32_t actor,
                      pe::ActionType action, uint64_t pot_before, uint64_t stack_after,
                      bool all_in = false, std::optional<uint64_t> target = std::nullopt,
                      std::optional<uint64_t> incremental = std::nullopt) {
  pe::TransitionEvent* event = state->add_transition_path();
  event->set_sequence(seq);
  pe::ActionEvent* ae = event->mutable_action();
  ae->set_sequence(seq);
  ae->set_street(street);
  ae->set_actor_seat(actor);
  ae->set_action(action);
  ae->set_pot_before(pot_before);
  ae->set_stack_after(stack_after);
  ae->set_all_in(all_in);
  if (target.has_value())
    ae->set_target_total(*target);
  if (incremental.has_value())
    ae->set_incremental_amount(*incremental);
}

inline void addBoardDeal(pe::HandState* state, uint64_t seq, pe::Street street,
                         std::initializer_list<pe::Card> cards) {
  pe::TransitionEvent* event = state->add_transition_path();
  event->set_sequence(seq);
  pe::BoardDeal* bd = event->mutable_board_deal();
  bd->set_street(street);
  for (const pe::Card& c : cards)
    *bd->add_cards() = c;
}

inline void addLayer(pe::PotState* pot, uint64_t level, uint64_t amount,
                     std::initializer_list<uint32_t> eligible, pe::LayerStatus status) {
  pe::PotLayer* layer = pot->add_layers();
  layer->set_contribution_level(level);
  layer->set_amount(amount);
  for (uint32_t seat : eligible)
    layer->add_eligible_seats(seat);
  layer->set_status(status);
}

inline void addLegal(pe::HandState* state, pe::ActionType type,
                     std::optional<uint64_t> min_target = std::nullopt,
                     std::optional<uint64_t> max_target = std::nullopt) {
  pe::LegalAction* legal = state->add_legal_actions();
  legal->set_type(type);
  if (min_target.has_value())
    legal->set_min_target_total(*min_target);
  if (max_target.has_value())
    legal->set_max_target_total(*max_target);
}

inline void setGameDefinition(pe::HandState* state, uint32_t table_capacity, uint64_t sb,
                              uint64_t bb, uint32_t dealt_seats) {
  pe::GameDefinition* game = state->mutable_game();
  game->set_variant(pe::GAME_VARIANT_NLHE);
  game->set_betting_structure(pe::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pe::GAME_TYPE_CASH);
  game->set_table_capacity(table_capacity);
  game->mutable_amount_unit()->set_name("chip");
  game->mutable_amount_unit()->set_decimal_places(0);
  game->set_small_blind(sb);
  game->set_big_blind(bb);
  pe::RuleProfile* profile = game->mutable_rule_profile();
  profile->set_settlement(pe::SETTLEMENT_RULE_CLOCKWISE_LEFT_OF_BUTTON);
  profile->set_reopening(pe::REOPENING_RULE_NEVER_CUMULATIVE);
  profile->set_runout(pe::RUNOUT_RULE_SINGLE);
  profile->set_dealt_seat_count(dealt_seats);
}

inline void setProvenance(pe::HandState* state, uint32_t event_count) {
  pe::ObservationProvenance* provenance = state->mutable_provenance();
  provenance->set_source(pe::OBSERVATION_SOURCE_OFFICIAL_RESPONSE);
  provenance->set_coverage(pe::OBSERVATION_COVERAGE_FULL);
  provenance->set_controller_epoch("test-epoch-1");
  provenance->set_observed_event_count(event_count);
}

inline void setOptions(pe::DecisionRequest* request, pe::SolverMode mode, uint64_t seed = 42,
                       bool sampled = true) {
  pe::DecisionOptions* options = request->mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(seed);
  options->set_include_sampled_action(sampled);
  options->set_solver_mode(mode);
  options->set_utility_profile(pe::UTILITY_PROFILE_CASH_CHIP);
}

// A serialized envelope on the single accepted transport version.
inline std::string envelopeFor(const std::string& id, pe::DecisionRequest request) {
  pe::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = std::move(request);
  return envelope.SerializeAsString();
}

inline std::string capabilitiesEnvelope(const std::string& id) {
  pe::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id(id);
  envelope.mutable_get_capabilities_request();
  return envelope.SerializeAsString();
}

// Vector 1 — ordinary open bet before a response.
// 6-max NLHE cash, blinds 1/2, 100 BB (200 chips) stacks. Button seat 0.
// Hero seat 2 (BB). Decision point: seat 2, street FLOP, to_call = 8.
// HEURISTIC mode answers deterministically with no resident services, so the
// fixture reaches the decision arm in the fuzz harness without an engine.
inline pe::DecisionRequest vector1Request(pe::SolverMode mode = pe::SOLVER_MODE_HEURISTIC) {
  pe::DecisionRequest request;
  pe::HandState* state = request.mutable_state();
  setGameDefinition(state, 6, 1, 2, 6);
  state->set_hand_id("vector1-hand");
  state->set_decision_index(1);
  state->set_street(pe::STREET_FLOP);
  state->set_button_seat(0);
  state->set_hero_seat(2);

  // Players (G/F ledger). street_committed is current-street only.
  addPlayer(state, 0, 200, 186, 8, 14, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 1, 200, 199, 0, 1, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 2, 200, 194, 0, 6, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 3, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 4, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 5, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);

  // Hero hole cards: Qh Qd.
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_DIAMONDS);

  // Board: As Kd 7c.
  *state->add_board() = card(pe::RANK_ACE, pe::SUIT_SPADES);
  *state->add_board() = card(pe::RANK_KING, pe::SUIT_DIAMONDS);
  *state->add_board() = card(pe::RANK_SEVEN, pe::SUIT_CLUBS);

  // Pot: 21, three PENDING layers.
  pe::PotState* pot = state->mutable_pot();
  pot->set_phase(pe::POT_PHASE_ACTION);
  addLayer(pot, 1, 3, {0, 2}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 6, 10, {0, 2}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 14, 8, {0}, pe::LAYER_STATUS_PENDING);
  pot->set_pot_total(21);

  // Forced contributions.
  addForced(state, 1, pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 1);
  addForced(state, 2, pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 2);

  // Transition path (9 events).
  addAction(state, 1, pe::STREET_PREFLOP, 3, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 2, pe::STREET_PREFLOP, 4, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 3, pe::STREET_PREFLOP, 5, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 4, pe::STREET_PREFLOP, 0, pe::ACTION_TYPE_RAISE, 3, 194, false, 6);
  addAction(state, 5, pe::STREET_PREFLOP, 1, pe::ACTION_TYPE_FOLD, 9, 199);
  addAction(state, 6, pe::STREET_PREFLOP, 2, pe::ACTION_TYPE_CALL, 9, 194, false, std::nullopt, 4);
  addBoardDeal(state, 7, pe::STREET_FLOP,
               {card(pe::RANK_ACE, pe::SUIT_SPADES), card(pe::RANK_KING, pe::SUIT_DIAMONDS),
                card(pe::RANK_SEVEN, pe::SUIT_CLUBS)});
  addAction(state, 8, pe::STREET_FLOP, 2, pe::ACTION_TYPE_CHECK, 13, 194);
  addAction(state, 9, pe::STREET_FLOP, 0, pe::ACTION_TYPE_BET, 13, 186, false, 8);

  // Legal actions: FOLD, CALL(8), RAISE(min 16, max 194).
  addLegal(state, pe::ACTION_TYPE_FOLD);
  addLegal(state, pe::ACTION_TYPE_CALL);
  addLegal(state, pe::ACTION_TYPE_RAISE, 16, 194);
  state->set_to_call(8);

  setProvenance(state, 9);
  setOptions(&request, mode);
  return request;
}

}  // namespace bs::conformance
