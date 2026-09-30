#include "counterfactual_reach.hpp"

#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bs::resolver::detail {
namespace {

using poker::GameDef;
using poker::PublicAction;

// Resident six-field root identity, generalized to a GameDef: the flop cards,
// the per-seat stacks and closed-street contributions, the pot, the big blind,
// and the button. Deliberately looser than poker::same_game_def (which also
// compares preflop/blinds_posted/ante/variant/terminal); the resolver matches
// the resident's documented identity, not the stricter predicate.
bool same_root(const GameDef& a, const GameDef& b) {
  if (a.player_count != b.player_count || a.pot != b.pot || a.big_blind != b.big_blind ||
      a.button != b.button)
    return false;
  if (a.board_size < 3 || b.board_size < 3)
    return false;
  for (std::size_t i = 0; i < 3; ++i)
    if (a.board[i] != b.board[i])
      return false;
  for (std::size_t p = 0; p < a.player_count; ++p)
    if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p])
      return false;
  return true;
}

// Probability of one exact action in a blueprint row; -1 when absent.
double action_probability(const BlueprintRowView& row, const Action& wanted) {
  for (std::size_t i = 0; i < row.size; ++i)
    if (row.actions[i] == wanted)
      return row.probabilities[i];
  return -1.0;
}

bool valid_distribution(const BlueprintRowView& row) {
  double sum = 0;
  for (std::size_t i = 0; i < row.size; ++i) {
    if (!std::isfinite(row.probabilities[i]) || row.probabilities[i] < 0)
      return false;
    sum += row.probabilities[i];
  }
  return std::abs(sum - 1.0) <= 1e-12;
}

}  // namespace

ResolveStatus build_model(const GameState& node, std::span<const PublicAction> history,
                          const BlueprintSource& blueprint, Budget& budget, ReachModel& model,
                          std::string& detail) {
  const UnifiedGame& game = blueprint.game();
  auto fail = [&](ResolveStatus status, const std::string& why) {
    detail = why;
    return status;
  };

  // W2c-ii-a: the state layer is seat-generic, but the resolver gadget and the
  // independent certifier below are still two-seat (a single hero and the
  // 1-hero responder). Three-or-more-seat resolves are a later stage.
  if (game.def.player_count != 2)
    return fail(ResolveStatus::Ineligible, "resolver supports two seats only");
  if (node.phase() != Phase::Action || !node.actor())
    return fail(ResolveStatus::Ineligible, "resolve node is not an action node");
  if (!same_root(game.def, node.def()))
    return fail(ResolveStatus::CoverageMiss, "node root does not match the blueprint game root");

  model.node = node;
  model.history.assign(history.begin(), history.end());
  model.game = &game;
  model.hero = *node.actor();
  model.responder = 1 - model.hero;

  // Declared, sorted per-combo weights and live per-combo prefix reach.
  std::map<std::array<int, 2>, double> declared[2];
  std::map<std::array<int, 2>, double> prefix_reach[2];
  for (std::size_t p = 0; p < 2; ++p) {
    for (const WeightedHand& hand : game.ranges[p]) {
      auto cards = hand.cards;
      std::sort(cards.begin(), cards.end());
      if (!std::isfinite(hand.weight) || hand.weight <= 0)
        return fail(ResolveStatus::InvalidInput, "blueprint range has non-positive weight");
      declared[p].emplace(cards, hand.weight);
      prefix_reach[p].emplace(cards, 1.0);
    }
  }

  // ---- Replay the public path from the flop root to the resolve node, ------
  // validating every blueprint action and accumulating HERO prefix reach only.
  // GameState is historyless, so the observed path arrives as a PublicAction
  // span; the blueprint row at each cursor is keyed by the cursor's board plus
  // the prefix span at that cursor (events 0..i-1).
  GameState cursor(game.def);
  try {
    auto deal_next = [&](int card) {
      cursor = cursor.after_card(card);
      for (std::size_t p = 0; p < 2; ++p)
        for (auto& [cards, reach] : prefix_reach[p])
          if (cards[0] == card || cards[1] == card)
            reach = 0;
    };

    for (std::size_t i = 0; i < history.size(); ++i) {
      const PublicAction& event = history[i];
      while (cursor.phase() == Phase::Deal) {
        const std::size_t next = cursor.board().size();
        if (next >= node.board().size())
          return fail(ResolveStatus::CoverageMiss, "history runs past the observed board");
        deal_next(node.board()[next]);
      }
      if (cursor.phase() != Phase::Action || !cursor.actor() || *cursor.actor() != event.seat)
        return fail(ResolveStatus::CoverageMiss, "replayed history does not reach the actor");

      const std::size_t actor = event.seat;
      const std::span<const PublicAction> prefix(history.data(), i);
      for (const auto& [cards, reach] : prefix_reach[actor]) {
        if (reach == 0)
          continue;
        const auto view = blueprint.row(cursor, prefix, actor, cards);
        if (!view)
          return fail(ResolveStatus::CoverageMiss, "missing blueprint prefix row");
        if (view->size == 0 || view->actions == nullptr || view->probabilities == nullptr)
          return fail(ResolveStatus::CoverageMiss, "malformed blueprint prefix row");
        const double probability = action_probability(*view, event.action);
        if (probability < 0)
          return fail(ResolveStatus::CoverageMiss, "observed action is off the blueprint tree");
        if (actor == model.hero)
          prefix_reach[actor].find(cards)->second *= probability;
      }
      cursor = cursor.after_action(actor, event.action);
    }
    while (cursor.phase() == Phase::Deal && cursor.board().size() < node.board().size())
      deal_next(node.board()[cursor.board().size()]);
  } catch (const std::exception& error) {
    return fail(ResolveStatus::CoverageMiss, std::string("prefix replay failed: ") + error.what());
  }

  // Element-wise board cross-check: GameState::board() returns a span, so !=
  // would compare storage, not the cards.
  const auto cursor_board = cursor.board();
  const auto node_board = node.board();
  bool board_matches = cursor_board.size() == node_board.size();
  if (board_matches)
    for (std::size_t i = 0; i < cursor_board.size(); ++i)
      if (cursor_board[i] != node_board[i]) {
        board_matches = false;
        break;
      }
  if (!board_matches || cursor.phase() != node.phase() || !cursor.actor().has_value() ||
      *cursor.actor() != model.hero || cursor.pot() != node.pot())
    return fail(ResolveStatus::CoverageMiss, "replayed node disagrees with the observed node");
  for (std::size_t p = 0; p < 2; ++p)
    if (cursor.players()[p].stack != node.players()[p].stack ||
        cursor.players()[p].street_committed != node.players()[p].street_committed)
      return fail(ResolveStatus::CoverageMiss, "replayed chip state disagrees with the node");

  model.node_actions = solver::abstract_actions(node, game.sizes);
  if (model.node_actions.empty())
    return fail(ResolveStatus::Ineligible, "resolve node offers no action");

  // Declared, board-unblocked combination lists.
  for (std::size_t p = 0; p < 2; ++p) {
    auto& sink = p == model.hero ? model.hero_declared : model.responder_declared;
    for (const auto& [cards, weight] : declared[p]) {
      (void)weight;
      if (!hand_blocks_board(cards, node.board()))
        sink.push_back(cards);
    }
  }
  for (const auto& [cards, reach] : prefix_reach[model.hero])
    if (reach > 0 && !hand_blocks_board(cards, node.board()))
      model.live_hero.push_back(cards);
  if (model.live_hero.empty())
    return fail(ResolveStatus::CoverageMiss, "no positive-reach hero combination at the node");

  // Terminal-only graph property over the exact continuation.
  bool terminal = false;
  try {
    terminal = is_terminal_only(game, node, model.node_actions);
  } catch (const std::exception&) {
    return fail(ResolveStatus::Ineligible, "terminal-only check failed");
  }
  if (!terminal)
    return fail(ResolveStatus::Ineligible, "node is not terminal-only");

  // ---- Counterfactual weights and responder -x infosets. -------------------
  std::map<std::array<int, 2>, std::size_t> infoset_index;
  for (const auto& hero_cards : model.live_hero) {
    const double hero_weight = declared[model.hero].at(hero_cards);
    const double hero_prefix = prefix_reach[model.hero].at(hero_cards);
    for (const auto& responder_cards : model.responder_declared) {
      if (cards_conflict(hero_cards, responder_cards))
        continue;
      const double responder_weight = declared[model.responder].at(responder_cards);
      const double weight = hero_weight * responder_weight * hero_prefix;
      if (!std::isfinite(weight) || weight <= 0)
        return fail(ResolveStatus::InvalidInput, "non-finite counterfactual weight");

      auto [it, inserted] = infoset_index.emplace(responder_cards, model.infosets.size());
      if (inserted)
        model.infosets.push_back(ResponderInfoset{responder_cards, 0, 0, {}});
      ResponderInfoset& infoset = model.infosets[it->second];
      infoset.mass += weight;
      infoset.deals.push_back(model.deals.size());
      model.deals.push_back(GadgetDeal{hero_cards, responder_cards, weight});
    }
  }
  for (const auto& infoset : model.infosets)
    model.total_mass += infoset.mass;
  for (const auto& cards : model.responder_declared)
    if (!infoset_index.contains(cards))
      model.zero_mass_responder.push_back(cards);
  if (model.total_mass <= 0 || !std::isfinite(model.total_mass))
    return fail(ResolveStatus::CoverageMiss, "no positive counterfactual mass at the node");

  // ---- Constant terminal payoff matrix and baseline margins b(I). ----------
  try {
    model.leaf_values.assign(model.deals.size(),
                             std::vector<double>(model.node_actions.size(), 0.0));
    for (std::size_t deal_index = 0; deal_index < model.deals.size(); ++deal_index) {
      const GadgetDeal& deal = model.deals[deal_index];
      std::array<std::array<int, 2>, 2> hands{};
      hands[model.hero] = deal.hero;
      hands[model.responder] = deal.responder;
      for (std::size_t a = 0; a < model.node_actions.size(); ++a) {
        const GameState after = node.after_action(model.hero, model.node_actions[a]);
        model.leaf_values[deal_index][a] =
            continuation_utility_responder(game, after, hands, model.responder, budget);
      }
    }
    for (ResponderInfoset& infoset : model.infosets) {
      double weighted_value = 0;
      for (std::size_t deal_index : infoset.deals) {
        const GadgetDeal& deal = model.deals[deal_index];
        const auto view = blueprint.row(node, model.history, model.hero, deal.hero);
        if (!view || view->size != model.node_actions.size())
          return fail(ResolveStatus::CoverageMiss, "missing/incompatible hero baseline row");
        if (!valid_distribution(*view))
          return fail(ResolveStatus::CoverageMiss, "hero baseline row is not a distribution");
        for (std::size_t i = 0; i < view->size; ++i) {
          if (view->actions[i] != model.node_actions[i])
            return fail(ResolveStatus::CoverageMiss, "baseline action identity mismatch");
        }
        double deal_value = 0;
        for (std::size_t a = 0; a < model.node_actions.size(); ++a)
          deal_value += view->probabilities[a] * model.leaf_values[deal_index][a];
        weighted_value += deal.weight * deal_value;
      }
      infoset.baseline = weighted_value / infoset.mass;
      if (!std::isfinite(infoset.baseline))
        return fail(ResolveStatus::InvalidInput, "non-finite baseline margin");
    }
  } catch (const Exhausted&) {
    return fail(ResolveStatus::SolveDeadline, "baseline evaluation exceeded its budget");
  } catch (const std::exception& error) {
    return fail(ResolveStatus::CoverageMiss,
                std::string("baseline evaluation failed: ") + error.what());
  }

  return ResolveStatus::Certified;
}

}  // namespace bs::resolver::detail
