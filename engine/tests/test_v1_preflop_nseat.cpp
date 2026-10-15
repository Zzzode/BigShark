// RFC 0019: independent public-state reconstruction checks at six seats and
// 100 BB. Native GameState replay supplies expected transitions; literal chip
// ledgers independently pin important cases. Scripted host rows below prove
// routing and guarantee-floor behavior only, never policy strength, available
// six-player artifacts, or completion of the six-max GTO feature.
#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/v1_protocol.hpp>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../src/protocol/v1_resident_mapper.hpp"

namespace {

namespace pv = bs::v1::pv;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::Chips;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::PublicAction;
using bs::poker::Street;
using bs::v1::ReconstructedPostflop;
using bs::v1::V1BlueprintMiss;

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

pv::Card card(int id) {
  pv::Card result;
  result.set_rank(static_cast<pv::Rank>(id / 4 + 1));
  result.set_suit(static_cast<pv::Suit>(id % 4 + 1));
  return result;
}

GameDef root(std::size_t count, std::size_t button) {
  GameDef result{};
  result.player_count = count;
  result.button = button;
  result.big_blind = 20;
  std::fill_n(result.stacks.begin(), count, Chips{2000});
  result.pot = 30;
  result.preflop = true;
  result.terminal = bs::poker::TerminalDepth::Flop;
  result.board = {-1, -1, -1, 0, 0};
  result.blinds_posted[bs::poker::small_blind_seat(result)] = 10;
  result.blinds_posted[bs::poker::big_blind_seat(result)] = 20;
  return result;
}

// The wire fixture is produced from public domain transitions, never from the
// reconstructor. Player IDs are deliberately opaque and input order reversed.
struct Fixture {
  GameState expected;
  pv::DecisionRequest request;
  std::vector<PublicAction> history;
  std::vector<int> seats;

  explicit Fixture(std::size_t count = 6, std::size_t button = 0, bool sparse = true)
      : expected(root(count, button)) {
    if (count == 6 && sparse)
      seats = {0, 2, 3, 5, 7, 9};
    else
      for (std::size_t i = 0; i < count; ++i)
        seats.push_back(static_cast<int>(i));
    auto* state = request.mutable_state();
    auto* game = state->mutable_game();
    game->set_variant(pv::GAME_VARIANT_NLHE);
    game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
    game->set_game_type(pv::GAME_TYPE_CASH);
    game->set_table_capacity(sparse && count == 6 ? 10 : count);
    game->mutable_amount_unit()->set_name("chip");
    game->mutable_amount_unit()->set_decimal_places(0);
    game->set_small_blind(10);
    game->set_big_blind(20);
    state->set_hand_id("six-seat-public-preflop");
    state->set_street(pv::STREET_PREFLOP);
    state->set_button_seat(seats[button]);
    *state->add_hero_hole_cards() = card(0);
    *state->add_hero_hole_cards() = card(5);
    for (std::size_t i = count; i > 0; --i) {
      auto* player = state->add_players();
      player->set_player_id(id(i - 1));
      player->set_seat(seats[i - 1]);
    }
    auto* sb = state->add_forced_contributions();
    sb->set_player_id(id(bs::poker::small_blind_seat(expected.def())));
    sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    sb->set_amount(10);
    auto* bb = state->add_forced_contributions();
    bb->set_player_id(id(bs::poker::big_blind_seat(expected.def())));
    bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    bb->set_amount(20);
    auto* options = request.mutable_options();
    options->set_strategy_profile("tag");
    options->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    options->set_seed(19);
    options->set_solve_time_budget_ms(1000);
    options->set_include_sampled_action(true);
    sync();
  }

  std::string id(std::size_t actor) const {
    return "opaque-player-" + std::to_string(seats[actor]);
  }

  pv::PlayerState* player(std::size_t actor) {
    for (auto& value : *request.mutable_state()->mutable_players())
      if (value.player_id() == id(actor))
        return &value;
    return nullptr;
  }

  void sync() {
    auto* state = request.mutable_state();
    state->set_decision_index(history.size() + 1);
    state->set_hero_player_id(id(*expected.actor()));
    state->mutable_pot()->set_pot_total(expected.pot());
    state->mutable_pot()->set_main_pot(expected.pot());
    for (std::size_t i = 0; i < expected.player_count(); ++i) {
      const auto& value = expected.players()[i];
      auto* wire = player(i);
      wire->set_stack(value.stack);
      wire->set_street_committed(value.street_committed);
      wire->set_status(value.folded   ? pv::PLAYER_STATUS_FOLDED
                       : value.all_in ? pv::PLAYER_STATUS_ALL_IN
                                      : pv::PLAYER_STATUS_ACTIVE);
    }
    state->clear_legal_actions();
    const auto legal = expected.legal();
    if (legal.fold)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
    if (legal.check)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
    if (legal.call)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
    state->set_to_call(legal.call_amount);
    if (legal.aggressive) {
      auto* aggression = state->add_legal_actions();
      // River uses RAISE even at the big-blind option, domain Bet.
      aggression->set_type(pv::ACTION_TYPE_RAISE);
      aggression->set_min_target_total(legal.aggressive->minimum);
      aggression->set_max_target_total(legal.aggressive->maximum);
    }
  }

  void act(Action action) {
    const std::size_t actor = *expected.actor();
    const Chips before = expected.players()[actor].stack;
    const Chips pot = expected.pot();
    expected = expected.after_action(actor, action);
    history.push_back({Street::Preflop, actor, action});
    auto* event = request.mutable_state()->add_action_history();
    event->set_sequence(history.size());
    event->set_street(pv::STREET_PREFLOP);
    event->set_actor_player_id(id(actor));
    event->set_pot_before(pot);
    event->set_stack_after(expected.players()[actor].stack);
    switch (action.type) {
      case ActionType::Fold:
        event->set_action(pv::ACTION_TYPE_FOLD);
        break;
      case ActionType::Check:
        event->set_action(pv::ACTION_TYPE_CHECK);
        break;
      case ActionType::Call:
        event->set_action(pv::ACTION_TYPE_CALL);
        event->set_incremental_amount(before - expected.players()[actor].stack);
        break;
      case ActionType::Bet:
      case ActionType::Raise:
        event->set_action(pv::ACTION_TYPE_RAISE);
        event->set_target_total(action.target_total);
        event->set_incremental_amount(before - expected.players()[actor].stack);
        break;
    }
    sync();
  }
};

void matches(const Fixture& fixture, const std::string& name) {
  ReconstructedPostflop actual;
  V1BlueprintMiss miss = V1BlueprintMiss::OffTree;
  bool success = false;
  try {
    success = bs::v1::reconstructPreflop(fixture.request, actual, miss, 2);
  } catch (...) {
    check(false, name + " must not throw");
  }
  check(success && actual.state.has_value() && miss == V1BlueprintMiss::None,
        name + " reconstructs");
  if (!success || !actual.state)
    return;
  const auto& state = *actual.state;
  const auto& expected = fixture.expected;
  check(bs::poker::same_game_def(state.def(), expected.def()), name + " complete root identity");
  check(state.player_count() == expected.player_count(), name + " retains dealt count");
  check(state.phase() == expected.phase() && state.street() == expected.street(), name + " phase");
  check(state.actor() == expected.actor() && actual.hero_actor == *expected.actor(),
        name + " actor");
  check(state.pot() == expected.pot(), name + " pot");
  check(state.board().empty(), name + " no board");
  check(state.last_full_raise() == expected.last_full_raise(), name + " full raise increment");
  check(actual.hero_cards == std::array<int, 2>{0, 5}, name + " known hero cards only");
  const auto live = state.live_players();
  const auto expectedLive = expected.live_players();
  check(std::equal(live.begin(), live.end(), expectedLive.begin(), expectedLive.end()),
        name + " live seat identity");
  for (std::size_t i = 0; i < expected.player_count(); ++i) {
    const auto& a = state.players()[i];
    const auto& b = expected.players()[i];
    const std::string seat = name + " seat " + std::to_string(i);
    check(a.stack == b.stack && a.street_committed == b.street_committed &&
              a.contributed == b.contributed && a.refunded == b.refunded,
          seat + " exact chip ledger");
    check(a.folded == b.folded && a.all_in == b.all_in && a.pending == b.pending &&
              a.raise_rights == b.raise_rights && state.can_raise(i) == expected.can_raise(i),
          seat + " status/pending/raise rights");
  }
  const auto a = state.legal();
  const auto b = expected.legal();
  check(
      a.fold == b.fold && a.check == b.check && a.call == b.call && a.call_amount == b.call_amount,
      name + " passive action menu");
  check(a.aggressive.has_value() == b.aggressive.has_value(), name + " raise admission");
  if (a.aggressive && b.aggressive)
    check(a.aggressive->type == b.aggressive->type &&
              a.aggressive->minimum == b.aggressive->minimum &&
              a.aggressive->maximum == b.aggressive->maximum &&
              a.aggressive->all_in_only == b.aggressive->all_in_only,
          name + " complete raise interval");
  check(actual.history.size() == fixture.history.size(), name + " full history length");
  for (std::size_t i = 0; i < actual.history.size() && i < fixture.history.size(); ++i)
    check(actual.history[i].street == fixture.history[i].street &&
              actual.history[i].seat == fixture.history[i].seat &&
              actual.history[i].action == fixture.history[i].action,
          name + " exact public event " + std::to_string(i));
}

void rejected(const pv::DecisionRequest& request, const std::string& name,
              std::uint32_t minor = 2) {
  // Seed populated output to exercise fail-closed cleanup, not just a fresh
  // default object whose optional state was already empty.
  ReconstructedPostflop actual;
  actual.state.emplace(root(6, 0));
  actual.history.push_back({Street::Preflop, 3, {ActionType::Call}});
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  bool success = true;
  try {
    success = bs::v1::reconstructPreflop(request, actual, miss, minor);
  } catch (...) {
    check(false, name + " rejects without throwing");
  }
  check(!success && miss != V1BlueprintMiss::None, name + " rejected");
  check(!actual.state && actual.history.empty() && !actual.oracle_state,
        name + " no usable partial output");
}

void positiveReplays() {
  for (std::size_t count = 2; count <= 10; ++count)
    for (std::size_t button = 0; button < count; ++button)
      matches(Fixture(count, button),
              "root count " + std::to_string(count) + " button " + std::to_string(button));
  for (std::size_t button = 0; button < 6; ++button) {
    Fixture f(6, button);
    f.request.mutable_state()->mutable_players()->SwapElements(0, 3);
    f.request.mutable_state()->mutable_players()->SwapElements(1, 4);
    matches(f, "non-monotonic sparse player ordering " + std::to_string(button));
    check(f.expected.pot() == 30 && *f.expected.actor() == (button + 3) % 6,
          "six-seat hand-calculated initial actor/pot");
    for (int step = 0; step < 5; ++step) {
      f.act({ActionType::Call});
      matches(f, "six-seat limp prefix " + std::to_string(button) + "/" + std::to_string(step));
    }
    check(f.expected.pot() == 120 && *f.expected.actor() == (button + 2) % 6 &&
              f.expected.legal().check && f.expected.legal().call_amount == 0,
          "six limpers each contributed 20, BB retains check/raise option");
    f.act({ActionType::Bet, 60});
    check(f.expected.pot() == 160 && f.expected.last_full_raise() == 40,
          "BB option pays 40 additional chips and full raise is 40");
    matches(f, "BB RAISE wire replays as domain Bet " + std::to_string(button));
  }
  Fixture f;
  for (int i = 0; i < 5; ++i)
    f.act({ActionType::Call});
  f.act({ActionType::Bet, 60});
  f.act({ActionType::Fold});
  matches(f, "folded UTG retained after limps");
  f.act({ActionType::Call});
  f.act({ActionType::Raise, 180});
  matches(f, "multiway reraise after a call and fold");
  f.act({ActionType::Fold});
  f.act({ActionType::Call});
  f.act({ActionType::Call});
  matches(f, "caller faces re-opened raise");
  check(
      f.expected.pot() == 640 && *f.expected.actor() == 4 && f.expected.legal().call_amount == 120,
      "literal 20+180+180+20+60+180 ledger totals 640");
  const std::array<Chips, 6> committed{20, 180, 180, 20, 60, 180};
  for (std::size_t i = 0; i < 6; ++i)
    check(f.expected.players()[i].street_committed == committed[i] &&
              f.expected.players()[i].stack == 2000 - committed[i],
          "literal reraise ledger seat " + std::to_string(i));

  Fixture folded;
  for (int i = 0; i < 4; ++i) {
    folded.act({ActionType::Fold});
    matches(folded, "four-fold prefix " + std::to_string(i));
  }
  check(folded.expected.player_count() == 6 && folded.expected.live_players().size() == 2 &&
            *folded.expected.actor() == 1 && folded.expected.pot() == 30,
        "six dealt seats do not become a HU root when four fold");
  folded.act({ActionType::Call});
  matches(folded, "two live of six dealt, BB option retains multiway identity");

  Fixture jam;
  jam.act({ActionType::Raise, 2000});
  matches(jam, "full 100BB all-in is retained while next seat acts");
  check(jam.expected.pot() == 2030 && jam.expected.players()[3].all_in &&
            jam.expected.legal().call_amount == 2000,
        "literal all-in ledger is 2000 plus 10/20 blinds");
  jam.act({ActionType::Call});
  matches(jam, "two all-in players retained while another seat decides");
  check(jam.expected.pot() == 4030, "literal two-all-in ledger");

  Fixture calls;
  calls.act({ActionType::Call});
  calls.act({ActionType::Raise, 60});
  calls.act({ActionType::Raise, 100});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  matches(calls, "same player calls once before and once after reraises");
  check(calls.expected.pot() == 560 && *calls.expected.actor() == 4 &&
            calls.expected.legal().call_amount == 40,
        "literal repeated-call pot is five times 100 plus 60");

  Fixture shortRaise;
  shortRaise.act({ActionType::Raise, 1000});
  shortRaise.act({ActionType::Raise, 1980});
  shortRaise.act({ActionType::Raise, 2000});
  matches(shortRaise, "short all-in does not count as a full raise");
  check(!shortRaise.expected.players()[4].raise_rights &&
            shortRaise.expected.players()[3].raise_rights,
        "short raise preserves differing rights of prior full raiser and earlier opener");
  for (int i = 0; i < 3; ++i)
    shortRaise.act({ActionType::Fold});
  shortRaise.act({ActionType::Call});
  matches(shortRaise, "short all-in does not reopen prior raiser rights");
  check(shortRaise.expected.pot() == 6010 && *shortRaise.expected.actor() == 4 &&
            shortRaise.expected.players()[4].stack == 20 &&
            shortRaise.expected.legal().call_amount == 20 && !shortRaise.expected.can_raise(4) &&
            !shortRaise.expected.legal().aggressive && shortRaise.expected.last_full_raise() == 980,
        "literal 2000+1980+2000+10+20 ledger, due20, no reopened raise rights");

  Fixture optionalLedgers;
  for (int i = 0; i < 3; ++i)
    optionalLedgers.act({ActionType::Call});
  for (auto& event : *optionalLedgers.request.mutable_state()->mutable_action_history()) {
    event.clear_pot_before();
    event.clear_stack_after();
  }
  matches(optionalLedgers, "per-event pot/stack ledgers optional on success path");
}

void invalidSnapshots() {
  const Fixture initial;
  auto malformed = initial.request;
  malformed.mutable_state()->mutable_forced_contributions(0)->set_player_id(initial.id(2));
  malformed.mutable_state()->mutable_forced_contributions(1)->set_player_id(initial.id(1));
  rejected(malformed, "swapped blind seats");
  malformed = initial.request;
  malformed.mutable_state()->mutable_forced_contributions(0)->set_type(
      pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  malformed.mutable_state()->mutable_forced_contributions(1)->set_type(
      pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  rejected(malformed, "swapped blind types with unchanged aggregate amounts");
  malformed = initial.request;
  *malformed.mutable_state()->add_forced_contributions() =
      malformed.state().forced_contributions(0);
  rejected(malformed, "duplicate small blind");
  malformed = initial.request;
  malformed.mutable_state()->mutable_game()->set_small_blind(9);
  rejected(malformed, "declared blind mismatch");
  malformed = initial.request;
  malformed.mutable_state()->mutable_players(0)->set_seat(malformed.state().players(1).seat());
  rejected(malformed, "duplicate physical seat");
  malformed = initial.request;
  malformed.mutable_state()->mutable_players(0)->set_player_id(
      malformed.state().players(1).player_id());
  rejected(malformed, "duplicate opaque identity");
  malformed = initial.request;
  malformed.mutable_state()->set_button_seat(8);
  rejected(malformed, "button not dealt");
  malformed = initial.request;
  malformed.mutable_state()->set_hero_player_id("unknown-player");
  rejected(malformed, "hero not dealt");
  malformed = initial.request;
  malformed.mutable_state()->set_hero_player_id(initial.id(4));
  rejected(malformed, "hero not current actor");
  malformed = initial.request;
  malformed.mutable_state()->mutable_pot()->set_pot_total(31);
  rejected(malformed, "wrong pot");
  malformed = initial.request;
  malformed.mutable_state()->mutable_players(0)->set_street_committed(1);
  rejected(malformed, "wrong street commitment");
  malformed = initial.request;
  malformed.mutable_state()->mutable_players(0)->set_stack(1999);
  rejected(malformed, "unequal start root constructor fails closed");
  malformed = initial.request;
  malformed.mutable_state()->set_to_call(21);
  rejected(malformed, "wrong amount owed");
  for (const auto status : {pv::PLAYER_STATUS_FOLDED, pv::PLAYER_STATUS_ALL_IN,
                            pv::PLAYER_STATUS_UNSPECIFIED, static_cast<pv::PlayerStatus>(99)}) {
    malformed = initial.request;
    malformed.mutable_state()->mutable_players(0)->set_status(status);
    rejected(malformed, "unexplained player status " + std::to_string(status));
  }
  malformed = initial.request;
  malformed.mutable_state()->mutable_hero_hole_cards(0)->set_rank(static_cast<pv::Rank>(99));
  rejected(malformed, "unknown rank");
  malformed = initial.request;
  malformed.mutable_state()->mutable_hero_hole_cards(0)->set_suit(static_cast<pv::Suit>(99));
  rejected(malformed, "unknown suit");
  malformed = initial.request;
  malformed.mutable_state()->mutable_hero_hole_cards(0)->set_suit(static_cast<pv::Suit>(5));
  rejected(malformed, "unknown suit cannot alias a valid native card ID");
  malformed = initial.request;
  *malformed.mutable_state()->mutable_hero_hole_cards(1) = malformed.state().hero_hole_cards(0);
  rejected(malformed, "duplicate hero cards");
  malformed = initial.request;
  malformed.mutable_state()->clear_hero_hole_cards();
  rejected(malformed, "missing hero cards");
  malformed = initial.request;
  *malformed.mutable_state()->add_board() = card(10);
  rejected(malformed, "preflop board must be empty");
  malformed = initial.request;
  malformed.mutable_state()->mutable_game()->set_ante(1);
  rejected(malformed, "ante remains unsupported");
  malformed = initial.request;
  malformed.mutable_state()->mutable_game()->mutable_rake()->set_basis_points(100);
  rejected(malformed, "rake remains unsupported");
  malformed = initial.request;
  malformed.mutable_state()->mutable_game()->mutable_straddle()->set_enabled(true);
  rejected(malformed, "straddle remains unsupported");
  malformed = initial.request;
  malformed.mutable_state()->mutable_pot()->add_side_pots()->set_amount(1);
  rejected(malformed, "side pots remain unsupported");
  malformed = initial.request;
  malformed.mutable_state()->clear_players();
  *malformed.mutable_state()->add_players() = initial.request.state().players(0);
  rejected(malformed, "one dealt seat rejected");
  malformed = Fixture(10).request;
  auto* extra = malformed.mutable_state()->add_players();
  extra->set_player_id("eleventh-player");
  extra->set_seat(10);
  extra->set_stack(2000);
  extra->set_status(pv::PLAYER_STATUS_ACTIVE);
  rejected(malformed, "eleven dealt seats rejected");

  Fixture folded;
  folded.act({ActionType::Fold});
  malformed = folded.request;
  malformed.mutable_state()->mutable_players(2)->set_status(pv::PLAYER_STATUS_ACTIVE);
  rejected(malformed, "observed fold cannot be erased from snapshot");
  malformed = folded.request;
  malformed.mutable_state()->clear_action_history();
  rejected(malformed, "snapshot fold requires replay history");
  malformed = folded.request;
  malformed.mutable_state()->mutable_action_history(0)->set_actor_player_id(folded.id(4));
  rejected(malformed, "out-of-turn historical actor");
  malformed = folded.request;
  malformed.mutable_state()->mutable_action_history(0)->set_actor_player_id("unknown-player");
  rejected(malformed, "unknown historical actor");
  malformed = folded.request;
  malformed.mutable_state()->mutable_action_history(0)->set_pot_before(31);
  rejected(malformed, "historical pot mismatch");
  malformed = folded.request;
  malformed.mutable_state()->mutable_action_history(0)->set_stack_after(1999);
  rejected(malformed, "historical stack mismatch");
  malformed = folded.request;
  malformed.mutable_state()->mutable_action_history(0)->set_street(pv::STREET_FLOP);
  rejected(malformed, "postflop event in preflop history");
  Fixture jam;
  jam.act({ActionType::Raise, 2000});
  malformed = jam.request;
  malformed.mutable_state()->mutable_players(2)->set_status(pv::PLAYER_STATUS_ACTIVE);
  rejected(malformed, "all-in status must agree with replay");

  Fixture calls;
  calls.act({ActionType::Call});
  calls.act({ActionType::Raise, 60});
  calls.act({ActionType::Raise, 100});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  calls.act({ActionType::Call});
  malformed = calls.request;
  malformed.mutable_state()->mutable_action_history(0)->set_incremental_amount(19);
  malformed.mutable_state()->mutable_action_history(6)->set_incremental_amount(81);
  for (auto& event : *malformed.mutable_state()->mutable_action_history()) {
    event.set_pot_before(0);
    event.set_stack_after(0);
  }
  rejected(malformed, "wrong per-call payments cannot cancel in the final chip ledger");

  rejected(initial.request, "minor1 multiway admission remains closed", 1);
  rejected(initial.request, "minor0 multiway admission remains closed", 0);
  ReconstructedPostflop out;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  check(!bs::v1::reconstructPreflop(initial.request, out, miss) &&
            miss == V1BlueprintMiss::RootNotSupported,
        "default direct-call minor remains legacy minor1");
}

// Every result here is a scripted row: no training, artifact, certification,
// range inference, or production solver is exercised by this seam fake.
class RoutingServices final : public bs::v1::V1HostServices {
 public:
  mutable int blueprintCalls = 0;
  mutable int resolveCalls = 0;
  mutable bool sawSixSeats = false;
  bool hit = true;
  bool resolve = false;
  std::array<Action, 3> actions{{{ActionType::Fold}, {ActionType::Call}, {ActionType::Raise, 40}}};
  std::array<double, 3> probabilities{0.25, 0.5, 0.25};
  const std::string digest = std::string(64, 'b');

  bool blueprintAdvertised() const noexcept override { return true; }
  bool resolvingAdvertised() const noexcept override { return true; }

  bs::v1::V1BlueprintRow row() const noexcept {
    bs::v1::V1BlueprintRow result;
    result.size = actions.size();
    result.actions = actions.data();
    result.probabilities = probabilities.data();
    result.artifact_sha256 = digest;
    return result;
  }

  bs::v1::V1BlueprintResult blueprintHeroDecision(const GameState& state,
                                                  std::span<const PublicAction> history,
                                                  const std::array<int, 2>& heroCards,
                                                  std::string_view) const noexcept override {
    ++blueprintCalls;
    sawSixSeats = state.player_count() == 6 && state.def().stacks[5] == 2000 && history.empty() &&
                  heroCards == std::array<int, 2>{0, 5};
    bs::v1::V1BlueprintResult result;
    result.hit = hit;
    result.miss = hit ? V1BlueprintMiss::None : V1BlueprintMiss::RootNotSupported;
    if (hit)
      result.row = row();
    return result;
  }

  bs::v1::V1ResolveResult resolvingDecision(const GameState& state,
                                            std::span<const PublicAction> history,
                                            const std::array<int, 2>& heroCards, std::string_view,
                                            std::uint32_t deadline) const noexcept override {
    ++resolveCalls;
    sawSixSeats = state.player_count() == 6 && state.def().stacks[5] == 2000 && history.empty() &&
                  heroCards == std::array<int, 2>{0, 5} && deadline == 1000;
    bs::v1::V1ResolveResult result;
    result.outcome = resolve ? bs::v1::V1ResolveOutcome::DeadlineBlueprint
                             : bs::v1::V1ResolveOutcome::Unsupported;
    result.miss = V1BlueprintMiss::RootNotSupported;
    if (resolve)
      result.row = row();
    return result;
  }
};

pv::Envelope send(pv::DecisionRequest request, std::uint32_t minor, const RoutingServices& host) {
  pv::Envelope input;
  input.set_protocol_minor(minor);
  input.set_request_id("six-seat-mechanical-route");
  *input.mutable_decision_request() = std::move(request);
  const auto result = bs::v1::handleEnvelope(input.SerializeAsString(), host);
  check(result.outcome == bs::v1::EnvelopeOutcome::Respond, "mechanical routing returns frame");
  pv::Envelope output;
  check(output.ParseFromString(result.response), "mechanical routing frame parses");
  check(output.protocol_minor() == minor, "mechanical routing preserves negotiated minor");
  return output;
}

void envelopeRouting() {
  for (const auto mode : {pv::SOLVER_MODE_BLUEPRINT, pv::SOLVER_MODE_AUTOMATIC}) {
    RoutingServices host;
    auto request = Fixture().request;
    request.mutable_options()->set_solver_mode(mode);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    auto result = send(request, 2, host);
    check(host.blueprintCalls == 1 && host.resolveCalls == 0 && host.sawSixSeats,
          "minor2 blueprint route reaches host with complete six-seat state");
    check(result.decision_response().has_expanded_strategy() &&
              result.decision_response().expanded_strategy().solver().source() ==
                  pv::SOLVER_SOURCE_BLUEPRINT &&
              result.decision_response().expanded_strategy().solver().guarantee_level() ==
                  "approximate",
          "scripted blueprint is approximate, never GTO quality evidence");
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    result = send(request, 2, host);
    check(host.blueprintCalls == 2 && host.resolveCalls == 0,
          "guarantee-floor enforcement does not duplicate six-seat host lookup");
    check(result.decision_response().has_error() &&
              result.decision_response().error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST &&
              !result.decision_response().has_expanded_strategy(),
          "six-seat mock approximate row cannot pass certified floor");
  }
  for (const auto mode : {pv::SOLVER_MODE_RESOLVING, pv::SOLVER_MODE_AUTOMATIC}) {
    RoutingServices host;
    host.hit = false;
    host.resolve = true;
    auto request = Fixture().request;
    request.mutable_options()->set_solver_mode(mode);
    const auto result = send(request, 2, host);
    check(host.resolveCalls == 1 && host.sawSixSeats &&
              host.blueprintCalls == (mode == pv::SOLVER_MODE_AUTOMATIC ? 1 : 0),
          "minor2 resolver route receives six seats once");
    check(result.decision_response().has_expanded_strategy() &&
              result.decision_response().expanded_strategy().solver().guarantee_level() ==
                  "approximate",
          "scripted deadline blueprint preserves approximate guarantee");
  }
  for (const auto mode :
       {pv::SOLVER_MODE_BLUEPRINT, pv::SOLVER_MODE_RESOLVING, pv::SOLVER_MODE_AUTOMATIC}) {
    RoutingServices host;
    auto request = Fixture().request;
    request.mutable_options()->set_solver_mode(mode);
    const auto result = send(request, 1, host);
    check(host.blueprintCalls == 0 && host.resolveCalls == 0,
          "minor1 never queries a host for multiway preflop");
    if (mode != pv::SOLVER_MODE_AUTOMATIC)
      check(result.decision_response().has_error(), "minor1 forced multiway mode refuses");
  }
  RoutingServices missing;
  missing.hit = false;
  const auto result = send(Fixture().request, 2, missing);
  check(missing.blueprintCalls == 1 && result.decision_response().has_error() &&
            result.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
        "valid six-seat request with no matching policy still refuses");
}

}  // namespace

int main() {
  positiveReplays();
  invalidSnapshots();
  envelopeRouting();
  if (failures != 0) {
    std::fprintf(stderr, "v1 N-seat preflop reconstruction: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("v1 N-seat preflop reconstruction and mechanical routing: PASS");
  return 0;
}
