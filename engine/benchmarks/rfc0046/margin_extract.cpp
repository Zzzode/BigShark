#include "margin_extract.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace bs::rfc0046 {
namespace {

using bs::poker::Action;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::solver::WeightedHand;
using bs::tree::AbstractTree;

struct View {
  std::array<int, 2> hero{};
  double reach = 0.0;
};

bool collide(const std::array<int, 2>& a, const std::array<int, 2>& b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

std::size_t board_ordinal(const GameState& state, int card) {
  std::size_t ordinal = 0;
  for (int c = 0; c < card; ++c) {
    bool on_board = false;
    for (int b : state.board())
      if (b == c)
        on_board = true;
    if (!on_board)
      ++ordinal;
  }
  return ordinal;
}

// Exact expected RESPONDER value under a fixed policy for a fixed joint deal,
// chance enumerated exactly. Views carry one hero combo; the responder holds a
// fixed combo across the whole per-combo extraction.
struct ValueWalker {
  const MarginExtractionInput& in;
  const AbstractTree& tree;
  const std::array<int, 2> resp;

  ValueWalker(const MarginExtractionInput& input, const std::array<int, 2>& responder_holding)
      : in(input), tree(*input.tree), resp(responder_holding) {}

  double terminal_util(const GameState& state, std::size_t node_index,
                       const std::array<int, 2>& hero) const {
    const auto& payload = tree.terminal(tree.node(node_index).terminal);
    if (state.phase() == Phase::Folded)
      return static_cast<double>(payload.chip_utility[in.responder]);
    std::array<std::array<int, 2>, 2> packed{};
    for (std::size_t i = 0; i < payload.live_count; ++i) {
      const std::size_t seat = payload.live_order[i];
      packed[i] = (seat == in.responder) ? resp : hero;
    }
    return static_cast<double>(
        state
            .settle_showdown(std::span<const std::array<int, 2>>(packed.data(), payload.live_count))
            .chip_utility[in.responder]);
  }

  double walk(const GameState& state, std::size_t node_index, const std::array<int, 2>& hero) {
    const auto& node = tree.node(node_index);
    if (node.is_terminal())
      return terminal_util(state, node_index, hero);
    if (node.is_chance()) {
      std::array<bool, 52> used{};
      for (int c : state.board())
        used[c] = true;
      used[resp[0]] = used[resp[1]] = true;
      used[hero[0]] = used[hero[1]] = true;
      std::size_t avail = 0;
      for (int c = 0; c < 52; ++c)
        if (!used[c])
          ++avail;
      double total = 0.0;
      for (int card = 0; card < 52; ++card) {
        if (used[card])
          continue;
        total += walk(state.after_card(card), node.children[board_ordinal(state, card)], hero);
      }
      return total / static_cast<double>(avail);
    }
    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;
    const auto own = (actor == in.responder) ? resp : hero;
    std::vector<double> sigma(menu.size(), 0.0);
    in.reference->probabilities(actor, node_index, static_cast<std::int64_t>(node_index), own, menu,
                                std::span<double>(sigma));
    double total = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      total += sigma[a] * walk(state.after_action(actor, menu[a]), node.children[a], hero);
    return total;
  }
};

}  // namespace

std::map<std::uint32_t, std::array<double, 2>> extract_per_combo_margins(
    const MarginExtractionInput& input) {
  std::map<std::uint32_t, std::array<double, 2>> out;
  if (!input.tree || !input.keyer || !input.reference)
    throw std::invalid_argument("extract_per_combo_margins: missing input");

  for (const WeightedHand& rc : input.ranges[input.responder]) {
    double weighted_value = 0.0;
    double mass = 0.0;
    ValueWalker walker(input, rc.cards);
    const GameState root(input.tree->def());
    for (const WeightedHand& hc : input.ranges[input.hero]) {
      if (collide(rc.cards, hc.cards))
        continue;
      const double w = rc.weight * hc.weight;
      weighted_value += w * walker.walk(root, input.tree->root_index(), hc.cards);
      mass += w;
    }
    if (mass > 0.0) {
      const std::uint32_t bucket = input.keyer->dense_for_live_pair(rc.cards);
      out[bucket] = {weighted_value / mass, mass};
    }
  }
  return out;
}

}  // namespace bs::rfc0046
