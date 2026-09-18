// RFC 0005 Stage 9 bounded-resolve microbenchmark (manual target, NOT a CTest).
//
// Measures solve + independent certification wall time, accepted bound slack,
// and iterations on the exact tiny fixed-runout terminal game used by the
// solver-level oracle (flop pot 2, matched 1/1, responder jams its last 1, the
// hero holds one winner and one loser against a single responder combo). The
// gadget equilibrium is exact (call the winner, fold the loser), so the
// certified candidate margin and the worst bound slack are deterministic.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/resolver.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::HeadsUpState;
using bs::solver::HeadsUpGame;
using bs::solver::InformationKey;
using bs::solver::PolicyRow;
using namespace bs::resolver;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

class MapBlueprint final : public BlueprintSource {
 public:
  explicit MapBlueprint(HeadsUpGame game) : game_(std::move(game)) {}
  void set_row(const HeadsUpState& state, std::size_t player, std::array<int, 2> cards,
               std::vector<Action> actions, std::vector<double> probs) {
    rows_[{bs::solver::information_key(state, cards), player}] = {std::move(actions),
                                                                  std::move(probs)};
  }
  const HeadsUpGame& game() const override { return game_; }
  std::string_view artifact_digest() const override { return digest_; }
  std::optional<BlueprintRowView> row(const HeadsUpState& state, std::size_t player,
                                      std::array<int, 2> cards) const override {
    const auto it = rows_.find({bs::solver::information_key(state, cards), player});
    if (it == rows_.end())
      return std::nullopt;
    return BlueprintRowView{it->second.first.data(), it->second.second.data(),
                            it->second.second.size()};
  }

 private:
  using Key = std::pair<InformationKey, std::size_t>;
  using Row = std::pair<std::vector<Action>, std::vector<double>>;
  HeadsUpGame game_;
  std::string digest_ = "benchmark0000000000000000000000000000000000000000000000000000";
  std::map<Key, Row> rows_;
};

}  // namespace

int main() {
  HeadsUpGame game;
  std::array<int, 2> W{card("Ac"), card("Ad")};
  std::array<int, 2> L{card("8c"), card("8d")};
  std::array<int, 2> O{card("Qc"), card("Qd")};
  std::sort(W.begin(), W.end());
  std::sort(L.begin(), L.end());
  std::sort(O.begin(), O.end());
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[1] = {{W, 1}, {L, 1}};
  game.ranges[0] = {{O, 1}};
  game.fixed_runout = {card("Js"), card("9c")};
  const HeadsUpState node = HeadsUpState(game.root).after_action(0, {ActionType::Bet, 1});
  const std::vector<Action> actions{{ActionType::Fold}, {ActionType::Call}};

  MapBlueprint blueprint(game);
  blueprint.set_row(HeadsUpState(game.root), 0, O, {{ActionType::Check}, {ActionType::Bet, 1}},
                    {0, 1});
  blueprint.set_row(node, 1, W, actions, {0, 1});  // baseline all-call, b(O)=0
  blueprint.set_row(node, 1, L, actions, {0, 1});

  Resolver resolver;
  for (std::uint64_t iterations : {10000ULL, 50000ULL, 100000ULL, 250000ULL}) {
    ResolveLimits limits;
    limits.iterations = iterations;
    limits.time = std::chrono::seconds(60);
    limits.max_nodes = 1000000000;
    const auto start = std::chrono::steady_clock::now();
    const ResolveResult result = resolver.resolve(node, blueprint, limits);
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    resolver.clear_cache();  // force fresh work each row
    if (result.status != ResolveStatus::Certified) {
      std::printf("iterations=%llu FAILED status=%d\n", static_cast<unsigned long long>(iterations),
                  static_cast<int>(result.status));
      return 1;
    }
    double worst_slack_pot = -1e300;
    for (const auto& margin : result.margins) {
      if (!margin.positive_mass)
        continue;
      worst_slack_pot = std::max(worst_slack_pot, margin.slack / result.root_pot);
    }
    std::printf(
        "bounded-resolve iterations=%7llu solve+cert=%8.3f ms nodes=%zu infosets=%zu "
        "worst_slack=%.3e pot\n",
        static_cast<unsigned long long>(iterations), elapsed, result.nodes, result.information_sets,
        worst_slack_pot);
  }
  return 0;
}
