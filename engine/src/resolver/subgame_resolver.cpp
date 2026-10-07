// RFC 0011 Phase 1: turn-rooted subgame resolver implementation.
#include "bs/subgame_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace bs::resolver {

const char* to_string(SubgameResolveStatus status) noexcept {
  switch (status) {
    case SubgameResolveStatus::Solved:
      return "solved";
    case SubgameResolveStatus::Ineligible:
      return "ineligible";
    case SubgameResolveStatus::SolveDeadline:
      return "solve-deadline";
    case SubgameResolveStatus::CoverageMiss:
      return "coverage-miss";
    case SubgameResolveStatus::InvalidInput:
      return "invalid-input";
  }
  return "unknown";
}

namespace {

// Find the probability of `target` in a blueprint row view. Returns -1.0 when
// the action is off the blueprint tree (not present in the row's menu).
double action_probability(const BlueprintRowView& view, poker::Action target) {
  for (std::size_t i = 0; i < view.size; ++i) {
    if (view.actions[i] == target)
      return view.probabilities[i];
  }
  return -1.0;
}

// Whether two cards share a card with the board.
bool blocks_board(std::span<const int> board, std::array<int, 2> cards) {
  for (int c : board)
    if (c == cards[0] || c == cards[1])
      return true;
  return false;
}

}  // namespace

poker::GameDef SubgameResolver::build_subgame_def(const poker::GameState& node,
                                                  const poker::GameDef& blueprint_def) {
  poker::GameDef def{};
  def.player_count = 2;  // Phase 1: two-seat only
  def.big_blind = blueprint_def.big_blind;
  // Set the button so the initial actor on the turn is the current actor
  // (the hero). In heads-up, button=0 means seat 1 acts first postflop.
  const std::size_t hero = *node.actor();
  def.button = (hero == 0) ? 1 : 0;
  def.ante = 0;
  for (std::size_t s = 0; s < 2; ++s) {
    def.stacks[s] = node.players()[s].stack;
    def.contributions[s] = node.players()[s].net_contributed();
  }
  def.pot = node.pot();
  const auto board = node.board();
  for (std::size_t i = 0; i < board.size() && i < 5; ++i)
    def.board[i] = board[i];
  def.board_size = static_cast<std::uint8_t>(board.size());
  def.preflop = false;
  def.terminal = poker::TerminalDepth::River;
  return def;
}

abstraction::ActionAbstraction SubgameResolver::reconstruct_action_abstraction(
    const abstraction::AbstractionId& action_id, const abstraction::SizeSchedule& sizes) {
  if (abstraction::is_declared_coarse_action_id(action_id))
    return abstraction::ActionAbstraction::declared(sizes, abstraction::CoverSeeds::DeclaredOnly);
  return abstraction::ActionAbstraction(sizes);
}

std::vector<std::vector<solver::WeightedHand>> SubgameResolver::derive_prior_ranges(
    const poker::GameState& flop_root, std::span<const poker::PublicAction> flop_history,
    const BlueprintSource& blueprint, const poker::GameState& subgame_root,
    SubgameResolveStatus& status) const {
  const solver::UnifiedGame& game = blueprint.game();
  const std::size_t seat_count = game.def.player_count;

  // Declared, sorted per-combo weights and live per-combo prefix reach.
  std::array<std::map<std::array<int, 2>, double>, poker::kMaxUnifiedSeats> declared;
  std::array<std::map<std::array<int, 2>, double>, poker::kMaxUnifiedSeats> prefix_reach;
  for (std::size_t p = 0; p < seat_count; ++p) {
    for (const solver::WeightedHand& hand : game.ranges[p]) {
      auto cards = hand.cards;
      std::sort(cards.begin(), cards.end());
      if (!std::isfinite(hand.weight) || hand.weight <= 0) {
        status = SubgameResolveStatus::InvalidInput;
        return {};
      }
      declared[p].emplace(cards, hand.weight);
      prefix_reach[p].emplace(cards, 1.0);
    }
  }

  // Replay the flop action line from the flop root, accumulating per-seat
  // prefix reach. GameState is historyless, so the observed path arrives as a
  // PublicAction span; the blueprint row at each cursor is keyed by the
  // cursor's board plus the prefix span at that cursor (events 0..i-1).
  poker::GameState cursor(flop_root);
  try {
    for (std::size_t i = 0; i < flop_history.size(); ++i) {
      const poker::PublicAction& event = flop_history[i];
      // Deal any pending public cards (the turn card) from the subgame root's
      // board. The flop root already has 3 cards; the subgame root has 4.
      while (cursor.phase() == poker::Phase::Deal) {
        const std::size_t next = cursor.board().size();
        if (next >= subgame_root.board().size()) {
          status = SubgameResolveStatus::CoverageMiss;
          return {};
        }
        const int card = subgame_root.board()[next];
        cursor = cursor.after_card(card);
        for (std::size_t p = 0; p < seat_count; ++p)
          for (auto& [cards, reach] : prefix_reach[p])
            if (cards[0] == card || cards[1] == card)
              reach = 0;
      }
      if (cursor.phase() != poker::Phase::Action || !cursor.actor() ||
          *cursor.actor() != event.seat) {
        status = SubgameResolveStatus::CoverageMiss;
        return {};
      }

      const std::size_t actor = event.seat;
      const std::span<const poker::PublicAction> prefix(flop_history.data(), i);
      for (const auto& [cards, reach] : prefix_reach[actor]) {
        if (reach == 0)
          continue;
        const auto view = blueprint.row(cursor, prefix, actor, cards);
        if (!view) {
          status = SubgameResolveStatus::CoverageMiss;
          return {};
        }
        if (view->size == 0 || view->actions == nullptr || view->probabilities == nullptr) {
          status = SubgameResolveStatus::CoverageMiss;
          return {};
        }
        const double probability = action_probability(*view, event.action);
        if (probability < 0) {
          status = SubgameResolveStatus::CoverageMiss;
          return {};
        }
        prefix_reach[actor].find(cards)->second *= probability;
      }
      cursor = cursor.after_action(actor, event.action);
    }
    // Deal the turn card if the cursor is still in a Deal phase.
    while (cursor.phase() == poker::Phase::Deal &&
           cursor.board().size() < subgame_root.board().size()) {
      const int card = subgame_root.board()[cursor.board().size()];
      cursor = cursor.after_card(card);
      for (std::size_t p = 0; p < seat_count; ++p)
        for (auto& [cards, reach] : prefix_reach[p])
          if (cards[0] == card || cards[1] == card)
            reach = 0;
    }
  } catch (const std::exception&) {
    status = SubgameResolveStatus::CoverageMiss;
    return {};
  }

  // Build the per-seat prior ranges: each hand's weight is its declared weight
  // times its prefix reach, filtered to hands that don't block the subgame
  // root's board.
  const auto subgame_board = subgame_root.board();
  std::vector<std::vector<solver::WeightedHand>> ranges(seat_count);
  for (std::size_t p = 0; p < seat_count; ++p) {
    for (const auto& [cards, reach] : prefix_reach[p]) {
      if (reach <= 0)
        continue;
      if (blocks_board(subgame_board, cards))
        continue;
      const double weight = declared[p].at(cards) * reach;
      if (weight > 0)
        ranges[p].push_back({cards, weight});
    }
    if (ranges[p].empty()) {
      status = SubgameResolveStatus::CoverageMiss;
      return {};
    }
  }

  status = SubgameResolveStatus::Solved;
  return ranges;
}

SubgameResolveResult SubgameResolver::resolve(const poker::GameState& node,
                                              std::span<const poker::PublicAction> history,
                                              const BlueprintSource& blueprint,
                                              const ResolveLimits& limits) const {
  SubgameResolveResult result;

  // Phase 1: two-seat only.
  const solver::UnifiedGame& game = blueprint.game();
  if (game.def.player_count != 2) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }

  // The node must be an action node on the turn (board_size = 4). River
  // decisions (board_size = 5) are not covered in Phase 1.
  if (node.phase() != poker::Phase::Action || !node.actor()) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }
  const auto board = node.board();
  if (board.size() != 4) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }

  // Build the subgame GameDef and tree.
  poker::GameDef subgame_def;
  try {
    subgame_def = build_subgame_def(node, game.def);
    poker::validate(subgame_def);
  } catch (const std::exception&) {
    result.status = SubgameResolveStatus::InvalidInput;
    return result;
  }

  abstraction::ActionAbstraction action =
      reconstruct_action_abstraction(game.action_id, game.sizes);

  std::unique_ptr<tree::AbstractTree> tree_ptr;
  try {
    tree::TreeLimits tree_limits;
    tree_limits.max_nodes = limits.max_nodes;
    tree_limits.max_depth = limits.max_depth;
    tree_limits.max_bytes = limits.max_bytes;
    tree_ptr = std::make_unique<tree::AbstractTree>(subgame_def, action, tree_limits);
  } catch (const tree::tree_resource_exhausted&) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  } catch (const std::exception&) {
    result.status = SubgameResolveStatus::InvalidInput;
    return result;
  }
  const tree::AbstractTree& tree = *tree_ptr;

  // Extract the flop action line from the history. The flop root is the
  // blueprint's game; the subgame root is the current node. The flop history
  // is the subset of the full history on the flop street.
  std::vector<poker::PublicAction> flop_history;
  for (const auto& pa : history) {
    if (pa.street == poker::Street::Flop)
      flop_history.push_back(pa);
  }

  // Derive prior ranges at the subgame root.
  poker::GameState flop_root(game.def);
  SubgameResolveStatus range_status = SubgameResolveStatus::Ineligible;
  auto prior_ranges = derive_prior_ranges(flop_root, flop_history, blueprint, node, range_status);
  if (range_status != SubgameResolveStatus::Solved) {
    result.status = range_status;
    return result;
  }

  // Solve the subgame with CFR.
  solver::NSeatTrainerLimits trainer_limits;
  trainer_limits.max_nodes = limits.max_nodes;
  trainer_limits.max_information_sets = limits.max_information_sets;
  trainer_limits.max_depth = limits.max_depth;
  trainer_limits.max_bytes = limits.max_bytes;
  trainer_limits.wall = limits.time;

  solver::NSeatTrainingResult train_result;
  try {
    train_result = solver::train_nseat(tree, prior_ranges, limits.iterations, limits.public_seed,
                                       trainer_limits, nullptr, nullptr);
  } catch (const solver::nseat_training_exhausted&) {
    result.status = SubgameResolveStatus::SolveDeadline;
    return result;
  } catch (const std::exception&) {
    result.status = SubgameResolveStatus::InvalidInput;
    return result;
  }

  if (train_result.termination == solver::NSeatTerminationPhase::ResourceLimit) {
    result.status = SubgameResolveStatus::SolveDeadline;
    return result;
  }

  // Extract the candidate at the root node.
  const tree::TreeNode& root_node = tree.node(tree.root_index());
  result.node_actions = root_node.actions;
  result.root_pot = static_cast<double>(node.pot());
  result.seat_count = 2;
  result.nodes = tree.size();
  result.information_sets = train_result.information_sets;
  result.completed_iterations = train_result.completed_iterations;

  const std::size_t hero = *node.actor();
  const std::vector<int> board_vec(board.begin(), board.end());
  for (const solver::WeightedHand& hand : prior_ranges[hero]) {
    const auto cards = hand.cards;
    const std::uint32_t bucket = static_cast<std::uint32_t>(
        abstraction::card_bucket(solver::kNSeatCardKind, cards, board_vec));
    const solver::NSeatInformationKey key{bucket, static_cast<std::uint64_t>(tree.root_index())};
    const auto it = train_result.policy.rows().find(key);
    if (it == train_result.policy.rows().end())
      continue;  // unvisited bucket: skip (uniform fallback would be identical)
    const solver::NSeatPolicyRow& row = it->second;
    solver::PolicyRow policy_row;
    policy_row.actions = row.actions;
    policy_row.probabilities = row.probabilities;
    const solver::InformationKey info_key =
        solver::make_information_key(hero, cards, board, history);
    result.candidate.emplace(info_key, std::move(policy_row));
  }

  if (result.candidate.empty()) {
    result.status = SubgameResolveStatus::SolveDeadline;
    return result;
  }

  result.status = SubgameResolveStatus::Solved;
  return result;
}

SubgameResolver::SubgameResolver() = default;
SubgameResolver::~SubgameResolver() = default;

}  // namespace bs::resolver
