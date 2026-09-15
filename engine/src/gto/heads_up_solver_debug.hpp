// Solver-private debug surface for RFC 0004 Stage 3 native tests. This header
// is not part of the public solver facade; production code must not include
// it. The pinned SplitMix64 trainer and sampler are unchanged by these hooks:
// scripted runs only substitute the 64-bit entropy stream, and seeded runs
// only prescribe the starting per-information-set policy of a fresh table.
#pragma once

#include <array>
#include <bs/heads_up_solver.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace bs::solver {

class HeadsUpSolverDebug;

namespace detail {

// Passkey into solver internals. Its constructor is private and only
// HeadsUpTrainer (which hosts the pinned sampled driver) and the debug driver
// below are befriended, so no other translation unit can construct or use the
// token even though the public header forward-declares the class name.
class HeadsUpDebugKey {
  friend class ::bs::solver::HeadsUpTrainer;
  friend class ::bs::solver::HeadsUpSolverDebug;
  HeadsUpDebugKey() = default;

 public:
  // Policy internals used by the debug driver while publishing a prospective
  // table; the public facade exposes neither mutator. The token must be
  // presented, so mere physical access to this header is not sufficient.
  static void set_policy_game(HeadsUpPolicy& policy, const HeadsUpGame& game,
                              const HeadsUpDebugKey& token);
  static void add_policy_row(HeadsUpPolicy& policy, InformationKey key, PolicyRow row,
                             const HeadsUpDebugKey& token);
};

}  // namespace detail

struct DebugRow {
  std::vector<poker::Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;
};

struct DebugTrainingOutput {
  TrainingResult result;
  std::map<InformationKey, DebugRow> rows;
  // 64-bit entropy values consumed by scripted runs.
  std::size_t entropy_consumed = 0;
};

// Prescribed starting policy for a seeded scripted/full run. Each vector is
// indexed by abstract-action order at its information set, must be
// nonnegative and sum to one, and the test fixtures pin exact zero entries.
using PrescribedPolicy = std::map<InformationKey, std::vector<double>>;

class HeadsUpSolverDebug {
 public:
  // Pinned RFC 0004 revision-1 SplitMix64 draws, exposed for PRNG conformance.
  static std::vector<std::uint64_t> splitmix64(std::uint64_t seed, std::size_t count);
  // Pinned sampled trainer with its real SplitMix64 stream; returns raw
  // per-information-set regret and average-sum state.
  static DebugTrainingOutput train_sampled(const HeadsUpGame& game, std::uint64_t iterations,
                                           std::uint64_t seed, TrainingLimits limits = {});
  // RFC 0005 Stage 5 resume driver (debug/test support only): continues the
  // pinned sampled traversal from a checkpoint's raw table and the SplitMix64
  // state recorded after its last committed iteration, running
  // additional_iterations more. The traversal driver and updates are
  // unchanged; only the initial table is supplied. result.completed_iterations
  // starts from completed_iterations.
  static DebugTrainingOutput resume_sampled(const HeadsUpGame& game,
                                            std::uint64_t additional_iterations, std::uint64_t seed,
                                            std::uint64_t initial_prng_state,
                                            std::uint64_t completed_iterations,
                                            const std::map<InformationKey, DebugRow>& initial,
                                            TrainingLimits limits = {});
  // One fresh-table sampled episode for a single traverser, used to enumerate
  // exact one-sweep update expectations. Entropy must be exactly consumed.
  static DebugTrainingOutput run_episode_scripted(const HeadsUpGame& game,
                                                  std::vector<std::uint64_t> draws,
                                                  std::size_t traverser,
                                                  TrainingLimits limits = {});
  // One scripted sampled episode over a SEEDED table: each listed information
  // set starts from the prescribed policy vector instead of uniform regret
  // matching. Used to enumerate fixed-policy outcomes with exact zero-policy
  // actions. Entropy must be exactly consumed.
  static DebugTrainingOutput run_episode_scripted_seeded(const HeadsUpGame& game,
                                                         std::vector<std::uint64_t> draws,
                                                         std::size_t traverser,
                                                         PrescribedPolicy prescribed,
                                                         TrainingLimits limits = {});
  // Full traversal with raw regret and average-sum state exposed.
  static DebugTrainingOutput train_full(const HeadsUpGame& game, std::uint64_t iterations,
                                        TrainingLimits limits = {});
  // One full traverser sweep (traverser 0 or 1) from an empty table, exposing
  // raw regrets and sums for enumerated single-sweep expectations.
  static DebugTrainingOutput train_full_sweep(const HeadsUpGame& game, std::size_t traverser,
                                              TrainingLimits limits = {});
  // One full traverser sweep starting from the same prescribed policy as
  // run_episode_scripted_seeded, so the enumerated fixed-policy identity
  // compares identical frozen policies.
  static DebugTrainingOutput train_full_sweep_seeded(const HeadsUpGame& game, std::size_t traverser,
                                                     PrescribedPolicy prescribed,
                                                     TrainingLimits limits = {});
  // Runs the modeled information-set response recursion from an arbitrary
  // state, aggregating over every joint deal compatible with `own`, each at
  // its normalized joint probability. best selects the responder argmax;
  // false evaluates the profile mixture at the responder's own nodes.
  static double response_value(const HeadsUpGame& game, const HeadsUpPolicy& policy,
                               const poker::HeadsUpState& state, std::size_t player,
                               std::array<int, 2> own, bool best, TrainingLimits limits = {});
  // As response_value, but the walk starts from a descendant state with the
  // caller-supplied per-deal reach (e.g. reach already advanced through the
  // responder opponent's root policy action).
  static double response_value_with_reach(const HeadsUpGame& game, const HeadsUpPolicy& policy,
                                          const poker::HeadsUpState& state, std::size_t player,
                                          std::array<int, 2> own, bool best,
                                          std::vector<double> reach, TrainingLimits limits = {});

  // The single pinned sampled driver over any SplitMix64-compatible entropy
  // source; the public trainer shares this implementation. export_raw_rows
  // additionally copies the committed table into DebugRow storage and is used
  // only by debug entry points, so production training pays no export cost
  // and cannot report ResourceLimit from a debug-only export allocation.
  template <typename Rng>
  static DebugTrainingOutput run_sampled(const HeadsUpGame& game, std::uint64_t iterations,
                                         Rng& rng, std::uint64_t seed, TrainingLimits limits,
                                         bool export_raw_rows,
                                         const std::map<InformationKey, DebugRow>* resume = nullptr,
                                         std::uint64_t start_iterations = 0);
};

}  // namespace bs::solver
