// RFC 0007: continuation-range export for preflop flop-terminal policies.
#include <algorithm>
#include <array>
#include <bs/continuation_range.hpp>
#include <bs/range.hpp>  // N_COMBOS
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs::solver {

namespace {

// Walk the preflop tree for one joint deal, following the average policy, and
// return the total reach of every flop chance node (the probability that this
// joint deal sees a flop under the policy). Fold leaves contribute zero; the
// walk stops at the first chance node (the flop deal), so frontier leaves are
// never visited.
//
// The result depends only on the per-seat Preflop169 buckets, not the specific
// hole cards: the policy lookup at every action node uses
// (card_bucket(Preflop169, own, {}), node_index), and the empty board makes
// that bucket a pure function of rank pair and suitedness. The caller
// memoizes by the bucket tuple — see reach_cache below.
double flop_reach(const NSeatPolicy& policy, const tree::AbstractTree& tree, std::size_t node_index,
                  std::span<const std::uint32_t> buckets, double reach) {
  const tree::TreeNode& node = tree.node(node_index);
  if (node.is_terminal())
    return 0.0;
  if (node.is_flop_deal())
    return reach;

  // Action node: branch on the acting seat's policy row.
  const std::size_t actor = node.actor;
  const std::uint32_t bucket = buckets[actor];
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
    total += flop_reach(policy, tree, node.children[a], buckets, reach * (*probs)[a]);
  }
  return total;
}

// Per-seat combo metadata for the joint-deal enumeration.
struct ComboInfo {
  std::array<int, 2> cards;
  double prior;
  int combo_index;
  std::uint32_t bucket;
  std::uint64_t card_mask;  // two-bit mask of the combo's cards
};

// One card-compatible joint deal with its accumulated weight.
struct JointDeal {
  std::array<int, 10> combo_indices{};  // combo index per seat (up to 10 seats)
  double weight = 0.0;                  // joint prior * reach
  std::uint64_t card_mask = 0;          // OR of every seat's combo card mask
};

// Recursively enumerate every card-compatible joint deal (no two seats share
// a card), compute the policy reach for each bucket tuple (memoized), and
// append deals with positive reach to `joint`.
void enumerate_joint_deals(std::size_t seat, std::size_t player_count,
                           const std::vector<std::vector<ComboInfo>>& combos,
                           std::vector<const ComboInfo*>& current,
                           std::vector<std::uint32_t>& bucket_tuple, std::uint64_t used_mask,
                           const NSeatPolicy& policy, const tree::AbstractTree& tree,
                           std::map<std::vector<std::uint32_t>, double>& reach_cache,
                           std::vector<JointDeal>& joint) {
  if (seat == player_count) {
    // All seats dealt: look up or compute the memoized reach.
    double reach;
    if (auto it = reach_cache.find(bucket_tuple); it != reach_cache.end()) {
      reach = it->second;
    } else {
      reach = flop_reach(policy, tree, tree.root_index(), bucket_tuple, 1.0);
      reach_cache.emplace(bucket_tuple, reach);
    }

    if (reach > 0.0) {
      JointDeal jd;
      double joint_prior = 1.0;
      for (std::size_t s = 0; s < player_count; ++s) {
        jd.combo_indices[s] = current[s]->combo_index;
        joint_prior *= current[s]->prior;
      }
      jd.weight = joint_prior * reach;
      jd.card_mask = used_mask;
      joint.push_back(jd);
    }
    return;
  }

  for (const auto& combo : combos[seat]) {
    if (used_mask & combo.card_mask)
      continue;  // card-colliding joint deal is impossible
    current.push_back(&combo);
    bucket_tuple.push_back(combo.bucket);
    enumerate_joint_deals(seat + 1, player_count, combos, current, bucket_tuple,
                          used_mask | combo.card_mask, policy, tree, reach_cache, joint);
    bucket_tuple.pop_back();
    current.pop_back();
  }
}

}  // namespace

std::vector<PolicyReachRangePair> export_continuation_ranges(
    const NSeatPolicy& policy, const tree::AbstractTree& tree,
    const std::vector<std::vector<WeightedHand>>& ranges) {
  const poker::GameDef& def = tree.def();
  if (def.player_count < 2 || def.player_count > 10)
    throw std::invalid_argument("continuation-range export requires 2..10 seats");
  if (def.board_size != 0 || !def.preflop || def.terminal != poker::TerminalDepth::Flop)
    throw std::invalid_argument("continuation-range export requires a preflop flop-terminal game");
  if (!poker::same_game_def(policy.game(), def))
    throw std::invalid_argument("policy and tree game definitions differ");
  if (ranges.size() != def.player_count)
    throw std::invalid_argument("continuation-range export requires one range per seat");
  for (const auto& seat : ranges)
    if (seat.empty())
      throw std::invalid_argument("continuation-range export requires a non-empty range per seat");

  const std::size_t player_count = def.player_count;

  // Normalize prior weights per seat and build per-seat combo metadata.
  std::vector<double> seat_total(player_count, 0.0);
  for (std::size_t seat = 0; seat < player_count; ++seat) {
    for (const auto& wh : ranges[seat])
      seat_total[seat] += wh.weight;
    if (seat_total[seat] <= 0.0)
      throw std::invalid_argument("range has non-positive total weight");
  }

  std::vector<std::vector<ComboInfo>> combos(player_count);
  for (std::size_t seat = 0; seat < player_count; ++seat) {
    for (const auto& wh : ranges[seat]) {
      const std::uint32_t bucket =
          static_cast<std::uint32_t>(abstraction::card_bucket(kNSeatCardKind, wh.cards, {}));
      combos[seat].push_back({wh.cards, wh.weight / seat_total[seat],
                              bs::comboIndex(wh.cards[0], wh.cards[1]), bucket,
                              (1ULL << wh.cards[0]) | (1ULL << wh.cards[1])});
    }
  }

  // Enumerate every card-compatible joint deal, computing the memoized reach
  // for each distinct bucket tuple.
  std::vector<JointDeal> joint;
  std::map<std::vector<std::uint32_t>, double> reach_cache;
  std::vector<const ComboInfo*> current;
  std::vector<std::uint32_t> bucket_tuple;
  current.reserve(player_count);
  bucket_tuple.reserve(player_count);
  enumerate_joint_deals(0, player_count, combos, current, bucket_tuple, 0, policy, tree,
                        reach_cache, joint);

  // Enumerate every 3-card flop and accumulate the continuation ranges.
  //
  // A joint deal contributes to a flop iff no seat's combo shares a card with
  // the flop. The card-mask test (joint.card_mask & flop_mask) is a single
  // 64-bit AND, so the per-flop scan over all joint deals is efficient.
  constexpr int N = bs::N_COMBOS;  // 1326
  std::vector<PolicyReachRangePair> result;
  result.reserve(22100);  // C(52,3) = 22,100

  // Dense per-seat accumulators, zeroed per flop.
  std::vector<std::vector<double>> accum(player_count, std::vector<double>(N, 0.0));
  std::vector<double> range_total(player_count, 0.0);

  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      for (int c2 = c1 + 1; c2 < 52; ++c2) {
        const std::uint64_t flop_mask = (1ULL << c0) | (1ULL << c1) | (1ULL << c2);

        for (auto& seat_accum : accum)
          std::fill(seat_accum.begin(), seat_accum.end(), 0.0);
        std::fill(range_total.begin(), range_total.end(), 0.0);

        for (const auto& jd : joint) {
          if (jd.card_mask & flop_mask)
            continue;  // a seat's combo blocks this flop
          for (std::size_t s = 0; s < player_count; ++s) {
            accum[s][jd.combo_indices[s]] += jd.weight;
            range_total[s] += jd.weight;
          }
        }

        bool any_zero = false;
        for (std::size_t s = 0; s < player_count; ++s) {
          if (range_total[s] <= 0.0) {
            any_zero = true;
            break;
          }
        }
        if (any_zero)
          continue;

        PolicyReachRangePair pair;
        pair.flop = {c0, c1, c2};
        pair.ranges.resize(player_count);
        for (std::size_t s = 0; s < player_count; ++s) {
          for (int i = 0; i < N; ++i) {
            if (accum[s][i] > 0.0)
              pair.ranges[s][i] = accum[s][i] / range_total[s];
          }
        }
        result.push_back(std::move(pair));
      }
    }
  }
  return result;
}

}  // namespace bs::solver
