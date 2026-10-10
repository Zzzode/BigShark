// RFC 0007: continuation-range export for preflop flop-terminal policies.
#include <algorithm>
#include <array>
#include <bs/continuation_range.hpp>
#include <bs/range.hpp>  // N_COMBOS
#include <stdexcept>

namespace bs::solver {

namespace {

// Walk the preflop tree for one joint deal, following the average policy, and
// return the total reach of every flop chance node (the probability that this
// joint deal sees a flop under the policy). Fold leaves contribute zero; the
// walk stops at the first chance node (the flop deal), so frontier leaves are
// never visited.
//
// The result depends only on the two seats' Preflop169 buckets, not the
// specific hole cards: the policy lookup at every action node uses
// (card_bucket(Preflop169, own, {}), node_index), and the empty board makes
// that bucket a pure function of rank pair and suitedness. The caller
// memoizes by (hero_bucket, opp_bucket) — see reach_cache below.
double flop_reach(const NSeatPolicy& policy, const tree::AbstractTree& tree, std::size_t node_index,
                  const std::array<int, 2>& hero, const std::array<int, 2>& opp, double reach) {
  const tree::TreeNode& node = tree.node(node_index);
  if (node.is_terminal())
    return 0.0;
  if (node.is_flop_deal())
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
    std::array<int, 2> hero_cards;
    std::array<int, 2> opp_cards;
    int hero_combo;
    int opp_combo;
    double joint_prior;
    double reach;
  };
  std::vector<JointEntry> joint;
  joint.reserve(combos[0].size() * combos[1].size());

  // Memoization: flop_reach from the root with reach=1.0 depends only on the
  // (hero_bucket, opp_bucket) pair. Preflop169 buckets are in [0, 168].
  // Without this cache the tree walk runs once per joint deal (~1.6M); with
  // it, at most 169*169 = 28,561 times.
  std::array<std::array<double, 169>, 169> reach_cache;
  for (auto& row : reach_cache)
    row.fill(-1.0);  // -1.0 = not yet computed (reach is always in [0, 1])

  for (std::size_t h = 0; h < combos[0].size(); ++h) {
    const auto& hc = combos[0][h].cards;
    const std::uint32_t hb =
        static_cast<std::uint32_t>(abstraction::card_bucket(kNSeatCardKind, hc, {}));
    for (std::size_t o = 0; o < combos[1].size(); ++o) {
      const auto& oc = combos[1][o].cards;
      if (hc[0] == oc[0] || hc[0] == oc[1] || hc[1] == oc[0] || hc[1] == oc[1])
        continue;  // card-colliding joint deal is impossible
      const std::uint32_t ob =
          static_cast<std::uint32_t>(abstraction::card_bucket(kNSeatCardKind, oc, {}));
      double r = reach_cache[hb][ob];
      if (r < 0.0) {
        r = flop_reach(policy, tree, root, hc, oc, 1.0);
        reach_cache[hb][ob] = r;
      }
      if (r > 0.0)
        joint.push_back({hc, oc, combos[0][h].combo_index, combos[1][o].combo_index,
                         combos[0][h].prior * combos[1][o].prior, r});
    }
  }

  // Build the dense weight matrix M[h][o] = joint_prior * reach for all
  // card-compatible joint deals. Colliding pairs are zero (not in joint).
  // This enables the subtraction trick below: for each flop, the range is
  // the total weight minus the weight of blocking combos, reducing the
  // inner loop from ~1,200 non-blocking combos to ~150 blocking ones.
  constexpr int N = bs::N_COMBOS;  // 1326
  std::vector<double> M(static_cast<std::size_t>(N) * N, 0.0);
  for (const auto& j : joint)
    M[static_cast<std::size_t>(j.hero_combo) * N + j.opp_combo] = j.joint_prior * j.reach;
  joint.clear();
  joint.shrink_to_fit();

  // Precompute per-hero and per-opp totals (sum over all opponents).
  std::array<double, N> T_hero{};
  std::array<double, N> T_opp{};
  for (int h = 0; h < N; ++h) {
    const std::size_t row = static_cast<std::size_t>(h) * N;
    for (int o = 0; o < N; ++o) {
      const double w = M[row + static_cast<std::size_t>(o)];
      T_hero[h] += w;
      T_opp[o] += w;
    }
  }

  // Enumerate every 3-card flop and accumulate the continuation ranges.
  //
  // For each flop, the range for hero combo h is T_hero[h] minus the weight
  // of opp combos that block the flop (share a card with it). There are at
  // most 3*51 - 3 = 150 blocking combos, so the inner loop is 1326 * 150
  // per seat per flop — an 8x reduction from iterating all joint entries.
  std::vector<PolicyReachRangePair> result;
  result.reserve(22100);  // C(52,3) = 22,100
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      for (int c2 = c1 + 1; c2 < 52; ++c2) {
        const std::array<int, 3> flop{c0, c1, c2};

        // Build the set of combos that block this flop (contain at least
        // one flop card). 3 flop cards * 51 partners - 3 double-counted
        // pairs = 150 unique combos.
        std::array<int, 150> blocking;
        std::array<bool, N> is_blocking{};
        int n_blocking = 0;
        const int flop_cards[3] = {c0, c1, c2};
        for (int fc : flop_cards) {
          for (int other = 0; other < 52; ++other) {
            if (other == fc)
              continue;
            const int ci = bs::comboIndex(fc, other);
            if (!is_blocking[ci]) {
              is_blocking[ci] = true;
              blocking[n_blocking++] = ci;
            }
          }
        }

        // Compute ranges via subtraction: total - blocking.
        std::array<std::array<double, N>, 2> accum{};
        std::array<double, 2> range_total{};
        for (int h = 0; h < N; ++h) {
          if (is_blocking[h])
            continue;
          const std::size_t row = static_cast<std::size_t>(h) * N;
          double S = 0.0;
          for (int i = 0; i < n_blocking; ++i)
            S += M[row + static_cast<std::size_t>(blocking[i])];
          accum[0][h] = T_hero[h] - S;
          range_total[0] += accum[0][h];
        }
        for (int o = 0; o < N; ++o) {
          if (is_blocking[o])
            continue;
          double S = 0.0;
          for (int i = 0; i < n_blocking; ++i)
            S += M[static_cast<std::size_t>(blocking[i]) * N + static_cast<std::size_t>(o)];
          accum[1][o] = T_opp[o] - S;
          range_total[1] += accum[1][o];
        }

        if (range_total[0] <= 0.0 || range_total[1] <= 0.0)
          continue;
        PolicyReachRangePair pair;
        pair.flop = flop;
        for (int i = 0; i < N; ++i) {
          if (accum[0][i] > 0.0)
            pair.ranges[0][i] = accum[0][i] / range_total[0];
          if (accum[1][i] > 0.0)
            pair.ranges[1][i] = accum[1][i] / range_total[1];
        }
        result.push_back(std::move(pair));
      }
    }
  }
  return result;
}

}  // namespace bs::solver
