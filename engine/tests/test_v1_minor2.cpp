// RFC 0008 stage 5 negotiated minor-2 host tests:
//  - negotiation/capabilities (minor 2 accepted, [0,1,2], v1.2.0, minor 3 out)
//  - every successful minor-2 decision is expanded_strategy with field 11 and
//    never field 10; the wire source is the policy-declared source
//  - the minimum_guarantee floor matrix (served, code 9 non-retryable, forced
//    coverage/deadline precedence, blueprint-cache vs certified distinction)
//  - request field 8 gates (minor 0/1 feature gate, present 0 / out-of-range
//    invalid at minor 2, forced experimental backends stay rejected)
//  - the certified_bound <-> modeled_exact_bound cross-minor bridge
//  - the chart-fold source divergence between minors 1 and 2
//  - malformed-row completeness precedence over the floor
//  - the boundary fail-closed wire-source table
// Fakes are deterministic and protobuf-free; no artifact or CFR run occurs.
#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/v1_protocol.hpp>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "../src/protocol/v1_mappers.hpp"

namespace {

namespace pv = bs::v1::pv;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::v1::EnvelopeOutcome;
using bs::v1::EnvelopeResult;
using bs::v1::V1BlueprintMiss;
using bs::v1::V1BlueprintResult;
using bs::v1::V1BlueprintRow;
using bs::v1::V1HostServices;
using bs::v1::V1ResolveOutcome;
using bs::v1::V1ResolveResult;

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

// Combined seam fake: scripted blueprint rows plus an optional resolver.
class FakeServices final : public V1HostServices {
 public:
  bool advertised = true;
  bool resolvingOn = true;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  V1ResolveOutcome outcome = V1ResolveOutcome::Certified;
  std::string sha = std::string(64, 'a');
  std::vector<Action> actions{{ActionType::Check}, {ActionType::Bet, 100}};
  std::vector<double> probabilities{0.5, 0.5};
  mutable int blueprintLookups = 0;
  mutable int resolveLookups = 0;

  bool blueprintAdvertised() const noexcept override { return advertised; }
  bool resolvingAdvertised() const noexcept override { return advertised && resolvingOn; }

  V1BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                          std::span<const bs::poker::PublicAction>,
                                          const std::array<int, 2>&,
                                          std::string_view) const noexcept override {
    ++blueprintLookups;
    V1BlueprintResult result;
    if (miss != V1BlueprintMiss::None) {
      result.hit = false;
      result.miss = miss;
      return result;
    }
    result.hit = true;
    result.row.size = actions.size();
    result.row.actions = actions.data();
    result.row.probabilities = probabilities.data();
    result.row.artifact_sha256 = sha;
    return result;
  }

  V1ResolveResult resolvingDecision(const bs::poker::GameState&,
                                    std::span<const bs::poker::PublicAction>,
                                    const std::array<int, 2>&, std::string_view,
                                    std::uint32_t) const noexcept override {
    ++resolveLookups;
    V1ResolveResult result;
    result.outcome = outcome;
    if (outcome == V1ResolveOutcome::Certified || outcome == V1ResolveOutcome::DeadlineBlueprint) {
      result.row.size = actions.size();
      result.row.actions = actions.data();
      result.row.probabilities = probabilities.data();
      result.row.artifact_sha256 = sha;
    }
    return result;
  }
};

pv::DecisionRequest flopRequest(pv::SolverMode mode, const std::string& holeRank = "weak") {
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
  state->set_hand_id("minor2-flop");
  state->set_decision_index(1);
  state->set_street(pv::STREET_FLOP);
  state->set_button_seat(0);
  state->set_hero_player_id("p1");
  pv::PlayerState* button = state->add_players();
  button->set_player_id("p0");
  button->set_seat(0);
  button->set_stack(1980);
  button->set_status(pv::PLAYER_STATUS_ACTIVE);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("p1");
  hero->set_seat(1);
  hero->set_stack(1980);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);
  if (holeRank == "weak") {
    *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_HEARTS);
    *state->add_hero_hole_cards() = card(pv::RANK_THREE, pv::SUIT_HEARTS);
  } else {
    *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_HEARTS);
    *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_CLUBS);
  }
  *state->add_board() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_KING, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_QUEEN, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(40);
  state->mutable_pot()->set_main_pot(40);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
  pv::LegalAction* bet = state->add_legal_actions();
  bet->set_type(pv::ACTION_TYPE_BET);
  bet->set_min_target_total(100);
  bet->set_max_target_total(1980);
  state->set_to_call(0);
  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(42);
  options->set_include_sampled_action(true);
  options->set_solver_mode(mode);
  return request;
}

// Preflop BB facing a BTN open with the weakest hand: the chart selects a
// fold ("fold vs open"), one of the four frozen inference discrepancies.
pv::DecisionRequest weakPreflopRequest(pv::SolverMode mode) {
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
  state->set_hand_id("minor2-pre");
  state->set_decision_index(1);
  state->set_street(pv::STREET_PREFLOP);
  state->set_button_seat(1);
  state->set_hero_player_id("p1");
  pv::PlayerState* villain = state->add_players();
  villain->set_player_id("p0");
  villain->set_seat(1);
  villain->set_stack(1940);
  villain->set_street_committed(60);
  villain->set_status(pv::PLAYER_STATUS_ACTIVE);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("p1");
  hero->set_seat(0);
  hero->set_stack(1980);
  hero->set_street_committed(20);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_SEVEN, pv::SUIT_CLUBS);
  *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_DIAMONDS);
  state->mutable_pot()->set_pot_total(70);
  state->mutable_pot()->set_main_pot(70);
  pv::ActionEvent* open = state->add_action_history();
  open->set_sequence(0);
  open->set_street(pv::STREET_PREFLOP);
  open->set_actor_player_id("p0");
  open->set_action(pv::ACTION_TYPE_RAISE);
  open->set_target_total(60);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  pv::LegalAction* raise = state->add_legal_actions();
  raise->set_type(pv::ACTION_TYPE_RAISE);
  raise->set_min_target_total(100);
  raise->set_max_target_total(2000);
  state->set_to_call(40);
  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(7);
  options->set_include_sampled_action(true);
  options->set_solver_mode(mode);
  return request;
}

// Facing-all-in flop snapshot (fold/call legal) so resolver-only
// reconstruction admits the node; blueprint rows use fold/call.
pv::DecisionRequest resolveRequest(pv::SolverMode mode) {
  pv::DecisionRequest request = flopRequest(mode);
  pv::HandState* state = request.mutable_state();
  state->set_hand_id("minor2-resolve");
  state->set_button_seat(1);
  pv::PlayerState* p0 = state->mutable_players(0);
  p0->set_stack(0);
  p0->set_street_committed(40);
  p0->set_status(pv::PLAYER_STATUS_ALL_IN);
  pv::PlayerState* p1 = state->mutable_players(1);
  p1->set_stack(40);
  p1->set_street_committed(0);
  state->clear_forced_contributions();
  auto* sb = state->add_forced_contributions();
  sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  sb->set_player_id("p1");
  sb->set_amount(10);
  auto* bb = state->add_forced_contributions();
  bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  bb->set_player_id("p0");
  bb->set_amount(20);
  state->clear_action_history();
  pv::ActionEvent* jam = state->add_action_history();
  jam->set_sequence(0);
  jam->set_street(pv::STREET_FLOP);
  jam->set_actor_player_id("p0");
  jam->set_action(pv::ACTION_TYPE_BET);
  jam->set_target_total(40);
  state->mutable_pot()->set_pot_total(80);
  state->mutable_pot()->set_main_pot(80);
  state->clear_legal_actions();
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  state->set_to_call(40);
  return request;
}

std::string envelopeFor(uint32_t minor, const std::string& id, pv::DecisionRequest request) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = std::move(request);
  return envelope.SerializeAsString();
}

std::string capsEnvelope(uint32_t minor) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id("caps");
  envelope.mutable_get_capabilities_request();
  return envelope.SerializeAsString();
}

pv::Envelope send(const std::string& frame, const V1HostServices& services) {
  const EnvelopeResult result = bs::v1::handleEnvelope(frame, services);
  check(result.outcome == EnvelopeOutcome::Respond, "host responds");
  pv::Envelope response;
  check(response.ParseFromString(result.response), "response parses");
  return response;
}

pv::Envelope heuristicDecision(const FakeServices& services, pv::SolverMode mode, uint32_t minor,
                               pv::GuaranteeLevel floor, bool setFloor) {
  pv::DecisionRequest request = flopRequest(mode);
  if (setFloor)
    request.mutable_options()->set_minimum_guarantee(floor);
  return send(envelopeFor(minor, "dec", request), services);
}

}  // namespace

int main() {
  // --- Negotiation and capabilities ---------------------------------------
  {
    FakeServices services;
    pv::Envelope response = send(capsEnvelope(2), services);
    check(response.protocol_minor() == 2, "minor-2 caps echo minor 2");
    const pv::GetCapabilitiesResponse& caps = response.get_capabilities_response();
    check(caps.supported_protocol_minors_size() == 3 && caps.supported_protocol_minors(0) == 0 &&
              caps.supported_protocol_minors(1) == 1 && caps.supported_protocol_minors(2) == 2,
          "minor-2 caps advertise 0, 1 and 2");
    check(caps.engine_build_version() == "bigshark-engine-v1.2.0",
          "minor-2 build version is v1.2.0");
    bool hasBlueprint = false;
    for (int i = 0; i < caps.solver_modes_size(); ++i)
      hasBlueprint |= caps.solver_modes(i) == pv::SOLVER_MODE_BLUEPRINT;
    check(hasBlueprint, "minor-2 keeps the minor-1 solver mode set");
  }
  {
    FakeServices services;
    pv::Envelope response = send(capsEnvelope(3), services);
    check(response.protocol_minor() == 0 &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_PROTOCOL,
          "minor 3 stays UNSUPPORTED_PROTOCOL");
  }

  // --- Heuristic minor-2 decision: declared source, field 11, no field 10 -
  {
    FakeServices services;
    pv::Envelope response = heuristicDecision(services, pv::SOLVER_MODE_HEURISTIC, 2,
                                              pv::GUARANTEE_LEVEL_APPROXIMATE, false);
    check(response.protocol_minor() == 2, "echo minor 2");
    check(response.decision_response().has_expanded_strategy(),
          "minor 2 always answers with expanded_strategy");
    check(!response.decision_response().has_strategy(), "minor 2 never uses the 5-capped Strategy");
    const pv::SolverMetadata& solver = response.decision_response().expanded_strategy().solver();
    check(solver.source() == pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC,
          "flop heuristic keeps the declared heuristic source");
    check(solver.has_guarantee_level() && solver.guarantee_level() == "approximate",
          "every minor-2 heuristic answer carries field 11 = approximate");
    check(!solver.has_guarantee(), "minor 2 never sets field 10");
    check(!solver.cache_hit(), "heuristic answer is not a cache hit");
  }

  // --- Floor matrix on the heuristic (all heuristic answers approximate) ---
  {
    FakeServices services;
    // Floor operational_fallback: served.
    pv::Envelope op = heuristicDecision(services, pv::SOLVER_MODE_HEURISTIC, 2,
                                        pv::GUARANTEE_LEVEL_OPERATIONAL_FALLBACK, true);
    check(op.decision_response().has_expanded_strategy(),
          "approximate answer meets an operational floor");
    // Floor approximate: served.
    pv::Envelope approx = heuristicDecision(services, pv::SOLVER_MODE_HEURISTIC, 2,
                                            pv::GUARANTEE_LEVEL_APPROXIMATE, true);
    check(approx.decision_response().has_expanded_strategy(),
          "approximate answer meets an approximate floor");
    // Floors above approximate: code 9, no payload, non-retryable.
    for (const auto pair : {std::pair<const char*, pv::GuaranteeLevel>{
                                "abstract", pv::GUARANTEE_LEVEL_ABSTRACT_SOLVED},
                            {"exact", pv::GUARANTEE_LEVEL_EXACT_SOLVED},
                            {"certified", pv::GUARANTEE_LEVEL_CERTIFIED_BOUND}}) {
      pv::Envelope refused =
          heuristicDecision(services, pv::SOLVER_MODE_HEURISTIC, 2, pair.second, true);
      const pv::DecisionResponse& dr = refused.decision_response();
      check(dr.has_error() && dr.error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
            std::string("floor ") + pair.first + " refuses with code 9");
      check(!dr.error().retryable(), "code 9 is non-retryable");
      check(!dr.has_strategy() && !dr.has_expanded_strategy(),
            "code 9 carries no strategy payload");
      check(refused.protocol_minor() == 2, "code 9 echoes minor 2");
    }
  }

  // --- AUTOMATIC blueprint hit is approximate: floor certified -> code 9 ----
  // (The cache_hit-vs-certified mutant: a cached blueprint row must never be
  // mistaken for a certified resolve.)
  {
    FakeServices services;
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_AUTOMATIC);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope response = send(envelopeFor(2, "bp-floor", request), services);
    check(response.protocol_minor() == 2, "the AUTOMATIC code-9 refusal echoes minor 2");
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
          "AUTOMATIC blueprint hit + certified floor is code 9 (never resolves)");
    check(services.resolveLookups == 0, "AUTOMATIC never invokes the resolver");

    // Same request with an approximate floor is served from the cache.
    pv::DecisionRequest served = flopRequest(pv::SOLVER_MODE_AUTOMATIC);
    served.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    pv::Envelope ok = send(envelopeFor(2, "bp-served", served), services);
    const pv::SolverMetadata& solver = ok.decision_response().expanded_strategy().solver();
    check(ok.decision_response().has_expanded_strategy() &&
              solver.source() == pv::SOLVER_SOURCE_BLUEPRINT && solver.cache_hit() &&
              solver.guarantee_level() == "approximate" && !solver.has_guarantee(),
          "cached blueprint served at approximate floor with field 11 only");
  }

  // --- Forced BLUEPRINT coverage errors take precedence over the floor -----
  {
    FakeServices services;
    services.miss = V1BlueprintMiss::OffTree;
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_BLUEPRINT);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_EXACT_SOLVED);
    pv::Envelope response = send(envelopeFor(2, "bp-miss-floor", request), services);
    check(response.protocol_minor() == 2, "coverage miss echoes minor 2");
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced blueprint coverage miss beats the floor");
  }

  // --- Forced RESOLVING with no advertised resolver is a minor-2 miss ------
  // (R16: same coverage error code/detail as minor 1, echo stays minor 2.)
  {
    FakeServices services;
    services.resolvingOn = false;  // blueprint advertised, resolver is not
    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    pv::Envelope response = send(envelopeFor(2, "resolve-off", request), services);
    check(response.protocol_minor() == 2, "resolver-miss response echoes minor 2");
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced resolving without an advertised resolver is UNSUPPORTED_FEATURE");
    check(services.resolveLookups == 0, "an unadvertised resolver is never called");
  }

  // --- Certified resolve: certified_bound bridge and floor behavior --------
  {
    FakeServices services;
    services.actions = {{ActionType::Fold}, {ActionType::Call}};
    services.probabilities = {0.3, 0.7};
    services.outcome = V1ResolveOutcome::Certified;

    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    pv::Envelope m2 = send(envelopeFor(2, "cert2", request), services);
    check(m2.decision_response().has_expanded_strategy(), "certified resolve serves minor 2");
    const pv::SolverMetadata& s2 = m2.decision_response().expanded_strategy().solver();
    check(s2.source() == pv::SOLVER_SOURCE_RESOLVING, "certified source is RESOLVING");
    check(s2.guarantee_level() == "certified_bound", "certified resolve earns certified_bound");
    check(!s2.has_guarantee(), "certified minor-2 row never sets field 10");
    check(s2.has_artifact_sha256(), "certified row keeps the field-9 digest");
    check(!s2.cache_hit() && s2.reason_code() == "resolving",
          "certified resolve is no cache hit and is reason-coded resolving");

    // The certified_bound <-> modeled_exact_bound bridge on minor 1.
    pv::Envelope m1 = send(envelopeFor(1, "cert1", request), services);
    const pv::SolverMetadata& s1 = m1.decision_response().expanded_strategy().solver();
    check(s1.guarantee() == "modeled_exact_bound" && !s1.has_guarantee_level(),
          "the same resolve is modeled_exact_bound on minor 1");
    check(s1.source() == pv::SOLVER_SOURCE_RESOLVING, "minor-1 bridge keeps RESOLVING");

    // Certified floor passes on minor 2.
    pv::DecisionRequest high = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    high.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope served = send(envelopeFor(2, "cert-high", high), services);
    check(served.decision_response().has_expanded_strategy(),
          "certified answer meets a certified floor");
  }

  // --- Deadline baseline: BLUEPRINT/approximate; floor certified -> code 9 --
  {
    FakeServices services;
    services.actions = {{ActionType::Fold}, {ActionType::Call}};
    services.probabilities = {0.3, 0.7};
    services.outcome = V1ResolveOutcome::DeadlineBlueprint;

    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    pv::Envelope served = send(envelopeFor(2, "deadline2", request), services);
    const pv::SolverMetadata& s = served.decision_response().expanded_strategy().solver();
    check(served.decision_response().has_expanded_strategy(), "deadline baseline serves");
    check(s.source() == pv::SOLVER_SOURCE_BLUEPRINT && s.cache_hit() &&
              s.guarantee_level() == "approximate" && !s.has_guarantee(),
          "deadline baseline is BLUEPRINT/cache-hit/approximate on minor 2");
    pv::Envelope m1 = send(envelopeFor(1, "deadline1", request), services);
    check(m1.decision_response().expanded_strategy().solver().guarantee() == "baseline",
          "deadline baseline is 'baseline' on minor 1");

    pv::DecisionRequest high = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    high.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope refused = send(envelopeFor(2, "deadline-high", high), services);
    check(refused.protocol_minor() == 2, "the deadline code-9 refusal echoes minor 2");
    check(refused.decision_response().has_error() &&
              refused.decision_response().error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
          "deadline baseline below a certified floor is code 9");
  }

  // --- Forced resolve with no baseline stays DEADLINE_EXCEEDED -------------
  {
    FakeServices services;
    services.actions = {{ActionType::Fold}, {ActionType::Call}};
    services.probabilities = {0.3, 0.7};
    services.outcome = V1ResolveOutcome::DeadlineExceeded;
    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_RESOLVING);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope response = send(envelopeFor(2, "deadline-exceeded", request), services);
    check(response.protocol_minor() == 2, "DEADLINE_EXCEEDED echoes minor 2");
    check(response.decision_response().error().code() == pv::ERROR_CODE_DEADLINE_EXCEEDED,
          "deadline with no baseline beats the floor");
  }

  // --- Field-8 request gates ------------------------------------------------
  {
    FakeServices services;
    // Presence on minor 0 is a feature negotiation failure, not invalid.
    pv::DecisionRequest r0 = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    r0.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    pv::Envelope m0 = send(envelopeFor(0, "floor0", r0), services);
    check(m0.protocol_minor() == 0, "the minor-0 feature error echoes minor 0");
    check(m0.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "minimum_guarantee on minor 0 is UNSUPPORTED_FEATURE");
    // Explicit UNSPECIFIED at minor 2 is INVALID_REQUEST.
    pv::DecisionRequest zero = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    zero.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_UNSPECIFIED);
    pv::Envelope mz = send(envelopeFor(2, "floor-zero", zero), services);
    check(mz.protocol_minor() == 2, "the INVALID_REQUEST floor error echoes minor 2");
    check(mz.decision_response().error().code() == pv::ERROR_CODE_INVALID_REQUEST,
          "present UNSPECIFIED floor at minor 2 is INVALID_REQUEST");
    // Out-of-range open-enum value at minor 2 is INVALID_REQUEST.
    pv::DecisionRequest bad = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    bad.mutable_options()->set_minimum_guarantee(static_cast<pv::GuaranteeLevel>(6));
    pv::Envelope mb = send(envelopeFor(2, "floor-oor", bad), services);
    check(mb.protocol_minor() == 2, "the out-of-range floor error echoes minor 2");
    check(mb.decision_response().error().code() == pv::ERROR_CODE_INVALID_REQUEST,
          "out-of-range floor at minor 2 is INVALID_REQUEST");
    // Forced experimental backends stay rejected at minor 2.
    for (const pv::SolverMode forced :
         {pv::SOLVER_MODE_RIVER_LP, pv::SOLVER_MODE_RIVER_DCFR, pv::SOLVER_MODE_MULTISTREET_CFR}) {
      pv::Envelope rejected = send(envelopeFor(2, "forced", flopRequest(forced)), services);
      check(rejected.protocol_minor() == 2, "the forced-backend rejection echoes minor 2");
      check(rejected.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
            "forced river/multistreet backend stays unsupported at minor 2");
    }
  }

  // --- Chart-fold source divergence ----------------------------------------
  {
    FakeServices services;
    // Minor 1 keeps the frozen reason-inference label.
    pv::Envelope m1 =
        send(envelopeFor(1, "fold1", weakPreflopRequest(pv::SOLVER_MODE_HEURISTIC)), services);
    const pv::ExpandedStrategy& e1 = m1.decision_response().expanded_strategy();
    check(e1.solver().source() == pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC &&
              !e1.solver().has_guarantee_level(),
          "minor 1 freezes the chart fold as postflop-heuristic with no field 11");
    // Minor 2 truthfully reports the chart and the approximate level.
    pv::Envelope m2 =
        send(envelopeFor(2, "fold2", weakPreflopRequest(pv::SOLVER_MODE_HEURISTIC)), services);
    const pv::ExpandedStrategy& e2 = m2.decision_response().expanded_strategy();
    check(e2.solver().source() == pv::SOLVER_SOURCE_PREFLOP_CHART &&
              e2.solver().guarantee_level() == "approximate" && !e2.solver().has_guarantee(),
          "minor 2 labels the chart fold PREFLOP_CHART/approximate");
  }

  // --- Completeness precedence: malformed row + high floor is INTERNAL -----
  {
    FakeServices services;
    services.sha = std::string(5, 'x');  // not a 64-hex digest
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_BLUEPRINT);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_EXACT_SOLVED);
    pv::Envelope response = send(envelopeFor(2, "bad-row", request), services);
    check(response.protocol_minor() == 2, "the INTERNAL completeness error echoes minor 2");
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_INTERNAL,
          "a malformed row with a high floor is INTERNAL, never code 9");
  }

  // --- R15 heuristic completeness is mapper-independent --------------------
  {
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    for (const bs::SourcedDecision& bad : {
             bs::SourcedDecision{{"bogus", 0, "unknown action verb", -1.0, -1.0},
                                 bs::DecisionSource::PostflopHeuristic},
             bs::SourcedDecision{{"fold", 0, "fold outside the check/bet window", -1.0, -1.0},
                                 bs::DecisionSource::PreflopChart},
         }) {
      bool threw = false;
      try {
        bs::v1::verifyHeuristicAnswerComplete(request, bad);
      } catch (const bs::v1::MappingError& error) {
        threw = true;
        check(error.code() == pv::ERROR_CODE_INTERNAL,
              "an un-mappable heuristic answer fails completeness as INTERNAL");
      }
      check(threw, "an un-mappable heuristic answer never reaches the floor comparison");
    }
  }

  // --- R6: the C++ token set equals the proto field-11 in: list ------------
  // Read from the descriptor's compiled buf.validate extension, not a second
  // hardcoded list, so a drift between the .proto vocabulary and guaranteeToken
  // goes red.
  {
    const google::protobuf::Descriptor* meta = pv::SolverMetadata::descriptor();
    const google::protobuf::FieldDescriptor* level = meta->FindFieldByName("guarantee_level");
    check(level != nullptr, "field 11 exists on SolverMetadata");
    if (level != nullptr) {
      const buf::validate::FieldRules& rules = level->options().GetExtension(buf::validate::field);
      const google::protobuf::RepeatedPtrField<std::string>& wire_in = rules.string().in();
      const std::array<bs::Guarantee, 5> levels{
          bs::Guarantee::OperationalFallback, bs::Guarantee::Approximate,
          bs::Guarantee::AbstractSolved, bs::Guarantee::ExactSolved, bs::Guarantee::CertifiedBound};
      check(wire_in.size() == levels.size(), "the proto in: list has exactly five tokens");
      for (const bs::Guarantee level_value : levels) {
        bool found = false;
        for (const std::string& wire_token : wire_in)
          found |= wire_token == bs::guaranteeToken(level_value);
        check(found, std::string("proto in: list contains the C++ token ") +
                         bs::guaranteeToken(level_value));
      }
    }
  }

  // --- Direct mapper vocabulary discipline ---------------------------------
  {
    FakeServices services;
    V1BlueprintRow row{services.actions.size(),
                       services.actions.data(),
                       services.probabilities.data(),
                       services.sha,
                       {}};
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_BLUEPRINT);
    pv::DecisionResponse minor1 = bs::v1::mapBlueprintExpandedResponse(request, row);
    check(minor1.expanded_strategy().solver().has_guarantee() &&
              !minor1.expanded_strategy().solver().has_guarantee_level(),
          "minor-1 storage mapper sets field 10 only");
    pv::DecisionResponse minor2 = bs::v1::mapGuaranteedBlueprintResponse(request, row);
    check(minor2.expanded_strategy().solver().has_guarantee_level() &&
              minor2.expanded_strategy().solver().guarantee_level() == "approximate" &&
              !minor2.expanded_strategy().solver().has_guarantee(),
          "minor-2 storage mapper sets field 11 only");
    pv::DecisionResponse certified = bs::v1::mapGuaranteedCertifiedResponse(request, row);
    check(certified.expanded_strategy().solver().source() == pv::SOLVER_SOURCE_RESOLVING &&
              certified.expanded_strategy().solver().guarantee_level() == "certified_bound",
          "certified mapper pins RESOLVING/certified_bound");
    // RFC 0009 W2c-ii-c: a multiway (3-seat) certified row carries its
    // diagnostic token on the wire (the string is bs::resolver::
    // kMultiwayCertificationToken, pinned here as a wire contract); a
    // two-seat row leaves diagnostic_reason unset so the frozen bytes are
    // unchanged.
    V1BlueprintRow multiway{services.actions.size(), services.actions.data(),
                            services.probabilities.data(), services.sha,
                            "multiway-certified:per-seat-unilateral-non-regression"};
    pv::DecisionResponse mw = bs::v1::mapGuaranteedCertifiedResponse(request, multiway);
    check(mw.expanded_strategy().solver().has_diagnostic_reason() &&
              mw.expanded_strategy().solver().diagnostic_reason() ==
                  "multiway-certified:per-seat-unilateral-non-regression",
          "multiway certified row carries the per-seat non-regression diagnostic token");
    check(!certified.expanded_strategy().solver().has_diagnostic_reason(),
          "two-seat certified row leaves diagnostic_reason unset");

    bs::SourcedDecision heuristic{{"check", 0, "check SDV/giveup", -1.0, -1.0},
                                  bs::DecisionSource::PostflopHeuristic};
    pv::DecisionResponse h = bs::v1::mapGuaranteedHeuristicResponse(request, heuristic);
    const pv::SolverMetadata& hs = h.expanded_strategy().solver();
    check(hs.source() == pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC &&
              hs.guarantee_level() == "approximate" && !hs.has_guarantee(),
          "minor-2 heuristic mapper field discipline");
  }

  // --- Boundary fail-closed wire-source table ------------------------------
  {
    using bs::Guarantee;
    check(bs::v1::failClosedGuaranteeForWireSource(pv::SOLVER_SOURCE_UNSPECIFIED) ==
              Guarantee::OperationalFallback,
          "UNSPECIFIED wire source fails closed to operational_fallback");
    check(bs::v1::failClosedGuaranteeForWireSource(static_cast<pv::SolverSource>(99)) ==
              Guarantee::OperationalFallback,
          "out-of-range wire source fails closed to operational_fallback");
    check(bs::v1::failClosedGuaranteeForWireSource(pv::SOLVER_SOURCE_RESOLVING) ==
              Guarantee::CertifiedBound,
          "RESOLVING wire source is certified_bound");
    for (const pv::SolverSource approximate :
         {pv::SOLVER_SOURCE_PREFLOP_CHART, pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC,
          pv::SOLVER_SOURCE_RIVER_LP, pv::SOLVER_SOURCE_RIVER_DCFR,
          pv::SOLVER_SOURCE_MULTISTREET_CFR, pv::SOLVER_SOURCE_BLUEPRINT})
      check(bs::v1::failClosedGuaranteeForWireSource(approximate) == Guarantee::Approximate,
            "known non-certified sources are approximate");
  }

  // --- HEURISTIC and BLUEPRINT answers are never operational_fallback ------
  // (RFC 0009 W3: an AUTOMATIC blueprint miss IS the operational fallback -
  // see the demotion guard below.)
  {
    FakeServices services;
    pv::DecisionRequest heuristic = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    pv::Envelope h = send(envelopeFor(2, "nofallback-h", heuristic), services);
    check(h.decision_response().expanded_strategy().solver().guarantee_level() !=
              "operational_fallback",
          "a heuristic minor-2 answer is never operational_fallback");
    pv::DecisionRequest blueprint = flopRequest(pv::SOLVER_MODE_BLUEPRINT);
    pv::Envelope b = send(envelopeFor(2, "nofallback-b", blueprint), services);
    check(b.decision_response().expanded_strategy().solver().guarantee_level() !=
              "operational_fallback",
          "a blueprint minor-2 answer is never operational_fallback");
  }

  // --- RFC 0009 W3 demotion guard + W4d resolver-on-miss ------------------
  // A minor-2 AUTOMATIC blueprint miss tries terminal-only resolving (W4d)
  // before ending at the declared operational fallback. When the resolver is
  // advertised but cannot resolve, the fallback still serves
  // (operational_fallback, UNSPECIFIED source), never the chart+heuristic
  // cascade. The companion minor-1 assertion pins that minor-1's AUTOMATIC
  // fallthrough is unchanged (still a heuristic source).
  {
    FakeServices services;
    services.miss = V1BlueprintMiss::OffTree;
    services.outcome = V1ResolveOutcome::Unsupported;  // resolver tried, misses
    pv::DecisionRequest automatic = flopRequest(pv::SOLVER_MODE_AUTOMATIC);
    pv::Envelope resp = send(envelopeFor(2, "w3-auto-miss", automatic), services);
    check(resp.protocol_minor() == 2, "AUTOMATIC miss echoes minor 2");
    const pv::ExpandedStrategy& es = resp.decision_response().expanded_strategy();
    check(es.solver().guarantee_level() == "operational_fallback",
          "minor-2 AUTOMATIC blueprint+resolver miss is the operational fallback");
    check(es.solver().source() == pv::SOLVER_SOURCE_UNSPECIFIED,
          "minor-2 AUTOMATIC fallback carries no heuristic source");
    check(es.actions(0).type() == pv::ACTION_TYPE_CHECK,
          "the fallback takes the legal check on the flop fixture");
    check(services.blueprintLookups == 1, "AUTOMATIC miss queried the blueprint once");
    check(services.resolveLookups == 1, "AUTOMATIC miss tried the resolver once (W4d)");

    // An explicit HEURISTIC request still runs the sourced cascade on the same
    // miss.
    pv::DecisionRequest heuristic = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    pv::Envelope h = send(envelopeFor(2, "w3-heuristic", heuristic), services);
    const pv::SolverMetadata& hm = h.decision_response().expanded_strategy().solver();
    check(hm.guarantee_level() != "operational_fallback" &&
              hm.source() != pv::SOLVER_SOURCE_UNSPECIFIED,
          "an explicit HEURISTIC request still runs the sourced cascade on a miss");

    // Minor-1 AUTOMATIC miss keeps its frozen heuristic fallthrough.
    pv::DecisionRequest minor1 = flopRequest(pv::SOLVER_MODE_AUTOMATIC);
    pv::Envelope m1 = send(envelopeFor(1, "w3-minor1", minor1), services);
    const pv::SolverMetadata& m1m = m1.decision_response().expanded_strategy().solver();
    check(m1m.source() != pv::SOLVER_SOURCE_UNSPECIFIED,
          "minor-1 AUTOMATIC miss still falls through to the heuristic engine");
  }

  // --- W4d: AUTOMATIC blueprint miss -> resolver Certified serves ----------
  // On a blueprint miss the AUTOMATIC path tries terminal-only resolving. A
  // certified resolve serves at certified_bound with the RESOLVING source,
  // exactly like the forced RESOLVING mode. The resolveRequest fixture is a
  // facing-all-in flop snapshot that the resolver reconstruction admits.
  {
    FakeServices services;
    services.actions = {{ActionType::Fold}, {ActionType::Call}};
    services.probabilities = {0.3, 0.7};
    services.outcome = V1ResolveOutcome::Certified;
    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_AUTOMATIC);
    pv::Envelope resp = send(envelopeFor(2, "w4d-auto-cert", request), services);
    check(resp.protocol_minor() == 2, "AUTOMATIC resolve echoes minor 2");
    const pv::SolverMetadata& s = resp.decision_response().expanded_strategy().solver();
    check(resp.decision_response().has_expanded_strategy(),
          "AUTOMATIC resolver certified serves an expanded strategy");
    check(s.source() == pv::SOLVER_SOURCE_RESOLVING,
          "AUTOMATIC resolver certified carries the RESOLVING source");
    check(s.guarantee_level() == "certified_bound",
          "AUTOMATIC resolver certified earns certified_bound");
    check(!s.has_guarantee(), "AUTOMATIC resolver row never sets field 10");
    check(s.has_artifact_sha256(), "AUTOMATIC resolver row keeps the field-9 digest");
    check(!s.cache_hit() && s.reason_code() == "resolving",
          "AUTOMATIC resolver certified is no cache hit and is reason-coded resolving");
    check(services.resolveLookups == 1, "AUTOMATIC resolver certified invoked the resolver once");

    // A certified floor passes on the AUTOMATIC resolver path.
    pv::DecisionRequest high = resolveRequest(pv::SOLVER_MODE_AUTOMATIC);
    high.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope served = send(envelopeFor(2, "w4d-auto-cert-floor", high), services);
    check(served.decision_response().has_expanded_strategy(),
          "AUTOMATIC resolver certified meets a certified floor");
  }

  // --- W4d: AUTOMATIC blueprint miss -> resolver DeadlineBlueprint ---------
  // A resolver that completes with a baseline row (but no certification)
  // serves at approximate with the BLUEPRINT source, exactly like the forced
  // RESOLVING mode's deadline baseline.
  {
    FakeServices services;
    services.actions = {{ActionType::Fold}, {ActionType::Call}};
    services.probabilities = {0.3, 0.7};
    services.outcome = V1ResolveOutcome::DeadlineBlueprint;
    pv::DecisionRequest request = resolveRequest(pv::SOLVER_MODE_AUTOMATIC);
    pv::Envelope resp = send(envelopeFor(2, "w4d-auto-deadline", request), services);
    check(resp.protocol_minor() == 2, "AUTOMATIC deadline baseline echoes minor 2");
    const pv::SolverMetadata& s = resp.decision_response().expanded_strategy().solver();
    check(resp.decision_response().has_expanded_strategy(),
          "AUTOMATIC resolver deadline baseline serves an expanded strategy");
    check(s.source() == pv::SOLVER_SOURCE_BLUEPRINT && s.cache_hit() &&
              s.guarantee_level() == "approximate" && !s.has_guarantee(),
          "AUTOMATIC resolver deadline baseline is BLUEPRINT/cache-hit/approximate");
    check(services.resolveLookups == 1, "AUTOMATIC deadline baseline invoked the resolver once");

    // A certified floor refuses the approximate baseline with code 9.
    pv::DecisionRequest high = resolveRequest(pv::SOLVER_MODE_AUTOMATIC);
    high.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    pv::Envelope refused = send(envelopeFor(2, "w4d-auto-deadline-floor", high), services);
    check(refused.decision_response().has_error() &&
              refused.decision_response().error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
          "AUTOMATIC resolver baseline below a certified floor is code 9");
  }

  // --- W4d: AUTOMATIC blueprint miss -> resolver not advertised -----------
  // When no resolver root is advertised the AUTOMATIC path skips the resolver
  // entirely and reaches the operational fallback without invoking it.
  {
    FakeServices services;
    services.miss = V1BlueprintMiss::OffTree;
    services.resolvingOn = false;
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_AUTOMATIC);
    pv::Envelope resp = send(envelopeFor(2, "w4d-auto-no-resolver", request), services);
    check(resp.protocol_minor() == 2, "AUTOMATIC no-resolver echoes minor 2");
    const pv::SolverMetadata& s = resp.decision_response().expanded_strategy().solver();
    check(s.guarantee_level() == "operational_fallback" &&
              s.source() == pv::SOLVER_SOURCE_UNSPECIFIED,
          "AUTOMATIC miss with no resolver is the operational fallback");
    check(services.resolveLookups == 0, "AUTOMATIC with an unadvertised resolver never invokes it");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 MINOR2 TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 MINOR2 TESTS PASSED");
  return 0;
}
