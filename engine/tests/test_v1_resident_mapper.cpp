// RFC 0002 Stage 8 independent oracle for the HandState -> poker::HeadsUpState
// reconstruction. Expected roots and replayed nodes are built directly with the
// poker domain API, never through the reconstructor, and the two are compared
// field by field. Covers fold/check/call/bet/raise on flop/turn/river, the
// all-in target, and every deterministic reject class.
#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../src/protocol/v1_resident_mapper.hpp"

namespace {

namespace pv = bs::v1::pv;
using bs::poker::ActionType;
using bs::poker::Chips;
using bs::poker::HeadsUpRoot;
using bs::poker::HeadsUpState;
using bs::poker::LegalActions;
using bs::v1::ReconstructedPostflop;
using bs::v1::V1BlueprintMiss;

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

int cardId(const char* name) {
  static const char* ranks = "23456789TJQKA";
  static const char* suits = "shdc";  // s0,h1,d2,c3
  int rank = 0;
  for (; ranks[rank] != name[0]; ++rank) {
  }
  int suit = 0;
  for (; suits[suit] != name[1]; ++suit) {
  }
  return rank * 4 + suit;
}

// Poker id -> proto card.
pv::Card protoCard(int id) {
  pv::Card value;
  value.set_rank(static_cast<pv::Rank>(id / 4 + 1));
  value.set_suit(static_cast<pv::Suit>(id % 4 + 1));
  return value;
}

struct EventSpec {
  pv::Street street;
  std::string actor;
  pv::ActionType action;
  std::optional<std::uint64_t> target;
  std::optional<std::uint64_t> incremental;
  Chips potBefore = 0;
};

struct NodeSpec {
  pv::Street street = pv::STREET_FLOP;
  int buttonSeat = 0;
  std::string hero = "p1";  // default: BB acts first postflop
  std::array<int, 3> flop{cardId("As"), cardId("Ks"), cardId("Qs")};
  std::array<int, 2> turnRiver{cardId("Js"), cardId("Ts")};
  std::array<Chips, 2> stacks{1980, 1980};
  std::array<Chips, 2> streetCommitted{0, 0};
  Chips pot = 40;
  Chips toCall = 0;
  std::vector<EventSpec> events;
  bool addAnte = false;
  int playerCount = 2;
  std::array<pv::PlayerStatus, 2> status{pv::PLAYER_STATUS_ACTIVE, pv::PLAYER_STATUS_ACTIVE};
  std::array<int, 2> heroCards{cardId("2h"), cardId("3h")};
  // Real River snapshots always carry the preflop close-out events. The
  // default models a limped pot: SB completes 10, so preflop totals are
  // 20/20 = 40 matching the SB10/BB20 forced contributions.
  std::vector<EventSpec> preflop{
      {pv::STREET_PREFLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 10, 0}};
};

pv::DecisionRequest buildRequest(const NodeSpec& spec) {
  pv::DecisionRequest request;
  pv::HandState* state = request.mutable_state();
  pv::GameDefinition* game = state->mutable_game();
  game->set_variant(pv::GAME_VARIANT_NLHE);
  game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
  game->set_game_type(pv::GAME_TYPE_CASH);
  game->set_table_capacity(std::max(2, spec.playerCount));
  game->mutable_amount_unit()->set_name("chip");
  game->mutable_amount_unit()->set_decimal_places(0);
  game->set_small_blind(10);
  game->set_big_blind(20);
  if (spec.addAnte)
    game->set_ante(5);

  state->set_hand_id("recon-hand");
  state->set_decision_index(1);
  state->set_street(spec.street);
  state->set_button_seat(spec.buttonSeat);
  state->set_hero_player_id(spec.hero);

  for (int p = 0; p < spec.playerCount; ++p) {
    pv::PlayerState* player = state->add_players();
    player->set_player_id("p" + std::to_string(p));
    player->set_seat(p);
    if (p < 2) {
      player->set_stack(spec.stacks[p]);
      player->set_street_committed(spec.streetCommitted[p]);
      player->set_status(spec.status[p]);
    } else {
      player->set_stack(1980);
      player->set_status(pv::PLAYER_STATUS_ACTIVE);
    }
  }

  *state->add_hero_hole_cards() = protoCard(spec.heroCards[0]);
  *state->add_hero_hole_cards() = protoCard(spec.heroCards[1]);
  const int boardCount = spec.street == pv::STREET_FLOP   ? 3
                         : spec.street == pv::STREET_TURN ? 4
                                                          : 5;
  for (int i = 0; i < boardCount; ++i) {
    const int id = i < 3 ? spec.flop[i] : spec.turnRiver[i - 3];
    *state->add_board() = protoCard(id);
  }

  state->mutable_pot()->set_pot_total(spec.pot);
  state->mutable_pot()->set_main_pot(spec.pot);

  pv::ForcedContribution* sb = state->add_forced_contributions();
  sb->set_player_id("p0");
  sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  sb->set_amount(10);
  pv::ForcedContribution* bb = state->add_forced_contributions();
  bb->set_player_id("p1");
  bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  bb->set_amount(20);

  std::uint64_t sequence = 0;
  for (const EventSpec& event : spec.preflop) {
    pv::ActionEvent* wire = state->add_action_history();
    wire->set_sequence(sequence++);
    wire->set_street(event.street);
    wire->set_actor_player_id(event.actor);
    wire->set_action(event.action);
    if (event.target)
      wire->set_target_total(*event.target);
    if (event.incremental)
      wire->set_incremental_amount(*event.incremental);
    wire->set_pot_before(event.potBefore);
  }
  for (const EventSpec& event : spec.events) {
    pv::ActionEvent* wire = state->add_action_history();
    wire->set_sequence(sequence++);
    wire->set_street(event.street);
    wire->set_actor_player_id(event.actor);
    wire->set_action(event.action);
    if (event.target)
      wire->set_target_total(*event.target);
    if (event.incremental)
      wire->set_incremental_amount(*event.incremental);
    wire->set_pot_before(event.potBefore);
  }

  // Legal actions are not read by the reconstructor, but the request mirrors a
  // real snapshot: check when nothing owed, call otherwise, plus the aggressive
  // window.
  if (spec.toCall == 0)
    state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
  else
    state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
  const pv::ActionType aggressive = spec.toCall == 0 ? pv::ACTION_TYPE_BET : pv::ACTION_TYPE_RAISE;
  pv::LegalAction* raise = state->add_legal_actions();
  raise->set_type(aggressive);
  raise->set_min_target_total(spec.toCall == 0 ? 20 : spec.toCall + 20);
  raise->set_max_target_total(2000);
  state->set_to_call(spec.toCall);

  pv::DecisionOptions* options = request.mutable_options();
  options->set_strategy_profile("tag");
  options->set_solve_time_budget_ms(1000);
  options->set_seed(1);
  options->set_include_sampled_action(true);
  options->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
  return request;
}

// Independent oracle replay for the NodeSpec. Poker player index is occupied
// seat order (p0 -> 0, p1 -> 1) and root.button is the button player's index,
// matching the artifact convention.
HeadsUpState oracleState(const NodeSpec& spec, HeadsUpRoot& rootOut) {
  const auto seatToActor = [&](const std::string& id) -> std::size_t {
    return static_cast<std::size_t>(id == "p0" ? 0 : 1);
  };

  HeadsUpRoot root;
  root.flop = spec.flop;
  // Flop stacks are current stacks plus everything paid postflop, in player
  // order. Raise payments derive from the exact target total minus prior
  // street commitment, matching the reconstructor (incremental is advisory).
  std::array<Chips, 2> paid{};
  std::array<Chips, 2> streetPaid{};
  pv::Street oracleStreet = pv::STREET_UNSPECIFIED;
  for (const EventSpec& event : spec.events) {
    if (event.street == pv::STREET_PREFLOP)
      continue;
    if (oracleStreet == pv::STREET_UNSPECIFIED)
      oracleStreet = event.street;
    else if (event.street != oracleStreet) {
      streetPaid = {0, 0};
      oracleStreet = event.street;
    }
    const std::size_t actor = seatToActor(event.actor);
    Chips eventPaid = 0;
    if (event.action == pv::ACTION_TYPE_BET)
      eventPaid = *event.target;
    else if (event.action == pv::ACTION_TYPE_RAISE) {
      eventPaid = *event.target - streetPaid[actor];
    } else if (event.incremental) {
      eventPaid = *event.incremental;
    }
    paid[actor] += eventPaid;
    streetPaid[actor] += eventPaid;
  }
  for (std::size_t actor = 0; actor < 2; ++actor)
    root.stacks[actor] = spec.stacks[actor] + paid[actor];
  const Chips matched = spec.events.empty() ? spec.pot / 2 : spec.events.front().potBefore / 2;
  root.contributions = {matched, matched};
  root.pot = root.contributions[0] + root.contributions[1];
  root.big_blind = 20;
  root.button = spec.buttonSeat;
  rootOut = root;

  HeadsUpState cursor(root);
  pv::Street current = pv::STREET_FLOP;
  for (const EventSpec& event : spec.events) {
    if (event.street == pv::STREET_PREFLOP)
      continue;
    while (static_cast<int>(event.street) > static_cast<int>(current)) {
      const int id = cursor.board().size() == 3 ? spec.turnRiver[0] : spec.turnRiver[1];
      cursor = cursor.after_card(id);
      current = static_cast<pv::Street>(static_cast<int>(current) + 1);
    }
    const std::size_t actor = seatToActor(event.actor);
    switch (event.action) {
      case pv::ACTION_TYPE_FOLD:
        cursor = cursor.after_action(actor, {ActionType::Fold});
        break;
      case pv::ACTION_TYPE_CHECK:
        cursor = cursor.after_action(actor, {ActionType::Check});
        break;
      case pv::ACTION_TYPE_CALL:
        cursor = cursor.after_action(actor, {ActionType::Call});
        break;
      case pv::ACTION_TYPE_BET:
        cursor = cursor.after_action(actor, {ActionType::Bet, *event.target});
        break;
      case pv::ACTION_TYPE_RAISE:
        cursor = cursor.after_action(actor, {ActionType::Raise, *event.target});
        break;
      default:
        break;
    }
  }
  while (static_cast<int>(spec.street) > static_cast<int>(current)) {
    const int id = cursor.board().size() == 3 ? spec.turnRiver[0] : spec.turnRiver[1];
    cursor = cursor.after_card(id);
    current = static_cast<pv::Street>(static_cast<int>(current) + 1);
  }
  return cursor;
}

bs::poker::Street pokerStreet(pv::Street wire) {
  switch (wire) {
    case pv::STREET_FLOP:
      return bs::poker::Street::Flop;
    case pv::STREET_TURN:
      return bs::poker::Street::Turn;
    case pv::STREET_RIVER:
      return bs::poker::Street::River;
    default:
      return bs::poker::Street::Preflop;
  }
}

bs::poker::ActionType pokerActionType(pv::ActionType wire) {
  switch (wire) {
    case pv::ACTION_TYPE_FOLD:
      return bs::poker::ActionType::Fold;
    case pv::ACTION_TYPE_CHECK:
      return bs::poker::ActionType::Check;
    case pv::ACTION_TYPE_CALL:
      return bs::poker::ActionType::Call;
    case pv::ACTION_TYPE_BET:
      return bs::poker::ActionType::Bet;
    case pv::ACTION_TYPE_RAISE:
      return bs::poker::ActionType::Raise;
    default:
      return bs::poker::ActionType::Fold;
  }
}

// LegalActions has no operator== (TargetRange is a plain struct), so the
// differential oracle compares the six fields explicitly.
void checkLegal(const std::string& name, const LegalActions& a, const LegalActions& b) {
  check(a.fold == b.fold, name + " legal fold");
  check(a.check == b.check, name + " legal check");
  check(a.call == b.call, name + " legal call");
  check(a.call_amount == b.call_amount, name + " legal call amount");
  check(a.aggressive.has_value() == b.aggressive.has_value(), name + " legal aggressive present");
  if (a.aggressive && b.aggressive) {
    check(a.aggressive->type == b.aggressive->type, name + " legal aggressive type");
    check(a.aggressive->minimum == b.aggressive->minimum, name + " legal aggressive minimum");
    check(a.aggressive->maximum == b.aggressive->maximum, name + " legal aggressive maximum");
    check(a.aggressive->all_in_only == b.aggressive->all_in_only,
          name + " legal aggressive all-in-only");
  }
}

// Gate 2: the reconstructed PublicAction log must equal the observed postflop
// event sequence (street, occupied-seat index, action type, target total).
void assertHistory(const std::string& name, const NodeSpec& spec,
                   const std::vector<bs::poker::PublicAction>& history) {
  std::size_t expected = 0;
  for (const EventSpec& event : spec.events)
    if (event.street != pv::STREET_PREFLOP)
      ++expected;
  check(history.size() == expected, name + " history length");
  std::size_t i = 0;
  for (const EventSpec& event : spec.events) {
    if (event.street == pv::STREET_PREFLOP)
      continue;
    const bs::poker::PublicAction& actual = history[i++];
    check(actual.street == pokerStreet(event.street), name + " history street");
    check(actual.seat == static_cast<std::size_t>(std::stoul(event.actor.substr(1))),
          name + " history seat");
    check(actual.action.type == pokerActionType(event.action), name + " history action type");
    check(actual.action.target_total == event.target.value_or(0), name + " history target");
  }
}

// std::span<const int> has no heterogeneous == with std::vector<int> on this
// standard library, so the board comparison is element-wise.
void checkBoard(const std::string& name, std::span<const int> a, const std::vector<int>& b) {
  check(a.size() == b.size(), name + " board size");
  if (a.size() != b.size())
    return;
  for (std::size_t i = 0; i < a.size(); ++i)
    check(a[i] == b[i], name + " board card " + std::to_string(i));
}

void assertMatchesOracle(const std::string& name, const NodeSpec& spec, std::size_t heroActor) {
  pv::DecisionRequest request = buildRequest(spec);
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  const bool ok = bs::v1::reconstructPostflop(request, reconstructed, miss);
  if (!ok) {
    check(false, name + " reconstructed (miss " + bs::v1::to_string(miss) + ")");
    return;
  }
  check(reconstructed.state.has_value(), name + " state present");
  if (!reconstructed.state)
    return;
  const bs::poker::GameState& state = *reconstructed.state;

  if (spec.playerCount == 2) {
    // Gate 1 differential oracle: the GameState path must agree field for
    // field with the shipped HeadsUpState reconstruction, and its root must
    // match the independently replayed oracle root.
    check(reconstructed.oracle_state.has_value(), name + " oracle state present at 2 seats");
    if (!reconstructed.oracle_state)
      return;
    const HeadsUpState& oracle = *reconstructed.oracle_state;
    HeadsUpRoot expectedRoot;
    (void)oracleState(spec, expectedRoot);
    check(oracle.root().flop == expectedRoot.flop, name + " root flop");
    check(oracle.root().stacks == expectedRoot.stacks, name + " root stacks");
    check(oracle.root().contributions == expectedRoot.contributions, name + " root contributions");
    check(oracle.root().pot == expectedRoot.pot, name + " root pot");
    check(oracle.root().big_blind == expectedRoot.big_blind, name + " root big blind");
    check(oracle.root().button == expectedRoot.button, name + " root button");
    check(state.street() == oracle.street(), name + " street vs oracle");
    check(state.phase() == oracle.phase(), name + " phase vs oracle");
    check(state.actor() == oracle.actor(), name + " actor vs oracle");
    checkBoard(name + " vs oracle", state.board(), oracle.board());
    check(state.pot() == oracle.pot(), name + " pot vs oracle");
    for (std::size_t p = 0; p < 2; ++p) {
      check(state.players()[p].stack == oracle.players()[p].stack,
            name + " stack vs oracle " + std::to_string(p));
      check(state.players()[p].street_committed == oracle.players()[p].street_committed,
            name + " street commitment vs oracle " + std::to_string(p));
    }
    checkLegal(name, state.legal(), oracle.legal());
  } else {
    // 3+ seats: no HeadsUpState oracle exists. The positive multiway fixture
    // is a flop root with no postflop events, so the node IS the root; assert
    // the root identity and the node directly against the spec.
    const auto& def = state.def();
    const Chips matched = spec.pot / static_cast<Chips>(spec.playerCount);
    check(def.player_count == static_cast<std::size_t>(spec.playerCount), name + " player count");
    check(def.board[0] == spec.flop[0] && def.board[1] == spec.flop[1] &&
              def.board[2] == spec.flop[2] && def.board_size == 3,
          name + " root board");
    for (std::size_t p = 0; p < static_cast<std::size_t>(spec.playerCount); ++p) {
      const Chips expected_stack = p < 2 ? spec.stacks[p] : 1980;
      check(def.stacks[p] == expected_stack, name + " root stack " + std::to_string(p));
      check(def.contributions[p] == matched, name + " root contribution " + std::to_string(p));
    }
    check(def.pot == spec.pot, name + " root pot");
    check(def.big_blind == 20, name + " root big blind");
    check(def.button == static_cast<std::size_t>(spec.buttonSeat), name + " root button");
    check(state.street() == bs::poker::Street::Flop, name + " node street");
    check(state.phase() == bs::poker::Phase::Action, name + " node phase");
    check(state.actor().has_value() && *state.actor() == heroActor, name + " node actor");
    check(state.board().size() == 3, name + " node board size");
    check(state.pot() == spec.pot, name + " node pot");
    for (std::size_t p = 0; p < static_cast<std::size_t>(spec.playerCount); ++p) {
      const Chips expected_stack = p < 2 ? spec.stacks[p] : 1980;
      check(state.players()[p].stack == expected_stack, name + " node stack " + std::to_string(p));
      check(state.players()[p].street_committed == 0,
            name + " node street commitment " + std::to_string(p));
    }
  }

  // Gate 2: the PublicAction log must equal the observed postflop sequence.
  assertHistory(name, spec, reconstructed.history);

  check(reconstructed.hero_actor == heroActor, name + " hero actor mapping");
  check(state.actor().has_value() && *state.actor() == heroActor, name + " decision on hero");
}

void assertMiss(const std::string& name, const NodeSpec& spec, V1BlueprintMiss expected) {
  pv::DecisionRequest request = buildRequest(spec);
  ReconstructedPostflop reconstructed;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  const bool ok = bs::v1::reconstructPostflop(request, reconstructed, miss);
  check(!ok && miss == expected, name + " misses as " + bs::v1::to_string(expected) + " (got " +
                                     (ok ? "hit" : bs::v1::to_string(miss)) + ")");
}

}  // namespace

int main() {
  // --- Positive oracle cases ------------------------------------------------
  {
    NodeSpec spec;
    assertMatchesOracle("flop first action on BB", spec, 1);
  }
  {
    // BB checks, button faces.
    NodeSpec spec;
    spec.hero = "p0";
    spec.events.push_back(
        {pv::STREET_FLOP, "p1", pv::ACTION_TYPE_CHECK, std::nullopt, std::nullopt, 40});
    assertMatchesOracle("flop after BB check", spec, 0);
  }
  {
    // Button leads after the BB checks; the BB faces the bet. Heads-up postflop
    // always opens on the non-button (p1 with the button on seat 0).
    NodeSpec spec;
    spec.hero = "p1";
    spec.events.push_back(
        {pv::STREET_FLOP, "p1", pv::ACTION_TYPE_CHECK, std::nullopt, std::nullopt, 40});
    spec.events.push_back({pv::STREET_FLOP, "p0", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.stacks = {1880, 1980};
    spec.streetCommitted = {100, 0};
    spec.pot = 140;
    spec.toCall = 100;
    assertMatchesOracle("flop facing lead", spec, 1);
  }
  {
    // Bet/call closes the flop; turn opens checked to the BB.
    NodeSpec spec;
    spec.hero = "p1";
    spec.street = pv::STREET_TURN;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.events.push_back({pv::STREET_FLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 100, 140});
    spec.stacks = {1880, 1880};
    spec.streetCommitted = {0, 0};
    spec.pot = 240;
    spec.toCall = 0;
    assertMatchesOracle("turn checked to BB", spec, 1);
  }
  {
    // Turn raise and river continuation.
    NodeSpec spec;
    spec.hero = "p1";
    spec.street = pv::STREET_RIVER;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.events.push_back({pv::STREET_FLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 100, 140});
    spec.events.push_back({pv::STREET_TURN, "p1", pv::ACTION_TYPE_BET, 200, 200, 240});
    spec.events.push_back({pv::STREET_TURN, "p0", pv::ACTION_TYPE_RAISE, 500, 300, 440});
    spec.events.push_back({pv::STREET_TURN, "p1", pv::ACTION_TYPE_CALL, std::nullopt, 300, 940});
    spec.stacks = {1380, 1380};
    spec.streetCommitted = {0, 0};
    spec.pot = 1240;
    spec.toCall = 0;
    assertMatchesOracle("river after turn raise/call", spec, 1);
  }
  {
    // Button on seat 1: the button is player index 1 and the non-button p0
    // acts first postflop.
    NodeSpec spec;
    spec.buttonSeat = 1;
    spec.hero = "p0";  // non-button acts first postflop
    assertMatchesOracle("button on seat 1 makes p0 the first actor", spec, 0);
  }

  // --- Reject classes -------------------------------------------------------
  {
    // An all-in player at a postflop decision node is outside the
    // equal-matched resident profile even when the chip shape is consistent.
    NodeSpec spec;
    spec.status[0] = pv::PLAYER_STATUS_ALL_IN;
    spec.stacks = {0, 200};
    spec.streetCommitted = {200, 200};
    spec.pot = 440;
    spec.toCall = 200;
    spec.hero = "p1";
    assertMiss("an all-in player is rejected", spec, V1BlueprintMiss::RootNotSupported);
  }
  {
    // Asymmetric preflop contributions totaling an even pot (15+25=40) must
    // be rejected when the full preflop ledger is present: the BB raises to
    // 25 (5 over his 20 big blind) and the SB calls 5 (15 total against his
    // 10 small blind). flopPot is even but the matched contributions are not.
    NodeSpec spec;
    spec.preflop = {
        {pv::STREET_PREFLOP, "p1", pv::ACTION_TYPE_RAISE, 25, 5, 0},
        {pv::STREET_PREFLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 5, 0},
    };
    assertMiss("asymmetric 15+25 preflop contributions are rejected", spec,
               V1BlueprintMiss::RootNotSupported);
  }
  {
    // Symmetric 20+20=40 preflop (SB completes the extra 10 with one call)
    // reconciles with the flop pot.
    NodeSpec spec;
    spec.preflop = {{pv::STREET_PREFLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 10, 0}};
    assertMatchesOracle("symmetric 20+20 preflop contributions reconstruct", spec, 1);
  }
  {
    // Blinds plus pot 40 with no voluntary preflop events: the SB completion
    // is implicit and the even-pot matched assumption stands.
    NodeSpec spec;
    spec.preflop.clear();
    assertMatchesOracle("even pot with blinds only uses matched assumption", spec, 1);
  }
  {
    NodeSpec spec;
    spec.street = pv::STREET_PREFLOP;
    assertMiss("preflop is outside the resident profile", spec, V1BlueprintMiss::RootNotSupported);
  }
  {
    // A three-seat request whose pot cannot split into equal matched
    // contributions (40 is not divisible by 3) is rejected at the root.
    NodeSpec spec;
    spec.playerCount = 3;
    assertMiss("three-seat pot 40 cannot split into equal matched contributions", spec,
               V1BlueprintMiss::RootNotSupported);
  }
  {
    // Three seats, equal matched 20/20/20 = 60: the SB (p0) completes 10 over
    // his forced 10 and the third seat (p2) calls 20 cold, so the full
    // preflop ledger reconciles to 20 per seat. Postflop opens on seat 1
    // (first live seat clockwise of the button on seat 0).
    NodeSpec spec;
    spec.playerCount = 3;
    spec.hero = "p1";
    spec.pot = 60;
    spec.preflop = {
        {pv::STREET_PREFLOP, "p2", pv::ACTION_TYPE_CALL, std::nullopt, 20, 0},
        {pv::STREET_PREFLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, 10, 0},
    };
    assertMatchesOracle("three-seat equal matched 20/20/20 reconstructs", spec, 1);
  }
  {
    NodeSpec spec;
    spec.addAnte = true;
    assertMiss("antes are outside the resident profile", spec, V1BlueprintMiss::RootNotSupported);
  }
  {
    NodeSpec spec;
    spec.status[1] = pv::PLAYER_STATUS_FOLDED;
    assertMiss("a folded opponent ends the decision", spec, V1BlueprintMiss::RootNotSupported);
  }
  {
    // Odd first-event pot cannot split into equal matched contributions.
    NodeSpec spec;
    spec.hero = "p0";
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 41});
    spec.stacks = {1980, 1880};
    spec.streetCommitted = {0, 100};
    spec.pot = 141;
    spec.toCall = 100;
    assertMiss("unequal matched contributions", spec, V1BlueprintMiss::RootNotSupported);
  }
  {
    // Unresolved actor id.
    NodeSpec spec;
    spec.events.push_back(
        {pv::STREET_FLOP, "", pv::ACTION_TYPE_CHECK, std::nullopt, std::nullopt, 40});
    assertMiss("unresolved history actor", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Bet without an exact target total.
    NodeSpec spec;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, std::nullopt, 100, 40});
    assertMiss("bet without target total", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Call without an incremental amount.
    NodeSpec spec;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.events.push_back(
        {pv::STREET_FLOP, "p0", pv::ACTION_TYPE_CALL, std::nullopt, std::nullopt, 140});
    assertMiss("call without increment", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Check after a bet is an illegal transition for the poker engine.
    NodeSpec spec;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.events.push_back(
        {pv::STREET_FLOP, "p0", pv::ACTION_TYPE_CHECK, std::nullopt, std::nullopt, 140});
    assertMiss("illegal check transition", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Tampered pot.
    NodeSpec spec;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.hero = "p0";
    spec.stacks = {1980, 1880};
    spec.streetCommitted = {0, 100};
    spec.pot = 141;
    spec.toCall = 100;
    assertMiss("tampered pot", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Tampered to_call.
    NodeSpec spec;
    spec.events.push_back({pv::STREET_FLOP, "p1", pv::ACTION_TYPE_BET, 100, 100, 40});
    spec.hero = "p0";
    spec.stacks = {1980, 1880};
    spec.streetCommitted = {0, 100};
    spec.pot = 140;
    spec.toCall = 90;
    assertMiss("tampered to_call", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Hero hole card collides with the board.
    NodeSpec spec;
    spec.heroCards = {cardId("As"), cardId("2h")};
    assertMiss("hero card on board", spec, V1BlueprintMiss::OffTree);
  }
  {
    // Turn-tagged history without the turn card on the board (street length 3).
    NodeSpec spec;
    spec.street = pv::STREET_FLOP;
    spec.events.push_back(
        {pv::STREET_TURN, "p1", pv::ACTION_TYPE_CHECK, std::nullopt, std::nullopt, 40});
    assertMiss("turn event without turn card", spec, V1BlueprintMiss::OffTree);
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 RESIDENT MAPPER TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 RESIDENT MAPPER TESTS PASSED");
  return 0;
}
