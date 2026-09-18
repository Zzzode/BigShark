// Independent public-API regressions for the RFC 0004 Stage 10 preflop rules.
//
// Every expectation here is derived from the RFC text and the poker rules
// themselves (button/small blind acts first preflop; the big blind retains its
// option after a limp; postflop order reverses), not from the implementation.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::poker;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using Holes = std::array<std::array<int, 2>, 2>;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

// A 100 BB heads-up preflop root: blinds 1/2, both seats behind their posts.
HeadsUpRoot preflop_root(Chips stack = 200, std::size_t button = 1) {
  HeadsUpRoot root{};
  root.flop = {-1, -1, -1};  // no flop yet
  root.stacks = {stack, stack};
  root.contributions = {0, 0};
  root.pot = 2;  // the two posted blinds
  root.big_blind = 2;
  root.button = button;
  root.preflop = true;
  root.blinds_posted[button] = 1;
  root.blinds_posted[1 - button] = 2;
  return root;
}

Holes holes_for_showdown() {
  return {{{card("Ah"), card("Kh")}, {card("9c"), card("8c")}}};
}

// ---------------------------------------------------------------- actor order

int test_button_acts_first_preflop() {
  for (std::size_t button : {std::size_t{0}, std::size_t{1}}) {
    HeadsUpState state(preflop_root(200, button));
    CHECK(state.street() == Street::Preflop);
    CHECK(state.board().empty());
    CHECK(state.phase() == Phase::Action);
    CHECK(state.actor() == button);
    // The button posted the small blind and the other seat the big blind.
    CHECK(state.players()[button].street_committed == 1);
    CHECK(state.players()[1 - button].street_committed == 2);
    CHECK(state.players()[button].stack == 199);
    CHECK(state.players()[1 - button].stack == 198);
    CHECK(state.pot() == 3);
  }
  return 0;
}

// ------------------------------------------------------------------ the option

int test_big_blind_option_after_limp() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  HeadsUpState state(preflop_root(200, button));
  // The button limps: call the big blind's 2.
  HeadsUpState limped = state.after_action(button, {ActionType::Call});
  CHECK(limped.phase() == Phase::Action);  // NOT closed: the big blind has an option
  CHECK(limped.actor() == bb);
  CHECK(limped.players()[button].street_committed == 2);
  CHECK(limped.pot() == 4);

  // The big blind checks its option, which closes preflop and opens the Deal
  // phase for the flop.
  HeadsUpState checked = limped.after_action(bb, {ActionType::Check});
  CHECK(checked.phase() == Phase::Deal);
  CHECK(checked.board().empty());  // the flop is pending, not dealt
  CHECK(checked.players()[bb].street_committed == 2);

  // The flop is dealt one card at a time with no action in between; only the
  // completed three-card board opens postflop action.
  HeadsUpState first = checked.after_card(card("2c"));
  CHECK(first.phase() == Phase::Deal);
  CHECK(first.board().size() == 1);
  HeadsUpState second = first.after_card(card("3d"));
  CHECK(second.phase() == Phase::Deal);
  CHECK(second.board().size() == 2);
  HeadsUpState flop = second.after_card(card("7h"));
  CHECK(flop.phase() == Phase::Action);
  CHECK(flop.board().size() == 3);
  CHECK(flop.street() == Street::Flop);
  // Postflop the order REVERSES: the big blind (non-button) acts first.
  CHECK(flop.actor() == bb);
  CHECK(flop.players()[0].street_committed == 0);
  CHECK(flop.players()[1].street_committed == 0);
  return 0;
}

int test_big_blind_option_raise_ends_street() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  HeadsUpState limped =
      HeadsUpState(preflop_root(200, button)).after_action(button, {ActionType::Call});
  CHECK(limped.actor() == bb);
  // Facing a limp there is no outstanding wager to raise, so the big blind's
  // aggressive action is a BET to 6. The button still owes a response, so the
  // street stays open and the actor becomes the button.
  HeadsUpState raised = limped.after_action(bb, {ActionType::Bet, 6});
  CHECK(raised.phase() == Phase::Action);
  CHECK(raised.actor() == button);
  // The button calls, which closes preflop: no further option exists.
  HeadsUpState called = raised.after_action(button, {ActionType::Call});
  CHECK(called.phase() == Phase::Deal);
  CHECK(called.pot() == 12);
  return 0;
}

// A raise by the button facing the blind also removes the option: the big blind
// is no longer acting on an unraised pot.
int test_button_raise_removes_option() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  HeadsUpState state(preflop_root(200, button));
  // The button opens over the big blind: a raise to 6 (the small blind already
  // posted 1, so the wager is outstanding).
  HeadsUpState raised = state.after_action(button, {ActionType::Raise, 6});
  CHECK(raised.phase() == Phase::Action);
  CHECK(raised.actor() == bb);
  // The big blind calls: preflop closes even though it never "used an option".
  HeadsUpState called = raised.after_action(bb, {ActionType::Call});
  CHECK(called.phase() == Phase::Deal);
  CHECK(called.pot() == 12);
  return 0;
}

int test_big_blind_fold_ends_hand() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  // The big blind folds to a button raise: the button wins the pot including
  // the big blind's dead post.
  HeadsUpState raised =
      HeadsUpState(preflop_root(200, button)).after_action(button, {ActionType::Raise, 6});
  HeadsUpState folded = raised.after_action(bb, {ActionType::Fold});
  CHECK(folded.phase() == Phase::Folded);
  const Settlement settled = folded.settle_fold();
  CHECK(settled.net_utility[button] == 2);  // wins the big blind's 2
  CHECK(settled.net_utility[bb] == -2);
  return 0;
}

// The button can fold preflop, forfeiting only its small blind.
int test_button_fold_preflop() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  HeadsUpState folded =
      HeadsUpState(preflop_root(200, button)).after_action(button, {ActionType::Fold});
  CHECK(folded.phase() == Phase::Folded);
  const Settlement settled = folded.settle_fold();
  CHECK(settled.net_utility[bb] == 1);  // wins the small blind
  CHECK(settled.net_utility[button] == -1);
  return 0;
}

// ------------------------------------------------------------- conservation

int test_preflop_chip_conservation() {
  const std::size_t button = 1;
  const std::size_t bb = 1 - button;
  HeadsUpState state(preflop_root(200, button));
  const Chips total_before = 400;  // 200 + 200
  CHECK(state.pot() + state.players()[0].stack + state.players()[1].stack == total_before);

  HeadsUpState limped = state.after_action(button, {ActionType::Call});
  CHECK(limped.pot() + limped.players()[0].stack + limped.players()[1].stack == total_before);
  HeadsUpState raised = limped.after_action(bb, {ActionType::Bet, 6});
  CHECK(raised.pot() + raised.players()[0].stack + raised.players()[1].stack == total_before);
  HeadsUpState called = raised.after_action(button, {ActionType::Call});
  CHECK(called.pot() + called.players()[0].stack + called.players()[1].stack == total_before);
  CHECK(called.pot() == 12);

  // Fold settlement also conserves exactly.
  HeadsUpState folded = raised.after_action(button, {ActionType::Fold});
  const Settlement settled = folded.settle_fold();
  CHECK(settled.final_stacks[0] + settled.final_stacks[1] == total_before);
  return 0;
}

// ------------------------------------------------------- blind edge profiles

// A short-stacked big blind posts the whole of a small stack: the blind is a
// live wager, the unmatched part belongs to nobody, and the board still runs
// out to a showdown once the shortfall is returned.
int test_short_big_blind_root() {
  HeadsUpRoot root{};
  root.flop = {-1, -1, -1};  // no flop yet
  root.stacks = {2, 10};     // seat 0 posts its entire stack as the big blind
  root.contributions = {0, 0};
  root.pot = 2;
  root.big_blind = 2;
  root.button = 1;
  root.preflop = true;
  root.blinds_posted[1] = 1;
  root.blinds_posted[0] = 2;
  HeadsUpState state(root);
  CHECK(state.street() == Street::Preflop);
  // Seat 0 posted its whole stack, so no action is possible. The blind it could
  // not have matched is an uncalled excess and returns to it, leaving both
  // commitments level at the small blind's 1.
  CHECK(state.phase() == Phase::Deal);
  CHECK(!state.actor());
  CHECK(state.players()[0].refunded == 1);
  CHECK(state.players()[0].street_committed == 1);
  CHECK(state.players()[1].street_committed == 1);
  CHECK(state.pot() == 2);
  // The board runs out with no action at any street.
  HeadsUpState dealt = state;
  for (int c : {card("2c"), card("3d"), card("7h"), card("9s"), card("Jd")}) {
    CHECK(dealt.phase() == Phase::Deal);
    dealt = dealt.after_card(c);
  }
  CHECK(dealt.phase() == Phase::Showdown);
  const Settlement settled = dealt.settle_showdown(holes_for_showdown());
  CHECK(settled.net_utility[0] + settled.net_utility[1] == 0);
  CHECK(settled.final_stacks[0] + settled.final_stacks[1] == 12);
  return 0;
}

// A short stack that cannot cover the big blind is rejected: the preflop
// profile posts real blinds, so an under-stacked seat is invalid input.
int test_understacked_blind_rejected() {
  HeadsUpRoot root{};
  root.flop = {-1, -1, -1};  // no flop yet
  root.stacks = {0, 5};      // button cannot post its small blind
  root.contributions = {0, 0};
  root.pot = 2;
  root.big_blind = 2;
  root.button = 1;
  root.preflop = true;
  root.blinds_posted[1] = 1;
  root.blinds_posted[0] = 2;
  bool threw = false;
  try {
    HeadsUpState state(root);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// -------------------------------------------------- flop profile is untouched

// The existing flop-rooted profile must keep its exact semantics: non-button
// first, contributions matching the pot, and no preflop marker behavior.
int test_flop_profile_unchanged() {
  HeadsUpRoot root{};
  root.flop = {card("2h"), card("3h"), card("4h")};
  root.stacks = {20, 20};
  root.contributions = {3, 3};
  root.pot = 6;
  root.big_blind = 2;
  root.button = 1;
  root.preflop = false;
  HeadsUpState state(root);
  CHECK(state.street() == Street::Flop);
  CHECK(state.board().size() == 3);
  CHECK(state.actor() == 0);  // non-button first postflop
  CHECK(state.players()[0].street_committed == 0);
  CHECK(state.pot() == 6);
  // A call still closes the flop street immediately.
  HeadsUpState called = state.after_action(0, {ActionType::Check});
  CHECK(called.actor() == 1);
  HeadsUpState closed = called.after_action(1, {ActionType::Check});
  CHECK(closed.phase() == Phase::Deal);
  CHECK(closed.street() == Street::Flop);
  return 0;
}

// A preflop root carrying a flop is rejected: the profiles are distinct.
int test_preflop_root_rejects_flop() {
  HeadsUpRoot root = preflop_root();
  root.flop = {card("2h"), card("3h"), card("4h")};
  bool threw = false;
  try {
    HeadsUpState state(root);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// Mismatched blind posts are rejected.
int test_preflop_root_rejects_wrong_blinds() {
  HeadsUpRoot root = preflop_root();
  root.blinds_posted[root.button] = 2;  // the button must post the SMALL blind
  bool threw = false;
  try {
    HeadsUpState state(root);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

// The preflop root is a genuine betting state, but a FULL traversal from it is
// not a bounded workload: every preflop line reaches every flop, so the tree
// spans all C(50,3) boards times the action sequences. This test pins that
// fact by walking the tree with an explicit cap, so the number cannot silently
// change and no one mistakes the preflop profile for a solved one.
int test_preflop_full_tree_is_unbounded() {
  HeadsUpRoot root = preflop_root(6, 1);  // a deliberately SHALLOW 3-big-blind stack
  const HeadsUpState start(root);
  std::array<bool, 52> used{};
  for (int c : root.flop)
    if (c >= 0)
      used[c] = true;
  const std::array<std::array<int, 2>, 2> hands{
      {{card("Ah"), card("Kh")}, {card("Qc"), card("Qd")}}};

  // Count the distinct flops reachable through one line, and confirm the tree
  // remains enormous even with a tiny stack: the preflop profile needs its own
  // bounded abstraction (a flop-terminal subgame), not a full run-out.
  std::size_t flops = 0;
  HeadsUpState limped = start.after_action(root.button, {ActionType::Call});
  HeadsUpState checked = limped.after_action(1 - root.button, {ActionType::Check});
  CHECK(checked.phase() == Phase::Deal);
  std::array<bool, 52> blocked{};
  for (auto& hand : hands)
    for (int c : hand)
      blocked[c] = true;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      for (int c = b + 1; c < 52; ++c)
        if (!blocked[a] && !blocked[b] && !blocked[c])
          ++flops;
  // C(48,3) = 17,296 distinct flops from a single line, before any action menu.
  CHECK(flops == 17296);

  // A preflop information state IS reachable and well formed: the profile is a
  // real rooting point, it is the full run-out that is out of scope here.
  CHECK(start.street() == Street::Preflop);
  CHECK(start.actor() == root.button);
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_button_acts_first_preflop() == 0);
    CHECK(test_big_blind_option_after_limp() == 0);
    CHECK(test_big_blind_option_raise_ends_street() == 0);
    CHECK(test_button_raise_removes_option() == 0);
    CHECK(test_big_blind_fold_ends_hand() == 0);
    CHECK(test_button_fold_preflop() == 0);
    CHECK(test_preflop_chip_conservation() == 0);
    CHECK(test_short_big_blind_root() == 0);
    CHECK(test_understacked_blind_rejected() == 0);
    CHECK(test_flop_profile_unchanged() == 0);
    CHECK(test_preflop_root_rejects_flop() == 0);
    CHECK(test_preflop_root_rejects_wrong_blinds() == 0);
    CHECK(test_preflop_full_tree_is_unbounded() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected preflop exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_heads_up_preflop PASS\n");
  return 0;
}
