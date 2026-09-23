// fuzz_seeds.hpp — structured decision-envelope seeds for fuzzing.
//
// Frame-level mutations alone never reach semantic validation, so the driver
// also mutates a set of serialized Envelopes spanning the validator's branch
// space: a valid decision, open-enum attacks (type/street/rank/suit 99 and
// -1), the big-blind option, a side-pot hand, and oversized chip values.
#pragma once

#include <bigshark/engine/v1/engine.pb.h>

#include <string>
#include <vector>

namespace bs::v1::fuzz {

namespace pv = ::bigshark::engine::v1;

inline pv::Card makeCard(pv::Rank rank, pv::Suit suit) {
  pv::Card card;
  card.set_rank(rank);
  card.set_suit(suit);
  return card;
}

inline std::string frame(const pv::Envelope& envelope) {
  return envelope.SerializeAsString();
}

inline pv::Envelope envelopeFor(const pv::DecisionRequest& request, const char* id) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(0);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = request;
  return envelope;
}

inline pv::Envelope envelopeForMinor(const pv::DecisionRequest& request, const char* id,
                                     std::uint32_t minor) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = request;
  return envelope;
}

inline pv::Envelope capabilitiesFor(std::uint32_t minor, const char* id) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id(id);
  envelope.mutable_get_capabilities_request();
  return envelope;
}

inline pv::DecisionRequest baseRequest() {
  pv::DecisionRequest request;
  pv::HandState* state = request.mutable_state();
  pv::GameDefinition* game = state->mutable_game();
  game->set_variant(pv::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv::GAME_TYPE_CASH);
  game->set_table_capacity(2);
  game->mutable_amount_unit()->set_name("chip");
  game->mutable_amount_unit()->set_decimal_places(0);
  game->set_small_blind(10);
  game->set_big_blind(20);
  state->set_hand_id("seed");
  state->set_decision_index(1);
  state->set_street(pv::STREET_FLOP);
  state->set_button_seat(1);
  state->set_hero_player_id("hero");
  pv::PlayerState* villain = state->add_players();
  villain->set_player_id("villain");
  villain->set_seat(1);
  villain->set_stack(1980);
  villain->set_status(pv::PLAYER_STATUS_ACTIVE);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("hero");
  hero->set_seat(0);
  hero->set_stack(1980);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = makeCard(pv::RANK_NINE, pv::SUIT_SPADES);
  *state->add_hero_hole_cards() = makeCard(pv::RANK_KING, pv::SUIT_HEARTS);
  *state->add_board() = makeCard(pv::RANK_QUEEN, pv::SUIT_DIAMONDS);
  *state->add_board() = makeCard(pv::RANK_SEVEN, pv::SUIT_CLUBS);
  *state->add_board() = makeCard(pv::RANK_TWO, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(60);
  state->mutable_pot()->set_main_pot(60);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
  pv::LegalAction* bet = state->add_legal_actions();
  bet->set_type(pv::ACTION_TYPE_BET);
  bet->set_min_target_total(20);
  bet->set_max_target_total(1980);
  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(1);
  options->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
  return request;
}

inline std::vector<std::string> structuredSeeds() {
  std::vector<std::string> seeds;

  seeds.push_back(frame(envelopeFor(baseRequest(), "valid")));

  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_legal_actions(1)->set_type(static_cast<pv::ActionType>(99));
    seeds.push_back(frame(envelopeFor(request, "type99")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_legal_actions(1)->set_type(static_cast<pv::ActionType>(-1));
    seeds.push_back(frame(envelopeFor(request, "type-neg")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->set_street(static_cast<pv::Street>(99));
    seeds.push_back(frame(envelopeFor(request, "street99")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_hero_hole_cards(0)->set_rank(static_cast<pv::Rank>(99));
    seeds.push_back(frame(envelopeFor(request, "rank99")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_board(0)->set_suit(static_cast<pv::Suit>(99));
    seeds.push_back(frame(envelopeFor(request, "suit99")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_pot()->set_pot_total(1ull << 40);
    request.mutable_state()->mutable_pot()->set_main_pot(1ull << 40);
    seeds.push_back(frame(envelopeFor(request, "hugepot")));
  }
  {
    // Each chip field fits a signed int but pot + to_call overflows one:
    // 2e9 + 2e9 = 4e9 > INT_MAX, the fold-to-call flip the boundary blocks.
    pv::DecisionRequest request = baseRequest();
    constexpr std::uint64_t twoBillion = 2'000'000'000ull;
    request.mutable_state()->mutable_pot()->set_pot_total(twoBillion);
    request.mutable_state()->mutable_pot()->set_main_pot(twoBillion);
    request.mutable_state()->set_to_call(twoBillion);
    seeds.push_back(frame(envelopeFor(request, "intoverflow")));
  }
  {
    // Exact INT_MAX boundary with a one-chip call.
    pv::DecisionRequest request = baseRequest();
    constexpr std::uint64_t intMax = 2'147'483'647ull;
    request.mutable_state()->mutable_pot()->set_pot_total(intMax);
    request.mutable_state()->mutable_pot()->set_main_pot(intMax);
    request.mutable_state()->set_to_call(1);
    seeds.push_back(frame(envelopeFor(request, "intmaxedge")));
  }
  {
    // Side-pot hand: main 40 plus a side pot over the two players.
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_pot()->set_pot_total(80);
    request.mutable_state()->mutable_pot()->set_main_pot(40);
    pv::SidePot* side = request.mutable_state()->mutable_pot()->add_side_pots();
    side->set_amount(40);
    side->add_eligible_player_ids("hero");
    side->add_eligible_player_ids("villain");
    seeds.push_back(frame(envelopeFor(request, "sidepot")));
  }
  {
    // Big blind option: preflop, no voluntary history, fold/check/raise.
    pv::DecisionRequest request = baseRequest();
    pv::HandState* state = request.mutable_state();
    state->set_street(pv::STREET_PREFLOP);
    state->clear_board();
    state->mutable_players(0)->set_street_committed(10);
    state->mutable_players(1)->set_street_committed(20);
    pv::ForcedContribution* sb = state->add_forced_contributions();
    sb->set_player_id("villain");
    sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    sb->set_amount(10);
    pv::ForcedContribution* bb = state->add_forced_contributions();
    bb->set_player_id("hero");
    bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    bb->set_amount(20);
    state->clear_legal_actions();
    state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
    state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
    pv::LegalAction* raise = state->add_legal_actions();
    raise->set_type(pv::ACTION_TYPE_RAISE);
    raise->set_min_target_total(40);
    raise->set_max_target_total(2000);
    state->set_to_call(0);
    seeds.push_back(frame(envelopeFor(request, "bboption")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(static_cast<pv::SolverMode>(99));
    seeds.push_back(frame(envelopeFor(request, "solver99")));
  }
  {
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_players(1)->set_status(static_cast<pv::PlayerStatus>(99));
    seeds.push_back(frame(envelopeFor(request, "status99")));
  }

  // ---- RFC 0002 Stage 8 minor-1 / expanded-strategy frames. ---------------
  // Capability queries at every minor reach the negotiation branch.
  seeds.push_back(frame(capabilitiesFor(0, "cap0")));
  seeds.push_back(frame(capabilitiesFor(1, "cap1")));
  seeds.push_back(frame(capabilitiesFor(2, "cap2")));
  seeds.push_back(frame(capabilitiesFor(3, "cap3")));
  seeds.push_back(frame(capabilitiesFor(0xffffffffu, "caphuge")));
  {
    // Minor 1 with the new BLUEPRINT mode: no resident services, so it must
    // resolve to a non-retryable UNSUPPORTED_FEATURE error, never a crash.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    seeds.push_back(frame(envelopeForMinor(request, "blueprint1", 1)));
  }
  {
    // Minor 1 with RESOLVING (7): with no resident services resolving is not
    // advertised, so it resolves to a non-retryable UNSUPPORTED_FEATURE error,
    // never a crash.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_RESOLVING);
    seeds.push_back(frame(envelopeForMinor(request, "resolve1", 1)));
  }
  {
    // RESOLVING (7) on minor 0 is rejected by the minor-aware validator before
    // any service call; the response stays on the frozen minor-0 byte path.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_RESOLVING);
    seeds.push_back(frame(envelopeFor(request, "resolve0")));
  }
  {
    // Minor 1 AUTOMATIC: reconstruction runs against a VALID base flop
    // request (check/bet, no resident root configured), then the heuristic
    // answers with an ExpandedStrategy, exercising the expanded response
    // invariants in the harness.
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_legal_actions(0)->set_type(pv::ACTION_TYPE_CHECK);
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
    seeds.push_back(frame(envelopeForMinor(request, "automatic1", 1)));
  }
  {
    // Minor 1 HEURISTIC bypasses resident code.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_HEURISTIC);
    seeds.push_back(frame(envelopeForMinor(request, "heuristic1", 1)));
  }
  {
    // BLUEPRINT on minor 0 is rejected even though the enum is known.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    seeds.push_back(frame(envelopeFor(request, "blueprint0")));
  }
  {
    // Minor 1 with an open enum one past RESOLVING.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(static_cast<pv::SolverMode>(8));
    seeds.push_back(frame(envelopeForMinor(request, "solver8-minor1", 1)));
  }
  {
    // Minor 1 with a malformed structured history exercising the postflop
    // reconstructor's miss path (unresolved actor).
    pv::DecisionRequest request = baseRequest();
    pv::ActionEvent* event = request.mutable_state()->add_action_history();
    event->set_sequence(0);
    event->set_street(pv::STREET_FLOP);
    event->set_actor_player_id("nobody");
    event->set_action(pv::ACTION_TYPE_CHECK);
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    seeds.push_back(frame(envelopeForMinor(request, "reconmiss1", 1)));
  }
  {
    // A manually constructed minor-1 expanded_strategy response must be
    // re-parseable by the fuzz harness (protobuf-unknown-oneof tolerance).
    pv::Envelope envelope;
    envelope.set_protocol_minor(1);
    envelope.set_request_id("expanded-response-seed");
    pv::ExpandedStrategy* expanded =
        envelope.mutable_decision_response()->mutable_expanded_strategy();
    pv::ActionPolicy* check = expanded->add_actions();
    check->set_type(pv::ACTION_TYPE_CHECK);
    check->set_probability(1.0);
    expanded->mutable_selected_action()->set_type(pv::ACTION_TYPE_CHECK);
    expanded->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
    expanded->mutable_solver()->set_artifact_sha256(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    expanded->mutable_solver()->set_guarantee("uncertified");
    seeds.push_back(frame(envelope));
  }

  // ---- RFC 0008 stage 5 minor-2 guarantee frames. -------------------------
  {
    // Minor-2 AUTOMATIC heuristic answer: expanded strategy carrying field 11
    // and never field 10 (symmetric presence oracle in the harness).
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_legal_actions(0)->set_type(pv::ACTION_TYPE_CHECK);
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
    seeds.push_back(frame(envelopeForMinor(request, "automatic2", 2)));
  }
  {
    // Minor-2 HEURISTIC with an approximate floor: served.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_HEURISTIC);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    seeds.push_back(frame(envelopeForMinor(request, "floorapprox2", 2)));
  }
  {
    // An exact_solved floor cannot be met by any stage-5 source: code 9.
    pv::DecisionRequest request = baseRequest();
    request.mutable_state()->mutable_legal_actions(0)->set_type(pv::ACTION_TYPE_CHECK);
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_EXACT_SOLVED);
    seeds.push_back(frame(envelopeForMinor(request, "floorexact2", 2)));
  }
  {
    // Field 8 on minor 1 is a feature negotiation failure, even in-range.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    seeds.push_back(frame(envelopeForMinor(request, "floor1", 1)));
  }
  {
    // Field 8 on minor 0 is the same feature failure on the frozen path.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    seeds.push_back(frame(envelopeFor(request, "floor0")));
  }
  {
    // Present UNSPECIFIED floor at minor 2 is INVALID_REQUEST.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_UNSPECIFIED);
    seeds.push_back(frame(envelopeForMinor(request, "floorzero2", 2)));
  }
  {
    // Out-of-range open-enum floor at minor 2 is INVALID_REQUEST.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_minimum_guarantee(static_cast<pv::GuaranteeLevel>(6));
    seeds.push_back(frame(envelopeForMinor(request, "floor6-2", 2)));
  }
  {
    // Forced experimental backends stay rejected at minor 2.
    for (const pv::SolverMode forced :
         {pv::SOLVER_MODE_RIVER_LP, pv::SOLVER_MODE_RIVER_DCFR, pv::SOLVER_MODE_MULTISTREET_CFR}) {
      pv::DecisionRequest request = baseRequest();
      request.mutable_options()->set_solver_mode(forced);
      seeds.push_back(frame(envelopeForMinor(request, "forced2", 2)));
    }
  }
  {
    // BLUEPRINT at minor 2 with no resident services is a coverage miss.
    pv::DecisionRequest request = baseRequest();
    request.mutable_options()->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    seeds.push_back(frame(envelopeForMinor(request, "blueprint2", 2)));
  }
  {
    // A manually constructed minor-2 guaranteed RESPONSE frame. The host
    // rejects response payloads ("decision responses are not accepted by the
    // host"), so this seed only pins re-parse tolerance for a field-11 frame;
    // it never reaches the disjoint-vocabulary oracle. Host-output seeds
    // (automatic2, floorapprox2) are what exercise that oracle.
    pv::Envelope envelope;
    envelope.set_protocol_minor(2);
    envelope.set_request_id("guaranteed-response-seed");
    pv::ExpandedStrategy* expanded =
        envelope.mutable_decision_response()->mutable_expanded_strategy();
    pv::ActionPolicy* check = expanded->add_actions();
    check->set_type(pv::ACTION_TYPE_CHECK);
    check->set_probability(1.0);
    expanded->mutable_selected_action()->set_type(pv::ACTION_TYPE_CHECK);
    expanded->mutable_solver()->set_source(pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC);
    expanded->mutable_solver()->set_guarantee_level("approximate");
    seeds.push_back(frame(envelope));
  }
  return seeds;
}

// Defined in fuzz_v1_proto.cpp alongside the entry point.
std::vector<std::string> structuredFuzzSeeds();

}  // namespace bs::v1::fuzz
