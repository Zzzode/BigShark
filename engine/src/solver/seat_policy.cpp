// RFC 0009 W2b: export_seat_policy and make_information_key (D3/D4).
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/unified_game.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace bs::solver {

using poker::Action;
using poker::GameState;
using poker::PublicAction;

InformationKey make_information_key(std::size_t actor, std::array<int, 2> cards,
                                    std::span<const int> board,
                                    std::span<const PublicAction> path) {
  if (actor > 9)
    throw std::invalid_argument("information-key actor must be in 0..9");
  std::sort(cards.begin(), cards.end());
  if (cards[0] < 0 || cards[1] > 51 || cards[0] == cards[1])
    throw std::invalid_argument("invalid private hand for information key");
  for (int card : board) {
    if (card == cards[0] || card == cards[1])
      throw std::invalid_argument("private card on public board");
  }
  const std::size_t board_count = board.size();
  if (board_count != 0 && board_count != 3 && board_count != 4 && board_count != 5)
    throw std::invalid_argument("information-key board count must be 0, 3, 4, or 5");

  InformationKey key{actor, static_cast<std::uint64_t>(cards[0]),
                     static_cast<std::uint64_t>(cards[1]), board_count};
  for (int card : board)
    key.push_back(static_cast<std::uint64_t>(card));
  for (const auto& event : path) {
    key.push_back(static_cast<std::uint64_t>(event.street));
    key.push_back(event.seat);
    key.push_back(static_cast<std::uint64_t>(event.action.type));
    key.push_back(event.action.target_total);
  }
  return key;
}

UnifiedGame to_unified_game(const HeadsUpGame& game) {
  UnifiedGame unified;
  const poker::HeadsUpRoot& root = game.root;
  poker::GameDef& def = unified.def;
  def.player_count = 2;
  def.button = root.button;
  def.big_blind = root.big_blind;
  def.ante = 0;  // the two-seat profile has no ante
  def.stacks[0] = root.stacks[0];
  def.stacks[1] = root.stacks[1];
  def.contributions[0] = root.contributions[0];
  def.contributions[1] = root.contributions[1];
  def.pot = root.pot;
  def.board[0] = root.flop[0];
  def.board[1] = root.flop[1];
  def.board[2] = root.flop[2];
  def.board_size = 3;
  def.blinds_posted[0] = root.blinds_posted[0];
  def.blinds_posted[1] = root.blinds_posted[1];
  def.preflop = root.preflop;
  def.variant = poker::RulesVariant::NoLimitHoldem;
  def.terminal = poker::TerminalDepth::River;
  unified.ranges[0] = game.ranges[0];
  unified.ranges[1] = game.ranges[1];
  unified.sizes = game.sizes;
  unified.fixed_runout = game.fixed_runout;
  return unified;
}

namespace {

// Recursive DFS over the tree, carrying a GameState cursor (internal nodes
// store no board or ledger, only leaves do) and the observed public-action
// path. At each Action node emits concrete rows for the acting seat.
void export_dfs(const tree::AbstractTree& tree, const NSeatPolicy& nseat_policy,
                std::map<InformationKey, SeatPolicyRow>& rows, std::size_t node_index,
                GameState& cursor, std::vector<PublicAction>& path) {
  const tree::TreeNode& node = tree.node(node_index);

  if (node.is_terminal())
    return;

  if (node.is_flop_deal())
    return;

  if (node.is_chance()) {
    for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
      const tree::TreeNode& child = tree.node(node.children[ci]);
      GameState next = cursor.after_card(child.incoming_card);
      export_dfs(tree, nseat_policy, rows, node.children[ci], next, path);
    }
    return;
  }

  // Action node: emit one concrete row per off-board combo for the acting
  // seat, looking up the sealed (bucket, node_index) row.
  const std::size_t seat = node.actor;
  const std::span<const int> board = cursor.board();
  const std::vector<int> board_vec(board.begin(), board.end());
  std::array<bool, 52> on_board{};
  for (int card : board)
    on_board[static_cast<std::size_t>(card)] = true;

  for (int c0 = 0; c0 < 52; ++c0) {
    if (on_board[static_cast<std::size_t>(c0)])
      continue;
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      if (on_board[static_cast<std::size_t>(c1)])
        continue;
      const std::array<int, 2> cards{c0, c1};
      const std::uint32_t bucket =
          abstraction::card_bucket(abstraction::CardBucketKind::CategoryTiersV1, cards, board_vec);
      const NSeatInformationKey nk{bucket, node_index};
      const auto it = nseat_policy.rows().find(nk);
      if (it == nseat_policy.rows().end())
        continue;  // exact miss: no sealed row
      const NSeatPolicyRow& nrow = it->second;
      SeatPolicyRow srow;
      srow.actions = nrow.actions;
      srow.probabilities = nrow.probabilities;
      srow.visits = nrow.visits;
      rows.emplace(make_information_key(seat, cards, board, path), std::move(srow));
    }
  }

  // Recurse into action children, advancing the cursor and the path.
  for (std::size_t ai = 0; ai < node.actions.size(); ++ai) {
    const Action& action = node.actions[ai];
    GameState next = cursor.after_action(seat, action);
    path.push_back(PublicAction{node.street, seat, action});
    export_dfs(tree, nseat_policy, rows, node.children[ai], next, path);
    path.pop_back();
  }
}

}  // namespace

SeatTrainingResult export_seat_policy(const NSeatTrainingResult& result,
                                      const tree::AbstractTree& tree,
                                      const std::vector<std::vector<WeightedHand>>& ranges,
                                      std::optional<abstraction::AbstractionId> card_id) {
  if (!poker::same_game_def(result.policy.game(), tree.def()))
    throw std::invalid_argument("export_seat_policy: result and tree disagree on game identity");
  if (result.policy.action_id() != tree.action_id())
    throw std::invalid_argument(
        "export_seat_policy: result and tree disagree on action abstraction");
  if (ranges.size() != tree.def().player_count)
    throw std::invalid_argument("export_seat_policy: range count must equal player count");

  SeatTrainingResult out;
  out.termination = result.termination;
  out.completed_iterations = result.completed_iterations;
  out.nodes = result.nodes;
  out.visits = result.visits;
  out.information_sets = result.information_sets;
  out.accounted_bytes = result.accounted_bytes;
  out.seed = result.seed;
  out.prng_state = result.prng_state;
  out.algorithm_revision = result.algorithm_revision;
  out.action_id = result.policy.action_id();
  out.card_id = card_id;
  out.terminal_depth = result.policy.game().terminal;

  out.policy.game_ = tree.def();
  out.policy.sizes_ = tree.action_abstraction().schedule();
  out.policy.ranges_ = ranges;
  out.policy.action_id_ = result.policy.action_id();
  out.policy.card_id_ = std::move(card_id);

  GameState cursor{tree.def()};
  std::vector<PublicAction> path;
  export_dfs(tree, result.policy, out.policy.rows_, tree.root_index(), cursor, path);

  return out;
}

}  // namespace bs::solver
