// RFC 0002 Stage 8 minor-1 host tests: minor negotiation and capability
// filtering, the forced-mode matrix, full 1..32 distribution mapping, the
// exact-seed sampler, selected-action membership, coverage-miss errors, and
// minor-0 byte/call-path identity. Resident lookup is injected through the
// protobuf-free V1HostServices seam with deterministic fakes; no artifacts or
// training are involved.
#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/prng.hpp>
#include <bs/v1_protocol.hpp>
#include <cmath>
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

// Deterministic seam fake: returns a scripted row or miss and records whether
// the resident path was touched.
class FakeServices final : public V1HostServices {
 public:
  bool advertised = true;
  mutable bool touched = false;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  std::vector<Action> actions;
  std::vector<double> probabilities;
  std::string sha = std::string(64, 'a');

  bool blueprintAdvertised() const noexcept override { return advertised; }

  V1BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                          std::span<const bs::poker::PublicAction>,
                                          const std::array<int, 2>&,
                                          std::string_view) const noexcept override {
    touched = true;
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
};

std::string envelopeFor(uint32_t minor, const std::string& id, pv::DecisionRequest request) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = std::move(request);
  return envelope.SerializeAsString();
}

std::string capabilitiesEnvelope(uint32_t minor, const std::string& id) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id(id);
  envelope.mutable_get_capabilities_request();
  return envelope.SerializeAsString();
}

pv::Envelope respond(const std::string& frame, const V1HostServices& services) {
  const EnvelopeResult result = bs::v1::handleEnvelope(frame, services);
  check(result.outcome == EnvelopeOutcome::Respond, "host responds");
  pv::Envelope response;
  check(response.ParseFromString(result.response), "response parses");
  return response;
}

// Valid heads-up flop snapshot: button on seat 0, hero is BB (seat 1), no
// postflop actions, check/bet(100..1980) legal.
pv::DecisionRequest flopRequest(pv::SolverMode mode, uint64_t seed = 42, bool sampled = true) {
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
  state->set_hand_id("flop-hand");
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
  *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pv::RANK_THREE, pv::SUIT_HEARTS);
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
  options->set_seed(seed);
  options->set_include_sampled_action(sampled);
  options->set_solver_mode(mode);
  return request;
}

// Heads-up preflop snapshot facing a BTN open: BB holds AsKs, villain opened
// to 60, hero faces 40 more.
pv::DecisionRequest preflopRequest(pv::SolverMode mode) {
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
  state->set_hand_id("preflop-hand");
  state->set_decision_index(1);
  state->set_street(pv::STREET_PREFLOP);
  state->set_button_seat(1);
  state->set_hero_player_id("hero");
  pv::PlayerState* villain = state->add_players();
  villain->set_player_id("villain");
  villain->set_seat(1);
  villain->set_stack(1940);
  villain->set_street_committed(60);
  villain->set_status(pv::PLAYER_STATUS_ACTIVE);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("hero");
  hero->set_seat(0);
  hero->set_stack(1980);
  hero->set_street_committed(20);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);
  *state->add_hero_hole_cards() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_hero_hole_cards() = card(pv::RANK_KING, pv::SUIT_SPADES);
  state->mutable_pot()->set_pot_total(70);
  state->mutable_pot()->set_main_pot(70);
  pv::ActionEvent* open = state->add_action_history();
  open->set_sequence(0);
  open->set_street(pv::STREET_PREFLOP);
  open->set_actor_player_id("villain");
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

// Independent copy of the pinned sampler convention (see bs/prng.hpp and
// v1_response_mapper.cpp): domain-separated SplitMix64, one draw, top 53 bits,
// first prefix bucket strictly greater than the point.
std::size_t expectedBucket(const std::vector<double>& probabilities, uint64_t seed) {
  uint64_t state = seed ^ bs::SplitMix64::kProtocolSamplerDomain;
  auto next = [&]() -> uint64_t {
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  };
  const double point = static_cast<double>(next() >> 11) * 0x1.0p-53;
  double prefix = 0.0;
  for (std::size_t i = 0; i < probabilities.size(); ++i) {
    prefix += probabilities[i];
    if (prefix > point)
      return i;
  }
  return probabilities.size();
}

std::vector<unsigned char> decodeHex(const std::string& text) {
  std::vector<unsigned char> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    bytes.push_back(static_cast<unsigned char>((nibble(text[i]) << 4) | nibble(text[i + 1])));
  }
  return bytes;
}

}  // namespace

int main() {
  // --- Negotiation ----------------------------------------------------------
  {
    FakeServices services;
    pv::Envelope response =
        respond(capabilitiesEnvelope(0, "capabilities-request-vector"), services);
    check(response.protocol_minor() == 0, "minor-0 caps echo minor 0");
    const pv::GetCapabilitiesResponse& caps = response.get_capabilities_response();
    check(caps.supported_protocol_minors_size() == 1 && caps.supported_protocol_minors(0) == 0,
          "minor-0 caps list minor 0 only");
    bool hasBlueprint = false;
    for (int i = 0; i < caps.solver_modes_size(); ++i)
      hasBlueprint |= caps.solver_modes(i) == pv::SOLVER_MODE_BLUEPRINT ||
                      caps.solver_modes(i) == pv::SOLVER_MODE_RESOLVING;
    check(!hasBlueprint, "minor-0 caps omit BLUEPRINT/RESOLVING even with roots loaded");
    check(!services.touched, "capabilities never perform a lookup");

    // Exact Stage-7 bytes for the minor-0 capability response.
    static const char* kStage7Hex =
        "121b6361706162696c69746965732d726571756573742d766563746f725a6a0a010012"
        "16626967736861726b2d656e67696e652d76312e302e301a01012201012802300a3a04"
        "01020304420501020304054801520201025a037461675a036c61675a0e7374617469"
        "6f6e2d68756e746572600368037002780180010188010190018080409801c0a907";
    const std::vector<unsigned char> expected = decodeHex(kStage7Hex);
    const std::string actual = response.SerializeAsString();
    // Compare as unsigned bytes: std::string's char is signed on this target,
    // so a plain std::equal against an unsigned vector miscompares bytes >= 0x80.
    const bool sizeMatch = actual.size() == expected.size();
    bool bytesMatch = sizeMatch;
    if (bytesMatch)
      for (std::size_t j = 0; j < expected.size(); ++j)
        bytesMatch &= expected[j] == static_cast<unsigned char>(actual[j]);
    check(sizeMatch && bytesMatch, "minor-0 capabilities are byte-identical to Stage 7");
  }
  {
    FakeServices services;
    services.advertised = true;
    pv::Envelope response = respond(capabilitiesEnvelope(1, "cap1"), services);
    check(response.protocol_minor() == 1, "minor-1 caps echo minor 1");
    const pv::GetCapabilitiesResponse& caps = response.get_capabilities_response();
    check(caps.supported_protocol_minors_size() == 2 && caps.supported_protocol_minors(0) == 0 &&
              caps.supported_protocol_minors(1) == 1,
          "minor-1 caps list 0 and 1");
    bool hasBlueprint = false;
    bool hasResolving = false;
    for (int i = 0; i < caps.solver_modes_size(); ++i) {
      hasBlueprint |= caps.solver_modes(i) == pv::SOLVER_MODE_BLUEPRINT;
      hasResolving |= caps.solver_modes(i) == pv::SOLVER_MODE_RESOLVING;
    }
    check(hasBlueprint, "BLUEPRINT advertised with a resident root");
    check(!hasResolving, "RESOLVING is never advertised");
  }
  {
    FakeServices services;
    services.advertised = false;
    pv::Envelope response = respond(capabilitiesEnvelope(1, "cap2"), services);
    const pv::GetCapabilitiesResponse& caps = response.get_capabilities_response();
    bool hasBlueprint = false;
    for (int i = 0; i < caps.solver_modes_size(); ++i)
      hasBlueprint |= caps.solver_modes(i) == pv::SOLVER_MODE_BLUEPRINT;
    check(!hasBlueprint, "no resident root means BLUEPRINT is not advertised");
  }
  {
    FakeServices services;
    pv::Envelope response = respond(capabilitiesEnvelope(3, "cap-bad"), services);
    check(response.protocol_minor() == 0, "rejected minor echoes 0");
    check(response.payload_case() == pv::Envelope::kDecisionResponse &&
              response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_PROTOCOL,
          "minor 3 is UNSUPPORTED_PROTOCOL");
  }
  {
    // RFC 0008 stage 5: minor 2 is now negotiated (covered fully in
    // test_v1_minor2); the old "minor 2 rejected" pin moved to minor 3 above.
    // minimum_guarantee remains a minor-2-only request field on this minor.
    FakeServices services;
    pv::DecisionRequest request = flopRequest(pv::SOLVER_MODE_HEURISTIC);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    pv::Envelope response = respond(envelopeFor(1, "m1floor", request), services);
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "minimum_guarantee on minor 1 is UNSUPPORTED_FEATURE");
  }

  // --- Forced BLUEPRINT: full distribution + exact-seed sampling ------------
  {
    FakeServices services;
    services.actions = {
        {ActionType::Check},     {ActionType::Bet, 100}, {ActionType::Bet, 200},
        {ActionType::Bet, 400},  {ActionType::Bet, 800}, {ActionType::Bet, 1200},
        {ActionType::Bet, 1980},
    };
    services.probabilities = {0.1, 0.15, 0.15, 0.15, 0.15, 0.15, 0.15};
    pv::Envelope response =
        respond(envelopeFor(1, "bp-hit", flopRequest(pv::SOLVER_MODE_BLUEPRINT, 42)), services);
    check(response.protocol_minor() == 1, "blueprint response on minor 1");
    check(response.decision_response().has_expanded_strategy(), "hit returns expanded strategy");
    check(!response.decision_response().has_strategy(), "hit never returns the v1.0 strategy");
    const pv::ExpandedStrategy& expanded = response.decision_response().expanded_strategy();
    check(expanded.actions_size() == 7, "seven-action distribution");
    double sum = 0;
    for (int i = 0; i < expanded.actions_size(); ++i) {
      check(expanded.actions(i).probability() == services.probabilities[i],
            "verbatim probability " + std::to_string(i));
      sum += expanded.actions(i).probability();
    }
    check(std::abs(sum - 1.0) <= 1e-12, "probabilities sum to one");
    check(expanded.actions(0).type() == pv::ACTION_TYPE_CHECK &&
              !expanded.actions(0).has_target_total(),
          "check carries no target");
    check(expanded.actions(6).target_total() == 1980 && expanded.actions(6).all_in(),
          "jam row is all-in");
    check(expanded.actions(1).target_total() == 100 && !expanded.actions(1).all_in(),
          "non-jam bet is not all-in");
    const pv::SolverMetadata& solver = expanded.solver();
    check(solver.source() == pv::SOLVER_SOURCE_BLUEPRINT, "source BLUEPRINT(6)");
    check(solver.artifact_sha256() == services.sha, "artifact digest echoed");
    check(solver.guarantee() == "uncertified", "complete blueprint is uncertified");
    check(solver.cache_hit(), "resident lookup is a cache hit");

    const std::size_t expected = expectedBucket(services.probabilities, 42);
    check(expanded.has_selected_action(), "sampled action present when requested");
    check(expanded.selected_action().type() == pv::ACTION_TYPE_BET, "sampled bucket kind");
    check(expanded.selected_action().target_total() == services.actions[expected].target_total,
          "exact-seed sampled target");
    // Hard-pinned convention: for {0.1,0.15,...,0.15} seed 42 the domain
    // separated draw lands at 0.44381..., selecting bucket 3 (bet 400).
    check(expected == 3, "seed 42 pins bucket 3");
    check(expanded.selected_action().target_total() == 400, "seed 42 samples bet 400");
    // Seeds 0 and 1 pin buckets 6 and 4, guarding the tie rule and the domain
    // constant against accidental changes to the sampler.
    check(expectedBucket(services.probabilities, 0) == 6, "seed 0 pins bucket 6");
    check(expectedBucket(services.probabilities, 1) == 4, "seed 1 pins bucket 4");
  }
  {
    // Lookup itself must not depend on the seed: changing the seed changes only
    // the sampled bucket, never the distribution.
    FakeServices a;
    a.actions = {{ActionType::Check}, {ActionType::Bet, 100}, {ActionType::Bet, 200}};
    a.probabilities = {0.2, 0.3, 0.5};
    FakeServices b = a;
    pv::Envelope ra = respond(envelopeFor(1, "s1", flopRequest(pv::SOLVER_MODE_BLUEPRINT, 1)), a);
    pv::Envelope rb =
        respond(envelopeFor(1, "s2", flopRequest(pv::SOLVER_MODE_BLUEPRINT, 999999)), b);
    const pv::ExpandedStrategy& ea = ra.decision_response().expanded_strategy();
    const pv::ExpandedStrategy& eb = rb.decision_response().expanded_strategy();
    bool same = ea.actions_size() == eb.actions_size();
    for (int i = 0; same && i < ea.actions_size(); ++i)
      same &= ea.actions(i).probability() == eb.actions(i).probability() &&
              ea.actions(i).target_total() == eb.actions(i).target_total();
    check(same, "distribution is seed-independent");
    check(ea.selected_action().target_total() != eb.selected_action().target_total() ||
              expectedBucket(a.probabilities, 1) == expectedBucket(b.probabilities, 999999),
          "seeds select the pinned buckets");
  }
  {
    // No selected action unless requested.
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 100}};
    services.probabilities = {0.5, 0.5};
    pv::Envelope response = respond(
        envelopeFor(1, "bp-nosample", flopRequest(pv::SOLVER_MODE_BLUEPRINT, 42, false)), services);
    check(!response.decision_response().expanded_strategy().has_selected_action(),
          "sampled action omitted when not requested");
  }
  {
    // Zero-probability buckets are never sampled.
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 100}, {ActionType::Bet, 200}};
    services.probabilities = {0.0, 0.5, 0.5};
    for (uint64_t seed = 0; seed < 32; ++seed) {
      pv::Envelope response = respond(
          envelopeFor(1, "z" + std::to_string(seed), flopRequest(pv::SOLVER_MODE_BLUEPRINT, seed)),
          services);
      check(response.decision_response().expanded_strategy().selected_action().type() ==
                pv::ACTION_TYPE_BET,
            "zero-probability check bucket never selected for seed " + std::to_string(seed));
    }
  }

  // --- Coverage misses ------------------------------------------------------
  {
    FakeServices services;
    services.miss = V1BlueprintMiss::OffTreeAmount;
    pv::Envelope response =
        respond(envelopeFor(1, "bp-miss", flopRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    const pv::EngineError& error = response.decision_response().error();
    check(response.decision_response().has_error() &&
              error.code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "resident miss is UNSUPPORTED_FEATURE");
    check(!error.retryable(), "coverage miss is not retryable");
    check(!response.decision_response().has_strategy() &&
              !response.decision_response().has_expanded_strategy(),
          "a miss never returns a fold strategy");
    check(error.message().find("off-tree-amount") != std::string::npos,
          "miss reason is diagnostic");
  }
  {
    FakeServices services;
    services.advertised = false;
    pv::Envelope response =
        respond(envelopeFor(1, "bp-noroot", flopRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced BLUEPRINT without an advertised root is unsupported");
    check(!services.touched, "no lookup happens without an advertised root");
  }
  {
    // Row action outside the client's legal window is an OffTreeAmount miss,
    // never a clamp.
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 50}};  // min is 100
    services.probabilities = {0.5, 0.5};
    pv::Envelope response =
        respond(envelopeFor(1, "bp-window", flopRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "off-window abstract amount is UNSUPPORTED_FEATURE");
  }
  {
    // Preflop forced BLUEPRINT: reconstruction coverage miss.
    FakeServices services;
    pv::Envelope response =
        respond(envelopeFor(1, "bp-pre", preflopRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "preflop blueprint is a coverage miss");
    check(!services.touched, "the resident set is never consulted for preflop");
  }

  // --- RESOLVING is always rejected ----------------------------------------
  {
    FakeServices services;
    pv::Envelope response =
        respond(envelopeFor(1, "resolve", flopRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "RESOLVING (7) is UNSUPPORTED_FEATURE on minor 1");
    check(!services.touched, "resolving never reaches resident code");
  }

  // --- Minor-0 isolation ----------------------------------------------------
  {
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 100}};
    services.probabilities = {0.5, 0.5};
    pv::Envelope response =
        respond(envelopeFor(0, "m0bp", flopRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "BLUEPRINT on minor 0 is UNSUPPORTED_FEATURE");
    check(!services.touched, "minor 0 never reaches resident code");
    check(!response.decision_response().has_expanded_strategy(),
          "minor 0 never sees expanded_strategy");
  }
  {
    // A valid minor-0 AUTOMATIC decision follows the exact Stage-7 path even
    // with resident roots loaded.
    FakeServices services;
    pv::Envelope response =
        respond(envelopeFor(0, "m0auto", flopRequest(pv::SOLVER_MODE_AUTOMATIC)), services);
    check(response.decision_response().has_strategy(), "minor 0 returns Strategy");
    check(!response.decision_response().has_expanded_strategy(),
          "minor 0 never returns ExpandedStrategy");
    check(response.decision_response().strategy().actions_size() == 1 &&
              response.decision_response().strategy().actions(0).probability() == 1.0,
          "minor 0 distribution stays degenerate");
    check(!services.touched, "minor 0 heuristic never consults resident services");
  }

  // --- Minor-1 AUTOMATIC/HEURISTIC fallback --------------------------------
  {
    // Preflop cannot reconstruct, so AUTOMATIC falls back to the heuristic and
    // reports its REAL source with no blueprint fields.
    FakeServices services;
    pv::Envelope response =
        respond(envelopeFor(1, "m1auto-pre", preflopRequest(pv::SOLVER_MODE_AUTOMATIC)), services);
    check(response.decision_response().has_expanded_strategy(),
          "minor-1 heuristic fallback is expanded");
    const pv::ExpandedStrategy& expanded = response.decision_response().expanded_strategy();
    check(expanded.actions_size() == 1 && expanded.actions(0).probability() == 1.0,
          "heuristic fallback stays degenerate");
    check(expanded.solver().source() != pv::SOLVER_SOURCE_BLUEPRINT,
          "heuristic source is never relabeled BLUEPRINT");
    check(!expanded.solver().has_artifact_sha256(), "heuristic fallback carries no digest");
    check(!expanded.solver().has_guarantee(), "heuristic fallback carries no guarantee");
    check(expanded.actions(0).type() == pv::ACTION_TYPE_RAISE &&
              expanded.actions(0).target_total() == 120,
          "AsKs facing an open reraises 120");
    check(expanded.selected_action().type() == pv::ACTION_TYPE_RAISE &&
              expanded.selected_action().target_total() == 120,
          "sampled action executes the heuristic decision");
  }
  {
    // Explicit HEURISTIC on a flop bypasses resident entirely.
    FakeServices services;
    services.miss = V1BlueprintMiss::OffTree;  // must be ignored
    pv::Envelope response =
        respond(envelopeFor(1, "m1heur", flopRequest(pv::SOLVER_MODE_HEURISTIC)), services);
    check(response.decision_response().has_expanded_strategy(),
          "minor-1 HEURISTIC returns expanded");
    check(response.decision_response().expanded_strategy().solver().source() ==
              pv::SOLVER_SOURCE_POSTFLOP_HEURISTIC,
          "postflop heuristic real source");
    check(!services.touched, "HEURISTIC never consults the resident set");
  }
  {
    // Minor-1 AUTOMATIC resident hit is used as-is.
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 100}};
    services.probabilities = {0.7, 0.3};
    pv::Envelope response =
        respond(envelopeFor(1, "m1autohit", flopRequest(pv::SOLVER_MODE_AUTOMATIC)), services);
    check(response.decision_response().has_expanded_strategy(), "automatic hit is expanded");
    check(response.decision_response().expanded_strategy().solver().source() ==
              pv::SOLVER_SOURCE_BLUEPRINT,
          "automatic hit reports BLUEPRINT");
    check(response.decision_response().expanded_strategy().actions_size() == 2,
          "automatic hit carries the full distribution");
  }

  // --- Sampler tolerance: rows passing the 1e-12 sum check must always
  // produce a deterministic positive bucket, including sums on either side of
  // one, and the all-zero tail must never be selected.
  for (const double tail : {1.0 + 1e-13, 1.0 - 1e-13}) {
    for (uint64_t seed = 0; seed < 64; ++seed) {
      FakeServices services;
      // Two positive buckets split the (near-unit) mass plus a zero tail.
      const double half = tail / 2.0;
      services.actions = {{ActionType::Check}, {ActionType::Bet, 100}, {ActionType::Bet, 200}};
      services.probabilities = {half, half, 0.0};
      pv::Envelope response =
          respond(envelopeFor(1, "tol" + std::to_string(seed) + "-" + (tail > 1.0 ? "hi" : "lo"),
                              flopRequest(pv::SOLVER_MODE_BLUEPRINT, seed)),
                  services);
      const pv::DecisionResponse& decision = response.decision_response();
      check(decision.has_expanded_strategy(), "near-unit distribution is accepted and expanded");
      if (decision.has_expanded_strategy()) {
        const pv::SelectedAction& selected = decision.expanded_strategy().selected_action();
        check(selected.type() == pv::ACTION_TYPE_CHECK ||
                  (selected.type() == pv::ACTION_TYPE_BET &&
                   (selected.target_total() == 100 || selected.target_total() == 200)),
              "near-unit sampler selects a live bucket");
        check(!(selected.type() == pv::ACTION_TYPE_BET && selected.target_total() == 200 &&
                services.probabilities[2] == 0.0),
              "zero-probability tail bucket never selected");
      }
    }
  }

  // --- Direct mapper invariants ---------------------------------------------
  {
    FakeServices services;
    services.actions = {{ActionType::Check}, {ActionType::Bet, 100}};
    services.probabilities = {0.4, 0.7};  // sums to 1.1
    V1BlueprintRow row{2, services.actions.data(), services.probabilities.data(), services.sha,
                       {}};
    check(!bs::v1::blueprintRowIsLegal(flopRequest(pv::SOLVER_MODE_BLUEPRINT), row),
          "non-unit distribution rejected");
    services.probabilities = {0.5, -0.5};
    check(!bs::v1::blueprintRowIsLegal(flopRequest(pv::SOLVER_MODE_BLUEPRINT), row),
          "negative probability rejected");
    std::vector<Action> many(33, Action{ActionType::Check});
    std::vector<double> flat(33, 1.0 / 33.0);
    V1BlueprintRow big{33, many.data(), flat.data(), services.sha, {}};
    check(!bs::v1::blueprintRowIsLegal(flopRequest(pv::SOLVER_MODE_BLUEPRINT), big),
          "distribution above 32 actions rejected");
    V1BlueprintRow empty{0, nullptr, nullptr, services.sha, {}};
    check(!bs::v1::blueprintRowIsLegal(flopRequest(pv::SOLVER_MODE_BLUEPRINT), empty),
          "empty distribution rejected");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 MINOR1 TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 MINOR1 TESTS PASSED");
  return 0;
}
