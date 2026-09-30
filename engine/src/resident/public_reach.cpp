#include "public_reach.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace bs::resident {
namespace {

bool combo_holds_card(int combo, int card) {
  const auto cards = bs::comboCards(combo);
  return cards[0] == card || cards[1] == card;
}

}  // namespace

ReachModel::ReachModel(ResidentScratch& scratch) : scratch_(scratch) {}

namespace {

// Exact recursive joint mass over one combination per seat, all pairwise
// card-disjoint and none sharing a root board card. Seats are visited most
// constrained first (smallest declared range) so the used-card prune cuts
// earliest. `product` is the weight product of the assignment so far.
double joint_mass_recurse(const solver::UnifiedGame& game,
                          const std::array<bool, 52>& board_blocked,
                          const std::vector<std::size_t>& order, std::size_t depth,
                          std::array<bool, 52>& used, double product) {
  if (depth == order.size())
    return product;
  const std::size_t seat = order[depth];
  double total = 0.0;
  for (const solver::WeightedHand& hand : game.ranges[seat]) {
    if (hand.weight <= 0)
      continue;
    if (board_blocked[static_cast<std::size_t>(hand.cards[0])] ||
        board_blocked[static_cast<std::size_t>(hand.cards[1])])
      continue;
    if (used[static_cast<std::size_t>(hand.cards[0])] ||
        used[static_cast<std::size_t>(hand.cards[1])])
      continue;
    used[static_cast<std::size_t>(hand.cards[0])] = true;
    used[static_cast<std::size_t>(hand.cards[1])] = true;
    total += joint_mass_recurse(game, board_blocked, order, depth + 1, used, product * hand.weight);
    used[static_cast<std::size_t>(hand.cards[0])] = false;
    used[static_cast<std::size_t>(hand.cards[1])] = false;
  }
  return total;
}

}  // namespace

double root_joint_mass(const solver::UnifiedGame& game) {
  const std::size_t seats = game.def.player_count;
  if (seats < bs::poker::kMinUnifiedSeats || seats > bs::poker::kMaxUnifiedSeats)
    return 0.0;
  // A seat with an empty declared range contributes no joint deal.
  for (std::size_t seat = 0; seat < seats; ++seat)
    if (game.ranges[seat].empty())
      return 0.0;
  std::vector<std::size_t> order;
  order.reserve(seats);
  for (std::size_t seat = 0; seat < seats; ++seat)
    order.push_back(seat);
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return game.ranges[a].size() < game.ranges[b].size();
  });
  std::array<bool, 52> board_blocked{};
  for (std::size_t i = 0; i < game.def.board_size && i < 3; ++i)
    board_blocked[static_cast<std::size_t>(game.def.board[i])] = true;
  std::array<bool, 52> used{};
  return joint_mass_recurse(game, board_blocked, order, 0, used, 1.0);
}

bool ReachModel::initialize(const solver::UnifiedGame& game) {
  // W2c-ii-a: the belief model is still two-seat; an n-seat artifact loads and
  // serves the resolver source, but the resident belief cannot condition it
  // until W2c-ii-b.
  if (game.def.player_count != 2)
    return false;
  for (std::size_t player = 0; player < 2; ++player) {
    scratch_.raw[player].fill(0.0);
    for (const solver::WeightedHand& hand : game.ranges[player]) {
      const int combo = bs::comboIndex(hand.cards[0], hand.cards[1]);
      scratch_.raw[player][combo] = hand.weight;
    }
  }
  // Root flop filtering. Declared artifact ranges already exclude board
  // cards, but the belief never trusts that implicitly.
  for (std::size_t i = 0; i < game.def.board_size && i < 3; ++i)
    if (!observe_card(game.def.board[i]))
      return false;
  return renormalize();
}

double ReachModel::recompute_joint_mass() {
  std::array<double, 2> total{};
  for (std::size_t player = 0; player < 2; ++player) {
    scratch_.card_mass[player].fill(0.0);
    for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
      const double mass = scratch_.raw[player][combo];
      total[player] += mass;
      if (mass != 0.0) {
        const auto cards = bs::comboCards(combo);
        scratch_.card_mass[player][cards[0]] += mass;
        scratch_.card_mass[player][cards[1]] += mass;
      }
    }
  }
  // Sum over player-0 combos of raw0[c0] times the total player-1 mass that
  // does not share either card of c0. The combination {c0a,c0b} is subtracted
  // twice through the per-card masses and added back once (the same combo can
  // legally appear in both declared ranges; its self-pair is incompatible and
  // contributes nothing). Combinations blocked by public cards carry zero.
  double joint = 0.0;
  for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
    const double mass0 = scratch_.raw[0][combo];
    if (mass0 == 0.0)
      continue;
    const auto cards = bs::comboCards(combo);
    const double compatible1 = total[1] - scratch_.card_mass[1][cards[0]] -
                               scratch_.card_mass[1][cards[1]] + scratch_.raw[1][combo];
    joint += mass0 * compatible1;
  }
  return joint;
}

bool ReachModel::renormalize() {
  const double joint = recompute_joint_mass();
  if (!std::isfinite(joint) || joint <= 0.0)
    return false;
  // Fold the normalization constant into player 0 only. The joint is what
  // carries semantics; per-combo action factors stay unmodified, and every
  // marginal computed below divides by the same joint mass. This mirrors the
  // solver convention in which per-key normalization cancels constants such
  // as the 1/legal-cards chance factor. card_mass[0] must be scaled by the
  // same factor so it stays in raw[0]'s units: has_positive_partner mixes a
  // freshly summed raw[opponent] total with the cached opponent card masses,
  // and the opponent can be player 0 when observing a player-1 action. The
  // next recompute_joint_mass rebuilds card_mass from scratch regardless.
  for (double& mass : scratch_.raw[0])
    mass /= joint;
  for (double& mass : scratch_.card_mass[0])
    mass /= joint;
  return true;
}

bool ReachModel::observe_card(int card) {
  for (std::size_t player = 0; player < 2; ++player)
    for (int combo = 0; combo < bs::N_COMBOS; ++combo)
      if (scratch_.raw[player][combo] != 0.0 && combo_holds_card(combo, card))
        scratch_.raw[player][combo] = 0.0;
  return renormalize();
}

bool ReachModel::observe_action(std::size_t actor,
                                const std::array<double, bs::N_COMBOS>& probability) {
  for (int combo = 0; combo < bs::N_COMBOS; ++combo)
    if (scratch_.raw[actor][combo] != 0.0)
      scratch_.raw[actor][combo] *= probability[combo];
  return renormalize();
}

bool ReachModel::has_positive_partner(std::size_t player, int combo) const {
  const auto cards = bs::comboCards(combo);
  const std::size_t opponent = 1 - player;
  double total = 0;
  for (int other = 0; other < bs::N_COMBOS; ++other)
    total += scratch_.raw[opponent][other];
  // Same inclusion-exclusion as recompute_joint_mass: add the self-pair back
  // once; its contribution is excluded by the product in the joint sum.
  const double compatible = total - scratch_.card_mass[opponent][cards[0]] -
                            scratch_.card_mass[opponent][cards[1]] + scratch_.raw[opponent][combo];
  return std::isfinite(compatible) && compatible > 0.0;
}

bool ReachModel::write_marginals() {
  const double joint = recompute_joint_mass();
  if (!std::isfinite(joint) || joint <= 0.0)
    return false;
  std::array<double, 2> total{};
  for (std::size_t player = 0; player < 2; ++player)
    for (int combo = 0; combo < bs::N_COMBOS; ++combo)
      total[player] += scratch_.raw[player][combo];
  for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
    const auto cards = bs::comboCards(combo);
    const double compatible1 = total[1] - scratch_.card_mass[1][cards[0]] -
                               scratch_.card_mass[1][cards[1]] + scratch_.raw[1][combo];
    const double compatible0 = total[0] - scratch_.card_mass[0][cards[0]] -
                               scratch_.card_mass[0][cards[1]] + scratch_.raw[0][combo];
    scratch_.marginal[0][combo] = scratch_.raw[0][combo] * compatible1 / joint;
    scratch_.marginal[1][combo] = scratch_.raw[1][combo] * compatible0 / joint;
  }
  return true;
}

void ReachModel::opponent_private_view(std::size_t opponent, std::array<int, 2> hero_cards,
                                       bool& fully_blocked) {
  scratch_.opponent_view = scratch_.marginal[opponent];
  for (int combo = 0; combo < bs::N_COMBOS; ++combo)
    if (scratch_.opponent_view[combo] != 0.0 &&
        (combo_holds_card(combo, hero_cards[0]) || combo_holds_card(combo, hero_cards[1])))
      scratch_.opponent_view[combo] = 0.0;
  double remaining = 0.0;
  for (double mass : scratch_.opponent_view)
    remaining += mass;
  fully_blocked = !(std::isfinite(remaining) && remaining > 0.0);
}

}  // namespace bs::resident
