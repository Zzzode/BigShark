// RFC 0011: subgame resolver tests.
//
// Verifies that the subgame resolver builds a turn-rooted River-terminal
// subtree, derives prior ranges from a flop-terminal blueprint, solves with
// CFR, and serves a valid candidate at `approximate`. The fixture is a
// flop check-through that reaches the turn, so the prior-range derivation
// is a simple prefix replay. Covers 2-seat and 3-seat, turn-rooted and
// river-rooted subgames.
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/subgame_resolver.hpp>
#include <bs/unified_game.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::resolver;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

int card(const char* name) {
  return bs::cardId(std::string(name));
}
bool near(double a, double b, double tol = 1e-9) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tol;
}

using Action = bs::poker::Action;

// Map-backed blueprint source for offline solver tests.
class MapBlueprint final : public BlueprintSource {
 public:
  explicit MapBlueprint(
      UnifiedGame game,
      std::string digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
      : game_(std::move(game)), digest_(std::move(digest)) {}

  void set_row(const GameState& state, std::span<const PublicAction> history, std::size_t player,
               std::array<int, 2> cards, std::vector<Action> actions, std::vector<double> probs) {
    rows_[{make_information_key(player, cards, state.board(), history), player}] = {
        std::move(actions), std::move(probs)};
  }

  const UnifiedGame& game() const override { return game_; }
  std::string_view artifact_digest() const override { return digest_; }
  std::optional<BlueprintRowView> row(const GameState& state, std::span<const PublicAction> history,
                                      std::size_t player, std::array<int, 2> cards) const override {
    const auto it =
        rows_.find({make_information_key(player, cards, state.board(), history), player});
    if (it == rows_.end())
      return std::nullopt;
    return BlueprintRowView{it->second.actions.data(), it->second.probs.data(),
                            it->second.probs.size()};
  }

 private:
  using Key = std::pair<InformationKey, std::size_t>;
  struct Row {
    std::vector<Action> actions;
    std::vector<double> probs;
  };
  UnifiedGame game_;
  std::string digest_;
  std::map<Key, Row> rows_;
};

// Build a flop-terminal 2-seat game with a check-through line.
// The flop is 2c 3d 7h; the turn is Js. Both players check the flop.
struct FlopCheckThrough {
  static GameDef make_def() {
    GameDef def{};
    def.player_count = 2;
    def.button = 0;
    def.big_blind = 2;
    def.stacks = {20, 20};
    def.contributions = {1, 1};
    def.pot = 2;
    def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
    def.board_size = 3;
    def.preflop = false;
    def.terminal = TerminalDepth::Flop;
    return def;
  }

  // The turn-rooted GameDef: same stacks/contributions/pot as the flop root
  // after check-check, but with the turn card dealt and terminal=River so the
  // state opens action (a flop-terminal def would set phase=Frontier after
  // dealing the turn card).
  static GameDef make_turn_def() {
    GameDef def = make_def();
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
    def.board_size = 4;
    def.terminal = TerminalDepth::River;
    return def;
  }

  UnifiedGame unified;
  GameState flop_root;
  GameState turn_root;
  std::vector<PublicAction> history;
  std::array<int, 2> hero_hand{};
  std::array<int, 2> opp_hand{};

  FlopCheckThrough()
      : unified([] {
          UnifiedGame g;
          g.def = make_def();
          g.sizes = bs::abstraction::default_size_schedule();
          return g;
        }()),
        flop_root(unified.def),
        turn_root(make_turn_def()) {
    hero_hand = {card("Ac"), card("Ad")};
    opp_hand = {card("Qc"), card("Qd")};
    std::sort(hero_hand.begin(), hero_hand.end());
    std::sort(opp_hand.begin(), opp_hand.end());
    unified.ranges[0] = {{opp_hand, 1.0}};
    unified.ranges[1] = {{hero_hand, 1.0}};
    history = {{Street::Flop, 1, {ActionType::Check}}, {Street::Flop, 0, {ActionType::Check}}};
  }
};

// Install the flop check-through rows into the blueprint.
void install_flop_rows(MapBlueprint& bp, const FlopCheckThrough& f) {
  const GameState root(f.unified.def);
  // Seat 1 checks at the flop root.
  bp.set_row(root, {}, 1, f.hero_hand, {{ActionType::Check}, {ActionType::Bet, 2}}, {1.0, 0.0});
  // Seat 0 checks after seat 1's check.
  const GameState after_check = root.after_action(1, {ActionType::Check});
  const std::vector<PublicAction> prefix = {{Street::Flop, 1, {ActionType::Check}}};
  bp.set_row(after_check, prefix, 0, f.opp_hand, {{ActionType::Check}, {ActionType::Bet, 2}},
             {1.0, 0.0});
}

// Build a flop-terminal 3-seat game with a check-through line.
// The flop is 2c 3d 7h; the turn is Js. All three players check the flop.
// Button=2 so seat 0 (SB) acts first postflop, matching the multiway
// convention. Stacks are 10 (5 BB) to keep the River-terminal subtree under
// the byte cap — a 3-seat tree with 20 BB stacks exceeds the default 1 GB.
struct FlopCheckThrough3P {
  static GameDef make_def() {
    GameDef def{};
    def.player_count = 3;
    def.button = 2;  // seat 0 (SB) acts first postflop
    def.big_blind = 2;
    def.stacks = {10, 10, 10};
    def.contributions = {1, 1, 1};
    def.pot = 3;
    def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
    def.board_size = 3;
    def.preflop = false;
    def.terminal = TerminalDepth::Flop;
    return def;
  }

  static GameDef make_turn_def() {
    GameDef def = make_def();
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
    def.board_size = 4;
    def.terminal = TerminalDepth::River;
    return def;
  }

  static GameDef make_river_def() {
    GameDef def = make_def();
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
    def.board_size = 5;
    def.terminal = TerminalDepth::River;
    return def;
  }

  UnifiedGame unified;
  GameState flop_root;
  GameState turn_root;
  GameState river_root;
  std::vector<PublicAction> flop_history;
  std::vector<PublicAction> turn_history;
  std::vector<PublicAction> full_history;
  std::array<int, 2> hand0{};
  std::array<int, 2> hand1{};
  std::array<int, 2> hand2{};

  FlopCheckThrough3P()
      : unified([] {
          UnifiedGame g;
          g.def = make_def();
          g.sizes = bs::abstraction::default_size_schedule();
          return g;
        }()),
        flop_root(unified.def),
        turn_root(make_turn_def()),
        river_root(make_river_def()) {
    hand0 = {card("Qc"), card("Qd")};
    hand1 = {card("Tc"), card("Td")};
    hand2 = {card("Ac"), card("Ad")};
    std::sort(hand0.begin(), hand0.end());
    std::sort(hand1.begin(), hand1.end());
    std::sort(hand2.begin(), hand2.end());
    unified.ranges[0] = {{hand0, 1.0}};
    unified.ranges[1] = {{hand1, 1.0}};
    unified.ranges[2] = {{hand2, 1.0}};
    // Flop: seat 0 checks, seat 1 checks, seat 2 checks.
    flop_history = {{Street::Flop, 0, {ActionType::Check}},
                    {Street::Flop, 1, {ActionType::Check}},
                    {Street::Flop, 2, {ActionType::Check}}};
    // Turn: seat 0 checks, seat 1 checks, seat 2 checks.
    turn_history = {{Street::Turn, 0, {ActionType::Check}},
                    {Street::Turn, 1, {ActionType::Check}},
                    {Street::Turn, 2, {ActionType::Check}}};
    full_history = flop_history;
    full_history.insert(full_history.end(), turn_history.begin(), turn_history.end());
  }
};

// Install the 3-seat flop check-through rows into the blueprint.
void install_flop_rows_3p(MapBlueprint& bp, const FlopCheckThrough3P& f) {
  const GameState root(f.unified.def);
  // Seat 0 checks at the flop root.
  bp.set_row(root, {}, 0, f.hand0, {{ActionType::Check}, {ActionType::Bet, 2}}, {1.0, 0.0});
  // Seat 1 checks after seat 0's check.
  const GameState after_0 = root.after_action(0, {ActionType::Check});
  const std::vector<PublicAction> prefix0 = {{Street::Flop, 0, {ActionType::Check}}};
  bp.set_row(after_0, prefix0, 1, f.hand1, {{ActionType::Check}, {ActionType::Bet, 2}}, {1.0, 0.0});
  // Seat 2 checks after seat 0 and seat 1's checks.
  const GameState after_01 = after_0.after_action(1, {ActionType::Check});
  const std::vector<PublicAction> prefix01 = {{Street::Flop, 0, {ActionType::Check}},
                                              {Street::Flop, 1, {ActionType::Check}}};
  bp.set_row(after_01, prefix01, 2, f.hand2, {{ActionType::Check}, {ActionType::Bet, 2}},
             {1.0, 0.0});
}

ResolveLimits test_limits(std::uint64_t iterations = 1000) {
  ResolveLimits limits;
  limits.iterations = iterations;
  limits.time = std::chrono::seconds(30);
  limits.max_nodes = 5000000;
  limits.max_information_sets = 1000000;
  limits.public_seed = 0x5151515151515151ULL;
  return limits;
}

// (i) Basic solve: a flop check-through reaches the turn, and the subgame
// resolver produces a valid candidate.
int test_basic_solve() {
  FlopCheckThrough f;
  MapBlueprint bp(f.unified);
  install_flop_rows(bp, f);

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.turn_root, f.history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  // Every candidate row is a valid probability distribution.
  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (ii) River solve: a river decision (board_size=5) builds a turn-rooted
// subgame, solves it, and walks the tree to the river node. The history
// includes the flop check-through plus a turn check-through.
int test_river_solve() {
  FlopCheckThrough f;
  MapBlueprint bp(f.unified);
  install_flop_rows(bp, f);

  // Construct a river-rooted state (board_size=5, river card 9c).
  GameDef river_def = f.unified.def;
  river_def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  river_def.board_size = 5;
  river_def.terminal = TerminalDepth::River;
  GameState river_root(river_def);

  // Full history: flop check-through + turn check-through.
  std::vector<PublicAction> full_history = f.history;
  full_history.push_back({Street::Turn, 1, {ActionType::Check}});
  full_history.push_back({Street::Turn, 0, {ActionType::Check}});

  SubgameResolver resolver;
  const auto result = resolver.resolve(river_root, full_history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (iii) Coverage miss: a missing blueprint row for the flop prefix returns
// CoverageMiss.
int test_coverage_miss() {
  FlopCheckThrough f;
  MapBlueprint bp(f.unified);
  // Intentionally do NOT install the flop rows.

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.turn_root, f.history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::CoverageMiss);
  return 0;
}

// (iv) 3-seat turn solve: a 3-seat flop check-through reaches the turn, and
// the subgame resolver produces a valid candidate with seat_count == 3.
int test_three_seat_turn_solve() {
  FlopCheckThrough3P f;
  MapBlueprint bp(f.unified);
  install_flop_rows_3p(bp, f);

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.turn_root, f.flop_history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (v) 3-seat river solve: a 3-seat river decision builds a turn-rooted
// subgame, solves it, and walks the tree to the river node.
int test_three_seat_river_solve() {
  FlopCheckThrough3P f;
  MapBlueprint bp(f.unified);
  install_flop_rows_3p(bp, f);

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.river_root, f.full_history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (vi) Folded player: a 3-seat game where one player folded on the flop is
// Solved — the GameDef folded bitmap carries the fold to the subgame tree,
// and the existing fold-aware machinery (next_actor, refresh_live,
// settlement, trainer) handles the folded seat as dead money.
int test_folded_player_solve() {
  FlopCheckThrough3P f;
  MapBlueprint bp(f.unified);

  // Use a River-terminal flop root so after_card opens Action (not Frontier)
  // after the flop line completes. The blueprint is flop-terminal, but the
  // resolver only reads blueprint rows at flop nodes — the terminal depth
  // of the state we advance does not affect row lookups.
  GameDef rt_def = f.unified.def;
  rt_def.terminal = TerminalDepth::River;
  GameState flop_root_rt(rt_def);

  // Advance through a legal line where seat 2 folds:
  // seat 0 checks, seat 1 bets 2, seat 2 folds, seat 0 calls.
  GameState cursor(flop_root_rt);
  cursor = cursor.after_action(0, {ActionType::Check});
  cursor = cursor.after_action(1, {ActionType::Bet, 2});
  cursor = cursor.after_action(2, {ActionType::Fold});
  cursor = cursor.after_action(0, {ActionType::Call});
  // Deal the turn card to reach a turn action node.
  cursor = cursor.after_card(card("Js"));

  // History: seat 0 checks, seat 1 bets 2, seat 2 folds, seat 0 calls.
  std::vector<PublicAction> fold_history = {{Street::Flop, 0, {ActionType::Check}},
                                            {Street::Flop, 1, {ActionType::Bet, 2}},
                                            {Street::Flop, 2, {ActionType::Fold}},
                                            {Street::Flop, 0, {ActionType::Call}}};

  // Install blueprint rows for the fold line. The resolver's
  // derive_prior_ranges walks this line and looks up a row at each step;
  // each row must include the observed action in its menu.
  // Seat 0 checks at the flop root.
  bp.set_row(flop_root_rt, {}, 0, f.hand0, {{ActionType::Check}, {ActionType::Bet, 2}}, {1.0, 0.0});
  // Seat 1 bets 2 after seat 0's check.
  const GameState after_0 = flop_root_rt.after_action(0, {ActionType::Check});
  const std::vector<PublicAction> prefix0 = {{Street::Flop, 0, {ActionType::Check}}};
  bp.set_row(after_0, prefix0, 1, f.hand1, {{ActionType::Check}, {ActionType::Bet, 2}}, {0.0, 1.0});
  // Seat 2 folds after seat 1's bet.
  const GameState after_01 = after_0.after_action(1, {ActionType::Bet, 2});
  const std::vector<PublicAction> prefix01 = {{Street::Flop, 0, {ActionType::Check}},
                                              {Street::Flop, 1, {ActionType::Bet, 2}}};
  bp.set_row(after_01, prefix01, 2, f.hand2, {{ActionType::Fold}, {ActionType::Call}}, {1.0, 0.0});
  // Seat 0 calls after seat 2's fold.
  const GameState after_012 = after_01.after_action(2, {ActionType::Fold});
  const std::vector<PublicAction> prefix012 = {{Street::Flop, 0, {ActionType::Check}},
                                               {Street::Flop, 1, {ActionType::Bet, 2}},
                                               {Street::Flop, 2, {ActionType::Fold}}};
  bp.set_row(after_012, prefix012, 0, f.hand0, {{ActionType::Fold}, {ActionType::Call}},
             {0.0, 1.0});

  SubgameResolver resolver;
  const auto result = resolver.resolve(cursor, fold_history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  // Every candidate row is a valid probability distribution.
  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (vi-b) Folded player on the river: a 3-seat river decision where one player
// folded on the flop builds a turn-rooted subgame, solves it, and walks the
// tree to the river node. The folded bitmap must survive the walk — the
// cursor's actor rotation skips the folded seat on the turn.
int test_folded_player_river_solve() {
  FlopCheckThrough3P f;
  MapBlueprint bp(f.unified);

  // Use a River-terminal flop root so after_card opens Action (not Frontier)
  // after the flop line completes.
  GameDef rt_def = f.unified.def;
  rt_def.terminal = TerminalDepth::River;
  GameState flop_root_rt(rt_def);

  // Flop line: seat 0 checks, seat 1 bets 2, seat 2 folds, seat 0 calls.
  GameState cursor(flop_root_rt);
  cursor = cursor.after_action(0, {ActionType::Check});
  cursor = cursor.after_action(1, {ActionType::Bet, 2});
  cursor = cursor.after_action(2, {ActionType::Fold});
  cursor = cursor.after_action(0, {ActionType::Call});
  // Deal the turn card.
  cursor = cursor.after_card(card("Js"));
  // Turn line: seat 0 checks, seat 1 checks (seat 2 is folded, skipped).
  cursor = cursor.after_action(0, {ActionType::Check});
  cursor = cursor.after_action(1, {ActionType::Check});
  // Deal the river card to reach a river action node.
  cursor = cursor.after_card(card("9c"));

  // Full history: flop fold line + turn check-through.
  std::vector<PublicAction> full_history = {
      {Street::Flop, 0, {ActionType::Check}}, {Street::Flop, 1, {ActionType::Bet, 2}},
      {Street::Flop, 2, {ActionType::Fold}},  {Street::Flop, 0, {ActionType::Call}},
      {Street::Turn, 0, {ActionType::Check}}, {Street::Turn, 1, {ActionType::Check}},
  };

  // Install blueprint rows for the flop fold line (same as test_folded_player_solve).
  bp.set_row(flop_root_rt, {}, 0, f.hand0, {{ActionType::Check}, {ActionType::Bet, 2}}, {1.0, 0.0});
  const GameState after_0 = flop_root_rt.after_action(0, {ActionType::Check});
  const std::vector<PublicAction> prefix0 = {{Street::Flop, 0, {ActionType::Check}}};
  bp.set_row(after_0, prefix0, 1, f.hand1, {{ActionType::Check}, {ActionType::Bet, 2}}, {0.0, 1.0});
  const GameState after_01 = after_0.after_action(1, {ActionType::Bet, 2});
  const std::vector<PublicAction> prefix01 = {{Street::Flop, 0, {ActionType::Check}},
                                              {Street::Flop, 1, {ActionType::Bet, 2}}};
  bp.set_row(after_01, prefix01, 2, f.hand2, {{ActionType::Fold}, {ActionType::Call}}, {1.0, 0.0});
  const GameState after_012 = after_01.after_action(2, {ActionType::Fold});
  const std::vector<PublicAction> prefix012 = {{Street::Flop, 0, {ActionType::Check}},
                                               {Street::Flop, 1, {ActionType::Bet, 2}},
                                               {Street::Flop, 2, {ActionType::Fold}}};
  bp.set_row(after_012, prefix012, 0, f.hand0, {{ActionType::Fold}, {ActionType::Call}},
             {0.0, 1.0});

  SubgameResolver resolver;
  const auto result = resolver.resolve(cursor, full_history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(result.completed_iterations > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());

  // Every candidate row is a valid probability distribution.
  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }

  return 0;
}

// (vii) Solve deadline: a zero-iteration cap returns InvalidInput (the trainer
// rejects zero iterations before the deadline check fires).
int test_solve_deadline() {
  FlopCheckThrough f;
  MapBlueprint bp(f.unified);
  install_flop_rows(bp, f);

  ResolveLimits limits = test_limits();
  limits.iterations = 0;  // cannot complete even one iteration

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.turn_root, f.history, bp, limits);

  CHECK(result.status == SubgameResolveStatus::InvalidInput);
  return 0;
}

// ---------------------------------------------------------------------------
// Blueprint-free (resolve_uniform) tests.
//
// resolve_uniform builds the subgame directly from the live node state with
// uniform prior ranges (every combo weight 1.0, board blockers removed). It
// roots the subgame at the current node and supports 2..10 seats. Both
// street-start nodes (hero can check) and mid-street nodes (hero faces a bet)
// are supported. No blueprint is consulted; the CoverageMiss path is
// unreachable.
// ---------------------------------------------------------------------------

// Build a street-start GameState on the turn (board_size=4) or river
// (board_size=5) for N seats. Every seat posted 1 chip as dead money; the
// pot equals the sum of contributions. The button is 0 for two seats (the
// BB acts first postflop, matching the heads-up convention) and N-1 for 3+
// seats (seat 0 acts first, matching the multiway convention). The state
// opens in Action phase with legal().check == true.
GameState make_uniform_state(std::size_t seats, bool river, Chips stack = 10) {
  GameDef def{};
  def.player_count = seats;
  def.button = seats == 2 ? 0 : seats - 1;
  def.big_blind = 2;
  for (std::size_t s = 0; s < seats; ++s) {
    def.stacks[s] = stack;
    def.contributions[s] = 1;
  }
  def.pot = static_cast<Chips>(seats);
  if (river) {
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
    def.board_size = 5;
  } else {
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
    def.board_size = 4;
  }
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return GameState(def);
}

// Verify that the candidate is non-empty and every row is a valid probability
// distribution over the node's ordered abstract actions.
int check_valid_candidate(const SubgameResolveResult& result) {
  CHECK(result.completed_iterations > 0);
  CHECK(result.nodes > 0);
  CHECK(result.information_sets > 0);
  CHECK(!result.node_actions.empty());
  CHECK(!result.candidate.empty());
  for (const auto& [key, row] : result.candidate) {
    CHECK(!row.actions.empty());
    CHECK(row.actions == result.node_actions);
    CHECK(row.actions.size() == row.probabilities.size());
    double sum = 0.0;
    for (double p : row.probabilities) {
      CHECK(p >= 0.0);
      CHECK(p <= 1.0);
      sum += p;
    }
    CHECK(near(sum, 1.0, 1e-9));
  }
  return 0;
}

// (viii) Uniform 2-seat turn: resolve_uniform with uniform prior ranges on a
// 2-seat turn decision produces a valid candidate with seat_count == 2.
int test_uniform_2seat_turn() {
  GameState state = make_uniform_state(2, /*river=*/false);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, no_history, test_limits());

  if (result.status != SubgameResolveStatus::Solved)
    std::fprintf(
        stderr,
        "uniform %zu-seat turn: status=%s samples=%llu nodes=%zu infosets=%zu candidate=%zu\n",
        state.player_count(), bs::resolver::to_string(result.status),
        static_cast<unsigned long long>(result.completed_iterations), result.nodes,
        result.information_sets, result.candidate.size());
  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  return check_valid_candidate(result);
}

// (ix) Uniform 3-seat turn integration: preserve the original game and action
// tree while bounding samples. Full-action averaging at 1,000 samples exceeds
// the one-million-information-set budget; quality is gated independently.
int test_uniform_3seat_turn() {
  GameState state = make_uniform_state(3, /*river=*/false);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, no_history, test_limits(8));

  if (result.status != SubgameResolveStatus::Solved)
    std::fprintf(
        stderr,
        "uniform %zu-seat turn: status=%s samples=%llu nodes=%zu infosets=%zu candidate=%zu\n",
        state.player_count(), bs::resolver::to_string(result.status),
        static_cast<unsigned long long>(result.completed_iterations), result.nodes,
        result.information_sets, result.candidate.size());
  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  return check_valid_candidate(result);
}

// (x) Uniform 4-seat turn: the key new capability — resolve_uniform supports
// 4 seats (the production flop blueprint library has no 4-seat rows, so the
// blueprint path cannot serve this seat count). Stacks are 6 (3 BB) to keep
// the River-terminal subtree under the byte cap — a 4-seat tree with deeper
// stacks exceeds the default 1 GB. Bound integration samples: 1,000 samples
// with full-action averaging can exceed the trainer's shared resource limits.
int test_uniform_4seat_turn() {
  GameState state = make_uniform_state(4, /*river=*/false, /*stack=*/6);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, no_history, test_limits(8));

  if (result.status != SubgameResolveStatus::Solved)
    std::fprintf(
        stderr,
        "uniform %zu-seat turn: status=%s samples=%llu nodes=%zu infosets=%zu candidate=%zu\n",
        state.player_count(), bs::resolver::to_string(result.status),
        static_cast<unsigned long long>(result.completed_iterations), result.nodes,
        result.information_sets, result.candidate.size());
  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 4);
  return check_valid_candidate(result);
}

// Resource failure must preserve the same deep game and publish no candidate,
// even if some sampled work completed before the information-set cap fired.
int test_uniform_deep_information_budget_refusal() {
  const auto state = make_uniform_state(3, /*river=*/false, /*stack=*/10);
  auto capped = test_limits();
  capped.max_information_sets = 100;
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, {}, capped);
  CHECK(result.status == SubgameResolveStatus::SolveDeadline);
  CHECK(result.completed_iterations == 0);
  CHECK(result.candidate.empty());
  CHECK(result.node_actions.empty());
  return 0;
}

// (xi) Uniform river: a river decision (board_size=5) with uniform priors
// roots the subgame at the river street-start node (no chance subtree).
int test_uniform_river() {
  GameState state = make_uniform_state(2, /*river=*/true);
  CHECK(state.legal().check);

  const std::vector<PublicAction> no_history;
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, no_history, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  return check_valid_candidate(result);
}

// (xii) Uniform mid-street faces bet: a turn state where the hero faces a bet
// (legal().check == false) is Solved — the subgame def carries the live
// street_committed, raise_rights, and last_full_raise, so the root opens at
// the decision point with a fold/call/raise menu (not check/bet).
int test_uniform_mid_street_faces_bet() {
  GameState state = make_uniform_state(2, /*river=*/false);
  const std::size_t bettor = *state.actor();
  state = state.after_action(bettor, {ActionType::Bet, 2});
  CHECK(state.phase() == Phase::Action);
  CHECK(!state.legal().check);
  CHECK(state.legal().call);

  const std::vector<PublicAction> history = {{Street::Turn, bettor, {ActionType::Bet, 2}}};
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, history, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  // The root pot includes the bet: 2 (dead) + 2 (bet) = 4.
  CHECK(result.root_pot == 4.0);
  // The root menu must reflect the wager: Fold and Call present, Check absent.
  bool has_fold = false, has_call = false, has_check = false;
  for (const Action& a : result.node_actions) {
    if (a.type == ActionType::Fold)
      has_fold = true;
    if (a.type == ActionType::Call)
      has_call = true;
    if (a.type == ActionType::Check)
      has_check = true;
  }
  CHECK(has_fold);
  CHECK(has_call);
  CHECK(!has_check);
  return check_valid_candidate(result);
}

// (xii-b) Uniform mid-street 3-seat: a 3-seat turn state where the hero faces
// a bet is Solved with seat_count == 3. The button is set so the hero is the
// root actor; the other pending seat (the one after the hero) is reached by
// the actor rotation after the hero acts.
int test_uniform_mid_street_3seat() {
  GameState state = make_uniform_state(3, /*river=*/false);
  const std::size_t bettor = *state.actor();
  state = state.after_action(bettor, {ActionType::Bet, 2});
  CHECK(state.phase() == Phase::Action);
  CHECK(!state.legal().check);
  CHECK(state.legal().call);

  const std::vector<PublicAction> history = {{Street::Turn, bettor, {ActionType::Bet, 2}}};
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, history, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 3);
  return check_valid_candidate(result);
}

// (xii-c) Uniform mid-street river: a river state where the hero faces a bet
// is Solved. The river-rooted subgame has no chance subtree, so the tree is
// small; the candidate is extracted at the root.
int test_uniform_mid_street_river() {
  GameState state = make_uniform_state(2, /*river=*/true);
  const std::size_t bettor = *state.actor();
  state = state.after_action(bettor, {ActionType::Bet, 2});
  CHECK(state.phase() == Phase::Action);
  CHECK(!state.legal().check);

  const std::vector<PublicAction> history = {{Street::River, bettor, {ActionType::Bet, 2}}};
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, history, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  return check_valid_candidate(result);
}

// (xii-d) Uniform mid-street faces raise: a turn state where the hero faces a
// raise (not just a bet) is Solved. The subgame def carries the live
// last_full_raise (the raise increment), so the root min-raise is correct.
int test_uniform_mid_street_faces_raise() {
  GameState state = make_uniform_state(2, /*river=*/false, /*stack=*/20);
  // Seat 1 (first actor) bets 4; seat 0 raises to 10; seat 1 (hero) faces
  // the raise. The raise increment is 6, so the hero's min-raise is
  // high + last_full_raise = 10 + 6 = 16.
  const std::size_t bettor = *state.actor();
  state = state.after_action(bettor, {ActionType::Bet, 4});
  const std::size_t raiser = *state.actor();
  state = state.after_action(raiser, {ActionType::Raise, 10});
  CHECK(state.phase() == Phase::Action);
  CHECK(!state.legal().check);
  CHECK(state.legal().call);

  const std::vector<PublicAction> history = {
      {Street::Turn, bettor, {ActionType::Bet, 4}},
      {Street::Turn, raiser, {ActionType::Raise, 10}},
  };
  SubgameResolver resolver;
  const auto result = resolver.resolve_uniform(state, history, test_limits());

  CHECK(result.status == SubgameResolveStatus::Solved);
  CHECK(result.seat_count == 2);
  // The root pot: seat 0 contributed 1+10=11, seat 1 contributed 1+4=5.
  CHECK(result.root_pot == 16.0);
  return check_valid_candidate(result);
}

// (xiii) Uniform 11-seat ineligible: GameDef validation rejects
// player_count > kMaxUnifiedSeats (10), so an 11-seat state cannot reach
// resolve_uniform. This test pins the validation gate that backs the
// resolver's seat-count guard.
int test_uniform_11seat_ineligible() {
  GameDef def{};
  def.player_count = 11;  // exceeds kMaxUnifiedSeats (10)
  // All other fields stay default; validate() rejects the seat count before
  // touching any per-seat array.
  bool threw = false;
  try {
    GameState state(def);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_basic_solve();
  failures += test_river_solve();
  failures += test_coverage_miss();
  failures += test_three_seat_turn_solve();
  failures += test_three_seat_river_solve();
  failures += test_folded_player_solve();
  failures += test_folded_player_river_solve();
  failures += test_solve_deadline();
  failures += test_uniform_2seat_turn();
  failures += test_uniform_3seat_turn();
  failures += test_uniform_4seat_turn();
  failures += test_uniform_deep_information_budget_refusal();
  failures += test_uniform_river();
  failures += test_uniform_mid_street_faces_bet();
  failures += test_uniform_mid_street_3seat();
  failures += test_uniform_mid_street_river();
  failures += test_uniform_mid_street_faces_raise();
  failures += test_uniform_11seat_ineligible();
  if (failures == 0)
    std::printf("All subgame resolver tests passed.\n");
  else
    std::printf("%d subgame resolver test(s) failed.\n", failures);
  return failures;
}
