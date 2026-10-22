// RFC 0029 AC3: the BehaviorPolicy adapters reproduce their sources exactly.
//
// Two properties are pinned, and they are different in kind:
//
//   1. NSeatBehaviorPolicy agrees with a DIRECT NSeatPolicy::lookup on every
//      information set of a named fixture. Comparing against `lookup` rather
//      than against a recorded expectation is deliberate: the adapter's job is
//      transport, so the source is the oracle.
//   2. Both adapters FAIL CLOSED. A miss throws and increments the miss count;
//      it never substitutes a uniform or check/call distribution. A fallback
//      would let a harness publish a figure that measures the fallback while
//      claiming to measure the policy, which is the failure mode AC4's
//      zero-miss requirement exists to exclude.
#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <bs/eval.hpp>
#include <bs/nash_conv_units.hpp>
#include <bs/stage6/measured_policy.hpp>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace bs::poker;
using namespace bs::stage6;
namespace solver = bs::solver;
namespace resident = bs::resident;
namespace tree = bs::tree;

int failures = 0;
void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

int card(const char* name) {
  return bs::cardId(name);
}

// Three seats with one chip each and a one-chip contribution, so the tree is
// tiny and every action node is reachable in a shallow walk. Same shape the
// n-seat trainer tests use, which keeps this fixture reproducible.
GameDef three_seat_def() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

std::vector<std::vector<solver::WeightedHand>> three_seat_ranges() {
  return {
      {{{card("Qh"), card("Kh")}, 2}, {{card("Ts"), card("Js")}, 3}},
      {{{card("Qd"), card("Kd")}, 5}, {{card("Th"), card("Jh")}, 7}},
      {{{card("Qs"), card("Ks")}, 11}, {{card("Td"), card("Jd")}, 13}},
  };
}

// Every reachable action node of the fixture, with the seat whose turn it is
// and that seat's own cards. Enumerated by breadth-first replay so the walk
// visits exactly the states a real traversal would.
struct Node {
  GameState state;
  std::size_t seat;
  HoleCards hole;
};

void collect_action_nodes(const tree::AbstractTree& t, const std::vector<HoleCards>& deal,
                          std::vector<Node>& out) {
  // The fixture is three seats with one chip each, so the whole tree is small;
  // a bounded DFS over concrete joint actions enumerates it without needing the
  // abstraction's menu.
  struct Frame {
    GameState state;
    int depth;
  };
  std::vector<Frame> stack{{t.def().preflop ? GameState(t.def()) : GameState(t.def()), 0}};
  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();
    if (f.depth > 24)
      continue;  // defensive bound; the fixture tree is far shallower
    switch (f.state.phase()) {
      case Phase::Action: {
        const std::size_t seat = *f.state.actor();
        out.push_back(Node{f.state, seat, deal[seat]});
        const LegalActions legal = f.state.legal();
        std::vector<Action> actions;
        if (legal.fold)
          actions.push_back({ActionType::Fold, 0});
        if (legal.check)
          actions.push_back({ActionType::Check, 0});
        if (legal.call)
          actions.push_back({ActionType::Call, 0});
        if (legal.aggressive) {
          actions.push_back({legal.aggressive->type, legal.aggressive->minimum});
          actions.push_back({legal.aggressive->type, legal.aggressive->maximum});
        }
        for (const Action& a : actions)
          stack.push_back({f.state.after_action(seat, a), f.depth + 1});
        break;
      }
      case Phase::Deal: {
        for (int c = 0; c < 52; ++c) {
          bool used = false;
          for (const auto& h : deal)
            if (h[0] == c || h[1] == c)
              used = true;
          for (int on_board : f.state.board())
            if (on_board == c)
              used = true;
          if (!used)
            stack.push_back({f.state.after_card(c), f.depth + 1});
        }
        break;
      }
      case Phase::Folded:
      case Phase::Showdown:
      case Phase::Frontier:
        break;
    }
  }
}

void test_nseat_adapter_matches_direct_lookup() {
  const GameDef def = three_seat_def();
  const tree::AbstractTree t(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_ranges();
  const solver::NSeatTrainerLimits limits{};
  const solver::NSeatTrainingResult trained = solver::train_nseat(t, ranges, 64, 777, limits);
  check(trained.termination == solver::NSeatTerminationPhase::Complete,
        "the fixture policy trains to completion");

  NSeatBehaviorPolicy adapter(trained.policy, t);
  PolicyContext ctx;

  // One joint deal from the declared ranges: each seat takes its first combo.
  const std::vector<HoleCards> deal{ranges[0][0].cards, ranges[1][0].cards, ranges[2][0].cards};
  std::vector<Node> nodes;
  collect_action_nodes(t, deal, nodes);
  check(!nodes.empty(), "the fixture reaches at least one action node");

  std::size_t compared = 0;
  std::size_t both_missed = 0;
  // One representative per distinct query, deduped on the full observable the
  // policy is asked about: street, board, EVERY seat's ledger, the actor, and
  // the actor's own cards. Enumerating every action sequence instead would
  // repeat the same lookup thousands of times, which is what made this test a
  // four-minute suite entry. Deduping on the whole ledger rather than on (pot,
  // street) keeps genuinely distinct nodes: two states can share a pot and
  // still differ in who committed what, and the policy's answer can differ.
  std::set<std::string> seen;
  for (const Node& node : nodes) {
    std::string key;
    key += std::to_string(static_cast<int>(node.state.street()));
    key += '|';
    for (int c : node.state.board())
      key += std::to_string(c) + ",";
    key += '|';
    for (const auto& p : node.state.players()) {
      key += std::to_string(p.stack) + "/" + std::to_string(p.street_committed) + "/" +
             std::to_string(p.contributed) + "/" + std::to_string(p.folded) + "/" +
             std::to_string(p.raise_rights) + ";";
    }
    key += '|' + std::to_string(node.seat) + '|' + std::to_string(node.hole[0]) + "-" +
           std::to_string(node.hole[1]);
    if (!seen.insert(key).second)
      continue;
    std::array<int, 2> own = node.hole;
    std::sort(own.begin(), own.end());
    const solver::NSeatPolicyRow* direct = trained.policy.lookup(t, node.state, node.seat, own);

    bool adapter_threw = false;
    std::vector<PolicyAction> dist;
    try {
      dist = adapter.distribution(node.state, node.seat, node.hole, ctx);
    } catch (const Stage6PolicyError&) {
      adapter_threw = true;
    }

    if (!direct) {
      // The source covers nothing here, so the adapter must refuse rather than
      // invent a distribution. This is the fail-closed property on a reachable
      // node, which is stronger than asserting it only for a synthetic input.
      check(adapter_threw, "the adapter refuses exactly where the source misses");
      ++both_missed;
      continue;
    }
    check(!adapter_threw, "the adapter answers wherever the source covers the node");
    if (adapter_threw)
      continue;
    check(dist.size() == direct->actions.size(), "adapter row length matches the policy row");
    for (std::size_t i = 0; i < dist.size() && i < direct->actions.size(); ++i) {
      check(dist[i].action == direct->actions[i], "adapter action order matches the policy row");
      check(std::abs(dist[i].probability - direct->probabilities[i]) < 1e-12,
            "adapter probability matches the policy row");
    }
    ++compared;
  }
  check(compared > 0, "the adapter was compared on at least one information set");
  check(adapter.served() == compared, "every comparison was a served answer");
  check(adapter.misses() == both_missed, "the adapter's miss count equals the source's");
  std::printf("[adapter] compared %zu infosets, %zu source misses mirrored, served %zu\n", compared,
              both_missed, adapter.served());
}

void test_nseat_adapter_fails_closed_on_miss() {
  const GameDef def = three_seat_def();
  const tree::AbstractTree t(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_ranges();
  const solver::NSeatTrainerLimits limits{};
  const solver::NSeatTrainingResult trained = solver::train_nseat(t, ranges, 8, 5, limits);

  NSeatBehaviorPolicy adapter(trained.policy, t);
  PolicyContext ctx;

  // A hole pair that shares a board card is an exact miss by contract. Asking
  // as the wrong seat is another. Neither may produce a distribution.
  bool threw = false;
  try {
    // 2c is on the flop.
    (void)adapter.distribution(GameState(def), 0, HoleCards{card("2c"), card("Qh")}, ctx);
  } catch (const Stage6PolicyError&) {
    threw = true;
  }
  check(threw, "a board-blocked combo throws instead of returning a fallback");
  check(adapter.misses() == 1, "the miss is counted");

  // Construction rejects a policy/tree mismatch rather than missing per call.
  const GameDef other = [] {
    GameDef d = three_seat_def();
    d.pot = 99;  // a different root identity
    return d;
  }();
  bool ctor_threw = false;
  try {
    const tree::AbstractTree other_tree(other, bs::abstraction::ActionAbstraction::identity());
    NSeatBehaviorPolicy bad(trained.policy, other_tree);
    (void)bad;
  } catch (const std::invalid_argument&) {
    ctor_threw = true;
  }
  check(ctor_threw, "a policy/tree game mismatch is rejected at construction");
}

void test_resident_adapter_rejects_non_acting_seat() {
  // An empty resident set answers nothing, but the seat check runs first and is
  // independent of any artifact: the serving path answers for the ACTING seat,
  // and asking for another one is a harness error rather than a miss.
  resident::ResidentPolicySet empty;
  ResidentBehaviorPolicy adapter(empty);
  PolicyContext ctx;
  const GameDef def = three_seat_def();
  const GameState state(def);
  const std::size_t actor = *state.actor();
  const std::size_t wrong = (actor + 1) % def.player_count;

  bool threw = false;
  try {
    (void)adapter.distribution(state, wrong, HoleCards{card("Qh"), card("Kh")}, ctx);
  } catch (const Stage6PolicyError& e) {
    threw = std::string(e.what()).find("acting seat") != std::string::npos;
  }
  check(threw, "the resident adapter rejects a non-acting seat with a named error");
  check(adapter.served() == 0, "no answer is counted for a rejected request");
}

void test_resident_adapter_reports_the_resident_miss_reason() {
  // With no artifact loaded, a legitimate request is a resident miss. The
  // adapter must surface the resident reason rather than a generic failure, and
  // must not produce a distribution.
  resident::ResidentPolicySet empty;
  ResidentBehaviorPolicy adapter(empty);
  PolicyContext ctx;
  const GameState state(three_seat_def());
  const std::size_t actor = *state.actor();

  bool threw = false;
  try {
    (void)adapter.distribution(state, actor, HoleCards{card("Qh"), card("Kh")}, ctx);
  } catch (const Stage6PolicyError& e) {
    threw = std::string(e.what()).find("resident miss") != std::string::npos;
  }
  check(threw, "a resident miss throws with the resident reason in the message");
  check(adapter.misses() == 1, "the resident miss is counted");
  check(adapter.last_miss() != resident::MissReason::None, "the miss reason is retained");
}

void test_history_conversion_is_street_ordered() {
  HandLog log;
  log.preflop.push_back(LoggedAction{0, Action{ActionType::Call, 0}});
  log.flop.push_back(LoggedAction{1, Action{ActionType::Check, 0}});
  log.flop.push_back(LoggedAction{2, Action{ActionType::Bet, 4}});
  log.turn.push_back(LoggedAction{0, Action{ActionType::Call, 4}});
  log.river.push_back(LoggedAction{1, Action{ActionType::Check, 0}});

  const std::vector<PublicAction> history = public_action_history(log);
  check(history.size() == 5, "every logged action appears once");
  check(history[0].street == Street::Preflop && history[0].seat == 0, "preflop first");
  check(history[1].street == Street::Flop && history[1].seat == 1, "flop preserves order");
  check(history[2].street == Street::Flop && history[2].seat == 2 &&
            history[2].action.type == ActionType::Bet && history[2].action.target_total == 4,
        "the second flop action carries its target total");
  check(history[3].street == Street::Turn && history[3].seat == 0, "turn follows the flop");
  check(history[4].street == Street::River && history[4].seat == 1, "river last");

  const std::vector<PublicAction> empty = public_action_history(HandLog{});
  check(empty.empty(), "an empty log converts to an empty history");
}

}  // namespace

int main() {
  test_nseat_adapter_matches_direct_lookup();
  test_nseat_adapter_fails_closed_on_miss();
  test_resident_adapter_rejects_non_acting_seat();
  test_resident_adapter_reports_the_resident_miss_reason();
  test_history_conversion_is_street_ordered();
  if (failures) {
    std::printf("STAGE6 MEASURED POLICY FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE6 MEASURED POLICY PASSED");
  return 0;
}
