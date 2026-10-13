// RFC 0016: synchronous external-sampling MCCFR for 2..10 seats.
// Each epoch freezes one profile. Workers sample regret sweeps independently,
// stage signed deltas, and reduce them in sample order before one RM+ clipping
// step. A separate chance-sampled traversal enumerates all public actions and
// accumulates each actor's own-reach-weighted average, including zero-opponent
// reach paths. Batch width one uses the same kernel as parallel training.
// Results identify sample count, epoch count, batch width, and revision.
// No multiplayer equilibrium guarantee follows from these mechanics.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/prng.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>
namespace bs::gto {
class FrontierEvaluator;  // RFC 0007: flop-terminal frontier evaluation
}

namespace bs::solver {

// The card bucket kind the n-seat trainer seals rows under. Exposed so
// consumers (continuation-range export, artifact readers) compute the same
// bucket the trainer keyed on; a consumer that buckets differently silently
// misses every row.
inline constexpr abstraction::CardBucketKind kNSeatCardKind =
    abstraction::CardBucketKind::Preflop169;

// Resource bounds for the n-seat trainer. Distinct from TrainingLimits: the
// node cap bounds the FINITE tree materialization (delegated to TreeLimits,
// which throws tree_resource_exhausted), `max_visits` bounds the unbounded
// random walk, and the information-set/byte caps bound the retained row store.
struct NSeatTrainerLimits {
  std::size_t max_nodes = 1'000'000;
  std::size_t max_information_sets = 1'000'000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;
  // Walk visits across the whole run: one per non-terminal walk() entry
  // (action and chance nodes; terminal leaves do not increment the counter).
  // Unlike the tree node cap this pair DOES need a wall clock: the sampled
  // walk is unbounded.
  std::size_t max_visits = 1'000'000'000'000ULL;
  std::chrono::milliseconds wall{60'000};
};

// Thrown when a resource cap is crossed before the run could complete. Not an
// invalid_argument (resource exhaustion is an expected, recoverable condition
// distinct from malformed input), matching tree_resource_exhausted and
// stage6_training_exhausted.
class nseat_training_exhausted : public std::runtime_error {
 public:
  explicit nseat_training_exhausted(const std::string& what) : std::runtime_error(what) {}
};

// How a run ended. Complete = every requested iteration finished; WallClock =
// the wall cap expired after at least one completed iteration (the result
// reports the last completed iteration counts); ResourceLimit = a
// node/information-set/byte/visit/depth cap was crossed. No partial iteration
// is ever published in any phase.
enum class NSeatTerminationPhase { Complete, WallClock, ResourceLimit };

// Stream roles for the pinned per-sweep derivation. Values are explicit so a
// reordering cannot silently move a stream.
enum class NSeatStreamPurpose : std::uint64_t {
  JointDeal = 1,
  OpponentAction = 2,
  ChanceCard = 3,
  AverageJointDeal = 4,
  AverageChanceCard = 5,
};

// The trainer's declared algorithm revision. Two revisions are different
// policies by definition (RFC 0009 D2), so this is stamped into every result.
inline constexpr std::uint32_t kNSeatAlgorithmRevision = 2;

// The pinned stream derivation: SplitMix64 seeded with
//   fnv1a("bsnseat|" + purpose(decimal) + "|" + master_seed(decimal) + "|" +
//         iteration(decimal) + "|" + traverser(decimal))
// where fnv1a is the 64-bit FNV-1a (offset basis 0xcbf29ce484222325, prime
// 0x100000001b3) over the ASCII bytes of that string (built with
// std::to_string). Deterministic and order sensitive in every argument; with
// MIXER = 0x42534E5354414D32 ("BSNSTAM2") the returned stream is
// SplitMix64(fnv1a(...) ^ MIXER). Deliberately NOT the stage-6 derivation
// (which mixes a role-tagged domain constant through mix64), because the two
// trainers are separate policy streams by definition.
SplitMix64 derive_nseat_stream(std::uint64_t master_seed, NSeatStreamPurpose purpose,
                               std::uint64_t iteration, std::size_t traverser);

// The row address: the acting seat's own card bucket plus the L3 public-node
// index of the tree the policy was trained on. Structurally the same
// (bucket, node) pair the stage-6 materialized rows use, minus the stage-6
// artifact stamp (this type is solver domain and has no artifacts dependency).
struct NSeatInformationKey {
  std::uint32_t own_card_bucket = 0;
  std::uint64_t node_index = 0;

  bool operator==(const NSeatInformationKey&) const = default;
  auto operator<=>(const NSeatInformationKey&) const = default;
};

// Pure feature readers of a key; the trainer's internal walk uses exactly
// these, so a consumer can address a row without re-deriving the layout.
inline std::uint64_t nseat_node_index(const NSeatInformationKey& key) {
  return key.node_index;
}
inline std::uint32_t nseat_own_card_bucket(const NSeatInformationKey& key) {
  return key.own_card_bucket;
}

// One sealed information set. `actions` is the tree node's ordered menu (the
// same values the walk enumerated); `probabilities` is the chance-sampled
// full-action average row, or uniform when no positive own-reach average
// contribution (deterministic, documented, and not counted anywhere).
struct NSeatPolicyRow {
  std::vector<poker::Action> actions;
  std::vector<double> probabilities;
  std::uint64_t visits = 0;
};

// The seat-generic policy: the full game and action-abstraction identity of
// the trained tree plus the sealed average rows. It is the n-seat
// generalization of HeadsUpPolicy; the two are deliberately separate types so
// the two-seat route's artifacts and bytes cannot drift.
struct NSeatTrainingResult;  // the seeding entry point below
struct NSeatInitialState;    // RFC 0012 Phase 4: warm-start state, defined below

class NSeatPolicy {
 public:
  // Default constructor: an empty policy (no rows). Needed by NSeatTrainingResult;
  // the intended construction path is from_parts (codec) or train_nseat.
  NSeatPolicy() = default;

  const poker::GameDef& game() const noexcept { return game_; }
  const abstraction::AbstractionId& action_id() const noexcept { return action_id_; }
  const std::map<NSeatInformationKey, NSeatPolicyRow>& rows() const noexcept { return rows_; }

  // RFC 0013 Stage 2: reconstruct a policy from saved rows (policy codec).
  // The game def and action id must match the tree the rows were trained on;
  // they are not stored in the policy file (reconstructed from the config).
  static NSeatPolicy from_parts(poker::GameDef game, abstraction::AbstractionId action_id,
                                std::map<NSeatInformationKey, NSeatPolicyRow> rows) {
    return NSeatPolicy(std::move(game), std::move(action_id), std::move(rows));
  }

  // Exact lookup. MISSES ARE EXACT: a null return means no sealed row covers
  // the request (an unvisited node, an off-tree state, a different tree, or a
  // different acting seat) -- there is no uniform fill, no nearest-node
  // translation, and no off-tree approximation. `tree` must be the tree the
  // policy was trained on: a tree whose def is not `same_game_def`-equal to the
  // sealed game or whose action id differs is an exact miss. The state must be
  // an Action phase whose acting seat is `seat`, with `seat < player_count` and
  // the own cards off the board.
  //
  // Addressing is a depth-first search of the materialized tree that accepts a
  // node only when its observable state (phase, street, board, actor, pot, and
  // every seat's chip ledger) equals the query state. The unified GameState is
  // deliberately historyless, so two nodes can share an observable state; the
  // search resolves that deterministically in child order, and a resulting
  // key/value ambiguity is documented behavior, never guessed around.
  const NSeatPolicyRow* lookup(const tree::AbstractTree& tree, const poker::GameState& state,
                               std::size_t seat, std::array<int, 2> own_cards) const;

 private:
  friend NSeatTrainingResult train_nseat(const tree::AbstractTree&,
                                         const std::vector<std::vector<WeightedHand>>&,
                                         std::uint64_t, std::uint64_t, const NSeatTrainerLimits&,
                                         const gto::FrontierEvaluator*, const NSeatInitialState*);
  friend NSeatTrainingResult train_nseat_parallel(const tree::AbstractTree&,
                                                  const std::vector<std::vector<WeightedHand>>&,
                                                  std::uint64_t, std::uint64_t,
                                                  const NSeatTrainerLimits&,
                                                  const gto::FrontierEvaluator*, std::size_t);
  // Private constructor used by from_parts (policy codec) and train_nseat.
  NSeatPolicy(poker::GameDef game, abstraction::AbstractionId action_id,
              std::map<NSeatInformationKey, NSeatPolicyRow> rows)
      : game_(std::move(game)), action_id_(std::move(action_id)), rows_(std::move(rows)) {}
  poker::GameDef game_{};
  abstraction::AbstractionId action_id_{};
  std::map<NSeatInformationKey, NSeatPolicyRow> rows_;
};

// Published cumulative RAW accumulators. The trainer's master row store is a map of
// these keyed by NSeatInformationKey; the sealed NSeatPolicyRow is derived
// from them at the end of a run. RFC 0012 Phase 4 exposes them on the result
// so the builder can checkpoint and warm-start the trainer.
struct NSeatRawRow {
  std::vector<poker::Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
  std::uint64_t visits = 0;
};

// Result of one n-seat training run. Reports solver status and cost only; it
// never carries a guarantee level (the normative source-to-guarantee map is
// L6).
struct NSeatTrainingResult {
  NSeatTerminationPhase termination = NSeatTerminationPhase::ResourceLimit;
  std::uint64_t completed_iterations = 0;
  std::size_t batch_size = 1;
  std::uint64_t completed_epochs = 0;
  std::size_t nodes = 0;  // materialized tree nodes
  std::size_t visits = 0;
  std::size_t information_sets = 0;
  // Conservative retained raw-row plus sealed-policy charges. In-flight
  // converted inputs, sampler buffers, deltas, registry and cohort controls
  // also share max_bytes, but are released before the result is returned.
  std::size_t accounted_bytes = 0;
  std::uint64_t seed = 0;
  // SplitMix64 witness state advanced exactly once per COMMITTED iteration
  // (the report stamps how much deterministic work finished); the per-sweep
  // streams are derived, not threaded.
  std::uint64_t prng_state = 0;
  std::uint32_t algorithm_revision = kNSeatAlgorithmRevision;
  NSeatPolicy policy;
  // RFC 0012 Phase 4: the trainer's raw regret/average-weight table, moved
  // out of the trainer on return so the builder can checkpoint it and
  // warm-start a resumed run. The sealed `policy` above is derived from the
  // same table; both are populated. A consumer that does not checkpoint can
  // ignore this field.
  std::map<NSeatInformationKey, NSeatRawRow> raw_rows;
};

// RFC 0012 Phase 4: optional initial state for warm-start training. When
// passed to train_nseat the trainer initializes its regret/average-weight
// table from `rows` and resumes the iteration counter at
// `completed_iterations`, so the per-iteration PRNG streams (derived from the
// master seed and the iteration number) are identical to a from-scratch run
// that reached the same point. The iteration logic and RM+ update rule are
// unchanged; only the starting table and iteration counter differ.
struct NSeatInitialState {
  std::map<NSeatInformationKey, NSeatRawRow> rows;
  std::uint64_t completed_iterations = 0;
  std::uint32_t algorithm_revision = kNSeatAlgorithmRevision;
  std::size_t batch_size = 1;
};

// Trains the materialized tree for the given per-seat ranges.
//
// Requirements (each violated one throws std::invalid_argument, the malformed
// input family; solve() pre-validates the shape-level ones with the typed
// unsupported_tree_shape before forwarding): one non-empty range per tree seat
// (`ranges.size()` must equal `tree.def().player_count`), each combo a sorted
// distinct in-range pair with a finite strictly positive weight (normalized
// internally by the joint sampler), a postflop root (`board_size` in 3..5; the
// card abstraction is defined on a complete flop) OR a 2..10-seat preflop root
// with `terminal == TerminalDepth::Flop` (RFC 0007; the frontier evaluator
// supplies leaf values and must be non-null), a positive iteration count, and
// the identity or a declared-coarse (RFC 0008) action abstraction. Resource
// caps throw
// `nseat_training_exhausted`; the wall cap instead returns the last completed
// iteration counts with phase WallClock.
//
// RFC 0016: a non-null revision-two, batch-one initial_state resumes its table
// and counter, running `iterations` ADDITIONAL samples
// beyond `initial_state->completed_iterations`. The per-iteration PRNG streams
// are derived from the master seed and the absolute iteration number, so a
// warm-started run that reaches iteration N is bitwise-identical to a
// from-scratch run that reaches iteration N (same seed, same tree, same
// ranges and revision). A null or fresh empty initial_state starts from zero.
NSeatTrainingResult train_nseat(const tree::AbstractTree& tree,
                                const std::vector<std::vector<WeightedHand>>& ranges,
                                std::uint64_t iterations, std::uint64_t master_seed,
                                const NSeatTrainerLimits& limits,
                                const gto::FrontierEvaluator* frontier = nullptr,
                                const NSeatInitialState* initial_state = nullptr);

// RFC 0016: synchronous parallel batches. Width is num_threads clamped to
// [1, iterations]; every sample's seat sweeps and independent averaging read
// the same immutable epoch profile. Signed deltas are averaged and clipped
// once by the coordinator. Fixed width/seed/input/sample count is reproducible;
// changing width changes the algorithm trajectory and number of update epochs.
// The byte cap covers master rows, all worker deltas, discovery registry and
// reduction controls, not process RSS or caller-owned inputs. The wall cap is
// checked only between complete epochs. This route has no warm-start input.
NSeatTrainingResult train_nseat_parallel(const tree::AbstractTree& tree,
                                         const std::vector<std::vector<WeightedHand>>& ranges,
                                         std::uint64_t iterations, std::uint64_t master_seed,
                                         const NSeatTrainerLimits& limits,
                                         const gto::FrontierEvaluator* frontier,
                                         std::size_t num_threads);

// Test-only single-sweep seam (the stage-6 `debug_run_one_sweep` /
// HeadsUpSolverDebug precedent): runs ONE external-sampling traverser sweep
// with caller-owned streams over the REAL walk, menu rule, key addressing,
// row store and terminal settlement. `rows` is both the input table and the
// committed post-sweep state. Revision two freezes input rows for this regret-
// only sweep: average sums stay unchanged. Full-epoch average mathematics is
// verified through train_nseat rather than this seam.
struct NSeatTraversalStreams {
  SplitMix64 joint_deal;
  SplitMix64 opponent_action;
  SplitMix64 chance_card;
};

void debug_run_one_nseat_sweep(const tree::AbstractTree& tree,
                               const std::vector<std::vector<WeightedHand>>& ranges,
                               std::size_t traverser, NSeatTraversalStreams& streams,
                               std::map<NSeatInformationKey, NSeatRawRow>& rows,
                               const gto::FrontierEvaluator* frontier = nullptr);

}  // namespace bs::solver
