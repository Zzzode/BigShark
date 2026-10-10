// RFC 0013 Stage 1: root-value extraction (implementation).

#include <bs/abstraction.hpp>
#include <bs/root_value_extractor.hpp>
#include <stdexcept>

namespace bs::gto {

namespace {

// Recursive tree walk. Returns the expected per-seat chip utility at the
// given node for the fixed joint deal.
std::vector<double> walk(const solver::NSeatPolicy& policy, const tree::AbstractTree& tree,
                         std::size_t node_idx, const poker::GameState& state,
                         std::span<const std::array<int, 2>> deal) {
  const tree::TreeNode& node = tree.node(node_idx);
  const std::size_t n = state.player_count();

  if (node.is_terminal()) {
    const tree::TerminalPayload& payload = tree.terminal(node.terminal);
    if (node.kind == tree::NodeKind::TerminalFold) {
      return std::vector<double>(payload.chip_utility.begin(), payload.chip_utility.end());
    }
    if (node.kind == tree::NodeKind::TerminalShowdown) {
      // Pack live seats' hole cards in live_order and settle.
      std::vector<std::array<int, 2>> live_holes;
      live_holes.reserve(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live_holes.push_back(deal[payload.live_order[i]]);
      const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
      return std::vector<double>(settlement.chip_utility.begin(), settlement.chip_utility.end());
    }
    throw std::runtime_error(
        "root_value_extractor: unexpected terminal kind (frontier leaf in "
        "river-terminal game)");
  }

  if (node.is_action()) {
    const std::size_t actor = node.actor;
    const std::array<int, 2>& hole = deal[actor];
    const auto board_span = state.board();
    const std::vector<int> board_vec(board_span.begin(), board_span.end());
    const std::uint32_t bucket = abstraction::card_bucket(solver::kNSeatCardKind, hole, board_vec);

    const solver::NSeatInformationKey key{bucket, node_idx};
    const auto& rows = policy.rows();
    const auto it = rows.find(key);

    // External-sampling MCCFR seals only info sets on sampled paths. A
    // flop-rooted river-terminal game has turn/river chance nodes, so many
    // action nodes behind them are never visited. The trainer's own
    // current_strategy on a fresh row is uniform; use the same default here
    // (matching the continuation-range export's fallback) so the traversal
    // can proceed through unsealed nodes. This is the trainer's default
    // strategy, not a heuristic substitution.
    const std::vector<double>* probs = nullptr;
    std::vector<double> uniform;
    if (it != rows.end()) {
      const solver::NSeatPolicyRow& row = it->second;
      if (row.actions.size() != node.actions.size())
        throw std::runtime_error("root_value_extractor: policy row action count mismatch");
      probs = &row.probabilities;
    } else {
      uniform.assign(node.actions.size(), 1.0 / static_cast<double>(node.actions.size()));
      probs = &uniform;
    }

    std::vector<double> expected(n, 0.0);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      const double prob = (*probs)[a];
      if (prob == 0.0)
        continue;
      const poker::GameState next = state.after_action(actor, node.actions[a]);
      const std::vector<double> child = walk(policy, tree, node.children[a], next, deal);
      for (std::size_t s = 0; s < n; ++s)
        expected[s] += prob * child[s];
    }
    return expected;
  }

  if (node.is_chance()) {
    // Enumerate legal runout cards: board-only list minus all hole cards.
    const std::vector<int> runout = tree::public_runout_cards(state);
    std::array<bool, 52> blocked{};
    for (const auto& hole : deal)
      for (int c : hole)
        blocked[c] = true;

    std::vector<double> expected(n, 0.0);
    std::size_t count = 0;
    for (std::size_t ordinal = 0; ordinal < runout.size(); ++ordinal) {
      const int card = runout[ordinal];
      if (blocked[card])
        continue;
      const poker::GameState next = state.after_card(card);
      const std::vector<double> child = walk(policy, tree, node.children[ordinal], next, deal);
      for (std::size_t s = 0; s < n; ++s)
        expected[s] += child[s];
      ++count;
    }
    if (count == 0)
      throw std::runtime_error("root_value_extractor: no legal runout cards");
    for (std::size_t s = 0; s < n; ++s)
      expected[s] /= static_cast<double>(count);
    return expected;
  }

  throw std::runtime_error("root_value_extractor: unexpected node kind");
}

}  // namespace

std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const tree::AbstractTree& tree,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal) {
  if (deal.size() != root_state.player_count())
    throw std::invalid_argument("root_value_extractor: deal size does not match player count");
  return walk(policy, tree, tree.root_index(), root_state, deal);
}

}  // namespace bs::gto
