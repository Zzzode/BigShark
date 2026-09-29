// RFC 0008 §L4: the unified solver interface.
//
//   SolveResult solve(const SolveRequest&);
//
// Every solver is reached through solve() and either returns its policy or
// throws a typed unsupported_tree_shape rather than approximating a model it
// does not solve. This header carries the solver seam only; the normative
// source->guarantee-level mapping is L6 (RFC 0008 stage 5), so NO closed
// Guarantee enum lives here -- a SolveResult reports solver status/cost and the
// abstraction identity, never a self-assigned guarantee level.
//
// Stage 4 routes the heads-up multistreet CFR behind solve() with ZERO numeric
// change (the unchanged HeadsUpTrainer is the numeric core), and registers the
// river LP/DCFR and experimental multistreet solvers as reachable refuse-only
// adapters. RFC 0009 W2a adds the seat-parameterized MCCFR trainer
// (SolverKind::NSeatCfr) that consumes the materialized tree directly for
// 3..10-seat identity trees, without moving the two-seat route: an Auto
// request on a two-seat identity tree still reaches HeadsUpCfr bit-for-bit.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::solver {

using poker::Chips;

// A solver was asked to solve an abstract tree shape it does not support.
// Deliberately does NOT derive from std::invalid_argument: an expected
// solver-selection refusal must not be caught by the host's malformed-request
// handlers and misreported as a bad protocol request.
class unsupported_tree_shape : public std::runtime_error {
 public:
  explicit unsupported_tree_shape(const std::string& what) : std::runtime_error(what) {}
};

// Full traversal (deterministic) vs external-sampling MCCFR (seeded). The two
// drivers produce numerically distinct results and are both conformance-gated.
enum class SolveMode { FullTraversal, ExternalSampling };

// Per-request resource bounds, mapped onto the trainer's TrainingLimits.
struct SolveLimits {
  std::size_t max_nodes = 1'000'000;
  std::size_t max_information_sets = 1'000'000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;
  std::chrono::milliseconds time{60'000};

  TrainingLimits training_limits() const {
    TrainingLimits l;
    l.max_nodes = max_nodes;
    l.max_information_sets = max_information_sets;
    l.max_depth = max_depth;
    l.max_bytes = max_bytes;
    l.time = time;
    return l;
  }

  // The n-seat trainer's limits. The tree build consumes max_nodes (the
  // materialized-tree cap) and max_depth; the sampled walk is bounded by the
  // wall clock (its visit cap stays at NSeatTrainerLimits' default, because a
  // visit is not a retained byte or a materialized node -- a caller that wants
  // a finite visit bound makes an NSeatTrainerLimits directly).
  NSeatTrainerLimits nseat_limits() const {
    NSeatTrainerLimits l;
    l.max_nodes = max_nodes;
    l.max_information_sets = max_information_sets;
    l.max_depth = max_depth;
    l.max_bytes = max_bytes;
    l.wall = time;
    return l;
  }
};

// Fixed turn/river conditioning, the owner-composed slot that is NOT GameDef
// root identity (same ownership split as HeadsUpGame.fixed_runout). nullopt
// entries are the free runout; a present card collapses that street's chance to
// that card (and reserves it on earlier streets), exactly as the shipped
// trainer's public_cards() does.
struct RunoutConditioning {
  std::optional<int> fixed_turn;
  std::optional<int> fixed_river;
};

// One weighted private holding per SEAT. Joint mutually-compatible deal
// enumeration and normalization are owned by the solver (L4), not the tree
// (L3 is hole-card-free). At two seats this maps to HeadsUpGame.ranges.
using SeatRanges = std::vector<WeightedHand>;

// Which solver to address. Auto routes to the solver whose model the tree IS:
// the heads-up multistreet CFR for a two-seat identity tree, and (RFC 0009
// W2a) the seat-parameterized MCCFR trainer for a 3..10-seat identity tree.
// The other RFC-named solvers are explicitly selectable so they are REACHABLE
// through solve(); their models are not L1 identity trees this stage, so each
// throws unsupported_tree_shape rather than being approximated. The RFC 0005
// resolver is a separate interface (different request shape) and is intentionally
// absent here.
//
// NSeatCfr is explicitly selectable on a TWO-seat tree as well (conformance
// fixtures compare the two routes on one game); it reports its own algorithm
// revision (NSeatTrainingResult::algorithm_revision) and never pretends to be
// the HeadsUpCfr stream. HeadsUpCfr on a 3+-seat tree keeps its typed refusal.
enum class SolverKind {
  Auto,
  HeadsUpCfr,
  NSeatCfr,
  RiverLp,
  RiverDcfr,
  MultistreetCfr,
};

struct SolveRequest {
  const tree::AbstractTree* tree = nullptr;
  SolverKind solver = SolverKind::Auto;
  // Indexed by seat; must match tree.def().player_count.
  std::vector<SeatRanges> ranges;
  SolveMode mode = SolveMode::FullTraversal;
  std::uint64_t iterations = 0;
  std::uint64_t seed = 0;  // ExternalSampling only.
  SolveLimits limits{};
  RunoutConditioning runout{};
};

// Move-only result. A DISCRIMINATED wrap: exactly one of the two concrete
// results is present, selected by which solver solve() routed to. The
// heads-up discriminant is byte-identical to the pre-W2a single-member wrap
// (the heads-up accessors still return the same TrainingResult by value), so
// the two-seat path's consumers and conformance pins do not move; the
// n-seat arm carries the seat-generic NSeatTrainingResult (RFC 0009 D2 chose
// this minimal change over widening TrainingResult, which the artifact
// boundary and every existing consumer key on). `training()` asserts its
// discriminant and throws std::runtime_error on misuse, so a caller that
// ignores the discriminant fails loudly rather than reading a default row.
// The result carries the abstraction identity so L5 can later key on it; it
// never carries a self-assigned guarantee level.
class SolveResult {
 public:
  explicit SolveResult(TrainingResult result, const abstraction::AbstractionId& action_id)
      : result_(std::move(result)), action_id_(action_id), is_heads_up_(true) {}
  explicit SolveResult(NSeatTrainingResult result, const abstraction::AbstractionId& action_id)
      : nseat_(std::move(result)), action_id_(action_id), is_heads_up_(false) {}

  SolveResult(const SolveResult&) = delete;
  SolveResult& operator=(const SolveResult&) = delete;
  SolveResult(SolveResult&&) noexcept = default;
  SolveResult& operator=(SolveResult&&) noexcept = default;

  // True iff this result came from the heads-up route (SolverKind::HeadsUpCfr
  // or the 2-seat Auto route).
  bool is_heads_up() const noexcept { return is_heads_up_; }

  // Heads-up arm. Throws std::runtime_error when the result is n-seat.
  const TrainingResult& training() const {
    require_heads_up();
    return result_;
  }
  TrainingResult take_training() {
    require_heads_up();
    return std::move(result_);
  }

  // N-seat arm. Throws std::runtime_error when the result is heads-up.
  const NSeatTrainingResult& nseat() const {
    require_nseat();
    return nseat_;
  }
  NSeatTrainingResult take_nseat() {
    require_nseat();
    return std::move(nseat_);
  }

  const abstraction::AbstractionId& action_id() const noexcept { return action_id_; }

 private:
  void require_heads_up() const {
    if (!is_heads_up_)
      throw std::runtime_error("SolveResult holds an n-seat result, not a heads-up one");
  }
  void require_nseat() const {
    if (is_heads_up_)
      throw std::runtime_error("SolveResult holds a heads-up result, not an n-seat one");
  }

  TrainingResult result_{};
  NSeatTrainingResult nseat_{};
  abstraction::AbstractionId action_id_ = abstraction::identity_action_id();
  bool is_heads_up_ = true;
};

// The unified entry point. Selects the solver by the tree's shape and runs it;
// throws unsupported_tree_shape if no registered solver accepts the request.
SolveResult solve(const SolveRequest& request);

}  // namespace bs::solver
