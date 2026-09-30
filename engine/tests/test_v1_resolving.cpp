// RFC 0005 Stage 9 minor-1 resolving protocol tests. The resolver is injected
// through the protobuf-free V1HostServices seam with deterministic fakes; no
// artifact or CFR run is involved. These cover source/guarantee discipline,
// deadline fallback, unsupported/digest-miss mapping, capabilities, the
// facing-all-in resolver-only reconstruction, and frozen minor-0 rejection.
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

// Facing-all-in flop snapshot: button p0 jams its last 40 after a 20/20
// preflop; hero p1 (BB) faces fold/call for 40, pot 80.
pv::DecisionRequest resolveRequest(pv::SolverMode mode) {
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
  state->set_hand_id("resolve-hand");
  state->set_decision_index(1);
  state->set_street(pv::STREET_FLOP);
  // Heads-up postflop the non-button (BB, p0) acts first and jams; the button
  // (SB, hero p1) acts last and faces the all-in.
  state->set_button_seat(1);
  state->set_hero_player_id("p1");

  pv::PlayerState* button = state->add_players();
  button->set_player_id("p0");
  button->set_seat(0);
  button->set_stack(0);
  button->set_street_committed(40);
  button->set_status(pv::PLAYER_STATUS_ALL_IN);
  pv::PlayerState* hero = state->add_players();
  hero->set_player_id("p1");
  hero->set_seat(1);
  hero->set_stack(40);
  hero->set_street_committed(0);
  hero->set_status(pv::PLAYER_STATUS_ACTIVE);

  *state->add_hero_hole_cards() = card(pv::RANK_TWO, pv::SUIT_HEARTS);
  *state->add_hero_hole_cards() = card(pv::RANK_THREE, pv::SUIT_HEARTS);
  *state->add_board() = card(pv::RANK_ACE, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_KING, pv::SUIT_SPADES);
  *state->add_board() = card(pv::RANK_QUEEN, pv::SUIT_SPADES);

  auto* sb = state->add_forced_contributions();
  sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  sb->set_player_id("p1");
  sb->set_amount(10);
  auto* bb = state->add_forced_contributions();
  bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  bb->set_player_id("p0");
  bb->set_amount(20);

  pv::ActionEvent* jam = state->add_action_history();
  jam->set_sequence(0);
  jam->set_street(pv::STREET_FLOP);
  jam->set_actor_player_id("p0");
  jam->set_action(pv::ACTION_TYPE_BET);
  jam->set_target_total(40);

  state->mutable_pot()->set_pot_total(80);
  state->mutable_pot()->set_main_pot(80);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
  state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  state->set_to_call(40);

  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(42);
  options->set_include_sampled_action(true);
  options->set_solver_mode(mode);
  return request;
}

class ResolveFake final : public V1HostServices {
 public:
  bool advertised = true;
  bool resolving = true;
  V1ResolveOutcome outcome = V1ResolveOutcome::Certified;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  std::string sha = std::string(64, 'c');
  std::vector<Action> actions{{ActionType::Fold}, {ActionType::Call}};
  std::vector<double> probabilities{0.3, 0.7};

  bool blueprintAdvertised() const noexcept override { return advertised; }
  bool resolvingAdvertised() const noexcept override { return advertised && resolving; }

  V1BlueprintResult blueprintHeroDecision(const bs::poker::GameState&,
                                          std::span<const bs::poker::PublicAction>,
                                          const std::array<int, 2>&,
                                          std::string_view) const noexcept override {
    V1BlueprintResult result;
    result.hit = advertised;
    result.miss = advertised ? V1BlueprintMiss::None : V1BlueprintMiss::RootNotSupported;
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
    V1ResolveResult result;
    result.outcome = outcome;
    result.miss = miss;
    if (outcome == V1ResolveOutcome::Certified || outcome == V1ResolveOutcome::DeadlineBlueprint) {
      result.row.size = actions.size();
      result.row.actions = actions.data();
      result.row.probabilities = probabilities.data();
      result.row.artifact_sha256 = sha;
    }
    return result;
  }
};

std::string envelopeFor(uint32_t minor, pv::DecisionRequest request) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id("resolve-id");
  *envelope.mutable_decision_request() = std::move(request);
  return envelope.SerializeAsString();
}

std::string capsEnvelope(uint32_t minor) {
  pv::Envelope envelope;
  envelope.set_protocol_minor(minor);
  envelope.set_request_id("caps-id");
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

const pv::ExpandedStrategy& expanded(const pv::Envelope& response) {
  return response.decision_response().expanded_strategy();
}

}  // namespace

int main() {
  // Capabilities: minor 1 advertises RESOLVING only with a live resolver.
  {
    ResolveFake services;
    pv::Envelope caps = send(capsEnvelope(1), services);
    bool sees_resolving = false;
    for (int i = 0; i < caps.get_capabilities_response().solver_modes_size(); ++i)
      sees_resolving |=
          caps.get_capabilities_response().solver_modes(i) == pv::SOLVER_MODE_RESOLVING;
    check(sees_resolving, "minor-1 caps advertise resolving when live");

    services.resolving = false;
    caps = send(capsEnvelope(1), services);
    for (int i = 0; i < caps.get_capabilities_response().solver_modes_size(); ++i)
      check(caps.get_capabilities_response().solver_modes(i) != pv::SOLVER_MODE_RESOLVING,
            "minor-1 caps omit resolving without a resolver");

    caps = send(capsEnvelope(0), ResolveFake{});
    for (int i = 0; i < caps.get_capabilities_response().solver_modes_size(); ++i)
      check(caps.get_capabilities_response().solver_modes(i) != pv::SOLVER_MODE_RESOLVING &&
                caps.get_capabilities_response().solver_modes(i) != pv::SOLVER_MODE_BLUEPRINT,
            "minor-0 caps never advertise new modes");
  }

  // Certified hit: source RESOLVING + modeled_exact_bound + digest.
  {
    ResolveFake services;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(!response.decision_response().has_error(), "certified resolve is not an error");
    const pv::ExpandedStrategy& strategy = expanded(response);
    check(strategy.solver().source() == pv::SOLVER_SOURCE_RESOLVING, "source is RESOLVING(7)");
    check(strategy.solver().guarantee() == "modeled_exact_bound", "guarantee modeled_exact_bound");
    check(strategy.solver().artifact_sha256() == services.sha, "certified digest is pinned");
    check(strategy.actions_size() == 2, "full fold/call distribution");
    check(strategy.has_selected_action(), "sampled action present");
  }

  // Deadline with a complete validated baseline row.
  {
    ResolveFake services;
    services.outcome = V1ResolveOutcome::DeadlineBlueprint;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(!response.decision_response().has_error(), "deadline baseline is not an error");
    const pv::ExpandedStrategy& strategy = expanded(response);
    check(strategy.solver().source() == pv::SOLVER_SOURCE_BLUEPRINT, "baseline source BLUEPRINT");
    check(strategy.solver().guarantee() == "baseline", "baseline guarantee label");
  }

  // Deadline without a baseline.
  {
    ResolveFake services;
    services.outcome = V1ResolveOutcome::DeadlineExceeded;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(response.decision_response().has_error(), "deadline is an error");
    check(response.decision_response().error().code() == pv::ERROR_CODE_DEADLINE_EXCEEDED,
          "forced resolve without baseline is DEADLINE_EXCEEDED");
  }

  // Unsupported node (terminal-only failure / off-tree / digest mismatch).
  {
    ResolveFake services;
    services.outcome = V1ResolveOutcome::Unsupported;
    services.miss = V1BlueprintMiss::OffTree;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(response.decision_response().has_error(), "unsupported resolve is an error");
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "unsupported resolve is UNSUPPORTED_FEATURE");
  }

  // Forced resolving without any advertised resolver.
  {
    ResolveFake services;
    services.resolving = false;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "forced resolving without coverage is unsupported");
  }

  // The facing-all-in node is admitted for resolving but REJECTED for the
  // ordinary BLUEPRINT gate.
  {
    ResolveFake services;
    pv::Envelope response =
        send(envelopeFor(1, resolveRequest(pv::SOLVER_MODE_BLUEPRINT)), services);
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "blueprint gate still rejects a facing-all-in node");
  }

  // Minor 0 forced resolving is rejected before any service call.
  {
    ResolveFake services;
    pv::Envelope response =
        send(envelopeFor(0, resolveRequest(pv::SOLVER_MODE_RESOLVING)), services);
    check(response.protocol_minor() == 0, "minor 0 echoes minor 0");
    check(response.decision_response().has_error() &&
              response.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
          "minor-0 resolving is UNSUPPORTED_FEATURE");
  }

  if (failures != 0) {
    std::fprintf(stderr, "test_v1_resolving had %d failures\n", failures);
    return 1;
  }
  std::printf("test_v1_resolving PASS\n");
  return 0;
}
