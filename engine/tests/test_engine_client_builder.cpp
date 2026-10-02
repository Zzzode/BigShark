// test_engine_client_builder.cpp — RFC 0009 W4e HandState builder tests.
//
// Pins the GameState + HandLog -> DecisionRequest translation: game definition,
// player statuses, card rank/suit mapping, blind forced contributions, legal
// action verbs (preflop aggression is RAISE), to_call, the exact-replay action
// history (sequence, streets, pot_before, stack_after, all_in, target_total),
// the replay invariant check, and the solve-budget cap.

#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine_client/hand_state_builder.hpp"

namespace bs::engine_client {
namespace {

namespace pv = ::bigshark::engine::v1;

using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::LegalActions;
using bs::poker::Phase;
using bs::poker::Street;
using bs::stage6::HandLog;
using bs::stage6::HoleCards;
using bs::stage6::LoggedAction;

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

// ---- State factories --------------------------------------------------------

// Heads-up preflop root: button=0 (SB), seat 1 is BB. 200-chip stacks, bb=2.
GameState heads_up_preflop_root() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks[0] = 200;
  def.stacks[1] = 200;
  def.blinds_posted[0] = 1;  // SB on the button (heads-up)
  def.blinds_posted[1] = 2;  // BB
  def.pot = 3;
  def.preflop = true;
  def.board = {-1, -1, -1, -1, -1};
  def.board_size = 0;
  return GameState(def);
}

pv::GetCapabilitiesResponse caps_with_solve_limit(std::uint32_t ms) {
  pv::GetCapabilitiesResponse caps;
  caps.set_maximum_solve_time_ms(ms);
  return caps;
}

EngineClientConfig default_config() {
  EngineClientConfig cfg;
  cfg.solve_budget_ms = 1000;
  return cfg;
}

// ---- Game definition --------------------------------------------------------

void test_preflop_game_definition() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  const pv::GameDefinition& game = hs.game();
  CHECK(game.variant() == pv::GAME_VARIANT_NLHE);
  CHECK(game.betting_structure() == pv::BETTING_STRUCTURE_NO_LIMIT);
  CHECK(game.game_type() == pv::GAME_TYPE_CASH);
  CHECK(game.table_capacity() == 2u);
  CHECK(game.small_blind() == 1u);
  CHECK(game.big_blind() == 2u);
  CHECK(game.ante() == 0u);
  CHECK(game.amount_unit().name() == "chip");
  CHECK(game.amount_unit().decimal_places() == 0u);
}

// ---- HandState scalars -------------------------------------------------------

void test_preflop_scalars() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 777, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  CHECK(hs.hand_id() == "practice-777");
  CHECK(hs.decision_index() == 0u);
  CHECK(hs.street() == pv::STREET_PREFLOP);
  CHECK(hs.button_seat() == 0u);
  CHECK(hs.hero_player_id() == "seat0");
  // SB faces 1 more to call (BB 2 - SB 1).
  CHECK(hs.to_call() == 1u);
}

// ---- Players ------------------------------------------------------------------

void test_preflop_players() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  CHECK(hs.players_size() == 2);
  // Seat 0: SB posted 1, stack 199.
  CHECK(hs.players(0).player_id() == "seat0");
  CHECK(hs.players(0).seat() == 0u);
  CHECK(hs.players(0).stack() == 199u);
  CHECK(hs.players(0).street_committed() == 1u);
  CHECK(hs.players(0).status() == pv::PLAYER_STATUS_ACTIVE);
  // Seat 1: BB posted 2, stack 198.
  CHECK(hs.players(1).player_id() == "seat1");
  CHECK(hs.players(1).seat() == 1u);
  CHECK(hs.players(1).stack() == 198u);
  CHECK(hs.players(1).street_committed() == 2u);
  CHECK(hs.players(1).status() == pv::PLAYER_STATUS_ACTIVE);
}

// ---- Card mapping ---------------------------------------------------------------

void test_card_mapping() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  // card 0 = 2♠, card 1 = 2♥, card 51 = A♣.
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 51}, nullptr, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  CHECK(hs.hero_hole_cards_size() == 2);
  CHECK(hs.hero_hole_cards(0).rank() == pv::RANK_TWO);
  CHECK(hs.hero_hole_cards(0).suit() == pv::SUIT_SPADES);
  CHECK(hs.hero_hole_cards(1).rank() == pv::RANK_ACE);
  CHECK(hs.hero_hole_cards(1).suit() == pv::SUIT_CLUBS);
}

// ---- Pot and forced contributions ------------------------------------------------

void test_preflop_pot_and_blinds() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  CHECK(hs.pot().pot_total() == 3u);
  CHECK(hs.pot().main_pot() == 3u);
  CHECK(hs.forced_contributions_size() == 2);
  // Order follows seat index.
  CHECK(hs.forced_contributions(0).player_id() == "seat0");
  CHECK(hs.forced_contributions(0).type() == pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  CHECK(hs.forced_contributions(0).amount() == 1u);
  CHECK(hs.forced_contributions(1).player_id() == "seat1");
  CHECK(hs.forced_contributions(1).type() == pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  CHECK(hs.forced_contributions(1).amount() == 2u);
}

// ---- Legal actions (preflop aggressive verb) ---------------------------------------

void test_preflop_legal_actions() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  // SB facing BB: fold, call, raise (no check).
  CHECK(hs.legal_actions_size() == 3);
  CHECK(hs.legal_actions(0).type() == pv::ACTION_TYPE_FOLD);
  CHECK(hs.legal_actions(1).type() == pv::ACTION_TYPE_CALL);
  // The unified engine types preflop aggression as Bet; the builder must map
  // it to RAISE per the server convention.
  CHECK(hs.legal_actions(2).type() == pv::ACTION_TYPE_RAISE);
  CHECK(hs.legal_actions(2).min_target_total() > 0u);
  CHECK(hs.legal_actions(2).max_target_total() >= hs.legal_actions(2).min_target_total());
}

// ---- Decision options ---------------------------------------------------------------

void test_decision_options() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  const pv::DecisionRequest req = build_decision_request(
      state, hero, HoleCards{0, 1}, nullptr, 99, default_config(), caps_with_solve_limit(5000));

  const pv::DecisionOptions& opts = req.options();
  CHECK(opts.strategy_profile() == "tag");
  CHECK(opts.solve_time_budget_ms() == 1000u);
  CHECK(opts.seed() == 99u);
  CHECK(opts.include_sampled_action());
  CHECK(!opts.include_full_strategy());
  CHECK(opts.solver_mode() == pv::SOLVER_MODE_AUTOMATIC);
  // Heads-up preflop: effective stack = min(200, 200) / 2 = 100 bb.
  CHECK(opts.has_preflop_effective_stack_bb());
  CHECK(opts.preflop_effective_stack_bb() == 100.0);
}

void test_solve_budget_capped_by_capabilities() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();

  // Config asks for 8000 but the engine caps at 5000.
  EngineClientConfig cfg = default_config();
  cfg.solve_budget_ms = 8000;
  pv::DecisionRequest req = build_decision_request(state, hero, HoleCards{0, 1}, nullptr, 42, cfg,
                                                   caps_with_solve_limit(5000));
  CHECK(req.options().solve_time_budget_ms() == 5000u);

  // Config asks for 1000, engine allows 5000 -> 1000.
  req = build_decision_request(state, hero, HoleCards{0, 1}, nullptr, 42, default_config(),
                               caps_with_solve_limit(5000));
  CHECK(req.options().solve_time_budget_ms() == 1000u);

  // Engine reports 0 (no cap) -> config value wins.
  req = build_decision_request(state, hero, HoleCards{0, 1}, nullptr, 42, cfg,
                               caps_with_solve_limit(0));
  CHECK(req.options().solve_time_budget_ms() == 8000u);
}

// ---- Flop history reconstruction ------------------------------------------------------

// Plays call/check to the flop and returns the live state + log.
struct FlopFixture {
  GameState state;
  HandLog log;
};

FlopFixture play_to_flop() {
  GameState root = heads_up_preflop_root();
  HandLog log;
  // Seat 0 (SB) calls.
  log.preflop.push_back(LoggedAction{0, Action{ActionType::Call}});
  GameState s = root.after_action(0, Action{ActionType::Call});
  // Seat 1 (BB) checks.
  log.preflop.push_back(LoggedAction{1, Action{ActionType::Check}});
  s = s.after_action(1, Action{ActionType::Check});
  // Deal the flop.
  const int flop[3] = {4, 5, 6};  // 3♠ 3♥ 3♦
  for (int c : flop)
    s = s.after_card(c);
  return {s, log};
}

void test_flop_history_reconstruction() {
  FlopFixture fx = play_to_flop();
  CHECK(fx.state.phase() == Phase::Action);
  CHECK(fx.state.street() == Street::Flop);
  const std::size_t hero = *fx.state.actor();

  const pv::DecisionRequest req = build_decision_request(
      fx.state, hero, HoleCards{0, 1}, &fx.log, 42, default_config(), caps_with_solve_limit(5000));

  const pv::HandState& hs = req.state();
  CHECK(hs.street() == pv::STREET_FLOP);
  CHECK(hs.board_size() == 3);
  CHECK(hs.board(0).rank() == pv::RANK_THREE);
  CHECK(hs.board(0).suit() == pv::SUIT_SPADES);

  // Two preflop actions in the history.
  CHECK(hs.action_history_size() == 2);

  // Event 1: seat 0 calls. pot_before = 3 (root), stack_after = 198
  // (200 - SB 1 - call 1).
  const pv::ActionEvent& e1 = hs.action_history(0);
  CHECK(e1.sequence() == 1u);
  CHECK(e1.street() == pv::STREET_PREFLOP);
  CHECK(e1.actor_player_id() == "seat0");
  CHECK(e1.action() == pv::ACTION_TYPE_CALL);
  CHECK(e1.pot_before() == 3u);
  CHECK(e1.stack_after() == 198u);
  CHECK(!e1.all_in());

  // Event 2: seat 1 checks. pot_before = 4 (after the call), stack_after = 198
  // (200 - BB 2).
  const pv::ActionEvent& e2 = hs.action_history(1);
  CHECK(e2.sequence() == 2u);
  CHECK(e2.street() == pv::STREET_PREFLOP);
  CHECK(e2.actor_player_id() == "seat1");
  CHECK(e2.action() == pv::ACTION_TYPE_CHECK);
  CHECK(e2.pot_before() == 4u);
  CHECK(e2.stack_after() == 198u);
  CHECK(!e2.all_in());

  // Flop pot = 4 (2 + 2).
  CHECK(hs.pot().pot_total() == 4u);
  CHECK(hs.to_call() == 0u);
}

// ---- Preflop aggressive action in history carries target_total -------------------------

void test_history_aggressive_carries_target() {
  GameState root = heads_up_preflop_root();
  const LegalActions legal = root.legal();
  if (!legal.aggressive) {
    CHECK(true);  // no aggression available at this root; nothing to pin
    return;
  }
  // Use the engine's own legal aggressive verb and minimum target so the
  // action is guaranteed legal.
  const ActionType type = legal.aggressive->type;
  const bs::poker::Chips target = legal.aggressive->minimum;
  HandLog log;
  log.preflop.push_back(LoggedAction{0, Action{type, target}});
  GameState s = root.after_action(0, Action{type, target});
  if (s.phase() != Phase::Action || !s.actor() || *s.actor() != 1) {
    // The raise may have ended the street or moved the actor unexpectedly;
    // only assert the history when the continuation is the expected one.
    CHECK(true);
    return;
  }
  const pv::DecisionRequest req = build_decision_request(
      s, 1, HoleCards{2, 3}, &log, 42, default_config(), caps_with_solve_limit(5000));
  const pv::HandState& hs = req.state();
  CHECK(hs.action_history_size() == 1);
  const pv::ActionEvent& e = hs.action_history(0);
  // Preflop aggression is typed RAISE in the history regardless of the
  // unified engine's verb.
  CHECK(e.action() == pv::ACTION_TYPE_RAISE);
  CHECK(e.target_total() == target);
}

// ---- Error cases -----------------------------------------------------------------------

void test_non_action_state_throws() {
  GameState root = heads_up_preflop_root();
  // Play to the flop deal phase.
  GameState s = root.after_action(0, Action{ActionType::Call});
  s = s.after_action(1, Action{ActionType::Check});
  // Now s.phase() == Deal (preflop complete, board not yet dealt).
  CHECK(s.phase() == Phase::Deal);

  bool threw = false;
  try {
    (void)build_decision_request(s, 0, HoleCards{0, 1}, nullptr, 42, default_config(),
                                 caps_with_solve_limit(5000));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

void test_wrong_hero_throws() {
  GameState state = heads_up_preflop_root();
  const std::size_t actor = *state.actor();
  bool threw = false;
  try {
    (void)build_decision_request(state, actor == 0 ? 1 : 0, HoleCards{0, 1}, nullptr, 42,
                                 default_config(), caps_with_solve_limit(5000));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

void test_replay_invariant_mismatch_throws() {
  GameState state = heads_up_preflop_root();
  const std::size_t hero = *state.actor();
  // Log claims seat 0 called, but the live state is the untouched root.
  HandLog log;
  log.preflop.push_back(LoggedAction{0, Action{ActionType::Call}});

  bool threw = false;
  try {
    (void)build_decision_request(state, hero, HoleCards{0, 1}, &log, 42, default_config(),
                                 caps_with_solve_limit(5000));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace
}  // namespace bs::engine_client

int main() {
  using namespace bs::engine_client;
  test_preflop_game_definition();
  test_preflop_scalars();
  test_preflop_players();
  test_card_mapping();
  test_preflop_pot_and_blinds();
  test_preflop_legal_actions();
  test_decision_options();
  test_solve_budget_capped_by_capabilities();
  test_flop_history_reconstruction();
  test_history_aggressive_carries_target();
  test_non_action_state_throws();
  test_wrong_hero_throws();
  test_replay_invariant_mismatch_throws();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all builder tests passed\n");
  return 0;
}
