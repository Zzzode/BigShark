// RFC 0039: NestedRiverValueModel — solves the one-street river subgame
// behind a turn depth-limit leaf from exact conditioned ranges, keeps the
// solve-scoped warm state, and produces range-conditioned per-combo
// marginal values.
#include <bs/continuation_value.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/range.hpp>
#include <bs/root_value_extractor.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bs::gto {

struct NestedRiverValueModel::Entry {
  solver::NSeatInitialState warm;
  ContinuationValues values;
  std::unique_ptr<tree::AbstractTree> tree;
  std::unique_ptr<solver::NSeatPolicy> policy;
  std::vector<std::vector<solver::WeightedHand>> ranges;
  poker::GameDef game{};
  std::uint64_t leaf_seed = 0;
  bool gap_passed = false;  // RFC 0039 §3 absolute-gap acceptance flag
};

namespace {

constexpr std::uint64_t kLeafSeedMix = 0x9E3779B97F4A7C15ULL;

std::uint64_t mix_seed(std::uint64_t master, std::size_t leaf) {
  std::uint64_t x = master ^ (kLeafSeedMix * (static_cast<std::uint64_t>(leaf) + 1));
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x ? x : 1;
}

// Enumerate opponent joint deals consistent with `own` and the five-card
// board, multiplying the per-seat conditioned marginals. Recursive over
// seats other than the traverser; folded seats still contribute blockers.
struct DealAccumulator {
  const std::vector<std::vector<solver::WeightedHand>>* ranges = nullptr;
  std::size_t player_count = 0;
  std::size_t traverser = 0;
  std::array<int, 2> own{};
  std::uint64_t board_mask = 0;
  std::vector<std::array<int, 2>> hands;
  double weighted_sum = 0.0;
  double utility_sum = 0.0;
  const solver::NSeatPolicy* policy = nullptr;
  const tree::AbstractTree* tree = nullptr;
  const poker::GameState* root_state = nullptr;
};

void accumulate_deals(DealAccumulator& acc, std::size_t seat, std::uint64_t used_mask,
                      double deal_weight) {
  if (seat == acc.player_count) {
    std::vector<std::array<int, 2>> deal(acc.player_count);
    for (std::size_t s = 0; s < acc.player_count; ++s)
      deal[s] = acc.hands[s];
    const std::vector<double> utility =
        extract_root_value(*acc.policy, *acc.tree, *acc.root_state, deal);
    acc.utility_sum += deal_weight * utility[acc.traverser];
    acc.weighted_sum += deal_weight;
    return;
  }
  if (seat == acc.traverser) {
    const std::uint64_t own_mask = (1ULL << acc.own[0]) | (1ULL << acc.own[1]);
    // Opponent seats are enumerated before the traverser at higher seat
    // indices, so reject the combination when the own holding collides with
    // a chosen opponent or the leaf board.
    if (used_mask & own_mask)
      return;
    acc.hands[seat] = acc.own;
    accumulate_deals(acc, seat + 1, used_mask | own_mask, deal_weight);
    return;
  }
  for (const solver::WeightedHand& wh : (*acc.ranges)[seat]) {
    const std::uint64_t m = (1ULL << wh.cards[0]) | (1ULL << wh.cards[1]);
    if (used_mask & m || (acc.board_mask & m))
      continue;
    acc.hands[seat] = wh.cards;
    accumulate_deals(acc, seat + 1, used_mask | m, deal_weight * wh.weight);
  }
}

// Range-conditioned marginal for the traverser holding `own`: the
// opponent-deal expectation of the exact policy-following root value.
// Returns nullopt when the holding has no card-compatible opponent deal
// under the conditioned ranges (such combos must be ABSENT from the
// marginal matrix, never stored as zero: a zero entry would be served as
// a real Solved value and is indistinguishable in the map from a true
// zero-EV holding).
std::optional<double> marginal_for_combo(DealAccumulator& acc, std::array<int, 2> own) {
  if ((acc.board_mask & ((1ULL << own[0]) | (1ULL << own[1]))) != 0)
    return std::nullopt;
  acc.own = own;
  acc.weighted_sum = 0.0;
  acc.utility_sum = 0.0;
  acc.hands.assign(acc.player_count, {0, 0});
  acc.hands[acc.traverser] = own;
  accumulate_deals(acc, 0, 0, 1.0);
  if (!(acc.weighted_sum > 0.0))
    return std::nullopt;
  return acc.utility_sum / acc.weighted_sum;
}

solver::NSeatTrainerLimits to_trainer_limits(const ContinuationBudget& b) {
  solver::NSeatTrainerLimits l;
  l.max_nodes = b.max_nodes;
  l.max_information_sets = b.max_information_sets;
  l.max_bytes = b.max_bytes;
  l.max_visits = b.max_visits;
  l.wall = b.wall;
  return l;
}

}  // namespace

NestedRiverValueModel::NestedRiverValueModel(const tree::AbstractTree& outer_turn_tree,
                                             std::uint64_t master_seed)
    : outer_(outer_turn_tree), master_seed_(master_seed) {}

NestedRiverValueModel::~NestedRiverValueModel() = default;

std::string NestedRiverValueModel::identity() const {
  return "rfc0039-nested-river-value";
}

namespace {
std::vector<std::vector<solver::WeightedHand>> normalize_sorted(
    const std::vector<std::vector<solver::WeightedHand>>& in) {
  std::vector<std::vector<solver::WeightedHand>> out;
  out.reserve(in.size());
  for (const auto& seat : in) {
    std::vector<solver::WeightedHand> copy = seat;
    for (auto& wh : copy)
      if (wh.cards[0] > wh.cards[1])
        std::swap(wh.cards[0], wh.cards[1]);
    out.push_back(std::move(copy));
  }
  return out;
}
}  // namespace

ContinuationStatus NestedRiverValueModel::refine(
    const RiverBoundaryLeaf& leaf,
    const std::vector<std::vector<solver::WeightedHand>>& conditioned_ranges,
    const std::vector<std::vector<solver::WeightedHand>>& root_support_ranges,
    const ContinuationBudget& budget) {
  // A refine() call certifies the leaf under freshly supplied conditioned
  // ranges, which drift between outer blocks. Any PREVIOUS gap certification
  // belongs to the old ranges and must stop serving immediately: invalidate
  // it BEFORE any failure can return, so a Decline/Invalid on this call can
  // never leave a stale matrix marked served (fail-closed). A successful
  // call replaces the whole entry below with a fresh uncertified one.
  if (auto old = entries_.find(leaf.outer_node_index); old != entries_.end())
    old->second->gap_passed = false;

  if (conditioned_ranges.size() != outer_.def().player_count ||
      root_support_ranges.size() != outer_.def().player_count)
    return ContinuationStatus::Invalid;
  for (const auto& seat : conditioned_ranges)
    if (seat.empty())
      return ContinuationStatus::Invalid;
  for (const auto& seat : root_support_ranges)
    if (seat.empty())
      return ContinuationStatus::Invalid;
  if (!(budget.added_iterations > 0))
    return ContinuationStatus::Decline;

  // train_nseat requires strictly sorted a<b combos.
  std::vector<std::vector<solver::WeightedHand>> sorted_ranges =
      normalize_sorted(conditioned_ranges);
  // Root support drives the marginal MATRIX own-combo coverage; opponents in
  // the marginal are still drawn from sorted_ranges.
  std::vector<std::vector<solver::WeightedHand>> root_sorted =
      normalize_sorted(root_support_ranges);

  poker::GameDef game;
  try {
    game = nested_river_game(leaf);
  } catch (const std::invalid_argument&) {
    return ContinuationStatus::Invalid;
  }

  std::unique_ptr<tree::AbstractTree> nested;
  try {
    nested = std::make_unique<tree::AbstractTree>(game, outer_.action_abstraction(),
                                                  tree::TreeLimits{}, tree::DepthLimit::none());
  } catch (const tree::tree_resource_exhausted&) {
    return ContinuationStatus::Decline;
  } catch (const std::invalid_argument&) {
    return ContinuationStatus::Invalid;
  } catch (const std::exception&) {
    return ContinuationStatus::Decline;
  }
  if (!(nested->action_id() == outer_.action_id()))
    return ContinuationStatus::Decline;

  const std::size_t player_count = game.player_count;
  const std::uint64_t leaf_seed = mix_seed(master_seed_, leaf.outer_node_index);
  const solver::NSeatTrainerLimits limits = to_trainer_limits(budget);

  auto it = entries_.find(leaf.outer_node_index);
  const bool warm_candidate = it != entries_.end() && poker::same_game_def(it->second->game, game);
  const solver::NSeatInitialState* warm = warm_candidate ? &it->second->warm : nullptr;
  // Tracks whether the ACTUAL completed run consumed warm state; reset when
  // the validation rejection forces the cold retry below.
  bool warm_used = warm_candidate;

  solver::NSeatTrainingResult trained;
  try {
    trained = solver::train_nseat(*nested, sorted_ranges, budget.added_iterations, leaf_seed,
                                  limits, nullptr, warm);
  } catch (const std::invalid_argument&) {
    // RFC 0039 AC4: a revision/batch/row-shape mismatch under drifting
    // ranges is a cold start, not a failure. Retry exactly once without
    // warm state.
    warm_used = false;
    try {
      trained = solver::train_nseat(*nested, sorted_ranges, budget.added_iterations, leaf_seed,
                                    limits, nullptr, nullptr);
    } catch (const solver::nseat_training_exhausted&) {
      return ContinuationStatus::Decline;
    } catch (const std::exception&) {
      return ContinuationStatus::Decline;
    }
  } catch (const solver::nseat_training_exhausted&) {
    return ContinuationStatus::Decline;
  } catch (const std::exception&) {
    return ContinuationStatus::Decline;
  }

  // Per-seat per-combo range-conditioned marginals.
  poker::GameState root_state(game);
  std::uint64_t board_mask = 0;
  for (int k = 0; k < 5; ++k)
    board_mask |= 1ULL << game.board[k];

  ContinuationValues cv;
  cv.marginals.resize(player_count);
  cv.inner_completed_iterations = trained.completed_iterations;
  cv.warm = warm_used;
  cv.termination = trained.termination == solver::NSeatTerminationPhase::Complete
                       ? ContinuationTermination::Complete
                   : trained.termination == solver::NSeatTerminationPhase::WallClock
                       ? ContinuationTermination::WallClock
                       : ContinuationTermination::ResourceLimit;

  DealAccumulator acc;
  acc.ranges = &sorted_ranges;
  acc.player_count = player_count;
  acc.board_mask = board_mask;
  acc.policy = &trained.policy;
  acc.tree = nested.get();
  acc.root_state = &root_state;

  // The matrix covers EVERY board-compatible combo of the seat's DECLARED
  // root support, not only combos with positive trunk reach to this leaf:
  // external-sampling MCCFR evaluates the counterfactual value of every own
  // action (including zero-probability deviations) at infosets, so V_p(c)
  // is defined for each own holding while opponents stay distributed by the
  // conditioned leaf marginals (DeepStack continuation-value convention).
  for (std::size_t p = 0; p < player_count; ++p) {
    acc.traverser = p;
    // Cover EVERY board-compatible combo of the seat's ROOT support,
    // including combos with zero trunk reach to this leaf: ES-MCCFR needs
    // the counterfactual value of every own action deviation.
    for (const solver::WeightedHand& wh : root_sorted[p]) {
      if ((board_mask & ((1ULL << wh.cards[0]) | (1ULL << wh.cards[1]))) != 0)
        continue;
      std::optional<double> v;
      try {
        v = marginal_for_combo(acc, wh.cards);
      } catch (const std::exception&) {
        // The marginal walk must never throw into an outer MCCFR sweep.
        return ContinuationStatus::Decline;
      }
      // Absent on zero compatible opponent mass (contract: zero-mass combos
      // are never zero-filled); lookup_scalar then Misses to the equity leaf.
      if (!v.has_value())
        continue;
      cv.marginals[p][bs::comboIndex(wh.cards[0], wh.cards[1])] = *v;
    }
    if (cv.marginals[p].empty())
      return ContinuationStatus::Invalid;
  }

  // Rotate solve-scoped warm state and publish the entry.
  auto owned = std::make_unique<Entry>();
  Entry& entry = *owned;
  entry.warm.rows = std::move(trained.raw_rows);
  entry.warm.completed_iterations = trained.completed_iterations;
  entry.warm.algorithm_revision = trained.algorithm_revision;
  entry.warm.batch_size = trained.batch_size;
  entry.game = game;
  entry.leaf_seed = leaf_seed;
  entry.tree = std::move(nested);
  entry.policy = std::make_unique<solver::NSeatPolicy>(std::move(trained.policy));
  entry.ranges = sorted_ranges;
  entry.values = std::move(cv);
  // A new solve under (possibly drifted) ranges invalidates the previous
  // acceptance: the offline driver must re-measure the absolute gap and call
  // record_inner_gap before this leaf is served again.
  (void)warm_candidate;
  entries_[leaf.outer_node_index] = std::move(owned);
  return ContinuationStatus::Solved;
}

ContinuationStatus NestedRiverValueModel::lookup_scalar(const RiverBoundaryLeaf& leaf,
                                                        std::size_t traverser,
                                                        std::array<int, 2> own_cards,
                                                        double& value_chips) const {
  const auto it = entries_.find(leaf.outer_node_index);
  if (it == entries_.end())
    return ContinuationStatus::Miss;
  // Fail closed: never serve a matrix whose absolute inner gap has not been
  // measured as passing (RFC 0039 §3).
  if (!it->second->values.has_inner_gap || !it->second->gap_passed)
    return ContinuationStatus::Miss;
  const ContinuationMatrix& m = it->second->values.marginals;
  if (traverser >= m.size())
    return ContinuationStatus::Invalid;
  const int ci = bs::comboIndex(own_cards[0], own_cards[1]);
  const auto vit = m[traverser].find(ci);
  if (vit == m[traverser].end())
    return ContinuationStatus::Miss;  // zero-mass own combo at this leaf
  value_chips = vit->second;
  return ContinuationStatus::Solved;
}

const ContinuationValues* NestedRiverValueModel::values(const RiverBoundaryLeaf& leaf) const {
  const auto it = entries_.find(leaf.outer_node_index);
  return it == entries_.end() ? nullptr : &it->second->values;
}

const solver::NSeatPolicy* NestedRiverValueModel::inner_policy(
    const RiverBoundaryLeaf& leaf) const {
  const auto it = entries_.find(leaf.outer_node_index);
  return it == entries_.end() ? nullptr : it->second->policy.get();
}

const tree::AbstractTree* NestedRiverValueModel::inner_tree(const RiverBoundaryLeaf& leaf) const {
  const auto it = entries_.find(leaf.outer_node_index);
  return it == entries_.end() ? nullptr : it->second->tree.get();
}

const std::vector<std::vector<solver::WeightedHand>>* NestedRiverValueModel::inner_ranges(
    const RiverBoundaryLeaf& leaf) const {
  const auto it = entries_.find(leaf.outer_node_index);
  return it == entries_.end() ? nullptr : &it->second->ranges;
}

void NestedRiverValueModel::record_inner_gap(const RiverBoundaryLeaf& leaf, double gap_chips,
                                             bool passed) {
  if (auto it = entries_.find(leaf.outer_node_index); it != entries_.end()) {
    it->second->values.has_inner_gap = true;
    it->second->values.inner_gap_chips = gap_chips;
    it->second->gap_passed = passed;
  }
}

}  // namespace bs::gto
