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
  const std::size_t seats = game.def.player_count;
  // W2c-ii-b: the belief model is exact for two and three seats. A 4..10-seat
  // game loads and advertises, but its per-node belief cannot be conditioned
  // exactly (a declared coverage limitation), so the caller misses declared.
  if (seats < bs::poker::kMinUnifiedSeats || seats > 3)
    return false;
  seat_count_ = seats;
  for (std::size_t player = 0; player < seat_count_; ++player) {
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

// W2c-ii-b: rebuild per-seat total and per-card summed mass from the current
// raw reaches. The two-seat path keeps its own inline rebuild (its arithmetic
// is golden-pinned); this serves the three-seat path.
void ReachModel::rebuild_totals() {
  for (std::size_t player = 0; player < seat_count_; ++player) {
    scratch_.card_mass[player].fill(0.0);
    total_[player] = 0.0;
    for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
      const double mass = scratch_.raw[player][combo];
      total_[player] += mass;
      if (mass != 0.0) {
        const auto cards = bs::comboCards(combo);
        scratch_.card_mass[player][cards[0]] += mass;
        scratch_.card_mass[player][cards[1]] += mass;
      }
    }
  }
}

// W2c-ii-b: exact compatible mass of `seat` given `n_blocked` distinct blocked
// cards: total minus per-card mass plus pairwise intersections. Triple-and-
// higher inclusion-exclusion terms vanish because a 2-card combo cannot hold
// three or more distinct cards, so the formula is exact for any blocker count.
double ReachModel::compatible_mass(std::size_t seat, const std::array<int, 4>& blocked,
                                   std::size_t n_blocked) const {
  double mass = total_[seat];
  for (std::size_t i = 0; i < n_blocked; ++i)
    mass -= scratch_.card_mass[seat][blocked[i]];
  for (std::size_t i = 0; i < n_blocked; ++i)
    for (std::size_t j = i + 1; j < n_blocked; ++j)
      mass += scratch_.raw[seat][bs::comboIndex(blocked[i], blocked[j])];
  return mass;
}

// W2c-ii-b: three-seat joint mass.
//   joint = sum over card-disjoint triples (c0,c1,c2) of
//           raw[0][c0] * raw[1][c1] * raw[2][c2]
//         = sum_{c0} raw[0][c0] * sum_{c1~c0} raw[1][c1] * M(2, c0 U c1)
// where M(2, S) is the exact seat-2 mass compatible with the blocked set S.
double ReachModel::recompute_joint_mass_three() {
  rebuild_totals();
  double joint = 0.0;
  for (int c0 = 0; c0 < bs::N_COMBOS; ++c0) {
    const double m0 = scratch_.raw[0][c0];
    if (m0 == 0.0)
      continue;
    const auto cards0 = bs::comboCards(c0);
    double inner = 0.0;
    for (int c1 = 0; c1 < bs::N_COMBOS; ++c1) {
      const double m1 = scratch_.raw[1][c1];
      if (m1 == 0.0)
        continue;
      const auto cards1 = bs::comboCards(c1);
      if (cards1[0] == cards0[0] || cards1[0] == cards0[1] || cards1[1] == cards0[0] ||
          cards1[1] == cards0[1])
        continue;
      const std::array<int, 4> blocked{cards0[0], cards0[1], cards1[0], cards1[1]};
      inner += m1 * compatible_mass(2, blocked, 4);
    }
    joint += m0 * inner;
  }
  return joint;
}

double ReachModel::recompute_joint_mass() {
  if (seat_count_ != 2)
    return recompute_joint_mass_three();
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
  if (seat_count_ != 2)
    // Three-seat: no fold. The raw reaches keep their absolute scale and every
    // marginal divides by the same joint mass, so the normalization constant
    // cancels exactly as it does in the two-seat fold.
    return true;
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
  for (std::size_t player = 0; player < seat_count_; ++player)
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

void ReachModel::prepare_partner_mass(std::size_t seat) {
  if (seat_count_ == 3)
    prepare_partner_mass_three(seat);
  // Two-seat: has_positive_partner answers in O(1) per combo, no cache needed.
}

// W2c-ii-b: cache J_{-player}(c) for every combo c of `player`, the partner
// mass of the other two seats conditioned on c being dealt:
//   J_{-player}(c) = sum_{ca ~ c} raw[a][ca] * M(b, c U ca)
// where {a,b} are the other two seats and ca ranges over combos card-disjoint
// from c. This is the exact numerator of player's marginal, so write_marginals
// reuses it, and has_positive_partner answers in O(1) from the cache.
void ReachModel::prepare_partner_mass_three(std::size_t player) {
  // Defensive: a malformed history can name a seat outside the game. Without
  // this guard the loop below would write a third entry into others[2].
  if (player >= seat_count_)
    return;
  std::size_t others[2];
  std::size_t count = 0;
  for (std::size_t seat = 0; seat < seat_count_; ++seat)
    if (seat != player)
      others[count++] = seat;
  const std::size_t a = others[0];
  const std::size_t b = others[1];
  rebuild_totals();
  for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
    const auto cards = bs::comboCards(combo);
    double partner = 0.0;
    for (int other = 0; other < bs::N_COMBOS; ++other) {
      const double mass_a = scratch_.raw[a][other];
      if (mass_a == 0.0)
        continue;
      const auto other_cards = bs::comboCards(other);
      if (other_cards[0] == cards[0] || other_cards[0] == cards[1] || other_cards[1] == cards[0] ||
          other_cards[1] == cards[1])
        continue;
      const std::array<int, 4> blocked{cards[0], cards[1], other_cards[0], other_cards[1]};
      partner += mass_a * compatible_mass(b, blocked, 4);
    }
    scratch_.partner_mass[player][combo] = partner;
  }
}

bool ReachModel::has_positive_partner(std::size_t player, int combo) const {
  if (seat_count_ != 2) {
    // Three-seat: the caller prepared the partner mass after the latest
    // mutation; an orphan combo has zero partner mass. A seat outside the
    // game (malformed history) has no partner by definition.
    if (player >= seat_count_)
      return false;
    const double partner = scratch_.partner_mass[player][combo];
    return std::isfinite(partner) && partner > 0.0;
  }
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

// W2c-ii-b: three-seat marginals. The joint equals sum_c raw[0][c] *
// J_{-0}(c), so seat 0's partner mass yields the joint for free; the other
// seats are prepared in turn. Each marginal is raw[seat][c] * J_{-seat}(c) /
// joint, which sums to one over c.
bool ReachModel::write_marginals_three() {
  prepare_partner_mass_three(0);
  double joint = 0.0;
  for (int combo = 0; combo < bs::N_COMBOS; ++combo)
    joint += scratch_.raw[0][combo] * scratch_.partner_mass[0][combo];
  if (!std::isfinite(joint) || joint <= 0.0)
    return false;
  for (std::size_t player = 0; player < seat_count_; ++player) {
    if (player != 0)
      prepare_partner_mass_three(player);
    for (int combo = 0; combo < bs::N_COMBOS; ++combo)
      scratch_.marginal[player][combo] =
          scratch_.raw[player][combo] * scratch_.partner_mass[player][combo] / joint;
  }
  return true;
}

bool ReachModel::write_marginals() {
  if (seat_count_ != 2)
    return write_marginals_three();
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
