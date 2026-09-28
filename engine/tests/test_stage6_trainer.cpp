// RFC 0008 stage 6 R12 step 7 gate battery for the external-sampling
// multiplayer MCCFR trainer.
//
// Coverage:
//   1. Algebra (scripted world): an INDEPENDENT textbook external-sampling
//      walk (its own recursion, table and update code — never TrainerRow)
//      consumes the same deal/flop/action/chance draws as debug_run_one_sweep
//      and reproduces every RM+ regret and kFull average cell within 1e-9.
//      This pins the three contract points: the unweighted
//      R <- max(0, R + v_a - v) traverser update, one
//      sums += pi_own * sigma per own-node visit (the exact full-CFR average
//      for every player count — an unweighted visit snapshot is unbiased only
//      for N=2), and sampled opponents / chance / joint deal carrying no reach
//      factor into the propagated own reach.
//   2. Statistics: 100k iterations at seeds 1/17/43 contract the block-average
//      L1 policy change; a byte-identical rerun seals the same artifact rows;
//      mode and cursor rules are pinned.
//   3. Dual-cursor alignment: exhaustive tree/path replay on a live==2 tree
//      (production verifier) and on an enlarged-limits shallow live==3 tree.
//   4. Multiplayer measure: the enumerable 3-player full-tree parity gate is a
//      separate binary (test_stage6_multiway_average).
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/infoset_id.hpp>
#include <bs/stage6/public_path.hpp>
#include <bs/stage6/trainer.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bs::poker;
using namespace bs::stage6;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::CoverSeeds;
using bs::abstraction::SizeSchedule;
using bs::tree::AbstractTree;
using bs::tree::TreeLimits;

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

bool near(double a, double b, double eps = 1e-9) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= eps;
}

// The pinned coarse postflop menu: one half-pot bet and one pot raise per
// street, declared targets only.
ActionAbstraction coarse_action() {
  SizeSchedule schedule = bs::abstraction::default_size_schedule();
  for (auto& street : schedule) {
    street.bets = {{1, 2}};
    street.raises = {{1, 1}};
  }
  return ActionAbstraction::declared(schedule, CoverSeeds::DeclaredOnly);
}

TrainingConfig make_config(std::uint64_t iterations, std::uint64_t seed) {
  TrainingConfig config;
  config.action = coarse_action();
  config.card_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
  config.iterations = iterations;
  config.master_seed = seed;
  config.geometry_matrix_hash = 0x123456789abcdef0ULL;
  config.chart_digest_sha256 = "test-digest";
  return config;
}

// Tiny live==2 representative (SPR ~0.5): two seats, 4 behind, 8 dead.
GeometryBucket bucket_2p() {
  GeometryBucket b;
  b.key.player_count = 2;
  b.key.pot_bb = 4;
  b.key.live_count = 2;
  b.key.acting_count = 2;
  b.actionable = true;
  b.big_blind = 2;
  b.representative_stacks = {4, 4};
  b.representative_contrib = {4, 4};
  b.representative_pot = 8;
  b.min_acting_stack = 4;
  return b;
}

// Shallow live==3 representative: three seats, 2 behind, 6 dead. Its coarse
// tree is ~100k nodes under enlarged limits — small enough for the exhaustive
// cursor-alignment replay, far smaller than real 3p postflop geometry.
GeometryBucket bucket_3p_shallow() {
  GeometryBucket b;
  b.key.player_count = 3;
  b.key.pot_bb = 3;
  b.key.live_count = 3;
  b.key.acting_count = 3;
  b.actionable = true;
  b.big_blind = 2;
  b.representative_stacks = {2, 2, 2};
  b.representative_contrib = {2, 2, 2};
  b.representative_pot = 6;
  b.min_acting_stack = 2;
  return b;
}

GameDef rooted_def(const GeometryBucket& b, const std::array<int, 3>& flop) {
  GameDef def{};
  def.player_count = b.key.live_count;
  def.button = 0;
  def.big_blind = b.big_blind;
  def.preflop = false;
  Chips total = 0;
  for (std::size_t p = 0; p < b.key.live_count; ++p) {
    def.stacks[p] = b.representative_stacks[p];
    def.contributions[p] = b.representative_contrib[p];
    total += b.representative_contrib[p];
  }
  def.pot = b.representative_pot;
  if (total != def.pot)
    throw std::runtime_error("test fixture pot does not reconcile");
  def.board = {flop[0], flop[1], flop[2], 0, 0};
  def.board_size = 3;
  return def;
}

// ------------------------------------------------------- independent reference

std::size_t ref_bounded(bs::SplitMix64& rng, std::size_t bound) {
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  for (;;) {
    const std::uint64_t value = rng.next_u64();
    if (value >= threshold)
      return static_cast<std::size_t>(value % limit);
  }
}

double ref_unit(bs::SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
}

std::size_t ref_weighted(bs::SplitMix64& rng, const std::vector<double>& w) {
  const double point = ref_unit(rng);
  double cum = 0.0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < w.size(); ++i) {
    if (w[i] <= 0.0)
      continue;
    cum += w[i];
    last = i;
    if (point < cum)
      return i;
  }
  return last;
}

struct RefRow {
  std::vector<Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
  std::uint64_t visits = 0;
};

struct RefWorld {
  const GeometryBucket* bucket = nullptr;
  const TrainingConfig* config = nullptr;
  const AbstractTree* tree = nullptr;
  bool streaming = false;
  std::size_t traverser = 0;
  std::vector<HoleCards> holes;
  bs::SplitMix64* action_rng = nullptr;
  bs::SplitMix64* chance_rng = nullptr;
  PublicPath* path = nullptr;
  std::map<AbstractInfosetKey, RefRow> table;
};

std::vector<double> ref_sigma(const RefRow& row) {
  double pos = 0.0;
  for (double r : row.regrets)
    pos += std::max(0.0, r);
  std::vector<double> sigma(row.actions.size(), 1.0 / row.actions.size());
  if (pos > 0.0)
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      sigma[i] = std::max(0.0, row.regrets[i]) / pos;
  return sigma;
}

std::vector<int> ref_runout_cards(const GameState& state, const std::vector<HoleCards>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const HoleCards& h : holes)
    used[h[0]] = used[h[1]] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

std::size_t ref_ordinal(const GameState& state, int card) {
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

double ref_terminal(RefWorld& w, const GameState& state) {
  if (state.phase() == Phase::Folded) {
    const ContributionSettlement s = state.settle_fold();
    return static_cast<double>(s.chip_utility[w.traverser]);
  }
  std::vector<std::array<int, 2>> live_holes;
  for (std::size_t seat : state.live_players())
    live_holes.push_back(w.holes[seat]);
  const ContributionSettlement s = state.settle_showdown(live_holes);
  return static_cast<double>(s.chip_utility[w.traverser]);
}

RefRow& ref_touch(RefWorld& w, const AbstractInfosetKey& key, const std::vector<Action>& menu) {
  auto it = w.table.find(key);
  if (it != w.table.end())
    return it->second;
  RefRow row;
  row.actions = menu;
  row.regrets.assign(menu.size(), 0.0);
  row.sums.assign(menu.size(), 0.0);
  return w.table.emplace(key, std::move(row)).first->second;
}

double ref_walk(RefWorld& w, const GameState& state, std::size_t node_index,
                double own_reach = 1.0) {
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
    return ref_terminal(w, state);
  if (state.phase() == Phase::Deal) {
    const std::vector<int> cards = ref_runout_cards(state, w.holes);
    const int card = cards[ref_bounded(*w.chance_rng, cards.size())];
    const std::size_t ordinal = ref_ordinal(state, card);
    GameState next = state.after_card(card);
    if (w.streaming) {
      w.path->on_chance(ordinal);
      return ref_walk(w, next, node_index, own_reach);
    }
    return ref_walk(w, next, w.tree->node(node_index).children.at(ordinal), own_reach);
  }

  const std::size_t actor = *state.actor();
  const std::vector<int> board(state.board().begin(), state.board().end());
  const std::uint32_t bucket_id = static_cast<std::uint32_t>(
      bs::abstraction::card_bucket(w.config->card_kind, w.holes[actor], board));
  AbstractInfosetKey key;
  key.own_card_bucket = bucket_id;
  if (w.streaming)
    key.path_hash = w.path->hash();
  else
    key.tree_node_index = node_index;
  const std::vector<Action> menu =
      bs::tree::abstract_node_menu(state, w.config->action, state.legal());
  RefRow& row = ref_touch(w, key, menu);
  const std::vector<double> sigma = ref_sigma(row);

  if (actor == w.traverser) {
    // kFull: own-reach-weighted average at the acting player's own node.
    for (std::size_t a = 0; a < menu.size(); ++a)
      row.sums[a] += own_reach * sigma[a];
    ++row.visits;
    std::vector<double> children(menu.size(), 0.0);
    for (std::size_t a = 0; a < menu.size(); ++a) {
      GameState next = state.after_action(actor, menu[a]);
      if (w.streaming) {
        const std::size_t prefix = w.path->tokens().size();
        w.path->on_action(actor, a);
        children[a] = ref_walk(w, next, node_index, own_reach * sigma[a]);
        while (w.path->tokens().size() > prefix)
          w.path->pop();
      } else {
        children[a] =
            ref_walk(w, next, w.tree->node(node_index).children.at(a), own_reach * sigma[a]);
      }
    }
    double value = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      value += sigma[a] * children[a];
    for (std::size_t a = 0; a < menu.size(); ++a) {
      const double r = row.regrets[a] + children[a] - value;
      row.regrets[a] = std::max(0.0, r);
    }
    return value;
  }

  // Opponent: sample one action; own reach of the traverser is unchanged.
  const std::size_t sampled = ref_weighted(*w.action_rng, sigma);
  GameState next = state.after_action(actor, menu[sampled]);
  if (w.streaming) {
    w.path->on_action(actor, sampled);
    return ref_walk(w, next, node_index, own_reach);
  }
  return ref_walk(w, next, w.tree->node(node_index).children.at(sampled), own_reach);
}

// Reproduces the trainer's deal/flop world with independently seeded but
// identically-keyed streams, then runs the reference walk. In materialized
// mode `tree` is the coarse tree and rows are node-index keyed; in streaming
// mode `tree` is null and the reference maintains its own PublicPath.
std::map<AbstractInfosetKey, RefRow> reference_sweep(const GeometryBucket& bucket,
                                                     const TrainingConfig& config,
                                                     const AbstractTree* tree,
                                                     std::size_t traverser, std::uint64_t iter,
                                                     std::uint64_t seed) {
  const std::size_t seats = bucket.key.live_count;
  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges(seats);
  for (std::size_t s = 0; s < seats; ++s)
    for (int a = 0; a < 52; ++a)
      for (int b = a + 1; b < 52; ++b)
        ranges[s].push_back(bs::solver::MultiwayWeightedHand{{a, b}, 1.0});

  bs::SplitMix64 deal_rng = derive_stream(seed, StreamPurpose::TrainJointDeal, iter, traverser);
  bs::SplitMix64 flop_rng = derive_stream(seed, StreamPurpose::TrainRootBoard, iter, traverser);
  bs::SplitMix64 action_rng =
      derive_stream(seed, StreamPurpose::TrainOpponentAction, iter, traverser);
  bs::SplitMix64 chance_rng = derive_stream(seed, StreamPurpose::TrainBoardRunout, iter, traverser);

  const bs::solver::MultiwayDeal deal =
      bs::solver::sample_scalable_joint_deal(ranges, {}, deal_rng);
  std::vector<HoleCards> holes;
  for (const auto& hand : deal.hands)
    holes.push_back(HoleCards{hand[0], hand[1]});

  std::array<bool, 52> blocked{};
  for (const HoleCards& h : holes)
    blocked[h[0]] = blocked[h[1]] = true;
  std::vector<int> available;
  for (int c = 0; c < 52; ++c)
    if (!blocked[c])
      available.push_back(c);
  std::array<int, 3> flop{};
  for (std::size_t pick = 0; pick < 3; ++pick) {
    const std::size_t index = ref_bounded(flop_rng, available.size());
    flop[pick] = available[index];
    available[index] = available.back();
    available.pop_back();
  }

  RefWorld world;
  const bool streaming = tree == nullptr;
  world.bucket = &bucket;
  world.config = &config;
  world.tree = tree;
  world.streaming = streaming;
  world.traverser = traverser;
  world.holes = std::move(holes);
  world.action_rng = &action_rng;
  world.chance_rng = &chance_rng;
  PublicPath path(geometry_bucket_token(bucket.key));
  world.path = &path;
  GameState state(rooted_def(bucket, flop));
  if (streaming)
    ref_walk(world, state, bs::tree::kNoNode);
  else
    ref_walk(world, state, tree->root_index());
  return std::move(world.table);
}

// ------------------------------------------------------------- part 1: algebra

// Pins one sweep of `bucket` against the independent reference. In materialized
// mode a coarse tree is built and rows are node-index keyed; in streaming mode
// no tree exists and both sides key by PublicPath hash.
int test_sweep_algebra(const GeometryBucket& bucket, const TrainingConfig& config, bool streaming) {
  std::unique_ptr<AbstractTree> owned;
  const AbstractTree* tree = nullptr;
  if (!streaming) {
    const GameDef def = rooted_def(bucket, {0, 6, 21});
    owned = std::make_unique<AbstractTree>(def, config.action);
    tree = owned.get();
  }
  int local_failures = 0;

  for (std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{43}}) {
    for (std::size_t traverser = 0; traverser < bucket.key.live_count; ++traverser) {
      for (std::uint64_t iter = 0; iter < 4; ++iter) {
        std::map<AbstractInfosetKey, TrainerRow> trained;
        bs::SplitMix64 deal_rng =
            derive_stream(seed, StreamPurpose::TrainJointDeal, iter, traverser);
        bs::SplitMix64 flop_rng =
            derive_stream(seed, StreamPurpose::TrainRootBoard, iter, traverser);
        bs::SplitMix64 action_rng =
            derive_stream(seed, StreamPurpose::TrainOpponentAction, iter, traverser);
        bs::SplitMix64 chance_rng =
            derive_stream(seed, StreamPurpose::TrainBoardRunout, iter, traverser);
        debug_run_one_sweep(bucket, config, tree, traverser, deal_rng, flop_rng, action_rng,
                            chance_rng, trained);

        auto reference = reference_sweep(bucket, config, tree, traverser, iter, seed);
        if (reference.size() != trained.size()) {
          std::printf("FAIL: algebra(%s) seed=%llu trav=%zu iter=%llu row count %zu != %zu\n",
                      streaming ? "stream" : "materialized", static_cast<unsigned long long>(seed),
                      traverser, static_cast<unsigned long long>(iter), reference.size(),
                      trained.size());
          ++local_failures;
          continue;
        }
        for (const auto& [key, ref_row] : reference) {
          auto it = trained.find(key);
          if (it == trained.end()) {
            std::printf("FAIL: algebra missing trained key seed=%llu\n",
                        static_cast<unsigned long long>(seed));
            ++local_failures;
            continue;
          }
          const TrainerRow& got = it->second;
          if (got.actions != ref_row.actions || got.regrets.size() != ref_row.regrets.size()) {
            std::printf("FAIL: algebra menu/shape mismatch\n");
            ++local_failures;
            continue;
          }
          for (std::size_t a = 0; a < ref_row.regrets.size(); ++a) {
            if (!near(got.regrets[a], ref_row.regrets[a])) {
              std::printf(
                  "FAIL: regret mismatch seed=%llu trav=%zu iter=%llu a=%zu got=%.12g "
                  "ref=%.12g\n",
                  static_cast<unsigned long long>(seed), traverser,
                  static_cast<unsigned long long>(iter), a, got.regrets[a], ref_row.regrets[a]);
              ++local_failures;
            }
            if (!near(got.sums[a], ref_row.sums[a])) {
              std::printf(
                  "FAIL: sums mismatch seed=%llu trav=%zu iter=%llu a=%zu got=%.12g "
                  "ref=%.12g\n",
                  static_cast<unsigned long long>(seed), traverser,
                  static_cast<unsigned long long>(iter), a, got.sums[a], ref_row.sums[a]);
              ++local_failures;
            }
          }
          if (got.visits != ref_row.visits) {
            std::printf("FAIL: visits mismatch got=%llu ref=%llu\n",
                        static_cast<unsigned long long>(got.visits),
                        static_cast<unsigned long long>(ref_row.visits));
            ++local_failures;
          }
        }
      }
    }
  }
  return local_failures;
}

// Visit-weighted mean L1 between two average policies over a FIXED mature row
// set: the keys in `endpoint` whose visit count is at least `min_visits` and
// present in both snapshots. Fixed-set (not "all rows") because external
// sampling keeps creating rows and an all-row mean is dominated by freshly
// touched, uninformative rows; visit-weighting (not a row mean) because a row
// visited 40k times deserves more weight than one visited 400 times. This is
// a STABILITY measure, never a Nash-convergence claim.
double mature_weighted_l1(const FrozenArtifactRows& endpoint, const FrozenArtifactRows& earlier,
                          const FrozenArtifactRows& later, std::uint64_t min_visits,
                          std::size_t* counted_out = nullptr) {
  double numerator = 0.0;
  double denominator = 0.0;
  std::size_t counted = 0;
  for (const auto& [key, end_row] : endpoint) {
    if (end_row.visits < min_visits)
      continue;
    auto a = earlier.find(key);
    auto b = later.find(key);
    if (a == earlier.end() || b == later.end() ||
        a->second.probabilities.size() != b->second.probabilities.size())
      continue;
    const double weight = static_cast<double>(std::min(a->second.visits, b->second.visits));
    double d = 0.0;
    for (std::size_t i = 0; i < a->second.probabilities.size(); ++i)
      d += std::abs(a->second.probabilities[i] - b->second.probabilities[i]);
    numerator += weight * d;
    denominator += weight;
    ++counted;
  }
  if (counted_out)
    *counted_out = counted;
  return counted ? numerator / denominator : std::numeric_limits<double>::infinity();
}

// Every sealed row is a finite, non-negative, unit-sum distribution.
int check_rows_are_distributions(const FrozenArtifactRows& rows) {
  int local_failures = 0;
  for (const auto& [key, row] : rows) {
    double total = 0.0;
    for (double p : row.probabilities) {
      if (!std::isfinite(p) || p < 0.0) {
        std::printf("FAIL: non-finite/negative average probability\n");
        return ++local_failures;
      }
      total += p;
    }
    if (!near(total, 1.0, 1e-9)) {
      std::printf("FAIL: average row does not sum to 1 (%.12g)\n", total);
      ++local_failures;
    }
  }
  return local_failures;
}

// -------------------------------------------------------- part 2: statistics
//
// Two tiers. The 100k tier runs under every preset and pins determinism,
// distribution validity and bounded mature-row drift (stability, NOT
// convergence). The long-horizon contraction tier (100k->600k on one fixed
// mature set) runs only when BS_STAGE6_TRAINER_LONG=1, because the debug and
// ASan builds instrument every map visit and would otherwise dominate ctest.
int test_statistics(const GeometryBucket& bucket) {
  int local_failures = 0;
  const bool long_tier = std::getenv("BS_STAGE6_TRAINER_LONG") != nullptr;

  for (std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{17}, std::uint64_t{43}}) {
    BucketTrainingResult r50k = train_bucket(bucket, make_config(50'000, seed));
    BucketTrainingResult result = train_bucket(bucket, make_config(100'000, seed));
    check(!result.report.wall_reached, "100k-iteration statistical run completes under the wall");
    check(result.report.iterations_completed == 100'000, "report stamps 100k iterations");
    check(result.rows.size() == result.report.information_sets, "report row count matches rows");
    check(!result.rows.empty(), "training creates information-set rows");
    check(r50k.rows.size() <= result.rows.size(), "longer training never loses information sets");
    local_failures += check_rows_are_distributions(result.rows);

    std::size_t mature = 0;
    const double stability = mature_weighted_l1(result.rows, r50k.rows, result.rows, 400, &mature);
    std::printf("[stats] seed=%llu infosets=%zu mature_rows=%zu weighted-L1(50k->100k)=%.5f\n",
                static_cast<unsigned long long>(seed), result.rows.size(), mature, stability);
    check(mature >= 10, "enough mature rows exist to measure stability");
    check(stability < 0.10,
          "mature average-policy drift over 50k->100k is below 0.10 (stability, not convergence)");

    // Byte-identical rerun: same (bucket, config) seals identical rows and
    // manifest.
    BucketTrainingResult again = train_bucket(bucket, make_config(100'000, seed));
    check(again.rows == result.rows, "identical rerun seals byte-identical average rows");
    check(manifest_for(bucket, make_config(100'000, seed), again.rows,
                       again.report.iterations_completed) ==
              manifest_for(bucket, make_config(100'000, seed), result.rows,
                           result.report.iterations_completed),
          "identical rerun seals an identical manifest");

    if (!long_tier)
      continue;

    // Long-horizon STABILITY on ONE fixed mature set (rows with >=1000 visits
    // at the 600k endpoint). The shallow fixture solves toward near-pure
    // policies, so the honest claim is that the mature average stops moving:
    // the 300k->600k displacement is small on every seed. This is explicitly
    // NOT a Nash-convergence guarantee (the Monte-Carlo displacement null for
    // a 2x block is ~sqrt(2), so a ratio test here would be near the noise
    // floor); the enumerable full-tree multiway parity gate elsewhere is what
    // validates the average MEASURE.
    BucketTrainingResult r300k = train_bucket(bucket, make_config(300'000, seed));
    BucketTrainingResult r600k = train_bucket(bucket, make_config(600'000, seed));
    std::size_t long_mature = 0;
    const double early =
        mature_weighted_l1(r600k.rows, result.rows, r300k.rows, 1000, &long_mature);
    const double late = mature_weighted_l1(r600k.rows, r300k.rows, r600k.rows, 1000, &long_mature);
    std::printf("[stats-long] seed=%llu mature=%zu weighted-L1 100k->300k=%.5f 300k->600k=%.5f\n",
                static_cast<unsigned long long>(seed), long_mature, early, late);
    check(long_mature >= 100, "long tier has a substantial mature row set");
    check(late < 0.07, "mature average policy is stable (<0.07 L1) over 300k->600k");
  }
  return local_failures;
}

// ----------------------------------------------- part 3: dual-cursor alignment

int test_alignment() {
  int local_failures = 0;
  {
    const GeometryBucket bucket = bucket_2p();
    const TrainingConfig config = make_config(1, 1);
    const AlignmentReport report = verify_materialized_path_alignment(bucket, config);
    check(report.action_nodes > 0 && report.chance_nodes > 0, "2p replay walks action+chance");
    check(report.distinct_action_paths == report.action_nodes,
          "2p every action node has a distinct path");
    std::printf("[align] 2p action=%zu chance=%zu terminal=%zu\n", report.action_nodes,
                report.chance_nodes, report.terminal_nodes);
  }
  {
    // Shallow live==3 tree under enlarged limits; production streaming never
    // builds it, but the same replay rule must hold there.
    const GeometryBucket bucket = bucket_3p_shallow();
    const TrainingConfig config = make_config(1, 1);
    const GameDef def = rooted_def(bucket, {0, 6, 21});
    TreeLimits limits;
    limits.max_nodes = 2'000'000;
    limits.max_bytes = std::size_t{1} << 30;
    AbstractTree tree(def, config.action, limits);
    const AlignmentReport report =
        debug_verify_tree_paths(tree, def, config, geometry_bucket_token(bucket.key));
    check(report.action_nodes > 0 && report.chance_nodes > 0, "3p replay walks action+chance");
    check(report.distinct_action_paths == report.action_nodes,
          "3p every action node has a distinct path");
    std::printf("[align] 3p action=%zu chance=%zu terminal=%zu\n", report.action_nodes,
                report.chance_nodes, report.terminal_nodes);
  }
  return local_failures;
}

// P1 (larger-table): the streaming PublicPath hash must distinguish two real,
// distinct action/card token sequences that the former boost::hash_combine
// idiom collided at live>=4 (a check-to node merged with a facing-a-bet node
// during the n=6 streamed table). This is the minimized collision captured
// from the production trainer.
int test_public_path_hash_injectivity() {
  int local_failures = 0;
  const std::vector<std::uint16_t> a = {41795, 5008, 8579, 2291, 32, 64,    96, 0, 32776,
                                        32,    65,   96,   1,    33, 32803, 32, 64};
  const std::vector<std::uint16_t> b = {41795, 5008, 8579, 2291, 32, 64,    96, 0, 32776,
                                        32,    65,   96,   1,    32, 32788, 64, 1};
  auto path_of = [](const std::vector<std::uint16_t>& tokens) {
    std::uint64_t geo = std::uint64_t(tokens[0]) | (std::uint64_t(tokens[1]) << 16) |
                        (std::uint64_t(tokens[2]) << 32) | (std::uint64_t(tokens[3]) << 48);
    PublicPath path(geo);
    for (std::size_t i = 4; i < tokens.size(); ++i)
      path.append_edge_token(tokens[i]);
    return path;
  };
  const std::uint64_t ha = path_of(a).hash();
  const std::uint64_t hb = path_of(b).hash();
  check(ha != hb, "the captured live4 path collision is resolved");

  // A trailing zero token changes the digest (length enters the hash).
  {
    std::vector<std::uint16_t> base = {1, 2, 3, 4, 32};
    std::vector<std::uint16_t> plus_zero = base;
    plus_zero.push_back(0);
    check(path_of(base).hash() != path_of(plus_zero).hash(),
          "a path and its trailing-zero extension have different hashes");
  }
  // Deterministic.
  check(ha == path_of(a).hash(), "path hash is deterministic");
  return local_failures;
}

int test_contract() {
  int local_failures = 0;
  const GeometryBucket b2 = bucket_2p();
  const GeometryBucket b3 = bucket_3p_shallow();
  check(materialize_or_stream(b2) == TrainerMode::Materialized, "live==2 materializes");
  check(materialize_or_stream(b3) == TrainerMode::Streaming, "live==3 streams");

  GeometryBucket runout = b2;
  runout.actionable = false;
  bool threw = false;
  try {
    train_bucket(runout, make_config(1, 1));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "training an all-in-at-flop runout bucket is refused");

  GeometryBucket no_bb = b2;
  no_bb.big_blind = 0;
  threw = false;
  try {
    train_bucket(no_bb, make_config(1, 1));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "training a bucket without its big blind is refused");

  // P1-2: a wall cap that lets ZERO iterations finish must refuse rather than
  // seal an empty artifact whose manifest claims the requested count.
  TrainingConfig exhausted = make_config(100'000, 1);
  exhausted.limits.wall = std::chrono::milliseconds(0);
  threw = false;
  try {
    (void)train_bucket(b2, exhausted);
  } catch (const stage6_training_exhausted&) {
    threw = true;
  }
  check(threw, "zero completed iterations before the wall cap is refused");
  return local_failures;
}

// P2-1: the two keys fed to derive_stream play distinct roles (e.g. iteration
// vs traverser seat). A commutative XOR mix would make (a,b) bit-identical to
// (b,a), silently reusing one sweep's random streams for an unrelated sweep.
int test_stream_order_sensitivity() {
  int local_failures = 0;
  constexpr std::uint64_t seed = 0x0123456789abcdefULL;
  for (StreamPurpose purpose :
       {StreamPurpose::TrainJointDeal, StreamPurpose::TrainRootBoard,
        StreamPurpose::TrainOpponentAction, StreamPurpose::TrainBoardRunout}) {
    for (std::uint64_t a : {1ULL, 7ULL, 100003ULL}) {
      for (std::uint64_t b : {2ULL, 11ULL, 900001ULL}) {
        if (a == b)
          continue;
        const std::uint64_t sab = derive_stream(seed, purpose, a, b).next_u64();
        const std::uint64_t sba = derive_stream(seed, purpose, b, a).next_u64();
        if (sab == sba) {
          std::printf("FAIL: derive_stream is commutative for purpose=%d a=%llu b=%llu\n",
                      static_cast<int>(purpose), static_cast<unsigned long long>(a),
                      static_cast<unsigned long long>(b));
          ++local_failures;
        }
      }
    }
  }
  // A different master seed must change the derived stream.
  {
    bs::SplitMix64 r1 = derive_stream(seed, StreamPurpose::TrainJointDeal, 5, 6);
    bs::SplitMix64 r2 = derive_stream(seed ^ 0xff, StreamPurpose::TrainJointDeal, 5, 6);
    check(r1.next_u64() != r2.next_u64(), "master seed changes the derived stream");
  }
  check(!local_failures, "derive_stream distinguishes key order and master seed");
  return local_failures;
}

// R11 Item 2 Chunk B: the second hole-range profile.
//
// (a) Sweep identity: the null/default path and an EXPLICIT all-1326 carrier
//     under the Uniform profile must drive bitwise identical sealed-row
//     updates over reconstructed identical RNG streams. resolve_ranges is
//     file-local in the trainer TU; producing the same dealt worlds (and thus
//     bitwise equal regrets/sums on the small materialized bucket) is the
//     observable proof that it resolves to exactly uniform_ranges(seats).
// (b) Hash behavior: range fields are INERT under Uniform (default vs explicit
//     carrier + nonzero range_content_hash keep identical artifact/config
//     hashes, so the uniform production path is byte-stable), and DISTINCT
//     under ChartReach (marker changes the hashes; the content hash changes
//     them again). ChartReach without a carrier, and malformed carriers, are
//     refused.
std::shared_ptr<const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>>
explicit_uniform_carrier(std::size_t seats) {
  auto carrier = std::make_shared<std::vector<std::vector<bs::solver::MultiwayWeightedHand>>>();
  carrier->resize(seats);
  for (std::size_t s = 0; s < seats; ++s)
    for (int a = 0; a < 52; ++a)
      for (int b = a + 1; b < 52; ++b)
        (*carrier)[s].push_back(bs::solver::MultiwayWeightedHand{{a, b}, 1.0});
  return carrier;
}

int test_range_profile() {
  int local_failures = 0;
  const GeometryBucket bucket = bucket_2p();
  const TrainingConfig base = make_config(2, 7);
  const GameDef def = rooted_def(bucket, {0, 6, 21});

  // (a) Bitwise sweep identity over a few sweeps of the materialized bucket.
  auto run_sweeps = [&](const TrainingConfig& cfg) {
    auto tree = std::make_unique<AbstractTree>(def, cfg.action);
    std::map<AbstractInfosetKey, TrainerRow> rows;
    for (std::uint64_t iter = 0; iter < 3; ++iter) {
      for (std::size_t traverser = 0; traverser < bucket.key.live_count; ++traverser) {
        bs::SplitMix64 deal_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainJointDeal, iter, traverser);
        bs::SplitMix64 flop_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainRootBoard, iter, traverser);
        bs::SplitMix64 action_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainOpponentAction, iter, traverser);
        bs::SplitMix64 chance_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainBoardRunout, iter, traverser);
        debug_run_one_sweep(bucket, cfg, tree.get(), traverser, deal_rng, flop_rng, action_rng,
                            chance_rng, rows);
      }
    }
    return rows;
  };

  TrainingConfig explicit_cfg = base;
  explicit_cfg.root_ranges = explicit_uniform_carrier(bucket.key.live_count);
  explicit_cfg.range_content_hash = 0xdeadbeefcafe1234ULL;  // must be inert under Uniform

  const std::map<AbstractInfosetKey, TrainerRow> null_rows = run_sweeps(base);
  const std::map<AbstractInfosetKey, TrainerRow> explicit_rows = run_sweeps(explicit_cfg);
  check(explicit_rows.size() == null_rows.size(),
        "explicit all-1326 carrier touches the same row set as the null path");
  if (explicit_rows.size() == null_rows.size()) {
    bool bitwise_equal = true;
    for (const auto& [key, null_row] : null_rows) {
      auto it = explicit_rows.find(key);
      if (it == explicit_rows.end() || it->second.actions != null_row.actions ||
          it->second.regrets != null_row.regrets || it->second.sums != null_row.sums ||
          it->second.visits != null_row.visits) {
        bitwise_equal = false;
        break;
      }
    }
    check(bitwise_equal, "explicit all-1326 carrier produces bitwise identical regret/sum deltas");
  }

  // (b) Hash behavior, observed through the public artifact hash and manifest.
  auto hashes_for = [&](const TrainingConfig& cfg) {
    const BucketTrainingResult result = train_bucket(bucket, cfg);
    const FrozenManifest manifest =
        manifest_for(bucket, cfg, result.rows, result.report.iterations_completed);
    return std::pair<std::uint64_t, std::uint64_t>{result.artifact_content_hash,
                                                   manifest.training_config_hash};
  };

  TrainingConfig uniform_explicit = base;
  uniform_explicit.root_ranges = explicit_uniform_carrier(bucket.key.live_count);
  uniform_explicit.range_content_hash = 0x0123456789abcdefULL;

  const auto default_hashes = hashes_for(base);
  const auto uniform_explicit_hashes = hashes_for(uniform_explicit);
  check(default_hashes.first == uniform_explicit_hashes.first,
        "artifact identity hash is uniform-byte-stable with an explicit carrier");
  check(default_hashes.second == uniform_explicit_hashes.second,
        "training config hash is uniform-byte-stable with a nonzero range hash");

  TrainingConfig chart_a = base;
  chart_a.range_profile = RangeProfile::ChartReach;
  chart_a.root_ranges = explicit_uniform_carrier(bucket.key.live_count);
  chart_a.range_content_hash = 0x1111111111111111ULL;
  const auto chart_a_hashes = hashes_for(chart_a);
  check(chart_a_hashes.first != default_hashes.first,
        "chart-reach changes the artifact identity hash");
  check(chart_a_hashes.second != default_hashes.second,
        "chart-reach changes the training config hash");

  TrainingConfig chart_b = chart_a;
  chart_b.range_content_hash = 0x2222222222222222ULL;
  const auto chart_b_hashes = hashes_for(chart_b);
  check(chart_b_hashes.first != chart_a_hashes.first &&
            chart_b_hashes.second != chart_a_hashes.second,
        "range_content_hash differentiates chart-reach runs");

  // The geometry token depends only on the bucket key in every profile.
  GeometryBucket bucket_copy = bucket;
  check(geometry_bucket_token(bucket.key) == geometry_bucket_token(bucket_copy.key),
        "geometry bucket token is profile-independent");

  // ChartReach without a carrier is refused.
  TrainingConfig chart_no_carrier = base;
  chart_no_carrier.range_profile = RangeProfile::ChartReach;
  bool threw = false;
  try {
    (void)train_bucket(bucket, chart_no_carrier);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "chart-reach without root_ranges throws invalid_argument");

  // Malformed carriers are refused (validation is profile-independent).
  const auto expect_bad_carrier =
      [&](std::vector<std::vector<bs::solver::MultiwayWeightedHand>> bad, const char* what) {
        auto carrier =
            std::make_shared<const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>>(
                std::move(bad));
        TrainingConfig bad_cfg = base;
        bad_cfg.root_ranges = carrier;
        bool bad_threw = false;
        try {
          (void)train_bucket(bucket, bad_cfg);
        } catch (const std::invalid_argument&) {
          bad_threw = true;
        }
        check(bad_threw, what);
      };
  {
    auto bad = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(2);
    bad[0].push_back({{0, 1}, 1.0});
    bad[1] = {};  // empty seat range
    expect_bad_carrier(std::move(bad), "empty seat range throws invalid_argument");
  }
  {
    auto bad = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(2);
    bad[0].push_back({{5, 5}, 1.0});  // duplicate / unsorted cards
    bad[1].push_back({{0, 1}, 1.0});
    expect_bad_carrier(std::move(bad), "unsorted duplicate combo throws invalid_argument");
  }
  {
    auto bad = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(2);
    bad[0].push_back({{0, 1}, 0.0});  // non-positive weight
    bad[1].push_back({{0, 1}, 1.0});
    expect_bad_carrier(std::move(bad), "non-positive combo weight throws invalid_argument");
  }
  {
    auto bad = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(3);  // wrong seat count
    for (auto& seat : bad)
      seat.push_back({{0, 1}, 1.0});
    expect_bad_carrier(std::move(bad), "seat-count mismatch throws invalid_argument");
  }
  {
    // The same sorted combo listed twice would double its marginal mass while
    // the content hash dedupes combos; resolve_ranges must reject it.
    auto bad = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(2);
    bad[0].push_back({{0, 1}, 1.0});
    bad[0].push_back({{0, 1}, 1.0});  // intra-seat duplicate
    bad[1].push_back({{2, 3}, 1.0});
    expect_bad_carrier(std::move(bad), "duplicated intra-seat combo throws invalid_argument");
  }

  // (c) Dealer CONSUMPTION of a genuinely restricted (non-uniform) carrier:
  // under IDENTICAL reconstructed RNG streams the restricted support must
  // draw different joint worlds than the all-1326 carrier, so the resulting
  // regret/sum content differs. (Infoset COUNTS are not the signal — at a few
  // hundred sweeps the sparse full carrier touches fewer rows than a board-
  // varying fixed holding; the signed row content is.) This is the mutation
  // gate for "resolve_ranges actually feeds the joint dealer": if the carrier
  // were ignored, the two runs would be bitwise identical.
  auto all1326 = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>{};
  for (std::size_t s = 0; s < bucket.key.live_count; ++s) {
    std::vector<bs::solver::MultiwayWeightedHand> seat;
    for (int a = 0; a < 52; ++a)
      for (int b = a + 1; b < 52; ++b)
        seat.push_back({{a, b}, 1.0});
    all1326.push_back(std::move(seat));
  }
  auto restricted = std::vector<std::vector<bs::solver::MultiwayWeightedHand>>(2);
  restricted[0].push_back({{0, 1}, 1.0});  // 2s2h
  restricted[1].push_back({{4, 5}, 1.0});  // 3s3h, disjoint

  auto run_with_carrier = [&](const TrainingConfig& cfg) {
    auto tree = std::make_unique<AbstractTree>(def, cfg.action);
    std::map<AbstractInfosetKey, TrainerRow> rows;
    for (std::uint64_t iter = 0; iter < 8; ++iter)
      for (std::size_t traverser = 0; traverser < bucket.key.live_count; ++traverser) {
        bs::SplitMix64 deal_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainJointDeal, iter, traverser);
        bs::SplitMix64 flop_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainRootBoard, iter, traverser);
        bs::SplitMix64 action_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainOpponentAction, iter, traverser);
        bs::SplitMix64 chance_rng =
            derive_stream(cfg.master_seed, StreamPurpose::TrainBoardRunout, iter, traverser);
        debug_run_one_sweep(bucket, cfg, tree.get(), traverser, deal_rng, flop_rng, action_rng,
                            chance_rng, rows);
      }
    return rows;
  };

  TrainingConfig full_cfg = base;
  full_cfg.range_profile = RangeProfile::ChartReach;
  full_cfg.root_ranges =
      std::make_shared<const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>>(
          std::move(all1326));
  TrainingConfig tiny_cfg = base;
  tiny_cfg.range_profile = RangeProfile::ChartReach;
  tiny_cfg.root_ranges =
      std::make_shared<const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>>(
          std::move(restricted));

  const std::map<AbstractInfosetKey, TrainerRow> full_rows = run_with_carrier(full_cfg);
  const std::map<AbstractInfosetKey, TrainerRow> tiny_rows = run_with_carrier(tiny_cfg);
  std::size_t differing_rows = 0;
  for (const auto& [key, fr] : full_rows) {
    auto it = tiny_rows.find(key);
    if (it == tiny_rows.end() || it->second.regrets != fr.regrets || it->second.sums != fr.sums)
      ++differing_rows;
  }
  std::printf("[range-profile] carrier-content differing_rows=%zu full_keys=%zu tiny_keys=%zu\n",
              differing_rows, full_rows.size(), tiny_rows.size());
  check(differing_rows > 0,
        "a restricted carrier changes the dealt worlds (regret/sum content differs under the "
        "same RNG)");
  check(!tiny_rows.empty(), "a restricted carrier still trains a non-empty table");
  return local_failures;
}

}  // namespace

int main() {
  failures += test_alignment();
  failures += test_contract();
  failures += test_public_path_hash_injectivity();
  failures += test_stream_order_sensitivity();
  failures += test_range_profile();
  {
    const GeometryBucket bucket = bucket_2p();
    const TrainingConfig config = make_config(1, 1);
    failures += test_sweep_algebra(bucket, config, /*streaming=*/false);
  }
  {
    // Streaming cursor algebra on a shallow live==3 bucket: no tree exists,
    // both the trainer sweep and the independent reference key by PublicPath
    // hash and recompute menus on the fly.
    const GeometryBucket bucket = bucket_3p_shallow();
    const TrainingConfig config = make_config(1, 1);
    failures += test_sweep_algebra(bucket, config, /*streaming=*/true);
  }
  failures += test_statistics(bucket_2p());
  if (failures) {
    std::printf("STAGE 6 TRAINER TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 TRAINER TESTS PASSED");
  return 0;
}
