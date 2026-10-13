// Exact structural-template and independent public-chance regression coverage.
#include <algorithm>
#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/root_value_extractor.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::printf("CHECK failed: %s at line %d\n", #condition, __LINE__); \
      return 1;                                                           \
    }                                                                     \
  } while (0)

namespace {
using namespace bs;
using namespace bs::poker;
using namespace bs::tree;
using namespace bs::solver;
using namespace bs::gto;
using namespace bs::stage6;

using Rows = std::map<NSeatInformationKey, NSeatPolicyRow>;

struct ConcreteProjection {
  const AbstractTree& tree;
  std::size_t size() const { return tree.size(); }
  PostflopEvaluationTemplate::Node node(std::size_t index) const {
    const auto& node = tree.node(index);
    return {node.kind, node.actor, node.actions, node.children, node.terminal};
  }
  std::span<const std::int64_t> fold_utility(std::size_t terminal) const {
    return tree.terminal(terminal).chip_utility;
  }
  std::span<const std::size_t> live_order(std::size_t terminal) const {
    const auto& payload = tree.terminal(terminal);
    return {payload.live_order.data(), payload.live_count};
  }
};

// Record all exposed structural fields exactly, rather than comparing hashes.
// The compact flat projection excludes concrete cards, boards, and seat ledgers.
template <typename Projection, typename Emit>
void visit_projection(const Projection& projection, Emit emit) {
  emit(projection.size());
  for (std::size_t index = 0; index < projection.size(); ++index) {
    const auto node = projection.node(index);
    emit(static_cast<std::uint64_t>(node.kind));
    emit(node.actor);
    emit(node.terminal);
    emit(node.actions.size());
    emit(node.children.size());
    for (const auto action : node.actions) {
      emit(static_cast<std::uint64_t>(action.type));
      emit(action.target_total);
    }
    for (auto child : node.children)
      emit(child);
    if (node.terminal != kNoNode) {
      const auto live = projection.live_order(node.terminal);
      emit(live.size());
      for (auto seat : live)
        emit(seat);
      const auto utility = projection.fold_utility(node.terminal);
      emit(utility.size());
      for (auto chips : utility)
        emit(static_cast<std::uint64_t>(chips));
    }
  }
}

std::vector<std::uint64_t> capture_projection(const AbstractTree& tree) {
  std::size_t count = 0;
  visit_projection(ConcreteProjection{tree}, [&](auto) { ++count; });
  std::vector<std::uint64_t> words;
  words.reserve(count);
  visit_projection(ConcreteProjection{tree}, [&](auto value) { words.push_back(value); });
  return words;
}

bool same_projection(const PostflopEvaluationTemplate& evaluation,
                     const std::vector<std::uint64_t>& expected) {
  std::size_t index = 0;
  bool matches = true;
  visit_projection(evaluation, [&](auto value) {
    if (index >= expected.size() || expected[index] != value)
      matches = false;
    ++index;
  });
  return matches && index == expected.size();
}

bool same_values(std::span<const double> actual, std::span<const double> expected,
                 double tolerance) {
  if (actual.size() != expected.size())
    return false;
  for (std::size_t seat = 0; seat < actual.size(); ++seat) {
    if (!std::isfinite(actual[seat]) || !std::isfinite(expected[seat]) ||
        std::abs(actual[seat] - expected[seat]) > tolerance) {
      std::printf("value mismatch: seat=%zu actual=%.15g expected=%.15g\n", seat, actual[seat],
                  expected[seat]);
      return false;
    }
  }
  return true;
}

std::array<std::array<int, 2>, 2> blocking_deal(const GameDef& def) {
  std::vector<int> available;
  for (int card = 0; card < 52; ++card) {
    if (std::find(def.board.begin(), def.board.begin() + def.board_size, card) ==
        def.board.begin() + def.board_size)
      available.push_back(card);
  }
  return {{{available.front(), available[available.size() / 2]},
           {available[available.size() / 2 + 1], available.back()}}};
}

Rows sparse_production_rows(const AbstractTree& tree) {
  Rows rows;
  std::array<bool, 4> saw_street{};
  for (const auto& node : tree.nodes()) {
    if (!node.is_action() || saw_street[static_cast<std::size_t>(node.street)])
      continue;
    saw_street[static_cast<std::size_t>(node.street)] = true;
    NSeatPolicyRow row;
    row.actions = node.actions;
    const double total = static_cast<double>(row.actions.size() * (row.actions.size() + 1)) / 2;
    for (std::size_t action = 0; action < row.actions.size(); ++action)
      row.probabilities.push_back(static_cast<double>(action + 1) / total);
    for (std::uint32_t bucket = 1; bucket <= 9; ++bucket)
      rows.emplace(NSeatInformationKey{bucket, node.index}, row);
  }
  return rows;
}

template <typename F>
bool invalid_request(F run) {
  try {
    run();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

int production_buckets() {
  constexpr std::array<int, 3> representative{0, 1, 2};
  constexpr std::array<std::array<int, 3>, 3> alternatives = {
      std::array<int, 3>{0, 21, 42},  // dry
      std::array<int, 3>{0, 1, 20},   // paired
      std::array<int, 3>{0, 16, 36},  // monotone
  };
  for (std::size_t bucket = 0; bucket < SprBucketConfig::kBucketCount; ++bucket) {
    const auto started = std::chrono::steady_clock::now();
    const auto game = make_postflop_def(alternatives[bucket % alternatives.size()], bucket);
    const auto action = make_postflop_abstraction(bucket);
    const auto deal = blocking_deal(game);
    TreeLimits limits;
    limits.max_nodes = 5'000'000;
    limits.max_bytes = std::size_t{4} << 30;
    std::vector<std::uint64_t> projection;
    std::vector<double> uniform_value;
    std::vector<double> sparse_value;
    Rows sparse_rows;
    std::size_t nodes = 0;
    // Destroy the concrete tree before constructing the representative: only
    // one full tree and the compact projection remain live at each stage.
    {
      const AbstractTree concrete(game, action, limits);
      nodes = concrete.size();
      projection = capture_projection(concrete);
      const auto empty = NSeatPolicy::from_parts(game, action.id(), {});
      uniform_value = extract_root_value(empty, concrete, GameState(game), deal);
      sparse_rows = sparse_production_rows(concrete);
      const auto sparse = NSeatPolicy::from_parts(game, action.id(), sparse_rows);
      CHECK(!sparse.rows().empty());
      CHECK(sparse.rows().size() < concrete.size());
      sparse_value = extract_root_value(sparse, concrete, GameState(game), deal);
    }
    PostflopEvaluationTemplate evaluation(
        AbstractTree(make_postflop_def(representative, bucket), action, limits));
    CHECK(evaluation.size() == nodes);
    CHECK(evaluation.accounted_bytes() > sizeof(PostflopEvaluationTemplate));
    CHECK(same_projection(evaluation, projection));
    const auto empty = NSeatPolicy::from_parts(game, action.id(), {});
    const auto sparse = NSeatPolicy::from_parts(game, action.id(), std::move(sparse_rows));
    CHECK(evaluation.matches(empty));
    CHECK(evaluation.matches(sparse));
    CHECK(same_values(extract_root_value(empty, evaluation, GameState(game), deal), uniform_value,
                      1e-9));
    CHECK(same_values(extract_root_value(sparse, evaluation, GameState(game), deal), sparse_value,
                      1e-9));
    if (bucket == 0) {
      auto wrong_game = game;
      ++wrong_game.stacks[0];
      const auto wrong = NSeatPolicy::from_parts(wrong_game, action.id(), {});
      CHECK(!evaluation.matches(wrong));
      CHECK(invalid_request(
          [&] { (void)extract_root_value(wrong, evaluation, GameState(wrong_game), deal); }));
      auto wrong_id = action.id();
      wrong_id.digest ^= 1;
      const auto wrong_action = NSeatPolicy::from_parts(game, wrong_id, {});
      CHECK(!evaluation.matches(wrong_action));
      CHECK(invalid_request(
          [&] { (void)extract_root_value(wrong_action, evaluation, GameState(game), deal); }));
      auto collision = deal;
      collision[0][0] = game.board[0];
      CHECK(invalid_request(
          [&] { (void)extract_root_value(empty, evaluation, GameState(game), collision); }));
      collision = deal;
      collision[1][0] = collision[0][0];
      CHECK(invalid_request(
          [&] { (void)extract_root_value(empty, evaluation, GameState(game), collision); }));
      CHECK(invalid_request(
          [&] { (void)extract_root_value(empty, evaluation, GameState(wrong_game), deal); }));
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    std::printf("bucket %zu: %zu nodes, projection %zu bytes, %.3f seconds\n", bucket, nodes,
                projection.size() * sizeof(std::uint64_t), elapsed.count());
    std::fflush(stdout);
  }
  return 0;
}

// Independently use the stage-6 declared legal menu, never tree-node menus or
// NSeatPolicy lookup. At one-chip stacks every aggressive total snaps to one,
// so this menu coincides with the abstraction without sharing its builder.
class SmallGameBehavior final : public BehaviorPolicy {
 public:
  explicit SmallGameBehavior(bool sparse) : sparse_(sparse) {}
  bool biased(const GameState& state) const { return sparse_ && state.board().back() % 3 == 0; }
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t seat, HoleCards,
                                         const PolicyContext&) const override {
    const auto menu = declared_behavior_menu(state, seat);
    std::vector<PolicyAction> result;
    const double total = static_cast<double>(menu.size() * (menu.size() + 1)) / 2;
    for (std::size_t action = 0; action < menu.size(); ++action) {
      const double probability = biased(state) ? static_cast<double>(action + 1) / total
                                               : 1.0 / static_cast<double>(menu.size());
      result.push_back({menu[action], probability});
    }
    return result;
  }

 private:
  bool sparse_;
};

Rows independent_rows(const AbstractTree& tree, const SmallGameBehavior& behavior) {
  struct Frame {
    std::size_t index;
    GameState state;
  };
  std::vector<Frame> stack;
  stack.push_back({0, GameState(tree.def())});
  Rows rows;
  while (!stack.empty()) {
    Frame frame = std::move(stack.back());
    stack.pop_back();
    const auto& node = tree.node(frame.index);
    if (node.is_action()) {
      const auto distribution = behavior.distribution(frame.state, node.actor, {0, 1}, {});
      if (distribution.size() != node.actions.size())
        throw std::runtime_error("independent menu differs from one-chip fixture");
      NSeatPolicyRow row;
      row.actions = node.actions;
      for (const auto action : node.actions) {
        const auto found = std::find_if(distribution.begin(), distribution.end(),
                                        [&](const auto& item) { return item.action == action; });
        if (found == distribution.end())
          throw std::runtime_error("independent menu action missing");
        row.probabilities.push_back(found->probability);
      }
      if (behavior.biased(frame.state)) {
        for (std::uint32_t bucket = 1; bucket <= 9; ++bucket)
          rows.emplace(NSeatInformationKey{bucket, node.index}, row);
      }
      for (std::size_t child = 0; child < node.children.size(); ++child)
        stack.push_back(
            {node.children[child], frame.state.after_action(node.actor, node.actions[child])});
    } else if (node.is_chance()) {
      for (const auto child : node.children)
        stack.push_back({child, frame.state.after_card(tree.node(child).incoming_card)});
    }
  }
  return rows;
}

GameDef small_game(std::size_t seats, std::size_t board_size, bool folded_blocker = false) {
  GameDef game{};
  game.player_count = seats;
  game.button = 1;
  game.big_blind = seats == 2 ? 1 : 2;
  game.pot = seats;
  for (std::size_t seat = 0; seat < seats; ++seat) {
    game.stacks[seat] = 1;
    game.contributions[seat] = 1;
  }
  game.board = {4, 20, 36, 48, 8};
  game.board_size = static_cast<std::uint8_t>(board_size);
  game.folded[2] = folded_blocker;
  return game;
}

int chance_oracle() {
  const auto action = abstraction::ActionAbstraction::identity();
  for (const auto& game : {small_game(2, 3), small_game(2, 4), small_game(3, 4, true)}) {
    const AbstractTree concrete(game, action);
    std::vector<HoleCards> deal;
    if (game.player_count == 2) {
      const auto pair = blocking_deal(game);
      deal.assign(pair.begin(), pair.end());
    } else {
      // The folded seat blocks the first and last chance ordinals; all six
      // private cards remain excluded from the river, not just the live four.
      deal = {{1, 25}, {26, 50}, {0, 51}};
    }
    for (bool sparse : {false, true}) {
      const SmallGameBehavior behavior(sparse);
      const auto rows = sparse ? independent_rows(concrete, behavior) : Rows{};
      const auto policy = NSeatPolicy::from_parts(game, action.id(), rows);
      const std::vector<const BehaviorPolicy*> behaviors(game.player_count, &behavior);
      OracleCounts counts;
      const auto oracle = exact_deal_utility(game, deal, behaviors, &counts);
      CHECK(counts.chance_nodes > 0);
      CHECK(counts.showdown_leaves > 0);
      CHECK(counts.fold_leaves > 0);
      const std::span<const double> expected{oracle.data(), game.player_count};
      const auto actual = extract_root_value(policy, concrete, GameState(game), deal);
      CHECK(same_values(actual, expected, 1e-9));
      if (game.player_count == 2 && game.board_size == 3) {
        auto representative = game;
        representative.board = {0, 1, 2};
        const PostflopEvaluationTemplate evaluation(AbstractTree(representative, action));
        CHECK(same_values(extract_root_value(policy, evaluation, GameState(game), deal), expected,
                          1e-9));
      }
      if (game.player_count == 3)
        CHECK(std::abs(actual[2] + 1.0) <= 1e-9);
      std::printf("independent chance oracle: %zu seats, board %u, sparse %d, %zu rows: PASS\n",
                  game.player_count, game.board_size, sparse, rows.size());
    }
  }
  return 0;
}
}  // namespace

int main() {
  CHECK(chance_oracle() == 0);
  CHECK(production_buckets() == 0);
  std::printf("postflop template structural and chance checks: PASS\n");
  return 0;
}
