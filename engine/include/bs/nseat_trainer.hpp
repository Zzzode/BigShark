// RFC 0009 W2a: the seat-parameterized MCCFR trainer (L4).
//
// One external-sampling MCCFR trainer for 2..10 seats that consumes the L3
// AbstractTree DIRECTLY (the design solve.hpp:15 already states) and keys its
// rows by the tree's public-node index plus the acting seat's own card bucket.
// It re-implements the conventions the stage-6 measurement trainer documents
// and pins -- RM+ clipped regrets, the kFull own-reach-weighted average
// (`sums[a] += own_reach * sigma[a]` once per traverser sweep at the
// traverser's own node), the raw regret update `regrets[a] += children[a] -
// value`, and the external-sampling walk signature
// `walk(state, node_index, own_reach)` in which every non-traverser seat's
// reach is integrated out by sampling -- but it does NOT depend on the stage-6
// targets (the offline guard forbids that edge; sharing code instead of
// re-deriving it is recorded as an RFC 0009 Open Question, not done here).
//
// What the walk does per traverser sweep:
//
//   * one joint deal is sampled per episode from the per-seat `ranges`
//     (product-conditional rejection draw through
//     bs::solver::sample_scalable_joint_deal, the RFC 0006 sampler);
//   * traverser action node: freeze sigma, enumerate EVERY menu action, recurse
//     into each sampled continuation, accumulate `sums[a] += own_reach *
//     sigma[a]` once, then apply the RM+ update;
//   * opponent action node: sample ONE action with the current policy and
//     recurse with own_reach unchanged (no average contribution);
//   * chance node: sample ONE public card uniformly over the sampled world's
//     legal runout list (`public_runout_cards(state)` minus every seat's two
//     hole cards, ascending -- the stage-6 convention), then address the
//     tree's chance child by the card's BOARD-ONLY ordinal (the tree's chance
//     children enumerate exactly the ascending {0..51}\board list), with the
//     child's `incoming_card` edge label asserted against the sampled card;
//   * terminal: fold leaves read the L3 TerminalPayload's stored
//     `chip_utility`; showdown leaves call `state.settle_showdown(live_holes)`
//     with the concrete per-seat hole cards in ascending live-seat order (the
//     payload's `live_order`). Both assert exact chip conservation
//     (`sum(chip_utility) == 0`) exactly as the stage-6 trainer does.
//
// One iteration runs one sweep per seat (0..n-1 as traverser), exactly like
// stage-6. A sweep writes its regret/average deltas directly to the shared
// row table (there is no local staging map); the atomicity guarantee is that
// a limit interruption never publishes a partial sweep. The node, depth,
// byte, information-set and visit caps all THROW out of the call (no result
// is returned), and the wall cap is checked at iteration boundaries only, so
// the returned policy is always the state after a whole number of completed
// iterations.
//
// Streams: three SplitMix64 streams per sweep, derived deterministically from
// the master seed by `derive_nseat_stream(seed, purpose, iteration, traverser)`
// (the exact derivation is documented at that function and is PINNED: changing
// it changes every sampled trajectory). The trainer holds no ambient
// randomness.
//
// Rows are addressed by `NSeatInformationKey{own_card_bucket, node_index}`.
// The node index abstracts the concrete board value (chance child positions
// are board-only ordinals), so one tree serves every sampled flop of the
// rooted profile. The own-card bucket uses the L2 card abstraction
// (`abstraction::CategoryTiersV1` by default, over the concrete sampled board
// and the acting seat's own two cards only), which is L4-owned here: the
// artifact reader/writer side later maps buckets to concrete decoding
// (RFC 0009 D3).
//
// Refusal layering (mirroring the stage-4 heads-up route): the trainer itself
// throws std::invalid_argument for a malformed request (wrong range count,
// empty or invalid ranges, zero iterations, a preflop root, a non-identity
// action abstraction) and `nseat_training_exhausted` for a crossed resource
// cap. `solve()` pre-validates the shape conditions it must report as typed
// solver-selection refusals (`unsupported_tree_shape`) BEFORE forwarding, so
// every condition today's Auto already refuses keeps its current typed
// behavior through the unified entry point.
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
};

// The trainer's declared algorithm revision. Two revisions are different
// policies by definition (RFC 0009 D2), so this is stamped into every result.
inline constexpr std::uint32_t kNSeatAlgorithmRevision = 1;

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
// same values the walk enumerated); `probabilities` is the kFull average row,
// or the uniform fallback when the row never received an own-node average
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

class NSeatPolicy {
 public:
  const poker::GameDef& game() const noexcept { return game_; }
  const abstraction::AbstractionId& action_id() const noexcept { return action_id_; }
  const std::map<NSeatInformationKey, NSeatPolicyRow>& rows() const noexcept { return rows_; }

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
                                         const gto::FrontierEvaluator*);
  poker::GameDef game_{};
  abstraction::AbstractionId action_id_{};
  std::map<NSeatInformationKey, NSeatPolicyRow> rows_;
};

// Result of one n-seat training run. Reports solver status and cost only; it
// never carries a guarantee level (the normative source-to-guarantee map is
// L6).
struct NSeatTrainingResult {
  NSeatTerminationPhase termination = NSeatTerminationPhase::ResourceLimit;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;  // materialized tree nodes
  std::size_t visits = 0;
  std::size_t information_sets = 0;
  std::size_t accounted_bytes = 0;
  std::uint64_t seed = 0;
  // SplitMix64 witness state advanced exactly once per COMMITTED iteration
  // (the report stamps how much deterministic work finished); the per-sweep
  // streams are derived, not threaded.
  std::uint64_t prng_state = 0;
  std::uint32_t algorithm_revision = kNSeatAlgorithmRevision;
  NSeatPolicy policy;
};

// Trains the materialized tree for the given per-seat ranges.
//
// Requirements (each violated one throws std::invalid_argument, the malformed
// input family; solve() pre-validates the shape-level ones with the typed
// unsupported_tree_shape before forwarding): one non-empty range per tree seat
// (`ranges.size()` must equal `tree.def().player_count`), each combo a sorted
// distinct in-range pair with a finite strictly positive weight (normalized
// internally by the joint sampler), a postflop root (`board_size` in 3..5; the
// card abstraction is defined on a complete flop) OR a heads-up preflop root
// with `terminal == TerminalDepth::Flop` (RFC 0007; the frontier evaluator
// supplies leaf values and must be non-null), a positive iteration count, and
// the tree's identity action abstraction. Resource caps throw
// `nseat_training_exhausted`; the wall cap instead returns the last completed
// iteration counts with phase WallClock.
NSeatTrainingResult train_nseat(const tree::AbstractTree& tree,
                                const std::vector<std::vector<WeightedHand>>& ranges,
                                std::uint64_t iterations, std::uint64_t master_seed,
                                const NSeatTrainerLimits& limits,
                                const gto::FrontierEvaluator* frontier = nullptr);

// One sweep's RAW accumulators, exposed only through the test seam below so a
// gate can compare regrets and kFull sums against an independently coded
// enumeration of the update rule.
struct NSeatRawRow {
  std::vector<poker::Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
  std::uint64_t visits = 0;
};

// Test-only single-sweep seam (the stage-6 `debug_run_one_sweep` /
// HeadsUpSolverDebug precedent): runs ONE external-sampling traverser sweep
// with caller-owned streams over the REAL walk, menu rule, key addressing,
// row store and terminal settlement. `rows` is both the input table and the
// committed post-sweep state. Production training never exposes streams; this
// is the only production-visible test seam of this component.
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
