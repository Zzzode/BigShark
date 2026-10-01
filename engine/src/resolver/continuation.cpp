#include <stdexcept>

#include "resolver_common.hpp"

namespace bs::resolver::detail {

std::vector<int> continuation_cards(const UnifiedGame& game, const GameState& state,
                                    std::span<const std::array<int, 2>> hands) {
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

// Exact expected-value walk over the remaining uniform/fixed public chance,
// writing every seat's expected net chip utility into `utility` (indexed by
// seat). Any ACTION phase below is a terminal-only violation. At a showdown
// the holes span is indexed by LIVE POSITION (live_players ascending), while
// the returned chip_utility is indexed by seat, so each seat's value is read
// back by its seat index.
void value_walk(const UnifiedGame& game, const GameState& state,
                std::span<const std::array<int, 2>> hands, std::span<double> utility,
                Budget& budget) {
  budget.visit();
  if (state.phase() == Phase::Folded) {
    const auto settle = state.settle_fold();
    for (std::size_t seat = 0; seat < utility.size(); ++seat)
      utility[seat] = static_cast<double>(settle.chip_utility[seat]);
    return;
  }
  if (state.phase() == Phase::Showdown) {
    std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> live_holes{};
    std::size_t live_count = 0;
    for (std::size_t seat : state.live_players())
      live_holes[live_count++] = hands[seat];
    const auto settle =
        state.settle_showdown(std::span<const std::array<int, 2>>(live_holes.data(), live_count));
    for (std::size_t seat = 0; seat < utility.size(); ++seat)
      utility[seat] = static_cast<double>(settle.chip_utility[seat]);
    return;
  }
  if (state.phase() != Phase::Deal)
    throw RequireFailure("continuation is not terminal-only");
  const auto cards = continuation_cards(game, state, hands);
  if (cards.empty())
    throw RequireFailure("continuation has no legal public card");
  const double each = 1.0 / static_cast<double>(cards.size());
  std::array<double, poker::kMaxUnifiedSeats> total{};
  for (int card : cards) {
    std::array<double, poker::kMaxUnifiedSeats> child{};
    value_walk(game, state.after_card(card), hands, std::span<double>(child.data(), utility.size()),
               budget);
    for (std::size_t seat = 0; seat < utility.size(); ++seat)
      total[seat] += each * child[seat];
  }
  for (std::size_t seat = 0; seat < utility.size(); ++seat)
    utility[seat] = total[seat];
}

// Structural terminal-only probe. After the hero's current action the game can
// only auto-run to a fold/showdown: which phase follows a public card depends
// solely on the remaining stacks and the street, never on the hole cards, so a
// single trace per action is sufficient. At a Deal phase we advance the first
// card not already on the board (hole-card exclusion changes the chance
// support and the payoff, not whether another ACTION node appears).
bool structural_walk(const GameState& state) {
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

void continuation_utility_all_seats(const UnifiedGame& game, const GameState& after,
                                    std::span<const std::array<int, 2>> hands,
                                    std::span<double> utility, Budget& budget) {
  value_walk(game, after, hands, utility, budget);
}

bool is_terminal_only(const UnifiedGame&, const GameState& node,
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
