// stage6/trainer.hpp — external-sampling multiplayer MCCFR trainer.
//
// RM+ regret matching with the kFull OWN-REACH-weighted average strategy (NOT
// CFR+ linear weighting, per RFC 0006:197-199). At the traverser's own node the
// sweep accumulates pi_own(I)*sigma(I,a). Because a sweep reaches I with
// probability pi_opponents(I), the sealed per-row policy is an unbiased
// estimator of the EXTERNAL-MEASURE full average (weighted by
// pi_own*pi_opponents); it coincides with the vanilla time-uniform full average
// per row when opponent reach is effectively deterministic (N=2, or at
// convergence). An unweighted per-visit snapshot omits that factor and is
// biased for N>=3 while play is mixed. One iteration runs N sweeps, one per
// traverser, each with one fresh product-conditional joint deal and a root
// flop sampled by the R3b measure; turn/river cards are drawn in-walk from the
// legal sublist (board + ALL 2N hole cards removed) and indexed by the
// board-only ordinal so materialized and streaming cursors stay aligned.
//
// Materialized mode (live==2): rows are keyed by the coarse AbstractTree node
// index. Streaming mode (live>=3): rows are keyed by PublicPath hash; no deep
// tree is built. The trainer never reaches the deployed policy or eval — it
// links only core + tree (+ solver for the joint sampler through core).
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/public_path.hpp>
#include <bs/stage6/random_streams.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bs::stage6 {

class stage6_training_exhausted : public std::runtime_error {
 public:
  explicit stage6_training_exhausted(const std::string& what) : std::runtime_error(what) {}
};

enum class TrainerMode { Materialized, Streaming };

// Decides (pure, deterministic) whether a representative materializes. The
// pinned rule: Materialized IFF live_count == 2.
TrainerMode materialize_or_stream(const GeometryBucket& bucket);

struct TrainerLimits {
  std::size_t max_information_sets = 1'000'000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;
  std::chrono::milliseconds wall{3'600'000};
};

// Selects the average-policy weighting accumulated in TrainerRow::sums.
//
//   OwnReach (the production default): kFull external-sampling average
//     sums += pi_own * sigma at the traverser's own node. Unbiased for the
//     external-measure (pi_own*pi_opp) policy at EVERY player count.
//
//   PerVisit (test-only): sums += sigma per own-node visit, dropping the own
//     reach weight. Its per-row seal is the time-average of sigma(I), not the
//     external-measure policy E[pi_own*sigma | reach of I under pi_-i]; it
//     coincides with OwnReach when the traverser's own prefix reach is ~{0,1}
//     (trivially at N==2, where the single opponent is integrated out, and at
//     a converged equilibrium) but is biased at N>=3 while play is mixed.
//     Exists solely so the multiplayer mixed-equilibrium gate can prove the
//     production weighting discriminates against the biased one. It is
//     deliberately excluded from artifact_identity_hash/training_config_hash:
//     no production artifact is ever sealed under PerVisit (production
//     entry points keep the OwnReach default), and the multiplayer gate uses
//     the debug fixed-world seam, not the artifact writer.
enum class AverageWeighting { OwnReach, PerVisit };

struct TrainingConfig {
  bs::abstraction::ActionAbstraction action = bs::abstraction::ActionAbstraction::declared(
      bs::abstraction::SizeSchedule(), bs::abstraction::CoverSeeds::DeclaredOnly);
  bs::abstraction::CardBucketKind card_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
  std::uint64_t iterations = 0;
  std::uint64_t master_seed = 0;
  std::uint64_t geometry_matrix_hash = 0;
  std::string chart_digest_sha256;
  // Production kFull weighting; PerVisit is a test-only deliberately-biased
  // alternative for the multiplayer mixed-equilibrium discrimination gate.
  AverageWeighting average_weighting = AverageWeighting::OwnReach;
  TrainerLimits limits{};
};

struct TrainingBucketReport {
  GeometryBucketKey key;
  TrainerMode mode = TrainerMode::Streaming;
  std::size_t information_sets = 0;
  std::size_t retained_bytes = 0;
  std::uint64_t iterations_completed = 0;
  std::uint64_t prng_final_state = 0;
  bool wall_reached = false;
};

// One mutable regret/average row during training.
struct TrainerRow {
  std::vector<poker::Action> actions;
  std::vector<double> regrets;  // RM+ (clipped at zero on read)
  std::vector<double> sums;     // kFull own-reach-weighted average
  std::uint64_t visits = 0;

  std::vector<double> current_strategy() const;
  AbstractPolicyRow average_row() const;
};

// Result of training one bucket.
struct BucketTrainingResult {
  GeometryBucketKey key;
  TrainerMode mode = TrainerMode::Streaming;
  // Identity stamped into every sealed AbstractInfosetKey of this run.
  std::uint64_t artifact_content_hash = 0;
  FrozenArtifactRows rows;
  TrainingBucketReport report;
};

// Trains one actionable geometry bucket on its reduced representative rooted
// game, over all-1326-combo primary ranges (joint-dealt by the R3b sampler).
// Deterministic for (bucket, config). Throws stage6_training_exhausted if a
// limit is crossed BEFORE a promotable artifact exists; the wall cap instead
// returns a partial result (wall_reached=true) once at least one iteration
// completed.
BucketTrainingResult train_bucket(const GeometryBucket& bucket, const TrainingConfig& config);

// Exhaustive structural proof that the streaming PublicPath cursor and the
// materialized TreeNode index address the SAME public nodes, run on one
// materialized (live==2) representative: replays every tree edge with the
// shared GameState transition rule and PublicPath tokens, recomputes each
// action node's coarse menu with the shared L3 rule, and asserts that no two
// action nodes share a path hash and that chance-child positions equal the
// board-only ordinals. Throws std::runtime_error on the first divergence.
struct AlignmentReport {
  std::size_t action_nodes = 0;
  std::size_t chance_nodes = 0;
  std::size_t terminal_nodes = 0;
  std::size_t distinct_action_paths = 0;
};
AlignmentReport verify_materialized_path_alignment(const GeometryBucket& bucket,
                                                   const TrainingConfig& config);

// Frame-replay core on an externally built tree: the production wrapper builds
// the tree for live==2 under the production limits; gate tests also build a
// shallow live>=3 tree under enlarged limits to prove cursor alignment there.
// `def` is the rooted definition the tree was built from.
AlignmentReport debug_verify_tree_paths(const bs::tree::AbstractTree& tree,
                                        const poker::GameDef& def, const TrainingConfig& config,
                                        std::uint64_t geometry_token);

// Fills a FrozenManifest for one trained bucket (identity fields), computing
// the rows content hash. `iterations_completed` is recorded verbatim (a
// wall-capped run seals fewer than config.iterations); the caller must pass
// BucketTrainingResult::report.iterations_completed. The translator digest and
// outer lock SHA-256 are sealed by the eval-side driver.
FrozenManifest manifest_for(const GeometryBucket& bucket, const TrainingConfig& config,
                            const FrozenArtifactRows& rows, std::uint64_t iterations_completed);

// Test-only single-sweep seam (the HeadsUpSolverDebug precedent): runs ONE
// external-sampling traverser sweep with CALLER-OWNED random streams against an
// existing row table, so the gate battery can pin one sweep's regret/average
// updates against an independent reference. Production training never exposes
// streams; this does, only for tests. `tree` is the materialized coarse tree
// for a live==2 bucket and null for a streaming bucket, exactly as
// train_bucket uses it.
void debug_run_one_sweep(const GeometryBucket& bucket, const TrainingConfig& config,
                         const bs::tree::AbstractTree* tree, std::size_t traverser,
                         bs::SplitMix64& deal_rng, bs::SplitMix64& flop_rng,
                         bs::SplitMix64& action_rng, bs::SplitMix64& chance_rng,
                         std::map<AbstractInfosetKey, TrainerRow>& rows);

// Test-only FIXED-WORLD sweep seam: runs ONE streaming external-sampling
// traverser sweep over a caller-supplied rooted definition and joint deal,
// with no tree and no deal/flop RNG. The production walk, menu rule and
// RowStore are used verbatim; only the world construction is supplied by the
// caller, so the multiplayer average-policy parity gate can drive the SAME
// walk over an enumerable restricted joint-deal support and compare it
// against an independent full-traversal CFR reference. `root` must describe a
// 3..10-seat rooted board (streaming mode); `holes` gives one pair per seat in
// seat order and must be mutually compatible with the board. Rows are keyed by
// PublicPath hashes rooted at `geometry_token`.
void debug_run_fixed_world_sweep(const TrainingConfig& config, const poker::GameDef& root,
                                 std::uint64_t geometry_token, const std::vector<HoleCards>& holes,
                                 std::size_t traverser, bs::SplitMix64& action_rng,
                                 bs::SplitMix64& chance_rng,
                                 std::map<AbstractInfosetKey, TrainerRow>& rows);

}  // namespace bs::stage6
