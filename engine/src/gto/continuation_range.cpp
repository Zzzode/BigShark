// RFC 0007: continuation-range export for preflop flop-terminal policies.
#include <algorithm>
#include <bs/continuation_range.hpp>
#include <stdexcept>

namespace bs::solver {

namespace {

// Walk the preflop tree for one joint deal, following the average policy, and
// return the total reach of every flop chance node (the probability that this
// joint deal sees a flop under the policy). Fold leaves contribute zero; the
// walk stops at the first chance node (the flop deal), so frontier leaves are
// never visited.
double flop_reach(const NSeatPolicy& policy, const tree::AbstractTree& tree, std::size_t node_index,
                  const std::array<int, 2>& hero, const std::array<int, 2>& opp, double reach) {
  const tree::TreeNode& node = tree.node(node_index);
  if (node.is_terminal())
    return 0.0;
  if (node.is_chance())
    return reach;

  // Action node: branch on the policy's action distribution.
  const std::size_t actor = node.actor;
  const std::array<int, 2>& own = (actor == 0) ? hero : opp;
  const std::uint32_t bucket =
      static_cast<std::uint32_t>(abstraction::card_bucket(kNSeatCardKind, own, {}));
  const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node_index)};
  const auto it = policy.rows().find(key);

  const std::vector<double>* probs = nullptr;
  std::vector<double> uniform;
  if (it != policy.rows().end()) {
    probs = &it->second.probabilities;
  } else {
    // Uniform fallback for an unvisited node (matches the trainer's
    // current_strategy on a fresh row).
    uniform.assign(node.actions.size(), 1.0 / static_cast<double>(node.actions.size()));
    probs = &uniform;
  }

  double total = 0.0;
  for (std::size_t a = 0; a < node.actions.size(); ++a) {
    if ((*probs)[a] <= 0.0)
      continue;
    total += flop_reach(policy, tree, node.children[a], hero, opp, reach * (*probs)[a]);
  }
  return total;
}

// Whether a flop shares a card with a combo.
bool blocks(const std::array<int, 3>& flop, const std::array<int, 2>& combo) {
  for (int card : flop)
    if (card == combo[0] || card == combo[1])
      return true;
  return false;
}

}  // namespace

std::vector<PolicyReachRangePair> export_continuation_ranges(
    const NSeatPolicy& policy, const tree::AbstractTree& tree,
    const std::vector<std::vector<WeightedHand>>& ranges) {
  const poker::GameDef& def = tree.def();
  if (def.player_count != 2)
    throw std::invalid_argument("continuation-range export requires a two-seat game");
  if (def.board_size != 0 || !def.preflop || def.terminal != poker::TerminalDepth::Flop)
    throw std::invalid_argument("continuation-range export requires a preflop flop-terminal game");
  if (!poker::same_game_def(policy.game(), def))
    throw std::invalid_argument("policy and tree game definitions differ");
  if (ranges.size() != 2)
    throw std::invalid_argument("continuation-range export requires two seat ranges");
  for (const auto& seat : ranges)
    if (seat.empty())
      throw std::invalid_argument("continuation-range export requires a non-empty range per seat");

  // Normalize prior weights per seat.
  std::array<double, 2> seat_total{};
  for (std::size_t seat = 0; seat < 2; ++seat) {
    for (const auto& wh : ranges[seat])
      seat_total[seat] += wh.weight;
    if (seat_total[seat] <= 0.0)
      throw std::invalid_argument("range has non-positive total weight");
  }

  struct ComboEntry {
    std::array<int, 2> cards;
    double prior;
    int combo_index;
  };
  std::array<std::vector<ComboEntry>, 2> combos;
  for (std::size_t seat = 0; seat < 2; ++seat) {
    for (const auto& wh : ranges[seat]) {
      combos[seat].push_back({{wh.cards[0], wh.cards[1]},
                              wh.weight / seat_total[seat],
                              bs::comboIndex(wh.cards[0], wh.cards[1])});
    }
  }

  // Compute R(h, o) for each card-compatible joint deal.
  const std::size_t root = tree.root_index();
  struct JointEntry {
    std::size_t hero_idx;
    std::size_t opp_idx;
    double joint_prior;
    double reach;
  };
  std::vector<JointEntry> joint;
  joint.reserve(combos[0].size() * combos[1].size());
  for (std::size_t h = 0; h < combos[0].size(); ++h) {
    for (std::size_t o = 0; o < combos[1].size(); ++o) {
      const auto& hc = combos[0][h].cards;
      const auto& oc = combos[1][o].cards;
      if (hc[0] == oc[0] || hc[0] == oc[1] || hc[1] == oc[0] || hc[1] == oc[1])
        continue;  // card-colliding joint deal is impossible
      const double r = flop_reach(policy, tree, root, hc, oc, 1.0);
      if (r > 0.0)
        joint.push_back({h, o, combos[0][h].prior * combos[1][o].prior, r});
    }
  }

  // Enumerate every 3-card flop and accumulate the continuation ranges.
  std::vector<PolicyReachRangePair> result;
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      for (int c2 = c1 + 1; c2 < 52; ++c2) {
        const std::array<int, 3> flop{c0, c1, c2};
        std::array<std::map<int, double>, 2> flop_ranges;
        std::array<double, 2> range_total{};
        for (const auto& j : joint) {
          const auto& hc = combos[0][j.hero_idx].cards;
          const auto& oc = combos[1][j.opp_idx].cards;
          if (blocks(flop, hc) || blocks(flop, oc))
            continue;
          const double contribution = j.joint_prior * j.reach;
          flop_ranges[0][combos[0][j.hero_idx].combo_index] += contribution;
          flop_ranges[1][combos[1][j.opp_idx].combo_index] += contribution;
          range_total[0] += contribution;
          range_total[1] += contribution;
        }
        if (range_total[0] <= 0.0 || range_total[1] <= 0.0)
          continue;
        for (auto& [idx, prob] : flop_ranges[0])
          prob /= range_total[0];
        for (auto& [idx, prob] : flop_ranges[1])
          prob /= range_total[1];
        result.push_back({flop, std::move(flop_ranges)});
      }
    }
  }
  return result;
}

}  // namespace bs::solver
