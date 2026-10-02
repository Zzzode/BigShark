#include "hand_state_builder.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace bs::engine_client {

namespace {

namespace pv = ::bigshark::engine::v1;

pv::Street map_street(bs::poker::Street street) {
  switch (street) {
    case bs::poker::Street::Preflop:
      return pv::STREET_PREFLOP;
    case bs::poker::Street::Flop:
      return pv::STREET_FLOP;
    case bs::poker::Street::Turn:
      return pv::STREET_TURN;
    case bs::poker::Street::River:
      return pv::STREET_RIVER;
  }
  throw std::invalid_argument("hand_state_builder: unknown street");
}

// Card id (0..51) -> protobuf rank (TWO=1..ACE=13).
pv::Rank map_rank(int card_id) {
  return static_cast<pv::Rank>((card_id >> 2) + 1);
}

// Card id (0..51) -> protobuf suit (SPADES=1..CLUBS=4).
pv::Suit map_suit(int card_id) {
  return static_cast<pv::Suit>((card_id & 3) + 1);
}

// Action type mapping with the preflop aggressive verb convention: the unified
// engine types preflop aggression as Bet, but the server vocabulary is raise.
// Preflop Bet/Raise both map to ACTION_TYPE_RAISE; postflop maps 1:1.
pv::ActionType map_action_type(bs::poker::ActionType type, bool preflop) {
  switch (type) {
    case bs::poker::ActionType::Fold:
      return pv::ACTION_TYPE_FOLD;
    case bs::poker::ActionType::Check:
      return pv::ACTION_TYPE_CHECK;
    case bs::poker::ActionType::Call:
      return pv::ACTION_TYPE_CALL;
    case bs::poker::ActionType::Bet:
      return preflop ? pv::ACTION_TYPE_RAISE : pv::ACTION_TYPE_BET;
    case bs::poker::ActionType::Raise:
      return pv::ACTION_TYPE_RAISE;
  }
  throw std::invalid_argument("hand_state_builder: unknown action type");
}

bool is_aggressive(bs::poker::ActionType type) {
  return type == bs::poker::ActionType::Bet || type == bs::poker::ActionType::Raise;
}

void fill_card(pv::Card* card, int card_id) {
  card->set_rank(map_rank(card_id));
  card->set_suit(map_suit(card_id));
}

}  // namespace

pv::DecisionRequest build_decision_request(const bs::poker::GameState& state, std::size_t hero_seat,
                                           const bs::stage6::HoleCards& hole,
                                           const bs::stage6::HandLog* log,
                                           std::uint64_t decision_seed,
                                           const EngineClientConfig& config,
                                           const pv::GetCapabilitiesResponse& capabilities) {
  if (state.phase() != bs::poker::Phase::Action)
    throw std::invalid_argument("build_decision_request requires an action phase");
  const auto actor = state.actor();
  if (!actor || *actor != hero_seat)
    throw std::invalid_argument("build_decision_request requires the acting seat");

  const bs::poker::GameDef& def = state.def();
  const std::size_t n = def.player_count;
  const bs::poker::Chips bb = def.big_blind;
  if (bb == 0)
    throw std::invalid_argument("build_decision_request requires a posted big blind");
  const bool preflop = state.street() == bs::poker::Street::Preflop;

  pv::DecisionRequest request;
  pv::HandState* hs = request.mutable_state();

  // Game definition.
  pv::GameDefinition* game = hs->mutable_game();
  game->set_variant(pv::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv::GAME_TYPE_CASH);
  game->set_table_capacity(static_cast<std::uint32_t>(n));
  pv::AmountUnit* amount_unit = game->mutable_amount_unit();
  amount_unit->set_name("chip");
  amount_unit->set_decimal_places(0);
  game->set_small_blind(bb / 2);
  game->set_big_blind(bb);
  game->set_ante(0);

  // HandState scalars.
  hs->set_hand_id("practice-" + std::to_string(decision_seed));
  hs->set_decision_index(0);
  hs->set_street(map_street(state.street()));
  hs->set_button_seat(static_cast<std::uint32_t>(def.button));
  hs->set_hero_player_id("seat" + std::to_string(hero_seat));
  hs->set_to_call(state.legal().call_amount);

  // Players.
  for (std::size_t p = 0; p < n; ++p) {
    const bs::poker::GamePlayer& gp = state.players()[p];
    pv::PlayerState* ps = hs->add_players();
    ps->set_player_id("seat" + std::to_string(p));
    ps->set_seat(static_cast<std::uint32_t>(p));
    ps->set_stack(gp.stack);
    ps->set_street_committed(gp.street_committed);
    if (gp.folded)
      ps->set_status(pv::PLAYER_STATUS_FOLDED);
    else if (gp.all_in)
      ps->set_status(pv::PLAYER_STATUS_ALL_IN);
    else
      ps->set_status(pv::PLAYER_STATUS_ACTIVE);
  }

  // Hero hole cards.
  for (int card : hole) {
    fill_card(hs->add_hero_hole_cards(), card);
  }

  // Board.
  for (int card : state.board()) {
    fill_card(hs->add_board(), card);
  }

  // Pot (no side pots in the practice simulator).
  pv::PotState* pot = hs->mutable_pot();
  pot->set_pot_total(state.pot());
  pot->set_main_pot(state.pot());

  // Forced contributions from posted blinds.
  for (std::size_t p = 0; p < n; ++p) {
    const bs::poker::Chips amount = def.blinds_posted[p];
    if (amount == 0)
      continue;
    pv::ForcedContribution* fc = hs->add_forced_contributions();
    fc->set_player_id("seat" + std::to_string(p));
    fc->set_type(amount == def.big_blind ? pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND
                                         : pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    fc->set_amount(amount);
  }

  // Action history via exact replay from the root. The HandLog records only
  // {seat, action}; pot_before/stack_after/all_in are reconstructed by
  // replaying the deterministic machine through the logged actions.
  if (log != nullptr) {
    bs::poker::GameState replay(def);
    std::uint64_t seq = 1;

    const std::vector<bs::stage6::LoggedAction>* street_logs[] = {&log->preflop, &log->flop,
                                                                  &log->turn, &log->river};
    const bs::poker::Street streets[] = {bs::poker::Street::Preflop, bs::poker::Street::Flop,
                                         bs::poker::Street::Turn, bs::poker::Street::River};

    for (std::size_t s = 0; s < 4; ++s) {
      const bool street_preflop = streets[s] == bs::poker::Street::Preflop;
      for (const bs::stage6::LoggedAction& la : *street_logs[s]) {
        pv::ActionEvent* event = hs->add_action_history();
        event->set_sequence(seq++);
        event->set_street(map_street(streets[s]));
        event->set_actor_player_id("seat" + std::to_string(la.seat));
        event->set_action(map_action_type(la.action.type, street_preflop));
        if (is_aggressive(la.action.type))
          event->set_target_total(la.action.target_total);
        event->set_pot_before(replay.pot());
        replay = replay.after_action(la.seat, la.action);
        event->set_stack_after(replay.players()[la.seat].stack);
        event->set_all_in(replay.players()[la.seat].all_in);
      }
      // Advance the board between streets. The guard prevents out-of-bounds
      // when the HandLog is inconsistent with the live state; the invariant
      // check below catches the mismatch.
      while (replay.phase() == bs::poker::Phase::Deal &&
             replay.board().size() < state.board().size()) {
        replay = replay.after_card(state.board()[replay.board().size()]);
      }
    }

    // Invariant check: the replayed state must match the live state. A
    // mismatch means the HandLog is inconsistent; the policy catches this as
    // a fallback.
    if (replay.pot() != state.pot() || replay.street() != state.street())
      throw std::runtime_error("hand_state_builder: replay invariant mismatch (pot/street)");
    for (std::size_t p = 0; p < n; ++p) {
      if (replay.players()[p].stack != state.players()[p].stack)
        throw std::runtime_error("hand_state_builder: replay invariant mismatch (stack)");
    }
  }

  // Legal actions in the same order as the adapter: fold, check, call, then
  // aggressive. Preflop aggression is typed RAISE per the server convention.
  const bs::poker::LegalActions legal = state.legal();
  if (legal.fold) {
    hs->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  }
  if (legal.check) {
    hs->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
  }
  if (legal.call) {
    hs->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  }
  if (legal.aggressive) {
    pv::LegalAction* la = hs->add_legal_actions();
    la->set_type(map_action_type(legal.aggressive->type, preflop));
    la->set_min_target_total(legal.aggressive->minimum);
    la->set_max_target_total(legal.aggressive->maximum);
    la->set_all_in_only(legal.aggressive->all_in_only);
  }

  // Decision options.
  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile(config.strategy_profile);
  const std::uint32_t budget =
      std::min(config.solve_budget_ms, capabilities.maximum_solve_time_ms() > 0
                                           ? capabilities.maximum_solve_time_ms()
                                           : config.solve_budget_ms);
  options->set_solve_time_budget_ms(budget);
  options->set_seed(decision_seed);
  options->set_include_sampled_action(true);
  options->set_include_full_strategy(false);
  options->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);

  // Preflop effective stack depth in big blinds (heads-up): the shorter of
  // hero and opponent total (stack + street_committed) divided by the big
  // blind. Sent only on the preflop decision, matching the server's hint
  // contract.
  if (preflop && n == 2) {
    const std::size_t opp = hero_seat == 0 ? 1 : 0;
    const bs::poker::Chips hero_total =
        state.players()[hero_seat].stack + state.players()[hero_seat].street_committed;
    const bs::poker::Chips opp_total =
        state.players()[opp].stack + state.players()[opp].street_committed;
    const bs::poker::Chips effective = std::min(hero_total, opp_total);
    options->set_preflop_effective_stack_bb(static_cast<double>(effective) /
                                            static_cast<double>(bb));
  }

  return request;
}

}  // namespace bs::engine_client
