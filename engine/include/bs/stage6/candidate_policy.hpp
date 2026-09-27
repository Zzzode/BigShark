// stage6/candidate_policy.hpp — the RFC 0008 stage 6 COMPOSED candidate.
//
// Charts preflop, frozen trained policy postflop:
//
//   * while fewer than three public cards are dealt, distribution() is exactly
//     the pinned chart decision (chart_preflop.hpp — the same path the
//     BaselineBehaviorPolicy runs), returned with probability one;
//   * once the flop exists the candidate binds the hand to its frozen
//     geometry bucket and plays the sealed average artifact row.
//
// Binding is stateless and deterministic from (state, log): the preflop root
// GameState is replayed through state.def() and the preflop log, then the three
// flop cards are dealt, yielding the exact flop GeometrySignature (the same
// value the simulator's GeometryCoverage checks). Postflop the candidate walks
// a reduced REPRESENTATIVE shadow game — the bucket's reconciled equal stacks
// and dead contributions with the real public cards — replaying observed
// actions mapped onto the representative coarse menu. Materialized (live==2)
// artifacts are addressed by cached AbstractTree node index; streaming
// (live>=3) artifacts by PublicPath hash.
//
// The sealed row is a distribution over representative-chip coarse actions.
// Those are projected onto the CURRENT concrete legal actions through the R7
// translator (translate_coarse_to_exact), which re-snaps aggressive totals to
// the real legal interval and merges/falls back declaratively. Pot-scale
// bucketing makes the representative a ≤1bb approximation of the member; that
// quantized delta is a declared, separately measured confound, never silently
// hidden.
//
// Lives in bigshark_stage6_eval (it links the chart policy and the translator).
// It never reads the deployed heuristic postflop.
#pragma once

#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::stage6 {

// One frozen artifact plus the exact reduced-representative inputs it was
// trained on. Supplied by the measurement driver after SHA-256 lock
// verification; the candidate never trains or loads artifacts itself.
struct CandidateBinding {
  GeometryBucket bucket;
  // The coarse action abstraction the artifact rows were trained under; the
  // candidate rebuilds streaming-mode menus from it.
  bs::abstraction::ActionAbstraction action = bs::abstraction::ActionAbstraction::declared(
      bs::abstraction::SizeSchedule(), bs::abstraction::CoverSeeds::DeclaredOnly);
  FrozenManifest manifest;
  FrozenArtifactRows rows;
};

// Typed refusal for a candidate lookup that cannot be satisfied. A missing
// binding, an identity mismatch, or a shadow state that cannot follow the
// observed action aborts the measurement — the candidate never clamps to the
// baseline, a different bucket, or a fabricated row.
class stage6_candidate_error : public std::runtime_error {
 public:
  explicit stage6_candidate_error(const std::string& what) : std::runtime_error(what) {}
};

// What the candidate does at a structurally-valid coarse node the sampled
// training never sealed a row for.
//
//   Throw: fail closed (the deployed default). A reached but unvisited
//     information set is an artifact-coverage error and aborts.
//
//   UniformOnUnvisited: return a uniform distribution over the coarse node
//     menu. This is NOT clamping away an error: it is exactly the value the
//     artifact format already assigns to a row that received zero average
//     contribution (TrainerRow::average_row / seal_rows freeze such rows to
//     uniform), applied to a structurally-valid coarse node no sampled sweep
//     happened to touch. It is used ONLY by the coarse-game NashConv
//     measurement, whose deviator explores the full coarse bush and therefore
//     necessarily reaches such nodes; the driver publishes the count so an
//     under-trained artifact cannot be mistaken for a covered one. A genuine
//     abstraction violation (no binding, wrong stamp, off-menu concrete
//     action, shadow desync) still throws regardless of this setting.
enum class CandidateMissPolicy { Throw, UniformOnUnvisited };

// How an observed CONCRETE aggressive action is mapped onto a coarse menu
// edge while replaying the public action line to the lookup node.
//
//   StrictCoarse (the deployed default): the exact bet/raise must be a coarse
//     menu entry of the SAME aggressive type (nearest target otherwise); a
//     size the trainer never built throws. This is the coarse-game lookup.
//
//   NearestCoarseEdge: the exact aggressive action maps to the nearest
//     aggressive coarse edge regardless of Bet/Raise type (within one street
//     the menu is uniformly one type; passives still require an exact entry).
//     Node addressing still advances with the matched COARSE action, so the
//     sealed distribution is unchanged — only which sealed row an off-menu
//     concrete line reads becomes defined. Used by the exact-game NashConv
//     measurement; an unseen off-coarse size otherwise has zero candidate
//     mass by construction.
enum class CandidateProjectionMode { StrictCoarse, NearestCoarseEdge };

class CandidateBehaviorPolicy final : public BehaviorPolicy {
 public:
  // `bindings` maps GeometryBucketKey (bucket.key.to_string()) to one frozen
  // artifact; the candidate copies the map and builds/caches the representative
  // tree for each live==2 binding. Throws stage6_candidate_error if a binding
  // is internally inconsistent (missing representative inputs, action
  // abstraction mismatch).
  explicit CandidateBehaviorPolicy(
      std::vector<CandidateBinding> bindings,
      CandidateMissPolicy miss_policy = CandidateMissPolicy::Throw,
      CandidateProjectionMode projection_mode = CandidateProjectionMode::StrictCoarse);
  ~CandidateBehaviorPolicy() override;

  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;

  std::size_t binding_count() const noexcept;

  // Number of structurally-valid coarse nodes answered with the uniform
  // fallback (UniformOnUnvisited mode only); zero under Throw and zero for a
  // fully sampled artifact.
  std::uint64_t unvisited_misses() const noexcept;

  // Number of decision points whose public line leaves the coarse tree
  // entirely (NearestCoarseEdge + UniformOnUnvisited only): an observed
  // aggression reaches a point where the reduced shadow chip game offers no
  // aggressive edge while the real game still does — coarse-edge overshoot
  // changed the commitment history. The candidate has no sealed node for such
  // a line and answers uniform over the EXACT declared menu; the driver
  // publishes the count so the off-tree proportion of an exact-game estimate
  // stays visible. Zero in every other mode.
  std::uint64_t off_tree_misses() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bs::stage6
