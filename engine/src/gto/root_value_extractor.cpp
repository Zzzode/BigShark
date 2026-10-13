// Exact policy-following extraction for concrete trees and structural templates.
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/root_value_extractor.hpp>
#include <cmath>
#include <stdexcept>

namespace bs::gto {

PostflopEvaluationTemplate::PostflopEvaluationTemplate(tree::AbstractTree representative)
    : representative_(std::make_unique<const tree::AbstractTree>(std::move(representative))) {
  const auto& def = representative_->def();
  if (def.player_count != 2 || def.board_size != 3 || def.preflop ||
      def.terminal != poker::TerminalDepth::River)
    throw std::invalid_argument("postflop template requires a heads-up flop-rooted river game");
}

bool PostflopEvaluationTemplate::matches(const solver::NSeatPolicy& policy) const {
  auto game = policy.game();
  if (game.board_size != 3)
    return false;
  game.board = representative_->def().board;
  return poker::same_game_def(game, representative_->def()) &&
         policy.action_id() == representative_->action_id();
}

PostflopEvaluationTemplate::Node PostflopEvaluationTemplate::node(std::size_t index) const {
  const auto& node = representative_->node(index);
  return {node.kind, node.actor, node.actions, node.children, node.terminal};
}

std::span<const std::int64_t> PostflopEvaluationTemplate::fold_utility(std::size_t terminal) const {
  return representative_->terminal(terminal).chip_utility;
}

std::span<const std::size_t> PostflopEvaluationTemplate::live_order(std::size_t terminal) const {
  const auto& payload = representative_->terminal(terminal);
  return {payload.live_order.data(), payload.live_count};
}

std::size_t PostflopEvaluationTemplate::size() const noexcept {
  return representative_->size();
}
std::size_t PostflopEvaluationTemplate::accounted_bytes() const noexcept {
  return sizeof(*this) + sizeof(tree::AbstractTree) + representative_->accounted_bytes();
}

namespace {

using Utility = std::array<double, poker::kMaxUnifiedSeats>;

// Both entry points instantiate one kernel; no second value policy exists.
struct ConcreteProjection {
  const tree::AbstractTree& tree;
  PostflopEvaluationTemplate::Node node(std::size_t index) const {
    const auto& n = tree.node(index);
    return {n.kind, n.actor, n.actions, n.children, n.terminal};
  }
  std::span<const std::int64_t> fold_utility(std::size_t terminal) const {
    return tree.terminal(terminal).chip_utility;
  }
  std::span<const std::size_t> live_order(std::size_t terminal) const {
    const auto& p = tree.terminal(terminal);
    return {p.live_order.data(), p.live_count};
  }
};

std::array<bool, 52> validate_request(const solver::NSeatPolicy& policy,
                                      const poker::GameState& state,
                                      std::span<const std::array<int, 2>> deal) {
  if (deal.size() != state.player_count() || deal.size() < 2 ||
      deal.size() > poker::kMaxUnifiedSeats || !poker::same_game_def(policy.game(), state.def()))
    throw std::invalid_argument("root_value_extractor: deal or root game mismatch");
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  std::array<bool, 52> blocked{};
  for (const auto& hole : deal) {
    for (int card : hole) {
      if (card < 0 || card >= 52 || used[card])
        throw std::invalid_argument("root_value_extractor: invalid or colliding private card");
      used[card] = true;
      blocked[card] = true;
    }
  }
  return blocked;
}

template <typename Projection>
Utility walk(const solver::NSeatPolicy& policy, const Projection& projection,
             std::size_t node_index, const poker::GameState& state,
             std::span<const std::array<int, 2>> deal, const std::array<bool, 52>& blocked) {
  const auto node = projection.node(node_index);
  const auto seats = state.player_count();
  Utility expected{};

  if (node.kind == tree::NodeKind::TerminalFold) {
    const auto utility = projection.fold_utility(node.terminal);
    if (utility.size() != seats)
      throw std::runtime_error("root_value_extractor: fold utility count mismatch");
    std::copy(utility.begin(), utility.end(), expected.begin());
    return expected;
  }
  if (node.kind == tree::NodeKind::TerminalShowdown) {
    std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> live_holes{};
    const auto live = projection.live_order(node.terminal);
    for (std::size_t i = 0; i < live.size(); ++i)
      live_holes[i] = deal[live[i]];
    const auto settlement = state.settle_showdown({live_holes.data(), live.size()});
    std::copy(settlement.chip_utility.begin(), settlement.chip_utility.end(), expected.begin());
    return expected;
  }
  if (node.kind == tree::NodeKind::Action) {
    if (node.actor >= seats || node.actions.empty() || node.actions.size() != node.children.size())
      throw std::runtime_error("root_value_extractor: invalid action node");
    const auto bucket =
        abstraction::card_bucket(solver::kNSeatCardKind, deal[node.actor], state.board());
    const auto it = policy.rows().find({bucket, node_index});
    const solver::NSeatPolicyRow* row = it == policy.rows().end() ? nullptr : &it->second;
    if (row && (row->actions.size() != node.actions.size() ||
                row->probabilities.size() != node.actions.size() ||
                !std::equal(row->actions.begin(), row->actions.end(), node.actions.begin())))
      throw std::runtime_error("root_value_extractor: policy row action menu mismatch");

    // Unsealed rows retain the trainer's uniform initial strategy.
    const auto uniform = 1.0 / static_cast<double>(node.actions.size());
    for (std::size_t action = 0; action < node.actions.size(); ++action) {
      const auto probability = row ? row->probabilities[action] : uniform;
      if (!std::isfinite(probability) || probability < 0.0)
        throw std::runtime_error("root_value_extractor: invalid probability");
      if (probability == 0.0)
        continue;
      const auto next = state.after_action(node.actor, node.actions[action]);
      const auto child = walk(policy, projection, node.children[action], next, deal, blocked);
      for (std::size_t seat = 0; seat < seats; ++seat)
        expected[seat] += probability * child[seat];
    }
    return expected;
  }
  if (node.kind == tree::NodeKind::Chance) {
    std::array<bool, 52> public_cards{};
    for (int card : state.board())
      public_cards[card] = true;
    std::size_t ordinal = 0;
    std::size_t count = 0;
    for (int card = 0; card < 52; ++card) {
      if (public_cards[card])
        continue;
      if (ordinal >= node.children.size())
        throw std::runtime_error("root_value_extractor: chance degree mismatch");
      const auto child_index = node.children[ordinal++];
      if (blocked[card])
        continue;
      const auto next = state.after_card(card);
      const auto child = walk(policy, projection, child_index, next, deal, blocked);
      for (std::size_t seat = 0; seat < seats; ++seat)
        expected[seat] += child[seat];
      ++count;
    }
    if (ordinal != node.children.size() || count == 0)
      throw std::runtime_error("root_value_extractor: invalid legal runout count");
    for (std::size_t seat = 0; seat < seats; ++seat)
      expected[seat] /= static_cast<double>(count);
    return expected;
  }
  throw std::runtime_error("root_value_extractor: unexpected node kind");
}

template <typename Projection>
std::vector<double> extract(const solver::NSeatPolicy& policy, const Projection& projection,
                            const poker::GameState& root_state,
                            std::span<const std::array<int, 2>> deal) {
  const auto blocked = validate_request(policy, root_state, deal);
  const auto utility = walk(policy, projection, 0, root_state, deal, blocked);
  return {utility.begin(), utility.begin() + deal.size()};
}

}  // namespace

std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const tree::AbstractTree& tree,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal) {
  if (!poker::same_game_def(policy.game(), tree.def()) || policy.action_id() != tree.action_id())
    throw std::invalid_argument("root_value_extractor: concrete tree identity mismatch");
  return extract(policy, ConcreteProjection{tree}, root_state, deal);
}

std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const PostflopEvaluationTemplate& evaluation,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal) {
  if (!evaluation.matches(policy))
    throw std::invalid_argument("root_value_extractor: template configuration mismatch");
  return extract(policy, evaluation, root_state, deal);
}

}  // namespace bs::gto
