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

  // W2c-ii-c: the resolver gadget is hero-vs-field. The field is the separable
  // sum of every non-hero seat, each owning its own -x infoset, so the gadget
  // and the per-seat certifier support two and three seats. Four-or-more-seat
  // resolves stay a later stage (the resident belief model is likewise exact
  // for two and three seats only).
  if (game.def.player_count < 2 || game.def.player_count > 3)
    return fail(ResolveStatus::Ineligible, "resolver supports two or three seats only");
  if (node.phase() != Phase::Action || !node.actor())
    return fail(ResolveStatus::Ineligible, "resolve node is not an action node");
  if (!same_root(game.def, node.def()))
    return fail(ResolveStatus::CoverageMiss, "node root does not match the blueprint game root");

  model.node = node;
  model.history.assign(history.begin(), history.end());
  model.game = &game;
  model.hero = *node.actor();
  model.seat_count = game.def.player_count;

  // Declared, sorted per-combo weights and live per-combo prefix reach.
  std::array<std::map<std::array<int, 2>, double>, poker::kMaxUnifiedSeats> declared;
  std::array<std::map<std::array<int, 2>, double>, poker::kMaxUnifiedSeats> prefix_reach;
  for (std::size_t p = 0; p < model.seat_count; ++p) {
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
      for (std::size_t p = 0; p < model.seat_count; ++p)
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
  for (std::size_t p = 0; p < model.seat_count; ++p)
    if (cursor.players()[p].stack != node.players()[p].stack ||
        cursor.players()[p].street_committed != node.players()[p].street_committed)
      return fail(ResolveStatus::CoverageMiss, "replayed chip state disagrees with the node");

  model.node_actions = solver::abstract_actions(node, game.sizes);
  if (model.node_actions.empty())
    return fail(ResolveStatus::Ineligible, "resolve node offers no action");

  // Declared, board-unblocked combination lists per seat.
  for (std::size_t p = 0; p < model.seat_count; ++p) {
    for (const auto& [cards, weight] : declared[p]) {
      (void)weight;
      if (!hand_blocks_board(cards, node.board()))
        model.seat_declared[p].push_back(cards);
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

  // ---- Counterfactual weights and per-seat -x infosets. --------------------
  // The hero's holding is the outermost enumeration dimension; the non-hero
  // seats ascend inside it. Each non-hero seat owns one -x infoset keyed by
  // (seat, cards). The hero prefix reach is folded into the weight LAST,
  // matching the frozen two-seat floating-point association.
  std::map<GadgetKey, std::size_t> infoset_index;
  std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> joint{};
  std::vector<std::size_t> others;
  for (std::size_t s = 0; s < model.seat_count; ++s)
    if (s != model.hero)
      others.push_back(s);

  ResolveStatus enum_status = ResolveStatus::Certified;
  auto enumerate = [&](auto&& self, std::size_t depth, double running) -> void {
    if (enum_status != ResolveStatus::Certified)
      return;
    if (depth == others.size()) {
      const double hero_prefix = prefix_reach[model.hero].at(joint[model.hero]);
      const double weight = running * hero_prefix;
      if (!std::isfinite(weight) || weight <= 0) {
        enum_status = ResolveStatus::InvalidInput;
        detail = "non-finite counterfactual weight";
        return;
      }
      const std::size_t deal_index = model.deals.size();
      model.deals.push_back(GadgetDeal{joint, weight});
      for (std::size_t s : others) {
        const GadgetKey key{s, joint[s]};
        auto [it, inserted] = infoset_index.emplace(key, model.infosets.size());
        if (inserted)
          model.infosets.push_back(SeatInfoset{s, joint[s], 0, 0, {}});
        SeatInfoset& infoset = model.infosets[it->second];
        infoset.mass += weight;
        infoset.deals.push_back(deal_index);
      }
      return;
    }
    const std::size_t s = others[depth];
    for (const auto& cards : model.seat_declared[s]) {
      bool conflicts = cards_conflict(cards, joint[model.hero]);
      for (std::size_t k = 0; k < depth && !conflicts; ++k)
        conflicts = cards_conflict(cards, joint[others[k]]);
      if (conflicts)
        continue;
      joint[s] = cards;
      self(self, depth + 1, running * declared[s].at(cards));
    }
  };

  for (const auto& hero_cards : model.live_hero) {
    joint[model.hero] = hero_cards;
    enumerate(enumerate, 0, declared[model.hero].at(hero_cards));
    if (enum_status != ResolveStatus::Certified)
      return enum_status;
  }
  if (enum_status != ResolveStatus::Certified)
    return enum_status;

  // Each joint deal contributes its weight to (seat_count - 1) per-seat
  // infosets, so the infoset-mass sum over-counts the deal-weight total by that
  // factor; the chance normalization needs the deal-weight total. At two seats
  // the factor is one and the association is the frozen one.
  for (const auto& infoset : model.infosets)
    model.total_mass += infoset.mass;
  model.total_mass /= static_cast<double>(model.seat_count - 1);
  for (std::size_t s : others)
    for (const auto& cards : model.seat_declared[s])
      if (!infoset_index.contains(GadgetKey{s, cards}))
        model.zero_mass[s].push_back(cards);
  if (model.total_mass <= 0 || !std::isfinite(model.total_mass))
    return fail(ResolveStatus::CoverageMiss, "no positive counterfactual mass at the node");

  // ---- Constant terminal payoff matrix and baseline margins b_s(I). --------
  try {
    model.leaf_values.assign(
        model.deals.size(),
        std::vector<std::array<double, poker::kMaxUnifiedSeats>>(model.node_actions.size()));
    for (std::size_t deal_index = 0; deal_index < model.deals.size(); ++deal_index) {
      const GadgetDeal& deal = model.deals[deal_index];
      for (std::size_t a = 0; a < model.node_actions.size(); ++a) {
        const GameState after = node.after_action(model.hero, model.node_actions[a]);
        continuation_utility_all_seats(
            game, after, std::span<const std::array<int, 2>>(deal.hands.data(), model.seat_count),
            std::span<double>(model.leaf_values[deal_index][a].data(), model.seat_count), budget);
      }
    }
    for (SeatInfoset& infoset : model.infosets) {
      const std::size_t s = infoset.seat;
      double weighted_value = 0;
      for (std::size_t deal_index : infoset.deals) {
        const GadgetDeal& deal = model.deals[deal_index];
        const auto view = blueprint.row(node, model.history, model.hero, deal.hands[model.hero]);
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
          deal_value += view->probabilities[a] * model.leaf_values[deal_index][a][s];
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
