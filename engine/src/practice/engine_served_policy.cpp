// practice/engine_served_policy.cpp — engine-host-backed BehaviorPolicy.
#include "bs/practice/engine_served_policy.hpp"

#include <bigshark/engine/v2/engine.pb.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::practice {
namespace {

// Convert an engine card id (0..51, rank = id >> 2, suit = id & 3) to a v2
// Card message.
pv2::Card to_v2_card(int card_id) {
  pv2::Card card;
  card.set_rank(static_cast<pv2::Rank>((card_id >> 2) + 1));
  card.set_suit(static_cast<pv2::Suit>((card_id & 3) + 1));
  return card;
}

// Convert the engine street enum to v2 Street.
pv2::Street to_v2_street(poker::Street street) {
  switch (street) {
    case poker::Street::Preflop:
      return pv2::STREET_PREFLOP;
    case poker::Street::Flop:
      return pv2::STREET_FLOP;
    case poker::Street::Turn:
      return pv2::STREET_TURN;
    case poker::Street::River:
      return pv2::STREET_RIVER;
  }
  return pv2::STREET_PREFLOP;
}

// Convert the engine ActionType to v2 ActionType.
pv2::ActionType to_v2_action_type(poker::ActionType type) {
  switch (type) {
    case poker::ActionType::Fold:
      return pv2::ACTION_TYPE_FOLD;
    case poker::ActionType::Check:
      return pv2::ACTION_TYPE_CHECK;
    case poker::ActionType::Call:
      return pv2::ACTION_TYPE_CALL;
    case poker::ActionType::Bet:
      return pv2::ACTION_TYPE_BET;
    case poker::ActionType::Raise:
      return pv2::ACTION_TYPE_RAISE;
  }
  return pv2::ACTION_TYPE_FOLD;
}

// Convert v2 ActionType to engine ActionType.
poker::ActionType from_v2_action_type(pv2::ActionType type) {
  switch (type) {
    case pv2::ACTION_TYPE_FOLD:
      return poker::ActionType::Fold;
    case pv2::ACTION_TYPE_CHECK:
      return poker::ActionType::Check;
    case pv2::ACTION_TYPE_CALL:
      return poker::ActionType::Call;
    case pv2::ACTION_TYPE_BET:
      return poker::ActionType::Bet;
    case pv2::ACTION_TYPE_RAISE:
      return poker::ActionType::Raise;
    default:
      return poker::ActionType::Fold;
  }
}

// Build the pot layers from the GameState's player contributions.
// Layers are contribution levels: for each unique net contribution level L
// (ascending), the layer amount is (L - prev) * count(net >= L), and the
// eligible seats are live players with net >= L.
std::vector<pv2::PotLayer> build_pot_layers(const poker::GameState& state) {
  const auto& players = state.players();
  const std::size_t n = state.player_count();

  // Collect net contributions per seat.
  std::vector<poker::Chips> nets(n);
  for (std::size_t i = 0; i < n; ++i)
    nets[i] = players[i].net_contributed();

  // Collect unique positive levels, sorted ascending.
  std::vector<poker::Chips> levels;
  for (std::size_t i = 0; i < n; ++i) {
    if (nets[i] > 0)
      levels.push_back(nets[i]);
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

  std::vector<pv2::PotLayer> layers;
  poker::Chips prev = 0;
  for (poker::Chips level : levels) {
    const poker::Chips diff = level - prev;
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (nets[i] >= level)
        ++count;
    }
    if (count == 0 || diff == 0)
      continue;

    pv2::PotLayer layer;
    layer.set_contribution_level(static_cast<std::uint64_t>(level));
    layer.set_amount(static_cast<std::uint64_t>(diff) * count);
    layer.set_status(pv2::LAYER_STATUS_PENDING);
    for (std::size_t i = 0; i < n; ++i) {
      if (nets[i] >= level && !players[i].folded)
        layer.add_eligible_seats(static_cast<std::uint32_t>(i));
    }
    layers.push_back(std::move(layer));
    prev = level;
  }
  return layers;
}

// Build the transition path from the HandLog. BoardDeal events are inserted
// at street boundaries with the actual board cards from the GameState.
// Call actions carry the correct incremental amount computed by tracking
// street commitments through the replay.
std::vector<pv2::TransitionEvent> build_transition_path(const poker::GameState& state,
                                                        const stage6::HandLog& log) {
  std::vector<pv2::TransitionEvent> path;
  std::uint64_t seq = 0;
  const auto& def = state.def();
  const std::size_t n = state.player_count();

  // Track street commitments and stacks for computing call amounts.
  // Start with the blinds as the initial street commitments.
  std::array<poker::Chips, 10> street_committed{};
  std::array<poker::Chips, 10> current_stack{};
  for (std::size_t i = 0; i < n; ++i) {
    street_committed[i] = def.blinds_posted[i];
    current_stack[i] = def.stacks[i] - def.blinds_posted[i];
  }

  auto current_high = [&]() -> poker::Chips {
    poker::Chips high = 0;
    for (std::size_t i = 0; i < n; ++i)
      high = std::max(high, street_committed[i]);
    return high;
  };

  auto add_board_deal = [&](poker::Street street, const std::vector<int>& cards) {
    pv2::TransitionEvent event;
    event.set_sequence(seq++);
    pv2::BoardDeal* deal = event.mutable_board_deal();
    deal->set_street(to_v2_street(street));
    for (int card : cards)
      deal->add_cards()->CopyFrom(to_v2_card(card));
    path.push_back(std::move(event));
    // Reset street commitments at the street boundary.
    street_committed.fill(0);
  };

  auto add_actions = [&](poker::Street street, const std::vector<stage6::LoggedAction>& actions) {
    for (const auto& logged : actions) {
      const std::size_t actor = logged.seat;
      poker::Chips paid = 0;
      pv2::TransitionEvent event;
      event.set_sequence(seq++);
      pv2::ActionEvent* action = event.mutable_action();
      action->set_sequence(seq);
      action->set_street(to_v2_street(street));
      action->set_actor_seat(static_cast<std::uint32_t>(actor));
      action->set_action(to_v2_action_type(logged.action.type));

      switch (logged.action.type) {
        case poker::ActionType::Fold:
        case poker::ActionType::Check:
          // No chip movement.
          break;
        case poker::ActionType::Call: {
          const poker::Chips high = current_high();
          paid = std::min(high - street_committed[actor], current_stack[actor]);
          action->set_incremental_amount(static_cast<std::uint64_t>(paid));
          break;
        }
        case poker::ActionType::Bet:
        case poker::ActionType::Raise: {
          action->set_target_total(static_cast<std::uint64_t>(logged.action.target_total));
          paid = logged.action.target_total - street_committed[actor];
          break;
        }
      }
      action->set_pot_before(0);   // 0 = not validated by the host
      action->set_stack_after(0);  // 0 = not validated by the host
      action->set_all_in(false);
      path.push_back(std::move(event));

      // Update the ledger.
      street_committed[actor] += paid;
      current_stack[actor] -= paid;
    }
  };

  // Preflop actions (no BoardDeal for preflop).
  add_actions(poker::Street::Preflop, log.preflop);

  // Board cards from the GameState.
  const auto board = state.board();
  const std::size_t board_size = board.size();

  // Flop: BoardDeal + actions.
  if (board_size >= 3) {
    std::vector<int> flop_cards(board.begin(), board.begin() + 3);
    add_board_deal(poker::Street::Flop, flop_cards);
    add_actions(poker::Street::Flop, log.flop);
  }

  // Turn: BoardDeal + actions.
  if (board_size >= 4) {
    std::vector<int> turn_cards{board[3]};
    add_board_deal(poker::Street::Turn, turn_cards);
    add_actions(poker::Street::Turn, log.turn);
  }

  // River: BoardDeal + actions.
  if (board_size >= 5) {
    std::vector<int> river_cards{board[4]};
    add_board_deal(poker::Street::River, river_cards);
    add_actions(poker::Street::River, log.river);
  }

  return path;
}

// Build the legal actions from the GameState's legal actions.
std::vector<pv2::LegalAction> build_legal_actions(const poker::LegalActions& legal) {
  std::vector<pv2::LegalAction> result;
  if (legal.fold) {
    pv2::LegalAction a;
    a.set_type(pv2::ACTION_TYPE_FOLD);
    result.push_back(std::move(a));
  }
  if (legal.check) {
    pv2::LegalAction a;
    a.set_type(pv2::ACTION_TYPE_CHECK);
    result.push_back(std::move(a));
  }
  if (legal.call) {
    pv2::LegalAction a;
    a.set_type(pv2::ACTION_TYPE_CALL);
    result.push_back(std::move(a));
  }
  if (legal.aggressive) {
    pv2::LegalAction a;
    a.set_type(to_v2_action_type(legal.aggressive->type));
    a.set_min_target_total(static_cast<std::uint64_t>(legal.aggressive->minimum));
    a.set_max_target_total(static_cast<std::uint64_t>(legal.aggressive->maximum));
    a.set_all_in_only(legal.aggressive->all_in_only);
    result.push_back(std::move(a));
  }
  return result;
}

// Build a v2 DecisionRequest from the GameState, HandLog, and HoleCards.
pv2::DecisionRequest build_request(const poker::GameState& state, std::size_t hero_seat,
                                   stage6::HoleCards hole, const stage6::HandLog& log,
                                   std::uint32_t solve_budget_ms, std::uint64_t seed,
                                   std::uint64_t decision_index) {
  const auto& def = state.def();
  const auto& players = state.players();
  const std::size_t n = state.player_count();

  pv2::DecisionRequest request;
  pv2::HandState* hand = request.mutable_state();

  // GameDefinition.
  pv2::GameDefinition* game = hand->mutable_game();
  game->set_variant(pv2::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv2::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv2::GAME_TYPE_CASH);
  game->set_table_capacity(static_cast<std::uint32_t>(n));
  pv2::AmountUnit* unit = game->mutable_amount_unit();
  unit->set_name("chip");
  unit->set_decimal_places(0);
  game->set_small_blind(static_cast<std::uint64_t>(def.big_blind / 2));
  game->set_big_blind(static_cast<std::uint64_t>(def.big_blind));
  game->set_ante(0);
  game->set_button_ante(0);
  pv2::RuleProfile* rules = game->mutable_rule_profile();
  rules->set_settlement(pv2::SETTLEMENT_RULE_CLOCKWISE_LEFT_OF_BUTTON);
  rules->set_reopening(pv2::REOPENING_RULE_NEVER_CUMULATIVE);
  rules->set_runout(pv2::RUNOUT_RULE_SINGLE);
  rules->set_dealt_seat_count(static_cast<std::uint32_t>(n));

  // Hand identity.
  hand->set_hand_id("practice-" + std::to_string(decision_index));
  hand->set_decision_index(decision_index);
  hand->set_street(to_v2_street(state.street()));
  hand->set_button_seat(static_cast<std::uint32_t>(def.button));
  hand->set_hero_seat(static_cast<std::uint32_t>(hero_seat));

  // Player states.
  for (std::size_t i = 0; i < n; ++i) {
    pv2::PlayerState* p = hand->add_players();
    p->set_seat(static_cast<std::uint32_t>(i));
    p->set_starting_stack(static_cast<std::uint64_t>(def.stacks[i]));
    p->set_stack(static_cast<std::uint64_t>(players[i].stack));
    p->set_street_committed(static_cast<std::uint64_t>(players[i].street_committed));
    p->set_gross_contributed(static_cast<std::uint64_t>(players[i].contributed));
    p->set_refunded(static_cast<std::uint64_t>(players[i].refunded));
    if (players[i].folded)
      p->set_status(pv2::PLAYER_STATUS_FOLDED);
    else if (players[i].all_in)
      p->set_status(pv2::PLAYER_STATUS_ALL_IN);
    else
      p->set_status(pv2::PLAYER_STATUS_ACTIVE);
  }

  // Forced contributions (blinds). Heads-up: button posts SB. 3+ seats:
  // button+1 posts SB, button+2 posts BB.
  const poker::Chips sb = def.big_blind / 2;
  const std::size_t sb_seat = (n == 2) ? def.button : (def.button + 1) % n;
  const std::size_t bb_seat = (n == 2) ? (def.button + 1) % n : (def.button + 2) % n;
  pv2::ForcedContribution* sb_forced = hand->add_forced_contributions();
  sb_forced->set_seat(static_cast<std::uint32_t>(sb_seat));
  sb_forced->set_type(pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  sb_forced->set_amount(static_cast<std::uint64_t>(sb));
  pv2::ForcedContribution* bb_forced = hand->add_forced_contributions();
  bb_forced->set_seat(static_cast<std::uint32_t>(bb_seat));
  bb_forced->set_type(pv2::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  bb_forced->set_amount(static_cast<std::uint64_t>(def.big_blind));

  // Hero hole cards.
  hand->add_hero_hole_cards()->CopyFrom(to_v2_card(hole[0]));
  hand->add_hero_hole_cards()->CopyFrom(to_v2_card(hole[1]));

  // Board.
  for (int card : state.board())
    hand->add_board()->CopyFrom(to_v2_card(card));

  // Pot.
  pv2::PotState* pot = hand->mutable_pot();
  pot->set_phase(pv2::POT_PHASE_ACTION);
  for (auto& layer : build_pot_layers(state))
    pot->add_layers()->Swap(&layer);
  pot->set_pot_total(static_cast<std::uint64_t>(state.pot()));

  // Transition path.
  for (auto& event : build_transition_path(state, log))
    hand->add_transition_path()->Swap(&event);

  // Legal actions.
  for (auto& action : build_legal_actions(state.legal()))
    hand->add_legal_actions()->Swap(&action);
  hand->set_to_call(static_cast<std::uint64_t>(state.legal().call_amount));

  // Observation provenance. FULL coverage requires a non-empty transition
  // path; the first decision of a hand (preflop, no prior actions) uses
  // MID_HAND_ATTACH and falls back to the safe distribution.
  pv2::ObservationProvenance* provenance = hand->mutable_provenance();
  provenance->set_source(pv2::OBSERVATION_SOURCE_SUPPLIED);
  provenance->set_coverage(hand->transition_path_size() > 0
                               ? pv2::OBSERVATION_COVERAGE_FULL
                               : pv2::OBSERVATION_COVERAGE_MID_HAND_ATTACH);
  provenance->set_controller_epoch("practice");
  provenance->set_observed_event_count(static_cast<std::uint32_t>(hand->transition_path_size()));

  // Decision options.
  pv2::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(solve_budget_ms);
  options->set_seed(seed);
  options->set_include_sampled_action(true);
  options->set_include_full_strategy(false);
  options->set_solver_mode(pv2::SOLVER_MODE_AUTOMATIC);
  options->set_utility_profile(pv2::UTILITY_PROFILE_CASH_CHIP);

  return request;
}

// Fallback distribution: check if legal, else call, else fold.
std::vector<stage6::PolicyAction> fallback_distribution(const poker::GameState& state,
                                                        std::size_t seat) {
  const auto legal = state.legal();
  std::vector<stage6::PolicyAction> result;
  if (legal.check) {
    result.push_back({{poker::ActionType::Check, 0}, 1.0});
  } else if (legal.call) {
    result.push_back({{poker::ActionType::Call, 0}, 1.0});
  } else {
    result.push_back({{poker::ActionType::Fold, 0}, 1.0});
  }
  return result;
}

}  // namespace

EngineServedPolicy::EngineServedPolicy(EngineProcess& engine, std::size_t seat,
                                       std::uint32_t solve_budget_ms)
    : engine_(engine), seat_(seat), solve_budget_ms_(solve_budget_ms) {}

std::vector<stage6::PolicyAction> EngineServedPolicy::distribution(
    const poker::GameState& state, std::size_t seat, stage6::HoleCards hole,
    const stage6::PolicyContext& context) const {
  const stage6::HandLog* log = context.hand_log;
  static const stage6::HandLog empty_log;
  if (!log)
    log = &empty_log;

  // Build the v2 DecisionRequest.
  pv2::DecisionRequest request = build_request(state, seat, hole, *log, solve_budget_ms_,
                                               context.decision_seed, hand_counter_++);

  // Wrap in an envelope.
  pv2::Envelope envelope;
  envelope.set_protocol_version(3);
  envelope.set_request_id("practice-" + std::to_string(hand_counter_));
  envelope.mutable_decision_request()->Swap(&request);

  // Send to the engine host.
  auto response = engine_.request(envelope);
  if (!response) {
    ++fallbacks_;
    return fallback_distribution(state, seat);
  }

  // Parse the response.
  if (!response->has_decision_response()) {
    ++fallbacks_;
    return fallback_distribution(state, seat);
  }

  const pv2::DecisionResponse& decision = response->decision_response();
  if (decision.has_error()) {
    ++fallbacks_;
    return fallback_distribution(state, seat);
  }

  // Extract the action policy list from either strategy shape.
  const google::protobuf::RepeatedPtrField<pv2::ActionPolicy>* actions = nullptr;
  if (decision.has_strategy())
    actions = &decision.strategy().actions();
  else if (decision.has_expanded_strategy())
    actions = &decision.expanded_strategy().actions();

  if (!actions || actions->empty()) {
    ++fallbacks_;
    return fallback_distribution(state, seat);
  }

  // Convert ActionPolicy[] to PolicyAction[].
  std::vector<stage6::PolicyAction> result;
  result.reserve(static_cast<std::size_t>(actions->size()));
  for (const pv2::ActionPolicy& ap : *actions) {
    poker::Action action;
    action.type = from_v2_action_type(ap.type());
    if (ap.has_target_total())
      action.target_total = static_cast<poker::Chips>(ap.target_total());
    result.push_back({action, ap.probability()});
  }

  ++served_;
  return result;
}

}  // namespace bs::practice
