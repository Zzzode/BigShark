#include "preflop_nash_conv_detail.hpp"

#include <algorithm>
#include <bs/prng.hpp>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace bs::benchmarks::preflop {
namespace {

using poker::Action;
using poker::ActionType;
using poker::GameState;
using poker::PublicAction;
using solver::InformationKey;

constexpr std::uint64_t kDealDomain = 0x70666465616c0001ULL;
constexpr std::uint64_t kOpponentDomain = 0x70666f7070000001ULL;
constexpr std::uint64_t kLearnDomain = 0x70666c6561726e01ULL;
constexpr std::uint64_t kConfirmDomain = 0x7066636f6e666901ULL;

[[noreturn]] void invalid(const char* message) {
  throw std::invalid_argument(std::string("preflop deviation measurement: ") + message);
}

std::size_t bounded(bs::SplitMix64& rng, std::size_t count) {
  if (count == 0)
    invalid("empty random choice");
  const auto bound = static_cast<std::uint64_t>(count);
  const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
  std::uint64_t draw;
  do {
    draw = rng.next_u64();
  } while (draw < threshold);
  return static_cast<std::size_t>(draw % bound);
}

void validate_inputs(const tree::AbstractTree& tree, const ProfileQuery& profile,
                     std::span<const World> worlds, std::size_t seat) {
  const auto& def = tree.def();
  if (def.terminal != poker::TerminalDepth::Flop || def.board_size != 0)
    invalid("requires a preflop-rooted flop-terminal tree");
  if (seat >= def.player_count || !profile || worlds.empty())
    invalid("invalid seat, profile, or empty worlds");
  std::set<std::uint64_t> seeds;
  for (const auto& world : worlds) {
    if (!seeds.insert(world.seed).second)
      invalid("duplicate world seed");
    if (world.holes.size() != def.player_count || world.flops.empty())
      invalid("wrong hand count or empty flop samples");
    std::array<bool, 52> used{};
    for (const auto& hand : world.holes) {
      if (hand[0] >= hand[1])
        invalid("hole cards must be sorted and distinct");
      for (const int card : hand) {
        if (card < 0 || card >= 52 || used[card])
          invalid("invalid or colliding hole card");
        used[card] = true;
      }
    }
    for (const auto& flop : world.flops) {
      auto blocked = used;
      for (const int card : flop) {
        if (card < 0 || card >= 52 || blocked[card])
          invalid("invalid or blocked flop card");
        blocked[card] = true;
      }
    }
  }
}

std::size_t fallback_index(std::span<const Action> menu) {
  for (const auto type : {ActionType::Check, ActionType::Call, ActionType::Fold})
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == type)
        return i;
  invalid("profile fallback has no check/call/fold action");
}

struct Walker {
  const tree::AbstractTree& tree;
  const ProfileQuery& profile;
  const gto::FrontierEvaluator& frontier;
  std::size_t seat;
  std::span<const World> worlds;
  Counts& counts;

  InformationKey key(const GameState& state, const World& world,
                     std::span<const PublicAction> path) const {
    return solver::make_information_key(*state.actor(), world.holes[*state.actor()], state.board(),
                                        path);
  }

  std::vector<double> probabilities(const InformationKey& key, const std::vector<Action>& menu) {
    ++counts.profile_queries;
    const auto row = profile(key, menu);
    if (!row) {
      ++counts.profile_fallbacks;
      std::vector<double> result(menu.size(), 0.0);
      result[fallback_index(menu)] = 1.0;
      return result;
    }
    if (row->actions != menu || row->probabilities.size() != menu.size() || menu.empty())
      invalid("profile menu or distribution size drift");
    double total = 0.0;
    for (double p : row->probabilities) {
      if (!std::isfinite(p) || p < 0.0)
        invalid("profile probabilities must be finite and nonnegative");
      total += p;
    }
    if (!std::isfinite(total) || std::abs(total - 1.0) > 1e-9)
      invalid("profile probabilities must sum to one");
    return row->probabilities;
  }

  std::size_t sample_action(const World& world, std::size_t node_index,
                            std::span<const double> probabilities) const {
    // The fixed public node token cannot depend on hidden holdings, responder,
    // traversal order, or whether this is the profile or deviation leg.
    bs::SplitMix64 token(node_index ^ kOpponentDomain);
    bs::SplitMix64 rng(world.seed ^ token.next_u64());
    const double draw = static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
    double cumulative = 0.0;
    std::size_t last_positive = 0;
    for (std::size_t a = 0; a < probabilities.size(); ++a) {
      if (probabilities[a] > 0.0)
        last_positive = a;
      cumulative += probabilities[a];
      if (draw < cumulative)
        return a;
    }
    return last_positive;
  }

  double leaf(const GameState& state, std::size_t node_index, const World& world) const {
    const auto& node = tree.node(node_index);
    if (node.kind == tree::NodeKind::TerminalFold) {
      const auto& values = tree.terminal(node.terminal).chip_utility;
      if (values.size() != tree.def().player_count)
        invalid("invalid fold utility count");
      return static_cast<double>(values[seat]);
    }
    if (!node.is_flop_deal())
      invalid("unexpected node: only action, fold, and flop-deal nodes are supported");
    double total = 0.0;
    for (const auto& flop : world.flops) {
      GameState next = state;
      for (int card : flop)
        next = next.after_card(card);
      if (next.phase() != poker::Phase::Frontier)
        invalid("flop sample did not reach the frontier");
      const auto values = frontier.evaluate(flop, world.holes, tree::make_frontier_payload(next));
      if (values.size() != tree.def().player_count)
        invalid("frontier returned the wrong utility count");
      for (double value : values)
        if (!std::isfinite(value))
          invalid("frontier returned nonfinite utility");
      total += values[seat];
    }
    const double value = total / static_cast<double>(world.flops.size());
    if (!std::isfinite(value))
      invalid("frontier utility average overflowed");
    return value;
  }

  void validate_node(const GameState& state, std::size_t node_index) const {
    const auto& node = tree.node(node_index);
    if (!state.actor() || node.actor != *state.actor() || node.actions.empty() ||
        node.children.size() != node.actions.size())
      invalid("invalid action node projection");
  }

  // Every child public history has a unique tree node. Descendant responses
  // can therefore be frozen before pooling this node without later revision.
  std::vector<double> learn(const GameState& state, std::size_t node_index,
                            std::span<const std::size_t> ids, const std::vector<PublicAction>& path,
                            std::map<InformationKey, FrozenAction>& response) {
    std::vector<double> result(ids.size());
    if (ids.empty())
      return result;
    const auto& node = tree.node(node_index);
    if (!node.is_action()) {
      for (std::size_t i = 0; i < ids.size(); ++i)
        result[i] = leaf(state, node_index, worlds[ids[i]]);
      return result;
    }
    validate_node(state, node_index);
    const auto actor = *state.actor();
    auto child_path = path;
    child_path.push_back({state.street(), actor, {}});
    if (actor != seat) {
      std::vector<std::vector<std::size_t>> groups(node.actions.size());
      for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& world = worlds[ids[i]];
        const auto probs = probabilities(key(state, world, path), node.actions);
        groups[sample_action(world, node_index, probs)].push_back(i);
      }
      for (std::size_t a = 0; a < groups.size(); ++a) {
        if (groups[a].empty())
          continue;
        std::vector<std::size_t> child_ids;
        child_ids.reserve(groups[a].size());
        for (auto i : groups[a])
          child_ids.push_back(ids[i]);
        child_path.back().action = node.actions[a];
        const auto values = learn(state.after_action(actor, node.actions[a]), node.children[a],
                                  child_ids, child_path, response);
        for (std::size_t i = 0; i < groups[a].size(); ++i)
          result[groups[a][i]] = values[i];
      }
      return result;
    }

    std::vector<InformationKey> keys;
    keys.reserve(ids.size());
    std::map<InformationKey, std::vector<double>> sums;
    for (auto id : ids) {
      auto own_key = key(state, worlds[id], path);
      // Reject invalid profile input even at responder nodes, where learning
      // otherwise needs no probability to evaluate alternative actions.
      probabilities(own_key, node.actions);
      sums.try_emplace(own_key, node.actions.size(), 0.0);
      keys.push_back(std::move(own_key));
    }
    std::vector<std::vector<double>> children;
    children.reserve(node.actions.size());
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      child_path.back().action = node.actions[a];
      children.push_back(learn(state.after_action(actor, node.actions[a]), node.children[a], ids,
                               child_path, response));
      for (std::size_t i = 0; i < ids.size(); ++i)
        sums.at(keys[i])[a] += children.back()[i];
    }
    for (const auto& [own_key, utilities] : sums) {
      for (double utility : utilities)
        if (!std::isfinite(utility))
          invalid("pooled utility overflowed");
      const auto chosen = static_cast<std::size_t>(
          std::max_element(utilities.begin(), utilities.end()) - utilities.begin());
      if (!response.emplace(own_key, FrozenAction{node.actions, chosen}).second)
        invalid("information key appeared at multiple public nodes");
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
      result[i] = children[response.at(keys[i]).chosen][i];
    return result;
  }

  double confirm(const GameState& state, std::size_t node_index, const World& world,
                 const std::vector<PublicAction>& path, const LearnedResponse* frozen) {
    const auto& node = tree.node(node_index);
    if (!node.is_action())
      return leaf(state, node_index, world);
    validate_node(state, node_index);
    const auto own_key = key(state, world, path);
    const auto probs = probabilities(own_key, node.actions);
    std::size_t chosen = sample_action(world, node_index, probs);
    const auto actor = *state.actor();
    if (frozen && actor == seat) {
      ++counts.response_queries;
      const auto found = frozen->actions.find(own_key);
      if (found == frozen->actions.end()) {
        ++counts.response_misses;
      } else {
        if (found->second.menu != node.actions || found->second.chosen >= node.actions.size())
          invalid("frozen response menu drift or invalid action");
        chosen = found->second.chosen;
      }
    }
    auto child_path = path;
    child_path.push_back({state.street(), actor, node.actions[chosen]});
    return confirm(state.after_action(actor, node.actions[chosen]), node.children[chosen], world,
                   child_path, frozen);
  }
};

}  // namespace

std::vector<World> sample_worlds(std::size_t seats, std::size_t count, std::size_t flops_per_world,
                                 std::uint64_t seed) {
  if (seats < poker::kMinUnifiedSeats || seats > poker::kMaxUnifiedSeats || count == 0 ||
      flops_per_world == 0)
    invalid("sampling requires 2..10 seats and positive world/flop counts");
  bs::SplitMix64 seeds(seed);
  std::vector<World> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    World world;
    world.seed = seeds.next_u64();
    bs::SplitMix64 rng(world.seed ^ kDealDomain);
    std::array<int, 52> deck;
    std::iota(deck.begin(), deck.end(), 0);
    for (std::size_t j = deck.size() - 1; j > 0; --j)
      std::swap(deck[j], deck[bounded(rng, j + 1)]);
    for (std::size_t s = 0; s < seats; ++s) {
      std::array<int, 2> hand{deck[2 * s], deck[2 * s + 1]};
      std::sort(hand.begin(), hand.end());
      world.holes.push_back(hand);
    }
    for (std::size_t f = 0; f < flops_per_world; ++f) {
      auto available = deck;
      std::array<int, 3> flop{};
      for (std::size_t c = 0; c < 3; ++c) {
        const auto start = 2 * seats + c;
        std::swap(available[start], available[start + bounded(rng, 52 - start)]);
        flop[c] = available[start];
      }
      world.flops.push_back(flop);
    }
    result.push_back(std::move(world));
  }
  return result;
}

LearnedResponse learn_response(const tree::AbstractTree& tree, const ProfileQuery& profile,
                               const gto::FrontierEvaluator& frontier, std::size_t seat,
                               std::span<const World> worlds) {
  validate_inputs(tree, profile, worlds, seat);
  LearnedResponse result;
  result.seat = seat;
  for (const auto& world : worlds)
    result.learn_seeds.push_back(world.seed);
  std::vector<std::size_t> ids(worlds.size());
  std::iota(ids.begin(), ids.end(), 0);
  Walker walker{tree, profile, frontier, seat, worlds, result.counts};
  walker.learn(GameState(tree.def()), tree.root_index(), ids, {}, result.actions);
  return result;
}

Estimate estimate(std::span<const double> samples) {
  if (samples.empty())
    invalid("cannot estimate an empty sample");
  double mean = 0.0;
  double m2 = 0.0;
  std::size_t n = 0;
  for (double value : samples) {
    if (!std::isfinite(value))
      invalid("nonfinite confirmation sample");
    ++n;
    const double delta = value - mean;
    mean += delta / static_cast<double>(n);
    m2 += delta * (value - mean);
  }
  if (!std::isfinite(mean) || !std::isfinite(m2))
    invalid("confirmation statistics overflowed");
  const double se =
      n > 1 ? std::sqrt(std::max(0.0, m2) / static_cast<double>(n - 1) / static_cast<double>(n))
            : std::numeric_limits<double>::quiet_NaN();
  return {mean, se, mean - 1.96 * se, mean + 1.96 * se};
}

SeatConfirmation confirm_response(const tree::AbstractTree& tree, const ProfileQuery& profile,
                                  const gto::FrontierEvaluator& frontier,
                                  const LearnedResponse& frozen, std::span<const World> worlds) {
  validate_inputs(tree, profile, worlds, frozen.seat);
  const std::set<std::uint64_t> learn_seeds(frozen.learn_seeds.begin(), frozen.learn_seeds.end());
  for (const auto& world : worlds)
    if (learn_seeds.contains(world.seed))
      invalid("learning and confirmation seeds overlap");
  SeatConfirmation result;
  std::vector<double> profile_values;
  std::vector<double> deviation_values;
  Walker walker{tree, profile, frontier, frozen.seat, worlds, result.counts};
  for (const auto& world : worlds) {
    const auto state = GameState(tree.def());
    const double baseline = walker.confirm(state, tree.root_index(), world, {}, nullptr);
    const double deviation = walker.confirm(state, tree.root_index(), world, {}, &frozen);
    profile_values.push_back(baseline);
    deviation_values.push_back(deviation);
    result.paired_gains.push_back(deviation - baseline);
  }
  result.profile_value = estimate(profile_values).mean;
  result.deviation_value = estimate(deviation_values).mean;
  result.gain = estimate(result.paired_gains);
  return result;
}

Measurement measure(const tree::AbstractTree& tree, const ProfileQuery& profile,
                    const gto::FrontierEvaluator& frontier, std::size_t deals, std::size_t flops,
                    std::uint64_t seed) {
  Measurement result;
  result.learn_seed = seed ^ kLearnDomain;
  result.confirm_seed = seed ^ kConfirmDomain;
  result.worlds_per_phase = deals;
  const auto learn = sample_worlds(tree.def().player_count, deals, flops, result.learn_seed);
  const auto confirm = sample_worlds(tree.def().player_count, deals, flops, result.confirm_seed);
  std::vector<double> sums(deals, 0.0);
  for (std::size_t seat = 0; seat < tree.def().player_count; ++seat) {
    result.learned.push_back(learn_response(tree, profile, frontier, seat, learn));
    result.seats.push_back(
        confirm_response(tree, profile, frontier, result.learned.back(), confirm));
    for (std::size_t i = 0; i < deals; ++i)
      sums[i] += result.seats.back().paired_gains[i];
  }
  result.summed_gain = estimate(sums);
  return result;
}

}  // namespace bs::benchmarks::preflop
