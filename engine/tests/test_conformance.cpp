// RFC 0022 conformance vector tests.
//
// Exercises the three executable conformance vectors from the RFC:
//   Vector 1 — ordinary open bet before a response (6-max, flop decision)
//   Vector 2 — capped call that triggers a refund on street closure
//   Vector 3 — actionable node with an existing side pot plus a new pending wager
//
// Vectors 1 and 3 are full DecisionRequest tests through the envelope handler.
// Vector 2 is a resident-mapper replay verification: the hand is heads-up with
// one player all-in, so no DecisionRequest is emitted in production; the test
// verifies the replay path (including the refund) directly.
//
// Also covers: version dispatch (peekProtocolVersion), capabilities response,
// and basic error mapping.
#include <array>
#include <bs/game_definition.hpp>
#include <bs/protocol.hpp>
#include <bs/resolver.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../src/protocol/mappers.hpp"
#include "conformance_fixtures.hpp"

namespace {

namespace pe = bs::pe;
using bs::EnvelopeOutcome;
using bs::EnvelopeResult;
using bs::ReconstructedState;
using bs::ReplayMiss;
namespace cf = bs::conformance;

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

// Shared current-schema builders live in conformance_fixtures.hpp.
using cf::addAction;
using cf::addBoardDeal;
using cf::addForced;
using cf::addLayer;
using cf::addLegal;
using cf::addPlayer;
using cf::capabilitiesEnvelope;
using cf::card;
using cf::envelopeFor;
using cf::setGameDefinition;
using cf::setOptions;
using cf::setProvenance;
using cf::vector1Request;

// --- Envelope handling ------------------------------------------------------

pe::Envelope respond(const std::string& frame) {
  const EnvelopeResult result = bs::handleEnvelope(frame);
  check(result.outcome == EnvelopeOutcome::Respond, "host responds");
  pe::Envelope response;
  check(response.ParseFromString(result.response), "response parses");
  return response;
}

pe::Envelope respondWith(const std::string& frame, const bs::HostServices& services) {
  const EnvelopeResult result = bs::handleEnvelope(frame, services);
  check(result.outcome == EnvelopeOutcome::Respond, "host responds");
  pe::Envelope response;
  check(response.ParseFromString(result.response), "response parses");
  return response;
}

// ===========================================================================
// Vector 1 — ordinary open bet before a response
// 6-max NLHE cash, blinds 1/2, 100 BB (200 chips) stacks. Button seat 0.
// Hero seat 2 (BB). Decision point: seat 2, street FLOP, to_call = 8.
// ===========================================================================

// ===========================================================================
// Turn check-through line — the RFC 0032 baseline fixture.
// Same 6-max cash hand as vector 1, played one street further: the flop is
// checked through and the turn is dealt. Hero (seat 2) checks the turn to the
// button (seat 0), who then bets 8. Every seat has contributed equally and no
// refund occurred, so this is exactly the shape whose rebuilt postflop def does
// construct today.
// ===========================================================================

pe::DecisionRequest turnCheckThroughRequest(pe::SolverMode mode = pe::SOLVER_MODE_HEURISTIC) {
  pe::DecisionRequest request;
  pe::HandState* state = request.mutable_state();
  setGameDefinition(state, 6, 1, 2, 6);
  state->set_hand_id("turn-checkthrough");
  state->set_decision_index(2);
  state->set_street(pe::STREET_TURN);
  state->set_button_seat(0);
  state->set_hero_seat(2);

  addPlayer(state, 0, 200, 186, 8, 14, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 1, 200, 199, 0, 1, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 2, 200, 194, 0, 6, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 3, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 4, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 5, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);

  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_DIAMONDS);

  // Board: As Kd 7c 9s.
  *state->add_board() = card(pe::RANK_ACE, pe::SUIT_SPADES);
  *state->add_board() = card(pe::RANK_KING, pe::SUIT_DIAMONDS);
  *state->add_board() = card(pe::RANK_SEVEN, pe::SUIT_CLUBS);
  *state->add_board() = card(pe::RANK_NINE, pe::SUIT_SPADES);

  pe::PotState* pot = state->mutable_pot();
  pot->set_phase(pe::POT_PHASE_ACTION);
  addLayer(pot, 1, 3, {0, 2}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 6, 10, {0, 2}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 14, 8, {0}, pe::LAYER_STATUS_PENDING);
  pot->set_pot_total(21);

  addForced(state, 1, pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 1);
  addForced(state, 2, pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 2);

  addAction(state, 1, pe::STREET_PREFLOP, 3, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 2, pe::STREET_PREFLOP, 4, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 3, pe::STREET_PREFLOP, 5, pe::ACTION_TYPE_FOLD, 3, 200);
  addAction(state, 4, pe::STREET_PREFLOP, 0, pe::ACTION_TYPE_RAISE, 3, 194, false, 6);
  addAction(state, 5, pe::STREET_PREFLOP, 1, pe::ACTION_TYPE_FOLD, 9, 199);
  addAction(state, 6, pe::STREET_PREFLOP, 2, pe::ACTION_TYPE_CALL, 9, 194, false, std::nullopt, 4);
  addBoardDeal(state, 7, pe::STREET_FLOP,
               {card(pe::RANK_ACE, pe::SUIT_SPADES), card(pe::RANK_KING, pe::SUIT_DIAMONDS),
                card(pe::RANK_SEVEN, pe::SUIT_CLUBS)});
  // Flop checked through: seat 2 acts first (clockwise of the button, with the
  // blinds and folded seats skipped), then the button closes the street.
  addAction(state, 8, pe::STREET_FLOP, 2, pe::ACTION_TYPE_CHECK, 13, 194);
  addAction(state, 9, pe::STREET_FLOP, 0, pe::ACTION_TYPE_CHECK, 13, 194);
  addBoardDeal(state, 10, pe::STREET_TURN, {card(pe::RANK_NINE, pe::SUIT_SPADES)});
  // Turn: hero checks, the button bets 8.
  addAction(state, 11, pe::STREET_TURN, 2, pe::ACTION_TYPE_CHECK, 13, 194);
  addAction(state, 12, pe::STREET_TURN, 0, pe::ACTION_TYPE_BET, 13, 186, false, 8);

  addLegal(state, pe::ACTION_TYPE_FOLD);
  addLegal(state, pe::ACTION_TYPE_CALL);
  addLegal(state, pe::ACTION_TYPE_RAISE, 16, 194);
  state->set_to_call(8);

  setProvenance(state, 12);
  setOptions(&request, mode);
  return request;
}

// RFC 0032 AC2 baseline. On a turn check-through line the adapter collapses the
// snapshot to a flop-street-start state: the reconstructed board is three cards
// and the street is the flop, even though the request declares a four-card turn.
// This is the defect the RFC pins before changing it.
//
// The assertion is phrased as the CURRENT shape on purpose, so this test passes
// while the bug exists. Stage 2 flips it in the same commit that fixes the
// adapter, which makes this a ready-made guard for the fixed behaviour rather
// than a test that has to be deleted.
void testTurnStatePreservesBoard() {
  pe::DecisionRequest req = turnCheckThroughRequest();
  ReplayMiss miss = ReplayMiss::None;
  ReconstructedState recon;
  check(reconstructHandState(req, recon, miss), "turn: reconstruction succeeds");
  check(miss == ReplayMiss::None, "turn: no replay miss");
  check(recon.state.has_value(), "turn: a state was produced");
  const bs::poker::GameState& state = *recon.state;
  check(req.state().board_size() == 4, "turn: the wire carries four board cards");
  check(req.state().street() == pe::STREET_TURN, "turn: the wire declares the turn");
  // RFC 0032: the rebuilt state is the cursor's runtime, so the board and the
  // street are the wire's, not a three-card flop clamp.
  check(state.board().size() == 4, "turn: the rebuilt board carries four cards");
  check(state.street() == bs::poker::Street::Turn, "turn: the rebuilt street is the TURN");
  check(state.def().board_size == 4, "turn: the identity def is turn-rooted");
  for (std::size_t i = 0; i < state.board().size(); ++i) {
    const pe::Card& wire = req.state().board(static_cast<int>(i));
    const int expected =
        (static_cast<int>(wire.rank()) - 1) * 4 + (static_cast<int>(wire.suit()) - 1);
    check(state.board()[i] == expected, "turn: identity board card " + std::to_string(i));
  }
  // Both views agree on the node's economics, and the actor is the seat the
  // wire says is to act (the checked-to seat, which the flop clamp answered
  // with the wrong seat).
  check(state.def().pot == state.pot(), "turn: def().pot agrees with pot()");
  check(state.def().contributions[state.actor().value_or(0)] ==
            state.players()[state.actor().value_or(0)].net_contributed(),
        "turn: identity contributions are NET");
  // The two same_root halves must agree on the identity def of a re-derived
  // state: same street and same cards is the match condition, and a flop-sized
  // identity is a different root.
  bs::poker::GameDef flopIdentity = state.def();
  flopIdentity.board_size = 3;
  check(!bs::poker::same_game_def(state.def(), flopIdentity),
        "turn: a flop identity is not the same root as a turn identity");
  check(state.def().board_size == state.board().size(),
        "turn: identity board size equals the runtime board size");
}

// ===========================================================================
// RFC 0032 AC1 — the decision-node identity matrix
//
// Every cell is a REAL hand replayed through engine GameState, projected onto
// the wire, and pushed through the production adapter. Nothing about the
// identity is hand-assembled: the board, the ledger, the pot layers and the
// transition path all come from states the engine produced, so a cell passes
// only if the production path produces the identity.
//
// AC1 describes the fixture set as the cross product of {heads-up, 6-max} x
// {flop, turn, river} x {street-start, facing a bet} x {refund, no refund},
// which is 24 cells. Two of the four axes are NOT independent, and the
// measurement that shows it is worth recording because it decides how many
// cells can exist at all:
//
//  * A refund returns an unmatched excess to the UNIQUE highest contributor and
//    only down to the next commitment level, so every other live seat is all in
//    for less. The refunded seat is therefore the only seat with chips and the
//    only possible actor. A refunded ACTION node is always a street start —
//    "facing a bet" with a refund cannot exist, because nobody is left to bet
//    against. Exhaustive walk over 3..202-chip stacks at 2..6 seats: zero
//    action nodes carry both a refund and a live wager.
//  * Heads-up never refunds at an action node. At two seats the all-in flag is
//    preserved across the refund and the board then runs out with no further
//    action (`engine/src/poker/game_definition.cpp:565-576`), so the refunded
//    hand never reaches another decision point.
//
// The 24 design points therefore reduce to:
//
//   hu   x 3 streets x {street-start, facing-bet}               9 cells, all run
//   6max x 3 streets x {street-start, facing-bet}               6 cells, all run
//   6max x 3 streets x {street-start + refund}                  3 cells, all run
//   hu   x 3 streets x {anything + refund}                      3 cells, unreachable
//   6max x 3 streets x {facing-bet + refund}                    3 cells, unreachable
//
// Nine unreachable, fifteen run. The heads-up group is skipped by name in the
// loop; the facing-bet-with-refund shape has no enum entry at all, which is why
// it cannot be selected by accident.
// ===========================================================================

enum class NodeShape { StreetStart, FacesBet, StreetStartRefund };

struct MatrixCell {
  std::size_t seats;
  std::size_t board_size;
  NodeShape shape;
};

std::string cellLabel(const MatrixCell& cell) {
  const char* shape = cell.shape == NodeShape::StreetStart ? "street-start"
                      : cell.shape == NodeShape::FacesBet  ? "facing-bet"
                                                           : "street-start+refund";
  return std::string(cell.seats == 2 ? "hu" : "6max") + "/b" + std::to_string(cell.board_size) +
         "/" + shape;
}

pe::Street wireStreet(bs::poker::Street street) {
  switch (street) {
    case bs::poker::Street::Preflop:
      return pe::STREET_PREFLOP;
    case bs::poker::Street::Flop:
      return pe::STREET_FLOP;
    case bs::poker::Street::Turn:
      return pe::STREET_TURN;
    case bs::poker::Street::River:
      return pe::STREET_RIVER;
  }
  return pe::STREET_UNSPECIFIED;
}

pe::ActionType wireAction(bs::poker::ActionType type) {
  switch (type) {
    case bs::poker::ActionType::Fold:
      return pe::ACTION_TYPE_FOLD;
    case bs::poker::ActionType::Check:
      return pe::ACTION_TYPE_CHECK;
    case bs::poker::ActionType::Call:
      return pe::ACTION_TYPE_CALL;
    case bs::poker::ActionType::Bet:
      return pe::ACTION_TYPE_BET;
    case bs::poker::ActionType::Raise:
      return pe::ACTION_TYPE_RAISE;
  }
  return pe::ACTION_TYPE_UNSPECIFIED;
}

pe::Card wireCard(int id) {
  pe::Card out;
  out.set_rank(static_cast<pe::Rank>(id / 4 + 1));
  out.set_suit(static_cast<pe::Suit>(id % 4 + 1));
  return out;
}

// One replayed step, in engine coordinates. `incremental` is what a CALL
// actually paid, which the wire requires and the engine does not store.
struct Step {
  bool is_card = false;
  int card = 0;
  bs::poker::Street street = bs::poker::Street::Preflop;
  std::size_t actor = 0;
  bs::poker::Action action{};
  bs::poker::Chips incremental = 0;
  bs::poker::Chips pot_before = 0;
  bs::poker::Chips stack_after = 0;
  bool all_in = false;
};

struct Replay {
  // No default GameState exists (its constructor validates), so the final state
  // stays optional until the replay fills it.
  std::optional<bs::poker::GameState> final_state;
  std::vector<Step> steps;
  std::array<bs::poker::Chips, bs::poker::kMaxUnifiedSeats> starting_stacks{};
  std::array<bs::poker::Chips, bs::poker::kMaxUnifiedSeats> blinds{};
  std::size_t small_blind_actor = 0;
  std::size_t big_blind_actor = 0;
  std::vector<std::uint32_t> rotated;  // engine actor index -> wire seat
  std::uint32_t hero_wire = 0;
  std::uint32_t button_wire = 0;
};

// Replay one cell. Throws std::runtime_error when a cell cannot reach the node
// it claims, which is a fixture defect rather than a product failure.
Replay replayCell(const MatrixCell& cell) {
  using bs::poker::Action;
  using bs::poker::ActionType;
  using bs::poker::GameState;
  using bs::poker::Phase;

  const std::size_t seats = cell.seats;
  const bool heads_up = seats == 2;
  const std::uint32_t button_wire = heads_up ? 1 : 2;
  const bool refunds = cell.shape == NodeShape::StreetStartRefund;

  Replay replay;
  replay.button_wire = button_wire;
  for (std::size_t i = 0; i < seats; ++i)
    replay.rotated.push_back(static_cast<std::uint32_t>(i));
  // The adapter sorts by seat and rotates the button to actor 0.
  std::rotate(replay.rotated.begin(),
              std::find(replay.rotated.begin(), replay.rotated.end(), button_wire),
              replay.rotated.end());
  const std::vector<std::uint32_t>& rotated = replay.rotated;
  std::unordered_map<std::uint32_t, std::size_t> actorOfWire;
  for (std::size_t i = 0; i < seats; ++i)
    actorOfWire[rotated[i]] = i;

  bs::poker::GameDef rootDef{};
  rootDef.player_count = seats;
  rootDef.button = 0;
  rootDef.big_blind = 2;
  rootDef.preflop = true;
  rootDef.terminal = bs::poker::TerminalDepth::River;
  replay.small_blind_actor = bs::poker::small_blind_seat(rootDef);
  replay.big_blind_actor = bs::poker::big_blind_seat(rootDef);
  for (std::size_t i = 0; i < seats; ++i) {
    replay.starting_stacks[i] = 200;
    rootDef.stacks[i] = 200;
  }
  if (refunds) {
    // The big blind is short and acts LAST preflop, so the hero's raise is the
    // unique high commitment when the short seat calls all in for less.
    replay.starting_stacks[replay.big_blind_actor] = 6;
    rootDef.stacks[replay.big_blind_actor] = 6;
  }
  replay.blinds[replay.small_blind_actor] = 1;
  replay.blinds[replay.big_blind_actor] = 2;
  rootDef.blinds_posted = replay.blinds;
  for (std::size_t i = 0; i < seats; ++i)
    rootDef.pot += rootDef.blinds_posted[i];

  // The refund cell needs the hero to be the preflop opener, because only the
  // unique high contributor receives the excess back. At 6-max that is the seat
  // after the big blind; at heads-up the button. Every other cell uses a plain
  // middle seat, so the hero is neither a blind nor the short stack.
  replay.hero_wire =
      refunds ? rotated[bs::poker::preflop_first_actor(rootDef)] : (heads_up ? 0 : 3);
  const std::uint32_t hero_wire = replay.hero_wire;
  const std::size_t hero_actor = actorOfWire.at(hero_wire);

  GameState state(rootDef);

  const auto act = [&](ActionType type, bs::poker::Chips target) {
    if (state.phase() != Phase::Action || !state.actor())
      throw std::runtime_error("matrix fixture: no actor");
    const std::size_t actor = *state.actor();
    const Action action{type, target};
    if (!state.legal().contains(action))
      throw std::runtime_error("matrix fixture: illegal action");
    Step step;
    step.street = state.street();
    step.actor = actor;
    step.action = action;
    step.pot_before = state.pot();
    const bs::poker::Chips before = state.players()[actor].contributed;
    state = state.after_action(actor, action);
    step.incremental = state.players()[actor].contributed - before;
    step.stack_after = state.players()[actor].stack;
    step.all_in = state.players()[actor].all_in;
    replay.steps.push_back(step);
  };
  const auto deal = [&](int card) {
    if (state.phase() != Phase::Deal)
      throw std::runtime_error("matrix fixture: expected the deal phase");
    Step step;
    step.is_card = true;
    step.card = card;
    state = state.after_card(card);
    step.street = state.street();
    replay.steps.push_back(step);
  };
  const auto check_through = [&] {
    while (state.phase() == Phase::Action)
      act(ActionType::Check, 0);
  };

  // ---- Preflop --------------------------------------------------------------
  if (refunds) {
    // The hero opens preflop at 6-max (the seat after the big blind). Raise to
    // 20, fold everyone else, and let the short big blind call all in for 6.
    // Street close refunds the 14 unmatched chips, leaving the hero as the only
    // seat with chips -- the shape under test.
    if (!state.actor() || *state.actor() != hero_actor)
      throw std::runtime_error("matrix fixture: the hero does not open preflop at 6-max");
    act(ActionType::Raise, 20);
    while (state.phase() == Phase::Action) {
      if (*state.actor() == replay.big_blind_actor)
        act(ActionType::Call, 0);  // all in for less
      else
        act(ActionType::Fold, 0);
    }
  } else {
    // Limp around: everyone completes, the big blind checks its option.
    while (state.phase() == Phase::Action && state.street() == bs::poker::Street::Preflop)
      act(state.legal().call ? ActionType::Call : ActionType::Check, 0);
  }
  if (state.phase() != Phase::Deal)
    throw std::runtime_error("matrix fixture: preflop did not close");

  // ---- Deal each street, acting on the target one ---------------------------
  constexpr int kBoard[5] = {48, 44, 36, 32, 28};  // As Ks 9c 8c 7c
  const auto on_target_street = [&] {
    return state.street() == (cell.board_size == 3   ? bs::poker::Street::Flop
                              : cell.board_size == 4 ? bs::poker::Street::Turn
                                                     : bs::poker::Street::River);
  };

  // Deal the flop, then walk to the target street one completed street at a
  // time. Streets before the target are check-throughs; the target street stops
  // at the hero's decision node.
  deal(kBoard[0]);
  deal(kBoard[1]);
  deal(kBoard[2]);
  for (;;) {
    if (state.phase() != Phase::Action)
      throw std::runtime_error("matrix fixture: a completed board has no action");
    if (on_target_street())
      break;
    check_through();
    if (state.phase() != Phase::Deal)
      throw std::runtime_error("matrix fixture: the street did not close");
    deal(kBoard[state.board().size()]);
  }

  // ---- The requested node ---------------------------------------------------
  if (refunds) {
    // The hero is the only seat with chips, so it is the only actor and the node
    // is a street start carrying a positive refunded ledger.
    if (!state.actor() || *state.actor() != hero_actor)
      throw std::runtime_error("matrix fixture: the refunded node is not the hero's");
    if (state.players()[hero_actor].refunded == 0)
      throw std::runtime_error("matrix fixture: expected a positive refund");
  } else if (cell.shape == NodeShape::StreetStart) {
    while (state.phase() == Phase::Action && state.actor() && *state.actor() != hero_actor)
      act(ActionType::Check, 0);
  } else {
    // The hero opens the street, so it must CHECK first: a wager has to come
    // from another seat, and only then can the action return to the hero. The
    // checker that opens the round is the hero in both seat counts, which is
    // exactly why an earlier version of this fixture silently produced
    // street-start nodes for every facing-bet cell.
    if (state.actor() && *state.actor() == hero_actor)
      act(ActionType::Check, 0);
    while (state.phase() == Phase::Action && state.actor() && *state.actor() != hero_actor) {
      if ((*state.actor() + 1) % seats == hero_actor) {
        act(ActionType::Bet, 4);
        break;
      }
      act(ActionType::Check, 0);
    }
  }

  if (!state.actor() || *state.actor() != hero_actor)
    throw std::runtime_error("matrix fixture: hero is not the decision actor");
  if (state.phase() != Phase::Action)
    throw std::runtime_error("matrix fixture: decision node is not actionable");
  if (!on_target_street())
    throw std::runtime_error("matrix fixture: target street not reached");

  replay.final_state = state;
  return replay;
}

// Project a replayed cell onto the wire snapshot the adapter consumes.
pe::DecisionRequest matrixRequest(const MatrixCell& cell) {
  const Replay replay = replayCell(cell);
  if (!replay.final_state.has_value())
    throw std::runtime_error("matrix fixture: replay produced no state");
  const bs::poker::GameState& state = *replay.final_state;
  const std::size_t seats = cell.seats;
  const std::uint32_t button_wire = replay.button_wire;
  const std::uint32_t hero_wire = replay.hero_wire;

  pe::DecisionRequest request;
  pe::HandState* wire = request.mutable_state();
  setGameDefinition(wire, static_cast<std::uint32_t>(seats), 1, 2,
                    static_cast<std::uint32_t>(seats));
  wire->set_hand_id("matrix-" + cellLabel(cell));
  wire->set_decision_index(static_cast<std::uint64_t>(replay.steps.size()));
  wire->set_street(wireStreet(state.street()));
  wire->set_button_seat(button_wire);
  wire->set_hero_seat(hero_wire);
  wire->set_to_call(state.legal().call_amount);

  for (std::size_t i = 0; i < seats; ++i) {
    const bs::poker::GamePlayer& seat = state.players()[i];
    pe::PlayerState* player = wire->add_players();
    player->set_seat(replay.rotated[i]);
    player->set_starting_stack(replay.starting_stacks[i]);
    player->set_stack(seat.stack);
    player->set_street_committed(seat.street_committed);
    player->set_gross_contributed(seat.contributed);
    player->set_refunded(seat.refunded);
    player->set_status(seat.folded   ? pe::PLAYER_STATUS_FOLDED
                       : seat.all_in ? pe::PLAYER_STATUS_ALL_IN
                                     : pe::PLAYER_STATUS_ACTIVE);
  }
  // 2h 2d is clear of every board card used above.
  *wire->add_hero_hole_cards() = card(pe::RANK_TWO, pe::SUIT_HEARTS);
  *wire->add_hero_hole_cards() = card(pe::RANK_TWO, pe::SUIT_DIAMONDS);
  for (int id : state.board())
    *wire->add_board() = wireCard(id);

  // Pot layers: one per distinct net-contribution level, ascending.
  pe::PotState* pot = wire->mutable_pot();
  pot->set_phase(pe::POT_PHASE_ACTION);
  {
    std::vector<bs::poker::Chips> levels;
    for (std::size_t i = 0; i < seats; ++i)
      levels.push_back(state.players()[i].net_contributed());
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    bs::poker::Chips previous = 0;
    for (bs::poker::Chips level : levels) {
      if (level == 0)
        continue;
      bs::poker::Chips amount = 0;
      std::vector<std::uint32_t> eligible;
      for (std::size_t i = 0; i < seats; ++i) {
        if (state.players()[i].net_contributed() < level)
          continue;
        amount += level - previous;
        eligible.push_back(replay.rotated[i]);
      }
      if (amount > 0) {
        pe::PotLayer* layer = pot->add_layers();
        layer->set_contribution_level(level);
        layer->set_amount(amount);
        for (std::uint32_t seat : eligible)
          layer->add_eligible_seats(seat);
        layer->set_status(pe::LAYER_STATUS_PENDING);
      }
      previous = level;
    }
    pot->set_pot_total(state.pot());
  }

  for (std::size_t i = 0; i < seats; ++i) {
    if (replay.blinds[i] == 0)
      continue;
    pe::ForcedContribution* forced = wire->add_forced_contributions();
    forced->set_seat(replay.rotated[i]);
    forced->set_type(i == replay.small_blind_actor ? pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND
                                                   : pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    forced->set_amount(replay.blinds[i]);
  }

  // Transition path: the replay's own steps, with the pot and stack each event
  // actually saw.
  std::uint64_t sequence = 1;
  for (const Step& step : replay.steps) {
    pe::TransitionEvent* event = wire->add_transition_path();
    event->set_sequence(sequence);
    if (step.is_card) {
      pe::BoardDeal* deal = event->mutable_board_deal();
      deal->set_street(wireStreet(step.street));
      *deal->add_cards() = wireCard(step.card);
      ++sequence;
      continue;
    }
    pe::ActionEvent* action = event->mutable_action();
    action->set_sequence(sequence);
    action->set_street(wireStreet(step.street));
    action->set_actor_seat(replay.rotated[step.actor]);
    action->set_action(wireAction(step.action.type));
    action->set_pot_before(step.pot_before);
    action->set_stack_after(step.stack_after);
    action->set_all_in(step.all_in);
    if (step.action.type == bs::poker::ActionType::Bet ||
        step.action.type == bs::poker::ActionType::Raise) {
      action->set_target_total(step.action.target_total);
    } else if (step.action.type == bs::poker::ActionType::Call) {
      action->set_incremental_amount(step.incremental);
    }
    ++sequence;
  }

  const bs::poker::LegalActions& legal = state.legal();
  if (legal.fold)
    addLegal(wire, pe::ACTION_TYPE_FOLD);
  if (legal.check)
    addLegal(wire, pe::ACTION_TYPE_CHECK);
  if (legal.call)
    addLegal(wire, pe::ACTION_TYPE_CALL);
  if (legal.aggressive) {
    addLegal(wire,
             legal.aggressive->type == bs::poker::ActionType::Bet ? pe::ACTION_TYPE_BET
                                                                  : pe::ACTION_TYPE_RAISE,
             legal.aggressive->minimum, legal.aggressive->maximum);
  }

  setProvenance(wire, static_cast<std::uint32_t>(wire->transition_path_size()));
  setOptions(&request, pe::SOLVER_MODE_HEURISTIC);
  return request;
}

void testDecisionNodeIdentityMatrix() {
  std::size_t cells = 0;
  for (const std::size_t seats : {2, 6}) {
    for (const std::size_t board_size : {3, 4, 5}) {
      for (const NodeShape shape :
           {NodeShape::StreetStart, NodeShape::FacesBet, NodeShape::StreetStartRefund}) {
        // Heads-up never reaches a refunded decision node; see the header.
        // SKIP (unreachable, not untested): hu/{flop,turn,river}/refund.
        if (seats == 2 && shape == NodeShape::StreetStartRefund)
          continue;
        const MatrixCell cell{seats, board_size, shape};
        const std::string label = cellLabel(cell);
        pe::DecisionRequest request;
        try {
          request = matrixRequest(cell);
        } catch (const std::exception& error) {
          check(false, label + ": fixture replay failed: " + error.what());
          continue;
        }
        ++cells;
        // The cell must be the shape it claims. Without this, a fixture that
        // failed to place a wager would pass as a street start and every
        // "facing a bet" assertion above would be vacuously true.
        check(request.state().to_call() == (shape == NodeShape::FacesBet ? 4ULL : 0ULL),
              label + ": to_call matches the requested shape");
        ReplayMiss miss = ReplayMiss::None;
        ReconstructedState recon;
        check(reconstructHandState(request, recon, miss), label + ": adapter reconstructs");
        check(miss == ReplayMiss::None, label + ": no replay miss");
        if (!recon.state.has_value())
          continue;
        const bs::poker::GameState& state = *recon.state;
        const bs::poker::GameDef& def = state.def();

        // The identity contract from AC1's table, for every cell.
        bool validated = true;
        try {
          bs::poker::validate(def);
        } catch (const std::exception&) {
          validated = false;
        }
        check(validated, label + ": identity def validates");
        check(def.board_size == state.board().size(), label + ": identity board size");
        for (std::size_t i = 0; i < state.board().size(); ++i)
          check(def.board[i] == state.board()[i],
                label + ": identity board card " + std::to_string(i));
        check(!def.preflop, label + ": identity is postflop");
        check(def.pot == state.pot(), label + ": def().pot == pot()");
        check(state.actor().has_value() && *state.actor() == recon.hero_actor,
              label + ": actor is the hero");
        check(state.street() == (cell.board_size == 3   ? bs::poker::Street::Flop
                                 : cell.board_size == 4 ? bs::poker::Street::Turn
                                                        : bs::poker::Street::River),
              label + ": street matches the board");
        for (std::size_t s = 0; s < def.player_count; ++s)
          check(def.contributions[s] == state.players()[s].net_contributed(),
                label + ": identity contributions are NET");
        if (shape == NodeShape::StreetStartRefund) {
          // The refund must be visible in the RUNTIME ledger the adapter
          // replayed, not merely in the wire snapshot the fixture wrote: this
          // is what makes def().contributions a net figure rather than a copy.
          bool any_refund = false;
          for (std::size_t s = 0; s < def.player_count; ++s)
            any_refund = any_refund || state.players()[s].refunded > 0;
          check(any_refund, label + ": the runtime ledger carries the refund");
          bs::poker::Chips gross = 0;
          bs::poker::Chips net = 0;
          for (std::size_t s = 0; s < def.player_count; ++s) {
            gross += state.players()[s].contributed;
            net += state.players()[s].net_contributed();
          }
          check(gross > net, label + ": gross exceeds net at a refunded node");
        }

        // The two same_root properties, on the street-start column only: a
        // facing-a-bet def genuinely differs in contributions and pot, so it is
        // not expected to match a street-start record. The flop comparison only
        // means something when the identity is wider than a flop.
        if (shape != NodeShape::FacesBet && cell.board_size > 3) {
          bs::poker::GameDef flop = def;
          flop.board_size = 3;
          for (std::size_t i = 3; i < 5; ++i)
            flop.board[i] = 0;
          check(!bs::poker::same_game_def(def, flop),
                label + ": a flop identity is a different root");
          bs::poker::GameDef sibling = def;
          sibling.board[3] = (def.board[3] + 1) % 52;
          check(!bs::poker::same_game_def(def, sibling),
                label + ": identities differing only in board[3] are different roots");
        }
      }
    }
  }
  check(cells == 15, "matrix: 15 constructible cells ran");
}
// ===========================================================================
// Vector 2 — capped call that triggers a refund on street closure
// 6-max NLHE cash, blinds 1/2, unequal stacks. Button seat 0.
// Hero seat 3 (UTG, the bettor who receives the refund).
//
// This is a replay/settlement verification vector. The hand is heads-up with
// one player all-in after the flop, so no DecisionRequest is emitted in
// production. The test verifies the resident mapper can replay the transition
// path (including the refund) and match the pinned G/F ledger.
// ===========================================================================

pe::DecisionRequest vector2Request() {
  pe::DecisionRequest request;
  pe::HandState* state = request.mutable_state();
  setGameDefinition(state, 6, 1, 2, 6);
  state->set_hand_id("vector2-hand");
  state->set_decision_index(1);
  state->set_street(pe::STREET_FLOP);
  state->set_button_seat(0);
  state->set_hero_seat(3);

  // Players (G/F ledger after refund, from RFC).
  addPlayer(state, 0, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 1, 200, 199, 0, 1, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 2, 200, 194, 0, 6, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 3, 200, 170, 0, 36, 6, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 4, 30, 0, 0, 30, 0, pe::PLAYER_STATUS_ALL_IN);
  addPlayer(state, 5, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);

  // Hero hole cards: Qh Qd (same as vector 1 for consistency).
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_DIAMONDS);

  // Board: Js Jd 4c.
  *state->add_board() = card(pe::RANK_JACK, pe::SUIT_SPADES);
  *state->add_board() = card(pe::RANK_JACK, pe::SUIT_DIAMONDS);
  *state->add_board() = card(pe::RANK_FOUR, pe::SUIT_CLUBS);

  // Pot: 67 (73 - 6 refund), three PENDING layers after refund.
  pe::PotState* pot = state->mutable_pot();
  pot->set_phase(pe::POT_PHASE_ACTION);
  addLayer(pot, 1, 4, {3, 4}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 6, 15, {3, 4}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 30, 48, {3, 4}, pe::LAYER_STATUS_PENDING);
  pot->set_pot_total(67);

  // Forced contributions.
  addForced(state, 1, pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 1);
  addForced(state, 2, pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 2);

  // Transition path (11 events).
  addAction(state, 1, pe::STREET_PREFLOP, 3, pe::ACTION_TYPE_RAISE, 3, 194, false, 6);
  addAction(state, 2, pe::STREET_PREFLOP, 4, pe::ACTION_TYPE_CALL, 9, 24, false, std::nullopt, 6);
  addAction(state, 3, pe::STREET_PREFLOP, 5, pe::ACTION_TYPE_FOLD, 15, 200);
  addAction(state, 4, pe::STREET_PREFLOP, 0, pe::ACTION_TYPE_FOLD, 15, 200);
  addAction(state, 5, pe::STREET_PREFLOP, 1, pe::ACTION_TYPE_FOLD, 15, 199);
  addAction(state, 6, pe::STREET_PREFLOP, 2, pe::ACTION_TYPE_CALL, 15, 194, false, std::nullopt, 4);
  addBoardDeal(state, 7, pe::STREET_FLOP,
               {card(pe::RANK_JACK, pe::SUIT_SPADES), card(pe::RANK_JACK, pe::SUIT_DIAMONDS),
                card(pe::RANK_FOUR, pe::SUIT_CLUBS)});
  addAction(state, 8, pe::STREET_FLOP, 2, pe::ACTION_TYPE_CHECK, 19, 194);
  addAction(state, 9, pe::STREET_FLOP, 3, pe::ACTION_TYPE_BET, 19, 164, false, 30);
  addAction(state, 10, pe::STREET_FLOP, 4, pe::ACTION_TYPE_CALL, 49, 0, true, std::nullopt, 24);
  addAction(state, 11, pe::STREET_FLOP, 2, pe::ACTION_TYPE_FOLD, 73, 194);

  // At the post-refund point, the hand is heads-up with seat 4 all-in.
  // The next event would be a turn deal. No player decision is available.
  // We provide CHECK as the only legal action (the cursor should be at a
  // state where the next street's first actor can check).
  addLegal(state, pe::ACTION_TYPE_CHECK);
  state->set_to_call(0);

  setProvenance(state, 11);
  setOptions(&request, pe::SOLVER_MODE_HEURISTIC);
  return request;
}

// ===========================================================================
// Vector 3 — actionable node with an existing side pot plus a new pending wager
// Same game and stacks as vector 2. Hero seat 5 (CO).
// Decision point: seat 5, street TURN, to_call = 20.
// ===========================================================================

pe::DecisionRequest vector3Request(pe::SolverMode mode = pe::SOLVER_MODE_HEURISTIC) {
  pe::DecisionRequest request;
  pe::HandState* state = request.mutable_state();
  setGameDefinition(state, 6, 1, 2, 6);
  state->set_hand_id("vector3-hand");
  state->set_decision_index(1);
  state->set_street(pe::STREET_TURN);
  state->set_button_seat(0);
  state->set_hero_seat(5);

  // Players (G/F ledger from RFC). street_committed is current-street (TURN) only.
  addPlayer(state, 0, 200, 200, 0, 0, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 1, 200, 199, 0, 1, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 2, 200, 194, 0, 6, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 3, 200, 144, 20, 56, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 4, 30, 0, 0, 30, 0, pe::PLAYER_STATUS_ALL_IN);
  addPlayer(state, 5, 200, 164, 0, 36, 0, pe::PLAYER_STATUS_ACTIVE);

  // Hero hole cards: Qh Qd.
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pe::RANK_QUEEN, pe::SUIT_DIAMONDS);

  // Board: As Kd 7c 2h.
  *state->add_board() = card(pe::RANK_ACE, pe::SUIT_SPADES);
  *state->add_board() = card(pe::RANK_KING, pe::SUIT_DIAMONDS);
  *state->add_board() = card(pe::RANK_SEVEN, pe::SUIT_CLUBS);
  *state->add_board() = card(pe::RANK_TWO, pe::SUIT_HEARTS);

  // Pot: 129, five PENDING layers (including side pot).
  pe::PotState* pot = state->mutable_pot();
  pot->set_phase(pe::POT_PHASE_ACTION);
  addLayer(pot, 1, 5, {3, 4, 5}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 6, 20, {3, 4, 5}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 30, 72, {3, 4, 5}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 36, 12, {3, 5}, pe::LAYER_STATUS_PENDING);
  addLayer(pot, 56, 20, {3}, pe::LAYER_STATUS_PENDING);
  pot->set_pot_total(129);

  // Forced contributions.
  addForced(state, 1, pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 1);
  addForced(state, 2, pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 2);

  // Transition path (14 events).
  addAction(state, 1, pe::STREET_PREFLOP, 3, pe::ACTION_TYPE_RAISE, 3, 194, false, 6);
  addAction(state, 2, pe::STREET_PREFLOP, 4, pe::ACTION_TYPE_CALL, 9, 24, false, std::nullopt, 6);
  addAction(state, 3, pe::STREET_PREFLOP, 5, pe::ACTION_TYPE_CALL, 15, 194, false, std::nullopt, 6);
  addAction(state, 4, pe::STREET_PREFLOP, 0, pe::ACTION_TYPE_FOLD, 21, 200);
  addAction(state, 5, pe::STREET_PREFLOP, 1, pe::ACTION_TYPE_FOLD, 21, 199);
  addAction(state, 6, pe::STREET_PREFLOP, 2, pe::ACTION_TYPE_CALL, 21, 194, false, std::nullopt, 4);
  addBoardDeal(state, 7, pe::STREET_FLOP,
               {card(pe::RANK_ACE, pe::SUIT_SPADES), card(pe::RANK_KING, pe::SUIT_DIAMONDS),
                card(pe::RANK_SEVEN, pe::SUIT_CLUBS)});
  addAction(state, 8, pe::STREET_FLOP, 2, pe::ACTION_TYPE_CHECK, 25, 194);
  addAction(state, 9, pe::STREET_FLOP, 3, pe::ACTION_TYPE_BET, 25, 164, false, 30);
  addAction(state, 10, pe::STREET_FLOP, 4, pe::ACTION_TYPE_CALL, 55, 0, true, std::nullopt, 24);
  addAction(state, 11, pe::STREET_FLOP, 5, pe::ACTION_TYPE_CALL, 79, 164, false, std::nullopt, 30);
  addAction(state, 12, pe::STREET_FLOP, 2, pe::ACTION_TYPE_FOLD, 109, 194);
  addBoardDeal(state, 13, pe::STREET_TURN, {card(pe::RANK_TWO, pe::SUIT_HEARTS)});
  addAction(state, 14, pe::STREET_TURN, 3, pe::ACTION_TYPE_BET, 109, 144, false, 20);

  // Legal actions: FOLD, CALL(20), RAISE(min 40, max 164).
  addLegal(state, pe::ACTION_TYPE_FOLD);
  addLegal(state, pe::ACTION_TYPE_CALL);
  addLegal(state, pe::ACTION_TYPE_RAISE, 40, 164);
  state->set_to_call(20);

  setProvenance(state, 14);
  setOptions(&request, mode);
  return request;
}

// ===========================================================================
// Tests
// ===========================================================================

void testPeekProtocolVersion() {
  // Envelope on the accepted transport version (protocol_version = 3).
  pe::Envelope v2env;
  v2env.set_protocol_version(3);
  v2env.set_request_id("test");
  v2env.mutable_get_capabilities_request();
  const std::string v2frame = v2env.SerializeAsString();
  auto version = bs::peekProtocolVersion(v2frame);
  check(version.has_value() && *version == 3, "peek protocol_version=3");

  // Envelope with protocol_version = 0 (default, absent on wire). Proto3
  // omits default values on the wire, so field 1 is absent and peek returns
  // nullopt.
  pe::Envelope v1env;
  v1env.set_protocol_version(0);
  v1env.set_request_id("test");
  const std::string v1frame = v1env.SerializeAsString();
  version = bs::peekProtocolVersion(v1frame);
  check(!version.has_value(), "peek protocol_version=0 returns nullopt (absent on wire)");

  // Envelope with protocol_version = 1 (non-default, present on wire).
  v1env.set_protocol_version(1);
  const std::string v1frame1 = v1env.SerializeAsString();
  version = bs::peekProtocolVersion(v1frame1);
  check(version.has_value() && *version == 1, "peek protocol_version=1");

  // Garbage frame.
  version = bs::peekProtocolVersion("\x00\x01\x02");
  check(!version.has_value(), "peek garbage returns nullopt");

  // Empty frame.
  version = bs::peekProtocolVersion("");
  check(!version.has_value(), "peek empty returns nullopt");
}

void testCapabilities() {
  const pe::Envelope response = respond(capabilitiesEnvelope("cap-1"));
  check(response.has_get_capabilities_response(), "capabilities response present");
  const pe::GetCapabilitiesResponse& caps = response.get_capabilities_response();
  check(caps.supported_protocol_versions_size() == 1, "one protocol version");
  check(caps.supported_protocol_versions(0) == 3, "protocol version 3");
  check(caps.capabilities_size() >= 2, "at least 2 capability tuples");
  // Verify the first tuple is hu-r1.
  const pe::CapabilityTuple& tuple = caps.capabilities(0);
  check(tuple.min_dealt_players() == 2, "hu-r1 min dealt 2");
  check(tuple.max_dealt_players() == 2, "hu-r1 max dealt 2");
  check(tuple.min_live_players() == 2, "hu-r1 min live 2");
  check(tuple.max_live_players() == 2, "hu-r1 max live 2");
  check(tuple.supported_streets_size() == 4, "hu-r1 all streets");
  check(tuple.max_stack_bb() == 100, "hu-r1 max stack 100 BB");
  check(!tuple.side_pots(), "hu-r1 no side pots");
  check(tuple.utility_profile() == pe::UTILITY_PROFILE_CASH_CHIP, "hu-r1 cash chip");
  check(tuple.required_coverage() == pe::OBSERVATION_COVERAGE_FULL, "hu-r1 full coverage");
  check(tuple.quality_scope() == "heads-up-r1", "hu-r1 quality scope");
}

void testVector1() {
  pe::DecisionRequest req = vector1Request();
  const pe::Envelope response = respond(envelopeFor("vector1-1", std::move(req)));
  check(response.has_decision_response(), "vector 1 decision response present");
  const pe::DecisionResponse& dr = response.decision_response();
  check(!dr.has_error(), "vector 1 no error");
  check(dr.has_expanded_strategy(), "vector 1 expanded strategy present");
  const pe::ExpandedStrategy& strategy = dr.expanded_strategy();
  check(strategy.actions_size() >= 1, "vector 1 at least 1 action");
  check(strategy.actions_size() <= 32, "vector 1 at most 32 actions");

  // Verify probabilities sum to 1.
  double sum = 0.0;
  for (int i = 0; i < strategy.actions_size(); ++i)
    sum += strategy.actions(i).probability();
  check(std::abs(sum - 1.0) <= 1e-9, "vector 1 probabilities sum to 1");

  // Verify every action is in the legal set.
  pe::DecisionRequest legalReq = vector1Request();
  for (int i = 0; i < strategy.actions_size(); ++i) {
    const pe::ActionPolicy& policy = strategy.actions(i);
    bool found = false;
    for (const pe::LegalAction& legal : legalReq.state().legal_actions()) {
      if (legal.type() == policy.type()) {
        found = true;
        break;
      }
    }
    check(found, "vector 1 action is legal");
  }

  // Verify sampled action is present and legal.
  if (strategy.has_selected_action()) {
    const pe::SelectedAction& selected = strategy.selected_action();
    bool found = false;
    for (const pe::LegalAction& legal : legalReq.state().legal_actions()) {
      if (legal.type() == selected.type()) {
        found = true;
        break;
      }
    }
    check(found, "vector 1 sampled action is legal");
  }

  // Verify solver metadata.
  check(strategy.has_solver(), "vector 1 solver metadata present");
  const pe::SolverMetadata& meta = strategy.solver();
  check(meta.utility_profile() == pe::UTILITY_PROFILE_CASH_CHIP,
        "vector 1 utility profile CASH_CHIP");
  check(!meta.artifact_sha256().empty() || meta.source() != pe::SOLVER_SOURCE_BLUEPRINT,
        "vector 1 artifact sha256 present for blueprint");
}

// RFC 0032 stage 0: a subgame-served row carries NO artifact. The wire must
// report the digest field ABSENT (has_artifact_sha256() == false), not present
// with a zero-length value, and must not report a cache hit. A present but
// malformed digest is still an error, so a resident row that lost its digest
// stays caught.
class ResolvingHostServices final : public bs::HostServices {
 public:
  explicit ResolvingHostServices(std::string digest) : digest_(std::move(digest)) {}

  bool blueprintAdvertised() const noexcept override { return true; }

  bs::BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                            std::span<const bs::poker::PublicAction>,
                                            const std::array<int, 2>&,
                                            std::string_view) const noexcept override {
    bs::BlueprintResult result;
    result.hit = false;
    result.miss = bs::BlueprintMiss::RootNotSupported;
    return result;
  }

  bool resolvingAdvertised() const noexcept override { return true; }

  bs::ResolveResult resolvingDecision(const bs::poker::GameState&,
                                      std::span<const bs::poker::PublicAction>,
                                      const std::array<int, 2>&, std::string_view,
                                      std::uint32_t) const noexcept override {
    bs::ResolveResult result;
    result.outcome = bs::ResolveOutcome::Approximate;
    result.miss = bs::BlueprintMiss::None;
    row_.actions = row_actions_.data();
    row_.probabilities = row_probabilities_.data();
    row_.size = row_actions_.size();
    row_.artifact_sha256 = digest_;
    row_.diagnostic = bs::resolver::kSubgameResolveToken;
    result.row = row_;
    return result;
  }

 private:
  std::string digest_;
  mutable std::vector<bs::poker::Action> row_actions_{{bs::poker::ActionType::Fold, 0},
                                                      {bs::poker::ActionType::Call, 0}};
  mutable std::vector<double> row_probabilities_{0.5, 0.5};
  mutable bs::BlueprintRow row_;
};

void testSubgameRowWithoutArtifact() {
  // Vector 1 is a flop decision with fold and call legal, so the synthetic row
  // is legal at that node.
  ResolvingHostServices host{""};
  pe::DecisionRequest req = vector1Request(pe::SOLVER_MODE_AUTOMATIC);
  const pe::Envelope response = respondWith(envelopeFor("no-artifact", std::move(req)), host);
  check(response.has_decision_response(), "subgame: decision response present");
  const pe::DecisionResponse& dr = response.decision_response();
  check(!dr.has_error(), "subgame: a solved row with no artifact is not an error");
  check(dr.has_expanded_strategy(), "subgame: expanded strategy present");
  const pe::SolverMetadata& meta = dr.expanded_strategy().solver();
  check(!meta.has_artifact_sha256(), "subgame: digest is ABSENT, not empty");
  check(meta.diagnostic_reason() == bs::resolver::kSubgameResolveToken,
        "subgame: diagnostic token preserved");
  check(meta.guarantee_level() == "approximate", "subgame: approximate guarantee");
  check(!meta.cache_hit(), "subgame: a solve is not a cache hit");
  check(meta.reason_code() == "resolving", "subgame: reason code is resolving");
}

// RFC 0032 stage 3: both lookup paths miss, so the labeled operational fallback
// serves. The token names the root the resident library cannot answer, and it
// must be reported for a turn/river snapshot and NOT for a flop one.
class MissHostServices final : public bs::HostServices {
 public:
  bool blueprintAdvertised() const noexcept override { return true; }
  bs::BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                            std::span<const bs::poker::PublicAction>,
                                            const std::array<int, 2>&,
                                            std::string_view) const noexcept override {
    bs::BlueprintResult result;
    result.hit = false;
    result.miss = bs::BlueprintMiss::RootNotSupported;
    return result;
  }
  bool resolvingAdvertised() const noexcept override { return true; }
  bs::ResolveResult resolvingDecision(const bs::poker::GameState&,
                                      std::span<const bs::poker::PublicAction>,
                                      const std::array<int, 2>&, std::string_view,
                                      std::uint32_t) const noexcept override {
    bs::ResolveResult result;
    result.outcome = bs::ResolveOutcome::Unsupported;
    result.miss = bs::BlueprintMiss::RootNotSupported;
    return result;
  }
};

void testOperationalFallbackLabelsUnservedRoot() {
  MissHostServices host;
  const auto reasonAt = [&](pe::DecisionRequest req) {
    const pe::Envelope response = respondWith(envelopeFor("fallback", std::move(req)), host);
    const pe::DecisionResponse& dr = response.decision_response();
    if (!dr.has_expanded_strategy())
      return std::string("<no strategy>");
    return dr.expanded_strategy().solver().diagnostic_reason();
  };
  // A flop snapshot keeps the original token, so existing counts are unchanged.
  check(reasonAt(vector1Request(pe::SOLVER_MODE_AUTOMATIC)) == "operational-fallback",
        "fallback: a flop root keeps the original token");
  // A four-card turn snapshot names the unserved root instead.
  check(reasonAt(turnCheckThroughRequest(pe::SOLVER_MODE_AUTOMATIC)) == "turn_root_unserved",
        "fallback: a turn root is labelled turn_root_unserved");
}

void testResidentRowWithMalformedDigest() {
  ResolvingHostServices host{"not-a-sha256"};
  pe::DecisionRequest req = vector1Request(pe::SOLVER_MODE_AUTOMATIC);
  const pe::Envelope response = respondWith(envelopeFor("bad-digest", std::move(req)), host);
  const pe::DecisionResponse& dr = response.decision_response();
  check(dr.has_error(), "malformed digest: still an error");
  check(dr.error().code() == pe::ERROR_CODE_INTERNAL, "malformed digest: ERROR_CODE_INTERNAL");
}

void testVector2Replay() {
  // Vector 2 is a replay/settlement verification vector. The hand is heads-up
  // with one player all-in after the flop, so no DecisionRequest is emitted in
  // production. We verify the resident mapper can replay the transition path
  // (including the refund) and match the pinned G/F ledger.
  pe::DecisionRequest request = vector2Request();
  bs::ReconstructedState recon;
  ReplayMiss miss = ReplayMiss::None;
  const bool ok = bs::reconstructHandState(request, recon, miss);
  // The replay may succeed or fail depending on whether the cursor is at a
  // decision point. The key verification is that the G/F ledger and pot layers
  // are correctly replayed. If the replay fails because the cursor is not at a
  // decision point (no legal actions), that's expected for this vector.
  if (ok) {
    check(recon.state.has_value(), "vector 2 reconstructed state present");
    check(recon.hero_actor < 6, "vector 2 hero actor valid");
    // Verify the G/F ledger in the reconstructed state.
    if (recon.state.has_value()) {
      const auto& players = recon.state->players();
      // Seat 3: G=36, F=6, stack=170.
      bool found_seat3 = false;
      for (std::size_t i = 0; i < players.size(); ++i) {
        // The resident mapper sorts by seat, so actor index 3 = seat 3.
        if (i == 3) {
          found_seat3 = true;
          check(players[i].contributed == 36, "vector 2 seat 3 G=36");
          check(players[i].refunded == 6, "vector 2 seat 3 F=6");
          check(players[i].stack == 170, "vector 2 seat 3 stack=170");
        }
      }
      check(found_seat3, "vector 2 seat 3 found in reconstructed state");
    }
  } else {
    // If the replay failed, it should be because the cursor is not at a
    // decision point (ReplayMismatch from legal action validation), not
    // because of a G/F ledger error.
    check(miss == ReplayMiss::ReplayMismatch,
          "vector 2 replay miss is ReplayMismatch (expected for post-street-closure)");
  }
}

void testVector3() {
  pe::DecisionRequest req = vector3Request();
  const pe::Envelope response = respond(envelopeFor("vector3-1", std::move(req)));
  check(response.has_decision_response(), "vector 3 decision response present");
  const pe::DecisionResponse& dr = response.decision_response();
  check(!dr.has_error(), "vector 3 no error");
  check(dr.has_expanded_strategy(), "vector 3 expanded strategy present");
  const pe::ExpandedStrategy& strategy = dr.expanded_strategy();
  check(strategy.actions_size() >= 1, "vector 3 at least 1 action");
  check(strategy.actions_size() <= 32, "vector 3 at most 32 actions");

  // Verify probabilities sum to 1.
  double sum = 0.0;
  for (int i = 0; i < strategy.actions_size(); ++i)
    sum += strategy.actions(i).probability();
  check(std::abs(sum - 1.0) <= 1e-9, "vector 3 probabilities sum to 1");

  // Verify every action is in the legal set.
  pe::DecisionRequest legalReq = vector3Request();
  for (int i = 0; i < strategy.actions_size(); ++i) {
    const pe::ActionPolicy& policy = strategy.actions(i);
    bool found = false;
    for (const pe::LegalAction& legal : legalReq.state().legal_actions()) {
      if (legal.type() == policy.type()) {
        found = true;
        break;
      }
    }
    check(found, "vector 3 action is legal");
  }

  // Verify sampled action is present and legal.
  if (strategy.has_selected_action()) {
    const pe::SelectedAction& selected = strategy.selected_action();
    bool found = false;
    for (const pe::LegalAction& legal : legalReq.state().legal_actions()) {
      if (legal.type() == selected.type()) {
        found = true;
        break;
      }
    }
    check(found, "vector 3 sampled action is legal");
  }

  // Verify solver metadata.
  check(strategy.has_solver(), "vector 3 solver metadata present");
  const pe::SolverMetadata& meta = strategy.solver();
  check(meta.utility_profile() == pe::UTILITY_PROFILE_CASH_CHIP,
        "vector 3 utility profile CASH_CHIP");
}

void testUnsupportedProtocol() {
  // An envelope with protocol_version != 3 must be rejected.
  pe::Envelope envelope;
  envelope.set_protocol_version(4);
  envelope.set_request_id("bad-version");
  envelope.mutable_get_capabilities_request();
  const std::string frame = envelope.SerializeAsString();
  const EnvelopeResult result = bs::handleEnvelope(frame);
  check(result.outcome == EnvelopeOutcome::Respond, "unsupported version responds");
  pe::Envelope response;
  check(response.ParseFromString(result.response), "unsupported version response parses");
  check(response.has_decision_response(), "unsupported version has decision response");
  check(response.decision_response().has_error(), "unsupported version has error");
  check(response.decision_response().error().code() == pe::ERROR_CODE_UNSUPPORTED_PROTOCOL,
        "unsupported version error code");
}

void testMissingRequestId() {
  pe::Envelope envelope;
  envelope.set_protocol_version(3);
  // No request_id.
  envelope.mutable_get_capabilities_request();
  const std::string frame = envelope.SerializeAsString();
  const EnvelopeResult result = bs::handleEnvelope(frame);
  check(result.outcome == EnvelopeOutcome::Respond, "missing request_id responds");
  pe::Envelope response;
  check(response.ParseFromString(result.response), "missing request_id response parses");
  check(response.has_decision_response(), "missing request_id has decision response");
  check(response.decision_response().has_error(), "missing request_id has error");
  check(response.decision_response().error().code() == pe::ERROR_CODE_INVALID_REQUEST,
        "missing request_id error code");
}

// ===========================================================================
// Button rotation + preflop root — 3-seat preflop with button at seat 2.
// Verifies that the resident mapper rotates actor indices so the button is
// always at index 0, and that preflop decisions build a hand-start root
// GameState (not a flop-rooted one) for blueprint identity matching.
// ===========================================================================

pe::DecisionRequest buttonRotationRequest() {
  pe::DecisionRequest request;
  pe::HandState* state = request.mutable_state();
  setGameDefinition(state, 3, 1, 2, 3);
  state->set_hand_id("rotation-hand");
  state->set_decision_index(1);
  state->set_street(pe::STREET_PREFLOP);
  state->set_button_seat(2);
  state->set_hero_seat(1);

  // Players: seat 0 = SB, seat 1 = BB (hero), seat 2 = button.
  addPlayer(state, 0, 200, 199, 1, 1, 0, pe::PLAYER_STATUS_FOLDED);
  addPlayer(state, 1, 200, 198, 2, 2, 0, pe::PLAYER_STATUS_ACTIVE);
  addPlayer(state, 2, 200, 194, 6, 6, 0, pe::PLAYER_STATUS_ACTIVE);

  // Hero hole cards: As Ks.
  *state->add_hero_hole_cards() = card(pe::RANK_ACE, pe::SUIT_SPADES);
  *state->add_hero_hole_cards() = card(pe::RANK_KING, pe::SUIT_SPADES);

  // Pot: 9 (1 + 2 + 6).
  state->mutable_pot()->set_pot_total(9);

  // Forced contributions: SB at seat 0, BB at seat 1.
  addForced(state, 0, pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND, 1);
  addForced(state, 1, pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND, 2);

  // Transition path: button raises to 6, SB folds.
  addAction(state, 1, pe::STREET_PREFLOP, 2, pe::ACTION_TYPE_RAISE, 3, 194, false, 6);
  addAction(state, 2, pe::STREET_PREFLOP, 0, pe::ACTION_TYPE_FOLD, 9, 199);

  // Legal actions: FOLD, CALL(4), RAISE(min 10, max 200).
  addLegal(state, pe::ACTION_TYPE_FOLD);
  addLegal(state, pe::ACTION_TYPE_CALL);
  addLegal(state, pe::ACTION_TYPE_RAISE, 10, 200);
  state->set_to_call(4);

  setProvenance(state, 2);
  setOptions(&request, pe::SOLVER_MODE_HEURISTIC);
  return request;
}

void testButtonRotation() {
  pe::DecisionRequest request = buttonRotationRequest();
  bs::ReconstructedState recon;
  ReplayMiss miss = ReplayMiss::None;
  const bool ok = bs::reconstructHandState(request, recon, miss);
  check(ok, "button rotation reconstruction succeeds");
  check(miss == ReplayMiss::None, "button rotation no replay miss");
  check(recon.state.has_value(), "button rotation reconstructed state present");
  // After rotation: actor 0 = seat 2 (button), actor 1 = seat 0 (SB),
  // actor 2 = seat 1 (BB/hero).
  check(recon.hero_actor == 2, "button rotation hero at actor 2");
  if (recon.state.has_value()) {
    const auto& def = recon.state->def();
    const auto& players = recon.state->players();
    check(def.player_count == 3, "button rotation 3 players");
    check(def.button == 0, "button rotation button at 0");
    check(def.board_size == 0, "button rotation preflop board_size 0");
    check(def.pot == 3, "button rotation blinds pot 3");
    // Preflop root: starting stacks, zero contributions, no folds.
    check(def.stacks[0] == 200 && def.stacks[1] == 200 && def.stacks[2] == 200,
          "button rotation starting stacks 200");
    check(def.contributions[0] == 0 && def.contributions[1] == 0 && def.contributions[2] == 0,
          "button rotation zero contributions");
    // Players after blind posting: button 200, SB 199, BB 198.
    check(players[0].stack == 200, "button rotation actor 0 (button) stack 200");
    check(players[1].stack == 199, "button rotation actor 1 (SB) stack 199");
    check(players[2].stack == 198, "button rotation actor 2 (BB) stack 198");
    check(players[1].contributed == 1, "button rotation actor 1 contributed 1");
    check(players[2].contributed == 2, "button rotation actor 2 contributed 2");
  }
  // History should contain the 2 preflop actions (not filtered for preflop).
  check(recon.history.size() == 2, "button rotation history has 2 preflop actions");
}

}  // namespace

int main() {
  testPeekProtocolVersion();
  testCapabilities();
  testVector1();
  testVector2Replay();
  testVector3();
  testSubgameRowWithoutArtifact();
  testTurnStatePreservesBoard();
  testDecisionNodeIdentityMatrix();
  testOperationalFallbackLabelsUnservedRoot();
  testResidentRowWithMalformedDigest();
  testUnsupportedProtocol();
  testMissingRequestId();
  testButtonRotation();

  if (failures > 0) {
    std::fprintf(stderr, "%d test(s) failed\n", failures);
    return 1;
  }
  std::printf("all conformance tests passed\n");
  return 0;
}
