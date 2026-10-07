// RFC 0011 Phase 1: subgame resolver tests.
//
// Verifies that the subgame resolver builds a turn-rooted River-terminal
// subtree, derives prior ranges from a flop-terminal blueprint, solves with
// CFR, and serves a valid candidate at `approximate`. The fixture is a
// heads-up flop check-through that reaches the turn, so the prior-range
// derivation is a simple two-step prefix replay.
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

// (ii) Ineligible: a river decision (board_size=5) is not covered in Phase 1.
int test_river_ineligible() {
  FlopCheckThrough f;
  MapBlueprint bp(f.unified);
  install_flop_rows(bp, f);

  // Construct a river-rooted state directly (board_size=5). The subgame
  // resolver only supports turn-rooted subgames in Phase 1.
  GameDef river_def = f.unified.def;
  river_def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  river_def.board_size = 5;
  river_def.terminal = TerminalDepth::River;
  GameState river_root(river_def);

  SubgameResolver resolver;
  const auto result = resolver.resolve(river_root, f.history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Ineligible);
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

// (iv) Ineligible: a 3-seat blueprint is not supported in Phase 1.
int test_three_seat_ineligible() {
  FlopCheckThrough f;
  // Mutate the blueprint to 3 seats. No flop rows are installed: the resolver
  // returns Ineligible on the seat-count check before any row lookup, and a
  // 3-seat flop-terminal GameDef would fail validate (the fixture's stacks
  // array only carries two seats).
  f.unified.def.player_count = 3;
  MapBlueprint bp(f.unified);

  SubgameResolver resolver;
  const auto result = resolver.resolve(f.turn_root, f.history, bp, test_limits());

  CHECK(result.status == SubgameResolveStatus::Ineligible);
  return 0;
}

// (v) Solve deadline: a zero-iteration cap returns InvalidInput (the trainer
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

}  // namespace

int main() {
  int failures = 0;
  failures += test_basic_solve();
  failures += test_river_ineligible();
  failures += test_coverage_miss();
  failures += test_three_seat_ineligible();
  failures += test_solve_deadline();
  if (failures == 0)
    std::printf("All subgame resolver tests passed.\n");
  else
    std::printf("%d subgame resolver test(s) failed.\n", failures);
  return failures;
}
