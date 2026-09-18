#include <stdexcept>

#include "resolver_common.hpp"

namespace bs::resolver::detail {

std::vector<int> continuation_cards(const HeadsUpGame& game, const HeadsUpState& state,
                                    const std::array<std::array<int, 2>, 2>& hands) {
  const std::size_t slot = state.board().size() - 3;
  if (game.fixed_runout[slot])
    return {*game.fixed_runout[slot]};
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const auto& hand : hands)
    for (int card : hand)
      used[card] = true;
  for (auto card : game.fixed_runout)
    if (card)
      used[*card] = true;
  std::vector<int> cards;
  for (int card = 0; card < 52; ++card)
    if (!used[card])
      cards.push_back(card);
  return cards;
}

namespace {

// Exact expected-value walk over the remaining uniform/fixed public chance.
// Any ACTION phase below is a terminal-only violation.
double value_walk(const HeadsUpGame& game, const HeadsUpState& state,
                  const std::array<std::array<int, 2>, 2>& hands, std::size_t responder,
                  Budget& budget) {
  budget.visit();
  if (state.phase() == Phase::Folded)
    return static_cast<double>(state.settle_fold().net_utility[responder]);
  if (state.phase() == Phase::Showdown)
    return static_cast<double>(state.settle_showdown(hands).net_utility[responder]);
  if (state.phase() != Phase::Deal)
    throw RequireFailure("continuation is not terminal-only");
  const auto cards = continuation_cards(game, state, hands);
  if (cards.empty())
    throw RequireFailure("continuation has no legal public card");
  const double each = 1.0 / static_cast<double>(cards.size());
  double total = 0;
  for (int card : cards)
    total += each * value_walk(game, state.after_card(card), hands, responder, budget);
  return total;
}

// Structural terminal-only probe. After the hero's current action the game can
// only auto-run to a fold/showdown: which phase follows a public card depends
// solely on the remaining stacks and the street, never on the hole cards, so a
// single trace per action is sufficient. At a Deal phase we advance the first
// card not already on the board (hole-card exclusion changes the chance
// support and the payoff, not whether another ACTION node appears).
bool structural_walk(const HeadsUpState& state) {
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
    return true;
  if (state.phase() != Phase::Deal)
    return false;
  int next_card = -1;
  for (int card = 0; card < 52; ++card) {
    bool on_board = false;
    for (int board_card : state.board())
      if (board_card == card)
        on_board = true;
    if (!on_board) {
      next_card = card;
      break;
    }
  }
  if (next_card < 0)
    return false;
  return structural_walk(state.after_card(next_card));
}

}  // namespace

double continuation_utility_responder(const HeadsUpGame& game, const HeadsUpState& after,
                                      const std::array<std::array<int, 2>, 2>& hands,
                                      std::size_t responder, Budget& budget) {
  return value_walk(game, after, hands, responder, budget);
}

bool is_terminal_only(const HeadsUpGame&, const HeadsUpState& node,
                      const std::vector<Action>& node_actions) {
  if (node.phase() != Phase::Action || !node.actor())
    return false;
  const std::size_t hero = *node.actor();
  for (const Action& action : node_actions) {
    bool terminal = false;
    try {
      terminal = structural_walk(node.after_action(hero, action));
    } catch (const std::exception&) {
      return false;
    }
    if (!terminal)
      return false;
  }
  return true;
}

}  // namespace bs::resolver::detail
