#include <bs/v1_protocol.hpp>
#include <cstdio>
#include <stdexcept>
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

pv::Card card(pv::Rank rank, pv::Suit suit) {
  pv::Card value;
  value.set_rank(rank);
  value.set_suit(suit);
  return value;
}

// Minimal valid heads-up preflop request facing an open.
pv::DecisionRequest facingOpen(bool sampled = true) {
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
  state->set_hand_id("hand-response");
  state->set_decision_index(1);
  state->set_street(pv::STREET_PREFLOP);
  state->set_button_seat(1);
  state->set_hero_player_id("hero");
  pv::PlayerState* villain = state->add_players();
  villain->set_player_id("villain");
  villain->set_seat(1);
  villain->set_stack(2000);
  villain->set_status(pv::PLAYER_STATUS_ACTIVE);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("hero");
  hero->set_seat(0);
  hero->set_stack(2000);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_hero_hole_cards() = card(pv::RANK_KING, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(50);
  state->mutable_pot()->set_main_pot(50);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  pv::LegalAction* raise = state->add_legal_actions();
  raise->set_type(pv::ACTION_TYPE_RAISE);
  raise->set_min_target_total(60);
  raise->set_max_target_total(2000);
  state->set_to_call(20);
  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(7);
  options->set_include_sampled_action(sampled);
  options->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
  return request;
}

}  // namespace

int main() {
  // Strategic raise: single probability-1 policy with exact target.
  {
    pv::DecisionRequest request = facingOpen();
    bs::Decision decision{"raise", 120, "3bet value AKs", -1, -1};
    pv::DecisionResponse response = bs::v1::mapDecisionResponse(request, decision);
    check(response.has_strategy(), "raise maps to strategy");
    check(!response.has_error(), "raise response has no error");
    const pv::Strategy& strategy = response.strategy();
    check(strategy.actions_size() == 1, "one action policy");
    const pv::ActionPolicy& policy = strategy.actions(0);
    check(policy.type() == pv::ACTION_TYPE_RAISE, "policy type raise");
    check(policy.has_target_total() && policy.target_total() == 120, "policy target 120");
    check(policy.probability() == 1.0, "policy probability 1");
    check(strategy.has_selected_action(), "selected action present when requested");
    check(strategy.selected_action().type() == pv::ACTION_TYPE_RAISE, "selected raise");
    check(strategy.selected_action().target_total() == 120, "selected target 120");
    check(strategy.solver().source() == pv::SOLVER_SOURCE_PREFLOP_CHART,
          "3bet maps to preflop chart");
    check(!strategy.solver().has_equity(), "equity sentinel stays unset");
    check(!strategy.solver().has_minimum_defense_frequency(), "mdf sentinel stays unset");
  }

  // Selected action omitted when not requested.
  {
    pv::DecisionRequest request = facingOpen(false);
    bs::Decision decision{"fold", 0, "fold pre BTN 72o", -1, -1};
    pv::DecisionResponse response = bs::v1::mapDecisionResponse(request, decision);
    check(response.strategy().actions(0).type() == pv::ACTION_TYPE_FOLD, "fold policy");
    check(!response.strategy().actions(0).has_target_total(), "fold has no target");
    check(!response.strategy().has_selected_action(), "selected omitted");
  }

  // Check/call carry no target; equity and mdf are forwarded.
  {
    pv::DecisionRequest request = facingOpen();
    request.mutable_state()->set_to_call(0);
    pv::HandState* state = request.mutable_state();
    state->clear_legal_actions();
    state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
    pv::LegalAction* bet = state->add_legal_actions();
    bet->set_type(pv::ACTION_TYPE_BET);
    bet->set_min_target_total(20);
    bet->set_max_target_total(2000);
    bs::Decision decision{"check", 0, "check SDV/giveup", 0.42, 0.8};
    pv::DecisionResponse response = bs::v1::mapDecisionResponse(request, decision);
    check(response.strategy().actions(0).type() == pv::ACTION_TYPE_CHECK, "check policy");
    check(response.strategy().solver().has_equity(), "equity forwarded");
    check(response.strategy().solver().minimum_defense_frequency() == 0.8, "mdf forwarded");
    check(response.strategy().solver().source() == pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC,
          "postflop reason maps to heuristic");
  }

  // River solver provenance.
  {
    pv::DecisionRequest request = facingOpen();
    bs::Decision exact{"raise", 250, "gto c", -1, 0.7};
    pv::DecisionResponse exactResponse = bs::v1::mapDecisionResponse(request, exact);
    check(exactResponse.strategy().solver().source() == pv::SOLVER_SOURCE_RIVER_LP,
          "gto maps to river LP");
    bs::Decision cfr{"raise", 250, "gto-cfr c", -1, 0.7};
    pv::DecisionResponse cfrResponse = bs::v1::mapDecisionResponse(request, cfr);
    check(cfrResponse.strategy().solver().source() == pv::SOLVER_SOURCE_RIVER_DCFR,
          "gto-cfr maps to river DCFR");
  }

  // A strategic fold is a Strategy, never an error.
  {
    pv::DecisionRequest request = facingOpen();
    bs::Decision decision{"fold", 0, "fold eq0<30", 0.0, 0.5};
    pv::DecisionResponse response = bs::v1::mapDecisionResponse(request, decision);
    check(response.has_strategy() && response.strategy().actions(0).type() == pv::ACTION_TYPE_FOLD,
          "strategic fold stays a strategy");
    check(!response.has_error(), "strategic fold is not an engine error");
  }

  // Non-member targets fail closed as INTERNAL instead of an illegal strategy.
  {
    pv::DecisionRequest request = facingOpen();
    bs::Decision decision{"raise", 40, "3bet value AKs", -1, -1};
    bool threw = false;
    try {
      (void)bs::v1::mapDecisionResponse(request, decision);
    } catch (const bs::v1::MappingError& error) {
      threw = true;
      check(error.code() == pv::ERROR_CODE_INTERNAL, "non-member target is internal error");
      check(error.retryable(), "internal error is retryable");
    }
    check(threw, "out-of-range target rejected");
  }

  {
    pv::DecisionRequest request = facingOpen();
    bs::Decision decision{"dance", 0, "", -1, -1};
    bool threw = false;
    try {
      (void)bs::v1::mapDecisionResponse(request, decision);
    } catch (const bs::v1::MappingError& error) {
      threw = true;
      check(error.code() == pv::ERROR_CODE_INTERNAL, "unknown action is internal error");
    }
    check(threw, "unknown action rejected");
  }

  // Capabilities advertise minor 0 only and the exact v1 amount semantics.
  {
    pv::GetCapabilitiesResponse capabilities = bs::v1::buildCapabilities();
    check(capabilities.supported_protocol_minors_size() == 1 &&
              capabilities.supported_protocol_minors(0) == 0,
          "only protocol minor 0");
    check(capabilities.maximum_request_bytes() == 1048576, "1 MiB request cap");
    check(capabilities.amount_semantics() == pv::AMOUNT_SEMANTICS_TARGET_TOTAL_INCLUSIVE,
          "target-total inclusive semantics");
    check(capabilities.supported_game_variants_size() == 1 &&
              capabilities.supported_game_variants(0) == pv::GAME_VARIANT_NLHE,
          "NLHE only");
    check(capabilities.side_pots() == pv::FEATURE_SUPPORT_UNSUPPORTED, "side pots unsupported");
    check(capabilities.tournament_icm() == pv::FEATURE_SUPPORT_UNSUPPORTED, "ICM unsupported");
    check(!capabilities.engine_build_version().empty(), "build version advertised");
    // Only engine-selected modes are force-acceptable in minor 0; the river
    // backends remain internal feature bits.
    check(capabilities.solver_modes_size() == 2, "two selectable solver modes");
    bool hasAutomatic = false;
    bool hasHeuristic = false;
    bool hasForcedBackend = false;
    for (int i = 0; i < capabilities.solver_modes_size(); ++i) {
      hasAutomatic |= capabilities.solver_modes(i) == pv::SOLVER_MODE_AUTOMATIC;
      hasHeuristic |= capabilities.solver_modes(i) == pv::SOLVER_MODE_HEURISTIC;
      hasForcedBackend |= capabilities.solver_modes(i) == pv::SOLVER_MODE_RIVER_LP ||
                          capabilities.solver_modes(i) == pv::SOLVER_MODE_RIVER_DCFR;
    }
    check(hasAutomatic && hasHeuristic, "automatic and heuristic advertised");
    check(!hasForcedBackend, "forced river backends not advertised as modes");
  }

  // The violation list is capped so a request with many bad fields cannot
  // produce a frame larger than the transport limit.
  {
    std::vector<pv::FieldViolation> violations;
    for (int i = 0; i < 5000; ++i) {
      pv::FieldViolation violation;
      violation.set_field_path("state.f" + std::to_string(i));
      violation.set_description("invalid");
      violations.push_back(violation);
    }
    pv::DecisionResponse response = bs::v1::errorResponse(
        pv::ERROR_CODE_INVALID_REQUEST, "invalid request", false, std::move(violations));
    check(response.error().violations_size() == 32, "violation list capped at 32");
    check(
        response.error().violations(31).description() == "additional field violations were omitted",
        "truncation marker present");
    std::string encoded;
    pv::Envelope envelope;
    envelope.set_protocol_minor(0);
    envelope.set_request_id("cap-test");
    *envelope.mutable_decision_response() = response;
    check(envelope.SerializeToString(&encoded) && encoded.size() < bs::v1::kMaxFrameBytes,
          "capped error response fits one frame");
  }

  // Error responses carry code, safe message, and retryability.
  {
    pv::DecisionResponse response =
        bs::v1::errorResponse(pv::ERROR_CODE_INVALID_REQUEST, "bad request", false);
    check(response.has_error(), "error branch set");
    check(response.error().code() == pv::ERROR_CODE_INVALID_REQUEST, "invalid code");
    check(!response.error().retryable(), "invalid request is not retryable");
    check(response.error().message() == "bad request", "message preserved");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 RESPONSE MAPPER TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 RESPONSE MAPPER TESTS PASSED");
  return 0;
}
