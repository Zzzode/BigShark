// RFC 0011: turn/river subgame resolver.
//
// When a turn or river decision arises that the flop-terminal blueprint does
// not cover and the terminal-only resolver cannot handle (remaining opponent
// actions), this resolver:
//
//   1. Builds a River-terminal subtree rooted at the turn state. The subtree
//      materializes the remaining action streets and the river chance (44
//      cards for 2 seats, 42 for 3), which is bounded and fast.
//   2. Derives prior ranges from the flop-terminal blueprint by walking the
//      observed flop action line. For each seat, the per-hand reach is the
//      product of blueprint action probabilities along the path.
//   3. Solves the subtree with CFR (train_nseat). River leaves are valued by
//      the existing settle_showdown settlement (exact equity, no new
//      evaluator).
//   4. Serves the candidate at `approximate` (no certification in Phase 1).
//
// Supports 2..3 seats. For a river decision the resolver still builds a
// turn-rooted subgame (the blueprint is flop-terminal and has no turn rows),
// solves it once, then walks the solved tree — following the observed turn
// actions and the observed river card — to extract the candidate at the
// river action node. The turn-rooted subgame's CFR policy at every node is
// the GTO policy for that node, so the river node's policy is correct.
//
// The resolver is offline (no wire, no credentials) and thread-affine (call
// from one thread). Folded seats are carried through the GameDef folded
// bitmap: the constructor seeds players_[p].folded, and the existing
// fold-aware machinery (next_actor, refresh_live, settlement, trainer)
// handles them correctly. Folded seats keep their gross contributions as
// dead money and never become the actor.
//
// The fallback chain is: blueprint -> terminal-only resolver -> subgame
// resolver -> operational fallback. A build failure, solve deadline, or
// coverage miss returns the typed status so the engine can fall through.
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

poker::GameDef SubgameResolver::build_subgame_def(const poker::GameState& turn_root,
                                                  std::size_t hero,
                                                  const poker::GameDef& blueprint_def) {
  poker::GameDef def{};
  def.player_count = blueprint_def.player_count;
  def.big_blind = blueprint_def.big_blind;
  // Set the button so the initial actor on the subgame's first action street
  // is the hero. The multiway state constructor (multiway.cpp) sets the first
  // postflop actor to next_actor(button) = (button + 1) % N, so the button
  // must be one seat counterclockwise of the hero. This formula is consistent
  // with the heads-up case: for N=2 it produces (hero == 0) ? 1 : 0, matching
  // the original heads-up convention.
  const std::size_t n = def.player_count;
  def.button = (hero + n - 1) % n;
  def.ante = blueprint_def.ante;
  for (std::size_t s = 0; s < def.player_count; ++s) {
    def.stacks[s] = turn_root.players()[s].stack;
    def.contributions[s] = turn_root.players()[s].net_contributed();
    def.folded[s] = turn_root.players()[s].folded;
  }
  def.pot = turn_root.pot();
  const auto board = turn_root.board();
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
    poker::GameState& turn_root_out, SubgameResolveStatus& status) const {
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
  // The cursor is declared outside the try so the blocks_board filter below
  // can use the turn root's board (4 cards), not the subgame root's board
  // (which may include the river card for river-rooted decisions).
  poker::GameState cursor(flop_root);
  try {
    for (std::size_t i = 0; i < flop_history.size(); ++i) {
      const poker::PublicAction& event = flop_history[i];
      // Deal any pending public cards (the turn card) from the subgame root's
      // board. The flop root already has 3 cards; the subgame root has 4 or 5.
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

  // Output the turn root so the caller can build the subgame def from it.
  turn_root_out = cursor;

  // Build the per-seat prior ranges: each hand's weight is its declared weight
  // times its prefix reach, filtered to hands that don't block the turn root's
  // board. The prior ranges are at the turn root (4 cards), so the filter uses
  // the cursor's board, not the subgame root's board (which may include the
  // river card for river-rooted decisions — hands blocking the river card are
  // still valid at the turn root, since the river card is dealt by the tree's
  // chance node).
  const auto turn_board = cursor.board();
  std::vector<std::vector<solver::WeightedHand>> ranges(seat_count);
  for (std::size_t p = 0; p < seat_count; ++p) {
    for (const auto& [cards, reach] : prefix_reach[p]) {
      if (reach <= 0)
        continue;
      if (blocks_board(turn_board, cards))
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

std::size_t SubgameResolver::walk_to_river_node(const tree::AbstractTree& tree,
                                                std::size_t root_index,
                                                std::span<const poker::PublicAction> turn_history,
                                                int river_card, const poker::GameDef& subgame_def) {
  std::size_t node_index = root_index;
  // The cursor is constructed from the River-terminal subgame def (not the
  // blueprint-derived turn root, which is Frontier because the blueprint is
  // flop-terminal). A River-terminal rooted board opens in Action phase.
  poker::GameState cursor(subgame_def);

  // Follow the observed turn actions through the tree.
  for (const auto& pa : turn_history) {
    const tree::TreeNode& node = tree.node(node_index);
    if (!node.is_action())
      return static_cast<std::size_t>(-1);
    if (!cursor.actor() || *cursor.actor() != pa.seat)
      return static_cast<std::size_t>(-1);

    // Find the observed action in the node's menu.
    std::size_t action_idx = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < node.actions.size(); ++i) {
      if (node.actions[i] == pa.action) {
        action_idx = i;
        break;
      }
    }
    if (action_idx == static_cast<std::size_t>(-1))
      return static_cast<std::size_t>(-1);  // observed action off the tree menu

    node_index = node.children[action_idx];
    try {
      cursor = cursor.after_action(pa.seat, pa.action);
    } catch (const std::exception&) {
      return static_cast<std::size_t>(-1);
    }
  }

  // After the turn action street, the tree has a Chance node for the river
  // card. The chance children enumerate public_runout_cards (ascending
  // board-only list); the river card's ordinal is its child index.
  const tree::TreeNode& chance_node = tree.node(node_index);
  if (!chance_node.is_chance())
    return static_cast<std::size_t>(-1);

  std::size_t ordinal = 0;
  for (int c = 0; c < river_card; ++c) {
    bool on_board = false;
    for (int b : cursor.board()) {
      if (b == c) {
        on_board = true;
        break;
      }
    }
    if (!on_board)
      ++ordinal;
  }
  if (ordinal >= chance_node.children.size())
    return static_cast<std::size_t>(-1);

  return chance_node.children[ordinal];
}

SubgameResolveResult SubgameResolver::resolve(const poker::GameState& node,
                                              std::span<const poker::PublicAction> history,
                                              const BlueprintSource& blueprint,
                                              const ResolveLimits& limits) const {
  SubgameResolveResult result;

  // Supports 2..3 seats (matching the belief model's exact range).
  const solver::UnifiedGame& game = blueprint.game();
  if (game.def.player_count < 2 || game.def.player_count > 3) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }

  // The node must be an action node on the turn (board_size = 4) or river
  // (board_size = 5).
  if (node.phase() != poker::Phase::Action || !node.actor()) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }
  const auto board = node.board();
  if (board.size() != 4 && board.size() != 5) {
    result.status = SubgameResolveStatus::Ineligible;
    return result;
  }

  // Guard against a node/blueprint seat-count mismatch. Folded seats are
  // carried through the GameDef folded bitmap and handled by the existing
  // fold-aware machinery, so no Ineligible guard is needed for them.
  if (node.player_count() != game.def.player_count) {
    result.status = SubgameResolveStatus::InvalidInput;
    return result;
  }

  // Extract the flop action line from the history. The flop root is the
  // blueprint's game. The flop history is the subset of the full history on
  // the flop street.
  std::vector<poker::PublicAction> flop_history;
  for (const auto& pa : history) {
    if (pa.street == poker::Street::Flop)
      flop_history.push_back(pa);
  }

  // Derive prior ranges at the turn root. The blueprint is flop-terminal, so
  // prior ranges are always at the turn root — even for river decisions. The
  // turn_root_out cursor is the state after replaying the flop line and
  // dealing the turn card; the subgame is built from it.
  poker::GameState flop_root(game.def);
  poker::GameState turn_root(flop_root);
  SubgameResolveStatus range_status = SubgameResolveStatus::Ineligible;
  std::vector<std::vector<solver::WeightedHand>> prior_ranges;
  try {
    prior_ranges =
        derive_prior_ranges(flop_root, flop_history, blueprint, node, turn_root, range_status);
  } catch (const std::exception&) {
    result.status = SubgameResolveStatus::InvalidInput;
    return result;
  }
  if (range_status != SubgameResolveStatus::Solved) {
    result.status = range_status;
    return result;
  }

  // Build the subgame GameDef and tree from the turn root. The subgame is
  // always turn-rooted (board_size=4, terminal=River), even for river
  // decisions: the blueprint has no turn rows, so the turn-rooted subgame's
  // CFR policy supplies the turn/river strategy at every node. The hero is
  // the decision node's actor (the turn_root cursor is Frontier, so its
  // actor() is nullopt).
  const std::size_t hero = *node.actor();
  poker::GameDef subgame_def;
  try {
    subgame_def = build_subgame_def(turn_root, hero, game.def);
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

  // Determine the candidate node: the root for a turn decision, the river
  // action node for a river decision (reached by walking the solved tree
  // along the observed turn actions and river card).
  std::size_t candidate_node = tree.root_index();
  if (board.size() == 5) {
    std::vector<poker::PublicAction> turn_history;
    for (const auto& pa : history) {
      if (pa.street == poker::Street::Turn)
        turn_history.push_back(pa);
    }
    const std::size_t river_node =
        walk_to_river_node(tree, tree.root_index(), turn_history, board[4], subgame_def);
    if (river_node == static_cast<std::size_t>(-1)) {
      result.status = SubgameResolveStatus::Ineligible;
      return result;
    }
    candidate_node = river_node;
  }

  // Extract the candidate at the candidate node.
  const tree::TreeNode& candidate_tree_node = tree.node(candidate_node);
  result.node_actions = candidate_tree_node.actions;
  result.root_pot = static_cast<double>(turn_root.pot());
  result.seat_count = game.def.player_count;
  result.nodes = tree.size();
  result.information_sets = train_result.information_sets;
  result.completed_iterations = train_result.completed_iterations;

  const std::vector<int> board_vec(board.begin(), board.end());
  for (const solver::WeightedHand& hand : prior_ranges[hero]) {
    const auto cards = hand.cards;
    const std::uint32_t bucket = static_cast<std::uint32_t>(
        abstraction::card_bucket(solver::kNSeatCardKind, cards, board_vec));
    const solver::NSeatInformationKey key{bucket, static_cast<std::uint64_t>(candidate_node)};
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
