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
// CRN pairing: candidate and baseline estimates use identical learn/confirm
// seed lists, so the per-bucket paired difference uses the same joint deals.
// Every figure is general-sum COARSE-GAME NashConv (the deviation space is the
// trainer's coarse abstraction) in chips/hand with a Student-t 95% CI on the
// scalar per-replicate seat sum; it is an estimate with no convergence
// guarantee, confirm_misses discloses unsaturated learn tables, and
// "exploitability" is never used.
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
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <bs/stage6/statistics.hpp>
#include <bs/stage6/trainer.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace poker = bs::poker;
using namespace bs::stage6;

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
};

struct TrainBudget {
  std::size_t max_infosets = 8'000'000;
  std::size_t max_bytes = std::size_t{1} << 33;  // 8 GiB
  std::chrono::milliseconds wall{0};             // 0 = run to the requested iterations
};

std::vector<TrainedBucket> train_buckets(std::size_t n, std::uint64_t iters, std::uint64_t seed,
                                         std::size_t limit, bool verbose,
                                         const TrainBudget& budget) {
  std::vector<GeometrySignature> sigs = enumerate_flop_geometries(n, kBigBlind, nullptr);
  std::vector<GeometryBucket> buckets = bucket_geometries(n, kBigBlind, sigs);
  TrainSetup setup = make_setup(sigs);

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
  for (const GeometryBucket& bucket : buckets) {
    if (!bucket.actionable)
      continue;
    if (limit && out.size() >= limit)
      break;
    if (verbose)
      std::fprintf(stderr, "[train] n=%zu bucket=%s iters=%llu\n", n,
                   bucket.key.to_string().c_str(), static_cast<unsigned long long>(iters));
    TrainedBucket tb;
    tb.bucket = bucket;
    tb.result = train_bucket(bucket, config);
    tb.manifest =
        manifest_for(bucket, config, tb.result.rows, tb.result.report.iterations_completed);
    out.push_back(std::move(tb));
  }
  return out;
}

int cmd_train(std::size_t n, std::uint64_t iters, std::uint64_t seed, std::size_t limit,
              const std::string& outdir, const TrainBudget& budget) {
  std::vector<TrainedBucket> trained = train_buckets(n, iters, seed, limit, true, budget);
  std::printf("trained_buckets=%zu\n", trained.size());
  if (!outdir.empty()) {
    for (const TrainedBucket& tb : trained) {
      const std::string dir = outdir + "/" + tb.bucket.key.to_string();
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

// Measures one profile's NashConv replicates on one reduced representative.
BrEstimatorResult measure_profile(const GeometryBucket& bucket,
                                  std::vector<const BehaviorPolicy*> policies,
                                  std::span<const std::uint64_t> learn,
                                  std::span<const std::uint64_t> confirm,
                                  const bs::abstraction::ActionAbstraction& coarse_action) {
  const std::array<int, 3> flop = {0, 6, 21};
  const poker::GameDef def = reduced_rooted_def(bucket, flop);
  auto ranges = uniform_ranges(bucket.key.live_count);
  BrEstimatorConfig config;
  config.require_stabilization = false;  // MC learn; report estimate, not fixed point
  // R11 measures COARSE-GAME general-sum NashConv: the deviator is restricted
  // to the same coarse abstraction the artifacts were trained in. An infoset
  // the learn seeds never froze is recorded as "no deviation there" rather
  // than aborting; its count is published so a saturated estimate can never be
  // mistaken for a converged one.
  config.deviation_space = DeviationSpace::CoarseAbstraction;
  config.coarse_action = &coarse_action;
  config.on_confirm_miss = FrozenMissPolicy::RecordMiss;
  return estimate_deviation_gains(def, ranges, policies, learn, confirm, nullptr, config);
}

int cmd_measure(std::size_t n, std::uint64_t iters, std::uint64_t seed, std::size_t limit,
                std::size_t learn_count, std::size_t confirm_count, const TrainBudget& budget) {
  std::vector<TrainedBucket> trained = train_buckets(n, iters, seed, limit, true, budget);

  // Identical disjoint seed lists for both profiles so the paired difference
  // uses exactly the same joint deals.
  const std::vector<std::uint64_t> learn = seed_series(1000, learn_count);
  const std::vector<std::uint64_t> confirm = seed_series(500000, confirm_count);
  const bs::abstraction::ActionAbstraction coarse = coarse_action();

  std::printf(
      "# R11 coarse-game general-sum NashConv (deviation space = trainer coarse "
      "{1/2 bet,1x raise}); estimate, no convergence guarantee\n");
  std::printf(
      "bucket,profile,nashconv_mean,nashconv_lo,nashconv_hi,paired_d_mean,paired_d_lo,"
      "paired_d_hi,replicates,br_confirm_misses,candidate_uniform_rows\n");
  for (const TrainedBucket& tb : trained) {
    const std::size_t seats = tb.bucket.key.live_count;

    // Candidate profile: every seat plays the composed candidate bound to THIS
    // bucket (the estimate is run on the bucket's reduced representative
    // game). Coarse-space NashConv explores the whole coarse bush, so it
    // reaches structurally-valid nodes a finite sampled artifact never sealed;
    // the candidate answers those with the artifact's own uniform-unvisited
    // rule and the count is published. A genuine abstraction violation still
    // throws.
    CandidateBinding binding;
    binding.bucket = tb.bucket;
    binding.action = coarse_action();
    binding.manifest = tb.manifest;
    binding.rows = tb.result.rows;
    std::vector<CandidateBinding> one_binding;
    one_binding.push_back(std::move(binding));
    CandidateBehaviorPolicy candidate_one(std::move(one_binding),
                                          CandidateMissPolicy::UniformOnUnvisited);
    std::vector<const BehaviorPolicy*> candidate_policies(seats, &candidate_one);
    // Baseline profile: every seat plays the pinned baseline.
    BaselineBehaviorPolicy baseline_one;
    std::vector<const BehaviorPolicy*> baseline_policies(seats, &baseline_one);

    const BrEstimatorResult cand =
        measure_profile(tb.bucket, candidate_policies, learn, confirm, coarse);
    const BrEstimatorResult base =
        measure_profile(tb.bucket, baseline_policies, learn, confirm, coarse);

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

    std::printf("%s,baseline,%.6f,%.6f,%.6f,,,,%zu,%llu,0\n", tb.bucket.key.to_string().c_str(),
                base_ci.mean, base_ci.lower, base_ci.upper, base.nashconv_replicates.size(),
                static_cast<unsigned long long>(base_misses));
    std::printf("%s,candidate,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%llu,%llu\n",
                tb.bucket.key.to_string().c_str(), cand_ci.mean, cand_ci.lower, cand_ci.upper,
                d_ci.mean, d_ci.lower, d_ci.upper, cand.nashconv_replicates.size(),
                static_cast<unsigned long long>(cand_misses),
                static_cast<unsigned long long>(cand_uniform));
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
    }
    budget.wall = std::chrono::milliseconds(wall_seconds * 1000);
    budget.max_infosets = max_infosets;
    budget.max_bytes = std::size_t{max_gib} << 30;
    if (command == "hashes")
      return cmd_hashes();
    if (command == "geometries")
      return cmd_geometries(n);
    if (command == "train")
      return cmd_train(n, iters, seed, limit, outdir, budget);
    if (command == "measure")
      return cmd_measure(n, iters, seed, limit, learn_count, confirm_count, budget);
    std::fprintf(stderr,
                 "usage: %s hashes|geometries|train|measure [--n N --iters K --seed S "
                 "--buckets K --learn L --confirm C --out dir --wall-seconds S "
                 "--max-infosets M --max-gib G]\n",
                 argv[0]);
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "measurement driver failed: %s\n", e.what());
    return 1;
  }
}
