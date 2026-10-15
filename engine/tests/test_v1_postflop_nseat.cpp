// RFC 0021: supplied-history reconstruction, not provider completeness or
// six-player GTO quality. Six-seat 100 BB public fixtures replay directly
// through native GameState; literal ledgers pin the independently derived
// flop root. Scripted envelope rows prove routing only, never certification.
#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/v1_protocol.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
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
using bs::poker::Phase;
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

pv::Street wireStreet(Street street) {
  switch (street) {
    case Street::Preflop:
      return pv::STREET_PREFLOP;
    case Street::Flop:
      return pv::STREET_FLOP;
    case Street::Turn:
      return pv::STREET_TURN;
    case Street::River:
      return pv::STREET_RIVER;
  }
  return pv::STREET_UNSPECIFIED;
}

GameDef handRoot(std::size_t count, std::size_t button, Chips shortStack) {
  GameDef result{};
  result.player_count = count;
  result.button = button;
  result.big_blind = 20;
  std::fill_n(result.stacks.begin(), count, Chips{2000});
  if (shortStack != 0)
    result.stacks[2] = shortStack;  // Refund rejection fixture only.
  result.pot = 30;
  result.preflop = true;
  result.terminal = bs::poker::TerminalDepth::River;
  result.board = {-1, -1, -1, 0, 0};
  result.blinds_posted[bs::poker::small_blind_seat(result)] = 10;
  result.blinds_posted[bs::poker::big_blind_seat(result)] = 20;
  return result;
}

struct Fixture {
  // hand retains the complete native hand-start definition and ledger.
  // expected is independently rooted at the native third-flop-card boundary.
  GameState hand;
  std::optional<GameState> expected;
  pv::DecisionRequest request;
  std::vector<PublicAction> history;
  std::vector<int> seats;

  explicit Fixture(std::size_t button = 0, std::size_t count = 6, Chips shortStack = 0)
      : hand(handRoot(count, button, shortStack)) {
    if (count == 6)
      seats = {0, 2, 3, 5, 7, 9};
    else
      for (std::size_t i = 0; i < count; ++i)
        seats.push_back(static_cast<int>(i));
    auto* state = request.mutable_state();
    auto* game = state->mutable_game();
    game->set_variant(pv::GAME_VARIANT_NLHE);
    game->set_betting_structure(pv::BETTING_STRUCTURE_NO_LIMIT);
    game->set_game_type(pv::GAME_TYPE_CASH);
    game->set_table_capacity(count == 6 ? 10 : count);
    game->mutable_amount_unit()->set_name("chip");
    game->mutable_amount_unit()->set_decimal_places(0);
    game->set_small_blind(10);
    game->set_big_blind(20);
    state->set_hand_id("nseat-public-whole-history");
    state->set_button_seat(seats[button]);
    *state->add_hero_hole_cards() = card(0);
    *state->add_hero_hole_cards() = card(5);
    for (std::size_t i = count; i > 0; --i) {
      auto* player = state->add_players();
      player->set_player_id(id(i - 1));
      player->set_seat(seats[i - 1]);
    }
    if (count == 6) {
      state->mutable_players()->SwapElements(0, 3);
      state->mutable_players()->SwapElements(1, 4);
    }
    auto* sb = state->add_forced_contributions();
    sb->set_player_id(id(bs::poker::small_blind_seat(hand.def())));
    sb->set_type(pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
    sb->set_amount(10);
    auto* bb = state->add_forced_contributions();
    bb->set_player_id(id(bs::poker::big_blind_seat(hand.def())));
    bb->set_type(pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
    bb->set_amount(20);
    auto* options = request.mutable_options();
    options->set_solver_mode(pv::SOLVER_MODE_BLUEPRINT);
    options->set_strategy_profile("tag");
    options->set_seed(21);
    options->set_solve_time_budget_ms(1000);
    options->set_include_sampled_action(true);
    sync();
  }

  std::string id(std::size_t actor) const { return "opaque-seat-" + std::to_string(seats[actor]); }

  pv::PlayerState* player(std::size_t actor) {
    for (auto& value : *request.mutable_state()->mutable_players())
      if (value.player_id() == id(actor))
        return &value;
    return nullptr;
  }

  void sync() {
    auto* state = request.mutable_state();
    state->set_decision_index(state->action_history_size() + 1);
    state->set_street(wireStreet(hand.street()));
    state->mutable_pot()->set_pot_total(hand.pot());
    state->mutable_pot()->set_main_pot(hand.pot());
    state->clear_board();
    for (int publicCard : hand.board())
      *state->add_board() = card(publicCard);
    for (std::size_t i = 0; i < hand.player_count(); ++i) {
      const auto& chips = hand.players()[i];
      auto* wire = player(i);
      wire->set_stack(chips.stack);
      wire->set_street_committed(chips.street_committed);
      wire->set_status(chips.folded   ? pv::PLAYER_STATUS_FOLDED
                       : chips.all_in ? pv::PLAYER_STATUS_ALL_IN
                                      : pv::PLAYER_STATUS_ACTIVE);
    }
    state->clear_legal_actions();
    state->set_to_call(0);
    if (!hand.actor())
      return;
    state->set_hero_player_id(id(*hand.actor()));
    const auto legal = hand.legal();
    if (legal.fold)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_FOLD);
    if (legal.check)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_CHECK);
    if (legal.call)
      state->add_legal_actions()->set_type(pv::ACTION_TYPE_CALL);
    state->set_to_call(legal.call_amount);
    if (legal.aggressive) {
      auto* aggressive = state->add_legal_actions();
      aggressive->set_type(hand.street() == Street::Preflop ||
                                   legal.aggressive->type == ActionType::Raise
                               ? pv::ACTION_TYPE_RAISE
                               : pv::ACTION_TYPE_BET);
      aggressive->set_min_target_total(legal.aggressive->minimum);
      aggressive->set_max_target_total(legal.aggressive->maximum);
    }
  }

  void act(Action action) {
    const std::size_t actor = *hand.actor();
    const Street street = hand.street();
    const Chips before = hand.players()[actor].stack;
    const Chips pot = hand.pot();
    hand = hand.after_action(actor, action);
    if (expected) {
      *expected = expected->after_action(actor, action);
      history.push_back({street, actor, action});
    }
    auto* state = request.mutable_state();
    auto* event = state->add_action_history();
    event->set_sequence(state->action_history_size());
    event->set_street(wireStreet(street));
    event->set_actor_player_id(id(actor));
    event->set_pot_before(pot);
    event->set_stack_after(hand.players()[actor].stack);
    switch (action.type) {
      case ActionType::Fold:
        event->set_action(pv::ACTION_TYPE_FOLD);
        break;
      case ActionType::Check:
        event->set_action(pv::ACTION_TYPE_CHECK);
        break;
      case ActionType::Call:
        event->set_action(pv::ACTION_TYPE_CALL);
        event->set_incremental_amount(before - hand.players()[actor].stack);
        break;
      case ActionType::Bet:
      case ActionType::Raise:
        event->set_action(street == Street::Preflop || action.type == ActionType::Raise
                              ? pv::ACTION_TYPE_RAISE
                              : pv::ACTION_TYPE_BET);
        event->set_target_total(action.target_total);
        event->set_incremental_amount(before - hand.players()[actor].stack);
        break;
    }
    sync();
  }

  void deal(int publicCard) {
    hand = hand.after_card(publicCard);
    if (expected)
      *expected = expected->after_card(publicCard);
    else if (hand.board().size() == 3) {
      GameDef flop{};
      flop.player_count = hand.player_count();
      flop.button = hand.def().button;
      flop.big_blind = hand.def().big_blind;
      flop.pot = hand.pot();
      flop.board_size = 3;
      std::copy(hand.board().begin(), hand.board().end(), flop.board.begin());
      for (std::size_t i = 0; i < hand.player_count(); ++i) {
        flop.stacks[i] = hand.players()[i].stack;
        flop.contributions[i] = hand.players()[i].net_contributed();
        flop.folded[i] = hand.players()[i].folded;
      }
      expected.emplace(flop);
    }
    sync();
  }

  void flop() {
    deal(12);
    deal(17);
    deal(22);
  }

  void limpedFlop() {
    for (std::size_t i = 0; i + 1 < hand.player_count(); ++i)
      act({ActionType::Call});
    act({ActionType::Check});
    flop();
  }

  void foldedFlop() {
    act({ActionType::Fold});
    act({ActionType::Call});
    act({ActionType::Fold});
    act({ActionType::Call});
    act({ActionType::Fold});
    act({ActionType::Check});
    flop();
  }

  void checkThrough() {
    while (hand.phase() == Phase::Action)
      act({ActionType::Check});
  }
};

void matches(const Fixture& fixture, const std::string& name, bool resolve = false,
             std::uint32_t minor = 2) {
  ReconstructedPostflop actual;
  V1BlueprintMiss miss = V1BlueprintMiss::OffTree;
  bool success = false;
  try {
    success = resolve ? bs::v1::reconstructPostflopForResolve(fixture.request, actual, miss, minor)
                      : bs::v1::reconstructPostflop(fixture.request, actual, miss, minor);
  } catch (...) {
    check(false, name + " does not throw");
  }
  check(success && actual.state && miss == V1BlueprintMiss::None, name + " reconstructs");
  if (!success || !actual.state || !fixture.expected)
    return;
  const auto& a = *actual.state;
  const auto& b = *fixture.expected;
  check(bs::poker::same_game_def(a.def(), b.def()), name + " exact true flop root identity");
  check(a.player_count() == fixture.hand.player_count() && a.street() == fixture.hand.street() &&
            a.phase() == fixture.hand.phase() && a.actor() == fixture.hand.actor() &&
            actual.hero_actor == *fixture.hand.actor() && a.pot() == fixture.hand.pot(),
        name + " full native hand agrees with reconstructed decision node");
  check(a.last_full_raise() == b.last_full_raise(), name + " last full raise");
  check(std::equal(a.board().begin(), a.board().end(), fixture.hand.board().begin(),
                   fixture.hand.board().end()),
        name + " complete ordered native board");
  for (std::size_t i = 0; i < a.player_count(); ++i) {
    const auto& x = a.players()[i];
    const auto& y = b.players()[i];
    const auto& whole = fixture.hand.players()[i];
    check(x.stack == whole.stack && x.street_committed == whole.street_committed &&
              x.contributed == whole.contributed && x.refunded == whole.refunded &&
              x.folded == whole.folded && x.all_in == whole.all_in,
          name + " whole-hand seat ledger " + std::to_string(i));
    check(x.pending == y.pending && x.raise_rights == y.raise_rights &&
              a.can_raise(i) == b.can_raise(i),
          name + " native rebased raise rights " + std::to_string(i));
  }
  const auto legal = a.legal();
  const auto expectedLegal = b.legal();
  check(legal.fold == expectedLegal.fold && legal.check == expectedLegal.check &&
            legal.call == expectedLegal.call && legal.call_amount == expectedLegal.call_amount,
        name + " passive legal menu");
  check(legal.aggressive.has_value() == expectedLegal.aggressive.has_value(),
        name + " raise admission");
  if (legal.aggressive && expectedLegal.aggressive)
    check(legal.aggressive->type == expectedLegal.aggressive->type &&
              legal.aggressive->minimum == expectedLegal.aggressive->minimum &&
              legal.aggressive->maximum == expectedLegal.aggressive->maximum &&
              legal.aggressive->all_in_only == expectedLegal.aggressive->all_in_only,
          name + " exact raise interval");
  check(actual.hero_cards == std::array<int, 2>{0, 5}, name + " only declared hero cards");
  check(actual.history.size() == fixture.history.size(),
        name + " service history excludes preflop");
  for (std::size_t i = 0; i < actual.history.size() && i < fixture.history.size(); ++i)
    check(actual.history[i].street == fixture.history[i].street &&
              actual.history[i].seat == fixture.history[i].seat &&
              actual.history[i].action == fixture.history[i].action &&
              actual.history[i].street != Street::Preflop,
          name + " exact postflop event " + std::to_string(i));
}

void rejected(const pv::DecisionRequest& request, const std::string& name, bool resolve = false,
              std::uint32_t minor = 2, V1BlueprintMiss expectedMiss = V1BlueprintMiss::None) {
  ReconstructedPostflop actual;
  actual.state.emplace(handRoot(6, 0, 0));
  actual.history.push_back({Street::Flop, 1, {ActionType::Check}});
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  bool success = true;
  try {
    success = resolve ? bs::v1::reconstructPostflopForResolve(request, actual, miss, minor)
                      : bs::v1::reconstructPostflop(request, actual, miss, minor);
  } catch (...) {
    check(false, name + " rejects without throwing");
  }
  check(!success && miss != V1BlueprintMiss::None, name + " rejects");
  if (expectedMiss != V1BlueprintMiss::None)
    check(miss == expectedMiss, name + " exact typed miss");
  check(!actual.state && actual.history.empty() && !actual.oracle_state,
        name + " clears unusable partial output");
}

void positiveHistories() {
  for (std::size_t button = 0; button < 6; ++button) {
    Fixture f(button);
    f.foldedFlop();
    const auto& root = f.expected->def();
    check(root.player_count == 6 && root.button == button && root.pot == 70 && root.pot % 6 != 0 &&
              *f.expected->actor() == (button + 2) % 6,
          "literal folded six-seat flop position/pot");
    const std::array<Chips, 6> contribution{20, 10, 20, 0, 20, 0};
    const std::array<bool, 6> folded{false, true, false, true, false, true};
    for (std::size_t relative = 0; relative < 6; ++relative) {
      const std::size_t seat = (button + relative) % 6;
      check(root.contributions[seat] == contribution[relative] &&
                root.stacks[seat] == 2000 - contribution[relative] &&
                root.folded[seat] == folded[relative] && root.blinds_posted[seat] == 0 &&
                root.street_committed[seat] == 0,
            "literal flop ledger/button-relative seat " + std::to_string(relative));
    }
    matches(f, "all-button folded flop " + std::to_string(button));
    matches(f, "all-button folded flop resolver " + std::to_string(button), true);
    f.checkThrough();
    f.deal(27);
    matches(f, "all-button complete turn " + std::to_string(button));
    f.checkThrough();
    f.deal(32);
    matches(f, "all-button complete river " + std::to_string(button));
    f.act({ActionType::Check});
    f.act({ActionType::Bet, 20});
    f.act({ActionType::Raise, 60});
    f.act({ActionType::Call});
    matches(f, "all-button river reraise response " + std::to_string(button));
    check(f.hand.pot() == 210 && f.hand.legal().call_amount == 40 &&
              *f.hand.actor() == (button + 4) % 6,
          "literal river pot210/call40 and correct six-seat actor");
  }
  for (std::size_t count = 2; count <= 10; ++count) {
    Fixture f(0, count);
    f.limpedFlop();
    matches(f, "complete limped flop seat count " + std::to_string(count));
  }

  Fixture allIn;
  allIn.limpedFlop();
  allIn.act({ActionType::Bet, 980});
  allIn.act({ActionType::Raise, 1960});
  allIn.act({ActionType::Raise, 1980});
  check(!allIn.expected->players()[2].raise_rights && allIn.expected->players()[1].raise_rights,
        "postflop short all-in preserves asymmetric prior raise rights");
  matches(allIn, "postflop short-all-in prefix", true);
  for (int i = 0; i < 3; ++i)
    allIn.act({ActionType::Fold});
  allIn.act({ActionType::Call});
  matches(allIn, "postflop short-all-in final response", true);
  check(allIn.hand.pot() == 6040 && *allIn.hand.actor() == 2 &&
            allIn.hand.players()[2].stack == 20 && allIn.hand.legal().call_amount == 20 &&
            !allIn.hand.can_raise(2) && allIn.hand.last_full_raise() == 980,
        "literal short-all-in pot6040/remaining20/call20 and no reopened raise");
  rejected(allIn.request, "blueprint retains non-hero-all-in restriction");
}

void invalidHistories() {
  Fixture base;
  base.foldedFlop();
  auto bad = base.request;
  bad.mutable_state()->mutable_action_history()->DeleteSubrange(0, 1);
  rejected(bad, "missing preflop fold cannot fabricate a live-set boundary");
  bad = base.request;
  bad.mutable_state()->mutable_action_history()->DeleteSubrange(5, 1);
  rejected(bad, "missing big-blind check cannot fabricate the flop boundary");
  bad = base.request;
  bad.mutable_state()->mutable_action_history()->SwapElements(0, 1);
  rejected(bad, "reordered preflop actors");
  bad = base.request;
  bad.mutable_state()->mutable_action_history(0)->set_actor_player_id("unknown-player");
  rejected(bad, "unknown preflop actor");
  bad = base.request;
  bad.mutable_state()->mutable_action_history(1)->set_street(pv::STREET_FLOP);
  rejected(bad, "street moves forward then backward");
  bad = base.request;
  bad.mutable_state()->mutable_pot()->set_pot_total(71);
  rejected(bad, "wrong final pot");
  bad = base.request;
  bad.mutable_state()->set_to_call(1);
  rejected(bad, "wrong final call amount");
  bad = base.request;
  bad.mutable_state()->set_hero_player_id(base.id(4));
  rejected(bad, "hero not final actor");
  for (auto status :
       {pv::PLAYER_STATUS_ACTIVE, pv::PLAYER_STATUS_ALL_IN, pv::PLAYER_STATUS_UNSPECIFIED}) {
    bad = base.request;
    for (auto& player : *bad.mutable_state()->mutable_players())
      if (player.player_id() == base.id(1))
        player.set_status(status);
    rejected(bad, "preflop fold status cannot be erased or replaced " + std::to_string(status));
  }
  bad = base.request;
  bad.mutable_state()->mutable_players(0)->set_street_committed(1);
  rejected(bad, "wrong current street commitment");
  bad = base.request;
  bad.mutable_state()->mutable_hero_hole_cards(0)->set_suit(static_cast<pv::Suit>(5));
  rejected(bad, "unknown hero suit cannot alias native ID");
  bad = base.request;
  bad.mutable_state()->mutable_board(0)->set_rank(static_cast<pv::Rank>(99));
  rejected(bad, "unknown public rank");
  bad = base.request;
  *bad.mutable_state()->mutable_board(1) = bad.state().board(0);
  rejected(bad, "duplicate public cards");
  bad = base.request;
  *bad.mutable_state()->mutable_board(0) = bad.state().hero_hole_cards(0);
  rejected(bad, "public card collides with hero card");
  bad = base.request;
  bad.mutable_state()->mutable_board()->RemoveLast();
  rejected(bad, "partial flop is not an action node");
  bad = base.request;
  bad.mutable_state()->mutable_forced_contributions(0)->set_player_id(base.id(2));
  bad.mutable_state()->mutable_forced_contributions(1)->set_player_id(base.id(1));
  rejected(bad, "swapped preflop blind seats");
  bad = base.request;
  bad.mutable_state()->mutable_forced_contributions(0)->set_type(
      pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND);
  bad.mutable_state()->mutable_forced_contributions(1)->set_type(
      pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND);
  rejected(bad, "swapped blind types cannot preserve amounts only");
  bad = base.request;
  bad.mutable_state()->mutable_action_history(1)->set_pot_before(31);
  rejected(bad, "wrong preflop optional pot ledger");
  bad = base.request;
  bad.mutable_state()->mutable_action_history(1)->set_stack_after(1979);
  rejected(bad, "wrong preflop optional stack ledger");

  Fixture turn;
  turn.foldedFlop();
  turn.checkThrough();
  turn.deal(27);
  bad = turn.request;
  bad.mutable_state()->mutable_action_history()->DeleteSubrange(6, 1);
  rejected(bad, "missing flop check cannot silently advance to turn");
  bad = turn.request;
  bad.mutable_state()->mutable_board()->RemoveLast();
  rejected(bad, "turn street requires turn card");
  bad = turn.request;
  bad.mutable_state()->mutable_action_history(6)->set_street(pv::STREET_TURN);
  rejected(bad, "turn action cannot precede completed flop betting");

  Fixture calls;
  calls.limpedFlop();
  calls.act({ActionType::Bet, 20});
  calls.act({ActionType::Call});
  calls.act({ActionType::Raise, 60});
  for (int i = 0; i < 5; ++i)
    calls.act({ActionType::Call});
  calls.deal(27);
  matches(calls, "valid complete repeated postflop calls");
  check(calls.hand.pot() == 480, "six seats each contributed 20 preflop and 60 flop");
  bad = calls.request;
  bad.mutable_state()->mutable_action_history(7)->set_incremental_amount(19);
  bad.mutable_state()->mutable_action_history(13)->set_incremental_amount(41);
  for (auto& event : *bad.mutable_state()->mutable_action_history()) {
    event.set_pot_before(0);
    event.set_stack_after(0);
  }
  rejected(bad, "postflop CALL19/41 cannot cancel native CALL20/40");
  rejected(bad, "resolver also rejects offsetting individual call errors", true);

  Fixture aggression;
  aggression.limpedFlop();
  aggression.act({ActionType::Bet, 20});
  aggression.act({ActionType::Raise, 60});
  matches(aggression, "valid postflop opening bet and raise");
  bad = aggression.request;
  bad.mutable_state()->mutable_action_history(7)->set_action(pv::ACTION_TYPE_BET);
  rejected(bad, "postflop BET cannot disguise a raise facing20", false, 2,
           V1BlueprintMiss::OffTree);
  rejected(bad, "resolver rejects wrong postflop BET kind", true, 2, V1BlueprintMiss::OffTree);
  bad = aggression.request;
  bad.mutable_state()->mutable_action_history(6)->set_action(pv::ACTION_TYPE_RAISE);
  rejected(bad, "postflop RAISE cannot disguise opening BET", false, 2, V1BlueprintMiss::OffTree);
  rejected(bad, "resolver rejects wrong postflop RAISE kind", true, 2, V1BlueprintMiss::OffTree);

  // A real refund is produced by the native engine, not fabricated in wire
  // fields. The short stack here is solely a negative utility-profile fixture.
  Fixture refund(0, 6, 100);
  refund.limpedFlop();
  refund.act({ActionType::Bet, 200});
  refund.act({ActionType::Call});
  for (int i = 0; i < 4; ++i)
    refund.act({ActionType::Fold});
  check(refund.hand.players()[1].refunded == 120 && refund.hand.pot() == 280,
        "native unmatched flop wager refunds exactly120 before turn");
  refund.deal(27);
  // The native N-seat profile still opens the next street for the only
  // active seat's check. Confirm that hero is active and really acting:
  // an inactive hero would reject before replay, hiding the refund guard.
  refund.request.mutable_state()->set_hero_player_id(refund.id(1));
  check(refund.player(1)->status() == pv::PLAYER_STATUS_ACTIVE && refund.player(1)->stack() > 0 &&
            refund.hand.actor() == std::optional<std::size_t>{1},
        "refund negative reaches replay without an inactive-hero admission miss");
  // Clear optional checks so an earlier stack-after discrepancy does not
  // masquerade as the specific native refund admission guard.
  for (auto& event : *refund.request.mutable_state()->mutable_action_history()) {
    event.set_pot_before(0);
    event.set_stack_after(0);
  }
  rejected(refund.request, "native postflop refund is unsupported", true, 2,
           V1BlueprintMiss::RootNotSupported);
  bad = refund.request;
  bad.mutable_state()->mutable_pot()->set_pot_total(400);
  for (auto& player : *bad.mutable_state()->mutable_players())
    if (player.player_id() == refund.id(1))
      player.set_stack(player.stack() + 120);
  for (auto& event : *bad.mutable_state()->mutable_action_history()) {
    event.set_pot_before(0);
    event.set_stack_after(0);
  }
  rejected(bad, "inflated aggregate ledgers cannot conceal native refunds", true, 2,
           V1BlueprintMiss::RootNotSupported);

  Fixture preflopRefund(0, 6, 100);
  preflopRefund.act({ActionType::Raise, 200});
  for (int i = 0; i < 4; ++i)
    preflopRefund.act({ActionType::Fold});
  preflopRefund.act({ActionType::Call});
  check(preflopRefund.hand.players()[3].refunded == 100 && preflopRefund.hand.pot() == 210,
        "native preflop refund100 includes folded small-blind dead money10");
  preflopRefund.flop();
  preflopRefund.request.mutable_state()->set_hero_player_id(preflopRefund.id(3));
  check(preflopRefund.player(3)->status() == pv::PLAYER_STATUS_ACTIVE &&
            preflopRefund.player(3)->stack() > 0 &&
            preflopRefund.hand.actor() == std::optional<std::size_t>{3},
        "preflop refund negative has no earlier inactive-hero rejection");
  for (auto& event : *preflopRefund.request.mutable_state()->mutable_action_history()) {
    event.set_pot_before(0);
    event.set_stack_after(0);
  }
  rejected(preflopRefund.request, "native refund cannot be discarded during flop rebase", true, 2,
           V1BlueprintMiss::RootNotSupported);

  for (int utility = 0; utility < 4; ++utility) {
    bad = base.request;
    if (utility == 0)
      bad.mutable_state()->mutable_game()->set_ante(1);
    else if (utility == 1)
      bad.mutable_state()->mutable_game()->mutable_rake()->set_basis_points(100);
    else if (utility == 2)
      bad.mutable_state()->mutable_game()->mutable_straddle()->set_enabled(true);
    else
      bad.mutable_state()->mutable_pot()->add_side_pots()->set_amount(1);
    rejected(bad, "unsupported utility profile " + std::to_string(utility));
    rejected(bad, "resolver unsupported utility profile " + std::to_string(utility), true);
  }
  rejected(base.request, "minor1 folded flop root remains unsupported", false, 1);
  rejected(base.request, "minor1 resolver does not gain strict unequal root", true, 1);
  ReconstructedPostflop out;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  check(!bs::v1::reconstructPostflop(base.request, out, miss), "default remains minor1 admission");
}

void legacyCompatibility() {
  Fixture f;
  f.limpedFlop();
  f.request.mutable_state()->clear_action_history();
  for (std::uint32_t minor : {0U, 1U, 2U}) {
    matches(f, "legacy postflop-only six-seat minor " + std::to_string(minor), false, minor);
    matches(f, "legacy postflop-only resolver minor " + std::to_string(minor), true, minor);
  }
  ReconstructedPostflop out;
  V1BlueprintMiss miss = V1BlueprintMiss::OffTree;
  check(bs::v1::reconstructPostflop(f.request, out, miss),
        "legacy post-only direct default survives");
}

// Deterministic seam only: no artifact, range propagation, training or solve.
class MechanicalHost final : public bs::v1::V1HostServices {
 public:
  mutable int blueprintCalls = 0;
  mutable int resolveCalls = 0;
  mutable bool receivedTrueRoot = false;
  bool hit = true;
  std::array<Action, 2> actions{{{ActionType::Check}, {ActionType::Bet, 20}}};
  std::array<double, 2> probabilities{0.5, 0.5};
  const std::string digest = std::string(64, 'c');

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
  void observe(const GameState& state, std::span<const PublicAction> history) const noexcept {
    receivedTrueRoot = state.player_count() == 6 && state.def().button == 0 &&
                       state.def().pot == 70 && state.def().contributions[1] == 10 &&
                       state.def().folded[1] && state.def().folded[3] && state.def().folded[5] &&
                       state.actor() == std::optional<std::size_t>{2} && history.empty();
  }
  bs::v1::V1BlueprintResult blueprintHeroDecision(const GameState& state,
                                                  std::span<const PublicAction> history,
                                                  const std::array<int, 2>&,
                                                  std::string_view) const noexcept override {
    ++blueprintCalls;
    observe(state, history);
    bs::v1::V1BlueprintResult result;
    result.hit = hit;
    result.miss = hit ? V1BlueprintMiss::None : V1BlueprintMiss::RootNotSupported;
    if (hit)
      result.row = row();
    return result;
  }
  bs::v1::V1ResolveResult resolvingDecision(const GameState& state,
                                            std::span<const PublicAction> history,
                                            const std::array<int, 2>&, std::string_view,
                                            std::uint32_t deadline) const noexcept override {
    ++resolveCalls;
    observe(state, history);
    receivedTrueRoot = receivedTrueRoot && deadline == 1000;
    bs::v1::V1ResolveResult result;
    result.outcome = bs::v1::V1ResolveOutcome::DeadlineBlueprint;
    result.row = row();
    return result;
  }
};

pv::Envelope send(pv::DecisionRequest request, std::uint32_t minor, const MechanicalHost& host) {
  pv::Envelope input;
  input.set_protocol_minor(minor);
  input.set_request_id("supplied-history-mechanical-test");
  *input.mutable_decision_request() = std::move(request);
  const auto result = bs::v1::handleEnvelope(input.SerializeAsString(), host);
  check(result.outcome == bs::v1::EnvelopeOutcome::Respond, "mechanical route returns frame");
  pv::Envelope output;
  check(output.ParseFromString(result.response), "mechanical route parses response");
  return output;
}

void envelopeRouting() {
  Fixture f;
  f.foldedFlop();
  for (auto mode :
       {pv::SOLVER_MODE_BLUEPRINT, pv::SOLVER_MODE_AUTOMATIC, pv::SOLVER_MODE_RESOLVING}) {
    MechanicalHost host;
    auto request = f.request;
    request.mutable_options()->set_solver_mode(mode);
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_APPROXIMATE);
    auto result = send(request, 2, host);
    check(host.receivedTrueRoot &&
              host.blueprintCalls == (mode == pv::SOLVER_MODE_RESOLVING ? 0 : 1) &&
              host.resolveCalls == (mode == pv::SOLVER_MODE_RESOLVING ? 1 : 0),
          "minor2 routes true flop root and no preflop service history exactly once");
    check(result.decision_response().has_expanded_strategy() &&
              result.decision_response().expanded_strategy().solver().guarantee_level() ==
                  "approximate",
          "scripted row is approximate, never a quality/certification claim");
    request.mutable_options()->set_minimum_guarantee(pv::GUARANTEE_LEVEL_CERTIFIED_BOUND);
    result = send(request, 2, host);
    check(result.decision_response().has_error() &&
              result.decision_response().error().code() == pv::ERROR_CODE_GUARANTEE_BELOW_REQUEST,
          "scripted approximate route cannot satisfy a certified floor");
    MechanicalHost legacy;
    auto legacyRequest = f.request;
    legacyRequest.mutable_options()->set_solver_mode(mode);
    result = send(legacyRequest, 1, legacy);
    check(legacy.blueprintCalls == 0 && legacy.resolveCalls == 0,
          "minor1 does not query host for newly reconstructed folded root");
    if (mode != pv::SOLVER_MODE_AUTOMATIC)
      check(result.decision_response().has_error(), "minor1 forced folded-root mode still refuses");
  }
  MechanicalHost miss;
  miss.hit = false;
  auto result = send(f.request, 2, miss);
  check(result.decision_response().has_error() &&
            result.decision_response().error().code() == pv::ERROR_CODE_UNSUPPORTED_FEATURE,
        "valid supplied history cannot invent an unavailable six-seat policy");
  auto automatic = f.request;
  automatic.mutable_options()->set_solver_mode(pv::SOLVER_MODE_AUTOMATIC);
  result = send(automatic, 2, miss);
  check(miss.blueprintCalls == 2 && miss.resolveCalls == 1 && miss.receivedTrueRoot &&
            result.decision_response().has_expanded_strategy(),
        "automatic miss forwards true root to mock deadline-baseline resolver");
}

}  // namespace

int main() {
  try {
    positiveHistories();
    invalidHistories();
    legacyCompatibility();
    envelopeRouting();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Unexpected native fixture exception: %s\n", error.what());
    return 1;
  }
  if (failures != 0) {
    std::fprintf(stderr, "v1 N-seat supplied-history postflop: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("v1 N-seat supplied-history postflop and mechanical routing: PASS");
  return 0;
}
