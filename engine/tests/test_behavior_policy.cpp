// RFC 0008 stage 6: unit tests for the offline behavior-policy interface and
// the declared fixed uniform reference. The estimator/simulator and the
// pinned-baseline adapter are built on this menu identity, so the menu's
// ordering, interval snapping, dedup, and probability normalization are
// pinned here before any of those components exist.
#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace bs;
using namespace bs::stage6;
using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::GameDef;
using poker::GameState;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}

// Three-seat unopened preflop state: button=2, so seat 0 is small blind,
// seat 1 big blind, and seat 2 (the button = UTG in three-handed play) acts
// first with fold + raise and no check/call.
GameDef three_way_preflop() {
  GameDef def{};
  def.player_count = 3;
  def.button = 2;
  def.big_blind = 2;
  for (std::size_t i = 0; i < 3; ++i)
    def.stacks[i] = 200;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.preflop = true;  // 3..10 seats: empty board + preflop flag select it
  def.board = {-1, -1, -1, -1, -1};
  def.board_size = 0;
  return def;
}

}  // namespace

int main() {
  const GameDef def = three_way_preflop();
  const GameState state(def);
  const std::optional<std::size_t> actor = state.actor();
  check(actor.has_value(), "the 3-seat preflop fixture reaches an action state");

  const std::vector<Action> menu = declared_behavior_menu(state, *actor);
  // Unopened: fold, a limp CALL (the big-blind amount; the domain carries
  // target 0 on a call and after_action derives the amount), and the
  // aggressive menu. No check for the first voluntary seat.
  check(menu.front().type == ActionType::Fold, "the menu opens with fold");
  bool saw_check = false;
  bool saw_call = false;
  for (const Action& a : menu) {
    if (a.type == ActionType::Check)
      saw_check = true;
    if (a.type == ActionType::Call)
      saw_call = true;
  }
  check(!saw_check, "the unopened first actor gets no check entry");
  check(saw_call, "the unopened first actor gets a limp-call entry");

  // Every aggressive entry is a raise inside the legal inclusive interval and
  // targets are strictly increasing after dedup.
  const poker::LegalActions legal = state.legal();
  check(legal.aggressive.has_value(), "a raise is legal for the deep-stacked opener");
  Chips previous = 0;
  bool first_raise = true;
  std::size_t raises = 0;
  for (const Action& a : menu) {
    if (a.type != ActionType::Raise)
      continue;
    ++raises;
    check(
        a.target_total >= legal.aggressive->minimum && a.target_total <= legal.aggressive->maximum,
        "every declared raise target is inside the legal inclusive interval");
    check(first_raise || a.target_total > previous,
          "raise targets are unique and ordered after dedup");
    first_raise = false;
    previous = a.target_total;
  }
  check(raises >= 3, "the declared menu contains at least min/fraction/cap raises");
  check(previous == legal.aggressive->maximum, "the final raise target is the all-in cap");

  // Uniform reference: equal mass over exactly the declared menu, summing to
  // one, and every emitted action legal.
  const UniformBehaviorPolicy uniform;
  const HoleCards hole{{0, 1}};
  const PolicyContext context{nullptr, 1};
  const std::vector<PolicyAction> d = uniform.distribution(state, *actor, hole, context);
  check(d.size() == menu.size(), "uniform distribution covers exactly the declared menu");
  double sum = 0.0;
  const double expected = 1.0 / static_cast<double>(menu.size());
  for (const PolicyAction& pa : d) {
    check(std::fabs(pa.probability - expected) < 1e-12,
          "uniform gives every menu entry equal mass");
    check(legal.contains(pa.action), "every uniform action is legal under GameState::legal()");
    sum += pa.probability;
  }
  check(std::fabs(sum - 1.0) < 1e-12, "uniform probabilities sum to one");

  // sample_distribution selects deterministically by cumulative mass and the
  // unit draw convention.
  const Action first = sample_distribution(d, 0.0);
  const Action last = sample_distribution(d, 0.999999);
  check(first.type == d.front().action.type && first.target_total == d.front().action.target_total,
        "a zero draw selects the first support action");
  check(last.type == d.back().action.type && last.target_total == d.back().action.target_total,
        "a near-one draw selects the last support action");

  // Interface rejects a non-actor seat (caller drives policies only on the
  // acting seat).
  bool threw = false;
  try {
    (void)declared_behavior_menu(state, (*actor + 1) % 3);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "the menu builder rejects a seat that is not the actor");

  if (failures != 0) {
    std::fprintf(stderr, "BEHAVIOR POLICY TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("BEHAVIOR POLICY TESTS PASSED");
  return 0;
}
