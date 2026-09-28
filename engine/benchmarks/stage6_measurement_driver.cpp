// RFC 0008 stage 6 R12 step 8 measurement driver.
//
// The single leaf binary that joins the eval and trainer siblings to run the
// measured larger-table comparison. It is NOT a ctest: its numbers are
// evidence, not pass/fail gates, and a negative or indistinguishable result is
// published as-is. Commands:
//
//   hashes                         print the frozen identity hashes
//                                  (charts, coarse action, deviation menu)
//   geometries --n N               enumerate+bucket reachable flop geometries
//   train --n N --iters K --seed S [--buckets k] [--out dir]
//                                  train every actionable bucket (or the first
//                                  k) and write rows.bin/manifest.bin per bucket
//   measure --n N --iters K --seed S --learn L --confirm C [--buckets k]
//                                  train, then estimate general-sum NashConv of
//                                  the composed-candidate profile and the
//                                  pinned-baseline profile on each reduced
//                                  representative game, and print the R11 CSV
//
// --ranges uniform|chart-reach (train/measure; default uniform): uniform uses
// the union geometry matrix and all-1326 per-seat ranges; chart-reach uses
// ONLY the chart-only reach matrix and the per-bucket pinned-chart-conditioned
// hole ranges, and the printed estimand is explicitly "NashConv CONDITIONAL ON
// CHART REACH" (coarse or EXACT-game according to --exact): absolute values
// are not comparable across range profiles and only the within-same-game
// paired d is meaningful.
//
// CRN pairing: candidate and baseline estimates use identical learn/confirm
// seed lists, so the per-bucket paired difference uses the same joint deals.
// `measure` reports general-sum COARSE-GAME NashConv (deviator restricted to
// the trainer menu); `measure --exact` reports EXACT-GAME NashConv over the
// declared five-pot-fraction action space with the candidate following
// off-coarse sizings via its nearest coarse EDGE. Both are in chips/hand
// with a Student-t 95% CI on the scalar per-replicate seat sum; they are
// estimates with no convergence guarantee, confirm_misses discloses
// unsaturated learn tables, candidate_uniform_rows/candidate_offtree_rows
// disclose no-opinion answers, and "exploitability" is never used.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/br_estimator.hpp>
#include <bs/stage6/candidate_policy.hpp>
#include <bs/stage6/chart_digest.hpp>
#include <bs/stage6/chart_reach.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <bs/stage6/statistics.hpp>
#include <bs/stage6/trainer.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace poker = bs::poker;
using namespace bs::stage6;

using bs::solver::MultiwayWeightedHand;

constexpr poker::Chips kBigBlind = 2;

bs::abstraction::ActionAbstraction coarse_action() {
  bs::abstraction::SizeSchedule schedule = bs::abstraction::default_size_schedule();
  for (auto& street : schedule) {
    street.bets = {{1, 2}};
    street.raises = {{1, 1}};
  }
  return bs::abstraction::ActionAbstraction::declared(schedule,
                                                      bs::abstraction::CoverSeeds::DeclaredOnly);
}

std::vector<std::vector<bs::solver::MultiwayWeightedHand>> uniform_ranges(std::size_t seats) {
  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges(seats);
  for (std::size_t s = 0; s < seats; ++s)
    for (int a = 0; a < 52; ++a)
      for (int b = a + 1; b < 52; ++b)
        ranges[s].push_back(bs::solver::MultiwayWeightedHand{{a, b}, 1.0});
  return ranges;
}

// --- range profile (uniform production path vs chart-reach-conditioned) ------

std::uint64_t driver_fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Deterministic content hash of one bucket's per-seat conditioned range:
// FNV-1a over the per-seat sorted "seat|c0,c1:weight\n" combo lines. Combos
// arrive sorted from the chart-reach builder; the defensive sort keeps the
// hash independent of producer order. Weights are hashed too (today every
// chart-reach weight is exactly 1.0, but folding the bits means a future
// weighted range cannot change the dealt game while keeping this hash).
std::uint64_t range_content_hash(const std::vector<std::vector<MultiwayWeightedHand>>& ranges) {
  std::string buffer;
  for (std::size_t seat = 0; seat < ranges.size(); ++seat) {
    std::vector<std::array<int, 2>> combos;
    combos.reserve(ranges[seat].size());
    for (const MultiwayWeightedHand& hand : ranges[seat])
      combos.push_back(hand.cards);
    std::sort(combos.begin(), combos.end());
    // Re-derive each sorted combo's weight from the unsorted source.
    for (const std::array<int, 2>& combo : combos) {
      double weight = 0.0;
      for (const MultiwayWeightedHand& hand : ranges[seat])
        if (hand.cards == combo) {
          weight = hand.weight;
          break;
        }
      buffer += std::to_string(seat);
      buffer += '|';
      buffer += std::to_string(combo[0]);
      buffer += ',';
      buffer += std::to_string(combo[1]);
      buffer += ':';
      buffer += std::to_string(weight);
      buffer += '\n';
    }
  }
  return driver_fnv1a(buffer);
}

// The exact geometry matrix a run trains and measures on. Uniform keeps the
// existing UNION matrix (chart + deviation grid); chart-reach uses ONLY the
// chart-only walk's reachable signatures, so the bucket representatives are
// restricted to chip lines a pinned-chart preflop actually reaches.
struct ProfileMatrix {
  std::vector<GeometrySignature> sigs;
  std::vector<GeometryBucket> buckets;
  // Full chart-only reach list, retained verbatim because
  // chart_reach_bucket_ranges indexes it per member signature. Empty under
  // uniform.
  std::vector<ReachableGeometry> reach;
};

ProfileMatrix build_profile_matrix(std::size_t n, RangeProfile profile) {
  ProfileMatrix matrix;
  if (profile == RangeProfile::Uniform) {
    matrix.sigs = enumerate_flop_geometries(n, kBigBlind, nullptr);
  } else {
    matrix.reach = enumerate_chart_flop_reach(n, kBigBlind);
    matrix.sigs.reserve(matrix.reach.size());
    for (const ReachableGeometry& rec : matrix.reach)
      matrix.sigs.push_back(rec.sig);
  }
  matrix.buckets = bucket_geometries(n, kBigBlind, matrix.sigs);
  return matrix;
}

poker::GameDef reduced_rooted_def(const GeometryBucket& bucket, const std::array<int, 3>& flop) {
  poker::GameDef def{};
  def.player_count = bucket.key.live_count;
  def.button = 0;
  def.big_blind = bucket.big_blind;
  def.preflop = false;
  poker::Chips total = 0;
  for (std::size_t i = 0; i < bucket.key.live_count; ++i) {
    def.stacks[i] = bucket.representative_stacks[i];
    def.contributions[i] = bucket.representative_contrib[i];
    total += bucket.representative_contrib[i];
  }
  def.pot = bucket.representative_pot;
  if (total != def.pot)
    throw std::runtime_error(
        "measurement driver representative contributions do not reconcile to pot");
  def.board = {flop[0], flop[1], flop[2], 0, 0};
  def.board_size = 3;
  return def;
}

int cmd_hashes() {
  const bs::abstraction::ActionAbstraction action = coarse_action();
  std::printf("chart_digest=%s\n", chart_digest_hex().c_str());
  std::printf("coarse_action=%s digest=%llu\n", action.id().name.c_str(),
              static_cast<unsigned long long>(action.id().digest));
  std::printf("deviation_menu_id=%llu\n",
              static_cast<unsigned long long>(declared_menu_identity_hash()));
  return 0;
}

int cmd_geometries(std::size_t n) {
  std::vector<GeometrySignature> sigs = enumerate_flop_geometries(n, kBigBlind, nullptr);
  std::vector<GeometryBucket> buckets = bucket_geometries(n, kBigBlind, sigs);
  std::size_t actionable = 0;
  for (const GeometryBucket& b : buckets)
    if (b.actionable)
      ++actionable;
  std::printf("n=%zu exact_geometries=%zu buckets=%zu actionable=%zu\n", n, sigs.size(),
              buckets.size(), actionable);
  for (const GeometryBucket& b : buckets)
    std::printf("  %s actionable=%d live=%zu act=%zu minstack=%llu pot=%llu\n",
                b.key.to_string().c_str(), static_cast<int>(b.actionable), b.key.live_count,
                b.key.acting_count, static_cast<unsigned long long>(b.min_acting_stack),
                static_cast<unsigned long long>(b.representative_pot));
  return 0;
}

struct TrainSetup {
  bs::abstraction::ActionAbstraction action;
  std::string chart_digest;
  std::uint64_t geometry_matrix_hash = 0;

  TrainSetup() : action(coarse_action()) {}
};

TrainSetup make_setup(const std::vector<GeometrySignature>& sigs) {
  TrainSetup setup;
  setup.chart_digest = chart_digest_hex();
  GeometryCoverage coverage(sigs);
  setup.geometry_matrix_hash = coverage.content_hash();
  return setup;
}

// Trains the first `limit` actionable buckets (or all when limit==0).
struct TrainedBucket {
  GeometryBucket bucket;
  BucketTrainingResult result;
  FrozenManifest manifest;
  // One conditioned range per reduced seat under chart-reach; empty outer
  // vector is the uniform sentinel (the BR estimator then self-builds the
  // all-1326 ranges, matching the trainer).
  std::vector<std::vector<MultiwayWeightedHand>> ranges;
  // Content hash the artifact was trained and stamped with; cmd_measure
  // asserts the measured carrier hashes back to this so train and measure can
  // never diverge on the conditioned support.
  std::uint64_t range_hash = 0;
};

struct TrainBudget {
  std::size_t max_infosets = 8'000'000;
  std::size_t max_bytes = std::size_t{1} << 33;  // 8 GiB
  std::chrono::milliseconds wall{0};             // 0 = run to the requested iterations
};

std::vector<TrainedBucket> train_buckets(std::size_t n, std::uint64_t iters, std::uint64_t seed,
                                         std::size_t limit, bool verbose, const TrainBudget& budget,
                                         RangeProfile profile) {
  ProfileMatrix matrix = build_profile_matrix(n, profile);
  TrainSetup setup = make_setup(matrix.sigs);

  // Chart-reach only: build the per-bucket conditioned range carrier once.
  std::map<std::string, std::vector<std::vector<MultiwayWeightedHand>>> bucket_ranges;
  if (profile == RangeProfile::ChartReach) {
    for (const GeometryBucket& bucket : matrix.buckets) {
      if (!bucket.actionable)
        continue;
      bucket_ranges.emplace(bucket.key.to_string(),
                            chart_reach_bucket_ranges(bucket, matrix.reach, kBigBlind));
    }
  }

  TrainingConfig config;
  config.action = setup.action;
  config.card_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
  config.iterations = iters;
  config.master_seed = seed;
  config.geometry_matrix_hash = setup.geometry_matrix_hash;
  config.chart_digest_sha256 = setup.chart_digest;
  // Measurement-scale caps: the library defaults (1M infosets / 1 GiB / 1 h)
  // are ctest guards. A real 100bb 2p coarse tree has ~0.55M action nodes and
  // up to ~9 card buckets each, so the offline driver raises the caps and
  // drives a finite wall budget; a wall-capped bucket seals whatever
  // iterations finished and its report stamps that number honestly.
  config.limits.max_information_sets = budget.max_infosets;
  config.limits.max_bytes = budget.max_bytes;
  // Driver wall 0 means "run to the requested iterations"; map it onto the
  // library's effectively-unbounded default rather than the literal 0 the
  // trainer reads as "already past the cap".
  config.limits.wall = budget.wall.count() > 0 ? budget.wall : std::chrono::milliseconds(3'600'000);

  std::vector<TrainedBucket> out;
  for (const GeometryBucket& bucket : matrix.buckets) {
    if (!bucket.actionable)
      continue;
    if (limit && out.size() >= limit)
      break;
    TrainingConfig bucket_config = config;
    std::vector<std::vector<MultiwayWeightedHand>> ranges;
    std::uint64_t range_hash = 0;
    if (profile == RangeProfile::ChartReach) {
      auto found = bucket_ranges.find(bucket.key.to_string());
      if (found == bucket_ranges.end())
        throw std::runtime_error("chart-reach bucket ranges missing for " + bucket.key.to_string());
      ranges = found->second;
      range_hash = range_content_hash(ranges);
      bucket_config.range_profile = RangeProfile::ChartReach;
      bucket_config.root_ranges =
          std::make_shared<const std::vector<std::vector<MultiwayWeightedHand>>>(ranges);
      bucket_config.range_content_hash = range_hash;
    }
    if (verbose)
      std::fprintf(stderr, "[train] n=%zu profile=%s bucket=%s iters=%llu\n", n,
                   profile == RangeProfile::ChartReach ? "chart-reach" : "uniform",
                   bucket.key.to_string().c_str(), static_cast<unsigned long long>(iters));
    TrainedBucket tb;
    tb.bucket = bucket;
    tb.ranges = std::move(ranges);
    tb.range_hash = range_hash;
    tb.result = train_bucket(bucket, bucket_config);
    tb.manifest =
        manifest_for(bucket, bucket_config, tb.result.rows, tb.result.report.iterations_completed);
    out.push_back(std::move(tb));
  }
  return out;
}

int cmd_train(std::size_t n, std::uint64_t iters, std::uint64_t seed, std::size_t limit,
              const std::string& outdir, const TrainBudget& budget, RangeProfile profile) {
  std::vector<TrainedBucket> trained = train_buckets(n, iters, seed, limit, true, budget, profile);
  const char* profile_name = profile == RangeProfile::ChartReach ? "chart-reach" : "uniform";
  std::printf("trained_buckets=%zu range_profile=%s\n", trained.size(), profile_name);
  if (!outdir.empty()) {
    for (const TrainedBucket& tb : trained) {
      // Segmented per profile AND table size so uniform and chart-reach
      // artifacts (and different player counts) can never overwrite.
      const std::string dir =
          outdir + "/" + profile_name + "/n" + std::to_string(n) + "/" + tb.bucket.key.to_string();
      write_artifact_dir(dir, tb.manifest, tb.result.rows);
      std::printf("wrote %s rows=%zu\n", dir.c_str(), tb.result.rows.size());
    }
  }
  for (const TrainedBucket& tb : trained)
    std::printf("  %s infosets=%zu iters_done=%llu/%llu mode=%d wall_capped=%d\n",
                tb.bucket.key.to_string().c_str(), tb.result.report.information_sets,
                static_cast<unsigned long long>(tb.result.report.iterations_completed),
                static_cast<unsigned long long>(iters), static_cast<int>(tb.result.mode),
                static_cast<int>(tb.result.report.wall_reached));
  return 0;
}

// One deterministic seeded series 1..count (the RFC 0006 pinned seed family is
// small; the driver takes explicit ranges elsewhere).
std::vector<std::uint64_t> seed_series(std::uint64_t base, std::size_t count) {
  std::vector<std::uint64_t> seeds;
  for (std::size_t i = 0; i < count; ++i)
    seeds.push_back(base + i * 0x9e3779b97f4a7c15ULL + 1);
  return seeds;
}

// Fixed flop shared by the BR estimate and the fully-blocked preflight.
constexpr std::array<int, 3> kMeasureFlop = {0, 6, 21};

// Fail closed if a seat's conditioned range is fully blocked by the fixed
// measurement flop (the joint dealer would then have no legal deal for that
// seat). Under the pinned chart this never happens empirically; an empty
// survivor would otherwise surface as a less explicit sampler error.
void preflight_ranges_play_on(const GeometryBucket& bucket,
                              const std::vector<std::vector<MultiwayWeightedHand>>& ranges) {
  for (std::size_t seat = 0; seat < ranges.size(); ++seat) {
    bool any_playable = false;
    for (const MultiwayWeightedHand& hand : ranges[seat]) {
      const bool blocked = hand.cards[0] == kMeasureFlop[0] || hand.cards[0] == kMeasureFlop[1] ||
                           hand.cards[0] == kMeasureFlop[2] || hand.cards[1] == kMeasureFlop[0] ||
                           hand.cards[1] == kMeasureFlop[1] || hand.cards[1] == kMeasureFlop[2];
      if (!blocked) {
        any_playable = true;
        break;
      }
    }
    if (!any_playable)
      throw std::runtime_error("measurement preflight: bucket " + bucket.key.to_string() +
                               " seat " + std::to_string(seat) +
                               " range is fully blocked by the fixed flop {0,6,21}");
  }
}

// Measures one profile's NashConv replicates on one reduced representative.
// `ranges` is the SAME per-bucket object for the candidate and baseline
// passes, so both are measured on the IDENTICAL restricted game (the CRN
// learn/confirm seeds are shared too). `exact_space` selects the declared
// five-pot-fraction EXACT deviation menu (the deviator plays concrete legal
// sizings) instead of the trainer coarse menu; the candidate must be built
// with NearestCoarseEdge to follow an exact off-coarse line back to a sealed
// node.
BrEstimatorResult measure_profile(const GeometryBucket& bucket,
                                  std::vector<const BehaviorPolicy*> policies,
                                  const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                                  std::span<const std::uint64_t> learn,
                                  std::span<const std::uint64_t> confirm,
                                  const bs::abstraction::ActionAbstraction& coarse_action,
                                  bool exact_space) {
  const poker::GameDef def = reduced_rooted_def(bucket, kMeasureFlop);
  BrEstimatorConfig config;
  config.require_stabilization = false;  // MC learn; report estimate, not fixed point
  // Coarse R11 restricts the deviator to the trainer menu; exact R11 lets it
  // pick the declared five-fraction concrete sizings the real game offers.
  // An infoset the learn seeds never froze is recorded as "no deviation
  // there"; its count is published so a saturated estimate can never be
  // mistaken for a converged one.
  if (exact_space) {
    config.deviation_space = DeviationSpace::Declared5Fraction;
    config.coarse_action = nullptr;
  } else {
    config.deviation_space = DeviationSpace::CoarseAbstraction;
    config.coarse_action = &coarse_action;
  }
  config.on_confirm_miss = FrozenMissPolicy::RecordMiss;
  return estimate_deviation_gains(def, ranges, policies, learn, confirm, nullptr, config);
}

int cmd_measure(std::size_t n, std::uint64_t iters, std::uint64_t seed, std::size_t limit,
                std::size_t learn_count, std::size_t confirm_count, const TrainBudget& budget,
                bool exact_space, RangeProfile profile) {
  std::vector<TrainedBucket> trained = train_buckets(n, iters, seed, limit, true, budget, profile);
  const bool chart_reach = profile == RangeProfile::ChartReach;
  const char* profile_name = chart_reach ? "chart-reach" : "uniform";

  // Identical disjoint seed lists for both profiles so the paired difference
  // uses exactly the same joint deals.
  const std::vector<std::uint64_t> learn = seed_series(1000, learn_count);
  const std::vector<std::uint64_t> confirm = seed_series(500000, confirm_count);
  const bs::abstraction::ActionAbstraction coarse = coarse_action();

  if (chart_reach) {
    std::printf(
        "# R11 general-sum %s NashConv CONDITIONAL ON CHART REACH "
        "(profile=chart-reach; per-seat hole deals restricted to holdings whose "
        "pinned-chart preflop actions reach the bucket; indicator mixture over member "
        "position-rotations; matrix=chart-only subset; %s; absolute values are not comparable "
        "across range profiles; estimate, no convergence guarantee)\n",
        exact_space ? "EXACT-game" : "coarse",
        exact_space
            ? "deviation menu = declared five pot fractions; candidate follows exact sizings via "
              "nearest coarse EDGE"
            : "deviation space = trainer coarse {1/2 bet,1x raise}");
  } else {
    std::printf(
        "# R11 %s general-sum NashConv (%s; range_profile=uniform); estimate, no convergence "
        "guarantee\n",
        exact_space ? "EXACT-game" : "coarse-game",
        exact_space ? "deviation menu = declared five pot fractions; candidate follows exact "
                      "sizings via nearest coarse EDGE"
                    : "deviation space = trainer coarse {1/2 bet,1x raise}");
  }
  std::printf(
      "bucket,profile,nashconv_mean,nashconv_lo,nashconv_hi,paired_d_mean,paired_d_lo,"
      "paired_d_hi,replicates,br_confirm_misses,candidate_uniform_rows,candidate_offtree_rows,"
      "range_profile\n");
  for (const TrainedBucket& tb : trained) {
    const std::size_t seats = tb.bucket.key.live_count;

    // The candidate and baseline passes measure the SAME restricted game.
    // Uniform explicitly self-builds the all-1326 ranges the trainer used;
    // chart-reach reuses the bucket's conditioned carrier verbatim.
    const std::vector<std::vector<MultiwayWeightedHand>> measure_ranges =
        chart_reach ? tb.ranges : uniform_ranges(seats);
    if (chart_reach) {
      // Train and measure must deal from the same conditioned support: the
      // measured carrier hashes back to the value the artifact was stamped
      // with (tb.range_hash is the per-bucket range_content_hash used at
      // training).
      if (range_content_hash(measure_ranges) != tb.range_hash)
        throw std::runtime_error(
            "chart-reach measure carrier differs from the trained carrier for " +
            tb.bucket.key.to_string());
      preflight_ranges_play_on(tb.bucket, measure_ranges);
    }

    // Candidate profile: every seat plays the composed candidate bound to THIS
    // bucket (the estimate is run on the bucket's reduced representative
    // game). NashConv explores the whole deviation bush, so it reaches
    // structurally-valid nodes a finite sampled artifact never sealed; the
    // candidate answers those with the artifact's own uniform-unvisited rule
    // and the count is published. A genuine abstraction violation still
    // throws. The exact-space pass uses NearestCoarseEdge addressing so an
    // off-coarse concrete deviation still lands on a sealed coarse node.
    CandidateBinding binding;
    binding.bucket = tb.bucket;
    binding.action = coarse_action();
    binding.manifest = tb.manifest;
    binding.rows = tb.result.rows;
    std::vector<CandidateBinding> one_binding;
    one_binding.push_back(std::move(binding));
    CandidateBehaviorPolicy candidate_one(std::move(one_binding),
                                          CandidateMissPolicy::UniformOnUnvisited,
                                          exact_space ? CandidateProjectionMode::NearestCoarseEdge
                                                      : CandidateProjectionMode::StrictCoarse);
    std::vector<const BehaviorPolicy*> candidate_policies(seats, &candidate_one);
    // Baseline profile: every seat plays the pinned baseline.
    BaselineBehaviorPolicy baseline_one;
    std::vector<const BehaviorPolicy*> baseline_policies(seats, &baseline_one);

    const BrEstimatorResult cand = measure_profile(tb.bucket, candidate_policies, measure_ranges,
                                                   learn, confirm, coarse, exact_space);
    const BrEstimatorResult base = measure_profile(tb.bucket, baseline_policies, measure_ranges,
                                                   learn, confirm, coarse, exact_space);

    const ConfidenceInterval cand_ci = confidence_interval_95(cand.nashconv_replicates);
    const ConfidenceInterval base_ci = confidence_interval_95(base.nashconv_replicates);
    // d = baseline - candidate: positive means the candidate is LESS
    // exploitable than the pinned baseline (a gain).
    const ConfidenceInterval d_ci =
        paired_difference_ci_95(base.nashconv_replicates, cand.nashconv_replicates);

    const std::uint64_t base_misses =
        std::accumulate(base.counts.confirm_misses_per_traverser.begin(),
                        base.counts.confirm_misses_per_traverser.end(), std::uint64_t{0});
    const std::uint64_t cand_misses =
        std::accumulate(cand.counts.confirm_misses_per_traverser.begin(),
                        cand.counts.confirm_misses_per_traverser.end(), std::uint64_t{0});
    const std::uint64_t cand_uniform = candidate_one.unvisited_misses();
    const std::uint64_t cand_offtree = candidate_one.off_tree_misses();

    std::printf("%s,baseline,%.6f,%.6f,%.6f,,,,%zu,%llu,0,0,%s\n",
                tb.bucket.key.to_string().c_str(), base_ci.mean, base_ci.lower, base_ci.upper,
                base.nashconv_replicates.size(), static_cast<unsigned long long>(base_misses),
                profile_name);
    std::printf("%s,candidate,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%llu,%llu,%llu,%s\n",
                tb.bucket.key.to_string().c_str(), cand_ci.mean, cand_ci.lower, cand_ci.upper,
                d_ci.mean, d_ci.lower, d_ci.upper, cand.nashconv_replicates.size(),
                static_cast<unsigned long long>(cand_misses),
                static_cast<unsigned long long>(cand_uniform),
                static_cast<unsigned long long>(cand_offtree), profile_name);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string command = argc > 1 ? argv[1] : "";
    std::size_t n = 2;
    std::uint64_t iters = 100000;
    std::uint64_t seed = 1;
    std::size_t limit = 0;
    std::size_t learn_count = 64;
    std::size_t confirm_count = 64;
    std::string outdir;
    TrainBudget budget;
    std::uint64_t wall_seconds = 0;
    std::size_t max_infosets = budget.max_infosets;
    std::uint64_t max_gib = 8;
    bool exact_space = false;
    std::string ranges_arg = "uniform";
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
      if (arg == "--n")
        n = std::stoul(next());
      else if (arg == "--iters")
        iters = std::stoull(next());
      else if (arg == "--seed")
        seed = std::stoull(next());
      else if (arg == "--buckets")
        limit = std::stoul(next());
      else if (arg == "--learn")
        learn_count = std::stoul(next());
      else if (arg == "--confirm")
        confirm_count = std::stoul(next());
      else if (arg == "--out")
        outdir = next();
      else if (arg == "--wall-seconds")
        wall_seconds = std::stoull(next());
      else if (arg == "--max-infosets")
        max_infosets = std::stoul(next());
      else if (arg == "--max-gib")
        max_gib = std::stoull(next());
      else if (arg == "--exact")
        exact_space = true;
      else if (arg == "--ranges")
        ranges_arg = next();
    }
    RangeProfile profile;
    if (ranges_arg == "uniform")
      profile = RangeProfile::Uniform;
    else if (ranges_arg == "chart-reach")
      profile = RangeProfile::ChartReach;
    else {
      std::fprintf(stderr, "usage: --ranges must be 'uniform' or 'chart-reach' (got '%s')\n",
                   ranges_arg.c_str());
      std::fprintf(stderr,
                   "usage: %s hashes|geometries|train|measure [--n N --iters K --seed S "
                   "--buckets K --learn L --confirm C --out dir --wall-seconds S "
                   "--max-infosets M --max-gib G --exact] [--ranges uniform|chart-reach]\n",
                   argv[0]);
      return 2;
    }
    budget.wall = std::chrono::milliseconds(wall_seconds * 1000);
    budget.max_infosets = max_infosets;
    budget.max_bytes = std::size_t{max_gib} << 30;
    if (command == "hashes")
      return cmd_hashes();
    if (command == "geometries")
      // Generic enumeration tool: stays on the UNION matrix and ignores
      // --ranges deliberately.
      return cmd_geometries(n);
    if (command == "train")
      return cmd_train(n, iters, seed, limit, outdir, budget, profile);
    if (command == "measure")
      return cmd_measure(n, iters, seed, limit, learn_count, confirm_count, budget, exact_space,
                         profile);
    std::fprintf(stderr,
                 "usage: %s hashes|geometries|train|measure [--n N --iters K --seed S "
                 "--buckets K --learn L --confirm C --out dir --wall-seconds S "
                 "--max-infosets M --max-gib G --exact] [--ranges uniform|chart-reach]\n",
                 argv[0]);
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "measurement driver failed: %s\n", e.what());
    return 1;
  }
}
