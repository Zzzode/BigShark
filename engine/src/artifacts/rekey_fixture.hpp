// RFC 0038 test/benchmark-only rekey helper. NOT a production artifact path:
// production training publishes schema-v2/v3 directly through train_nseat and
// export_seat_policy. This header exists because the resident fixture, the
// resident test scenarios, and the resident-lookup benchmark train on FIXED
// RUNOUTS through the in-memory heads-up full traversal (HeadsUpSolverDebug),
// which the chance-sampled seat trainer cannot reproduce (it emits only
// visited rows and cannot full-chance materialize a fixed board tree).
//
// The helper keeps that in-memory training and transfers the trained rows
// into a flop-rooted, river-terminal schema-v2 SeatTrainingResult through
// SeatPolicyAssembler, on the byte-identical heads-up/seat information keys
// (proven by test_two_seat_byte_identity). The result carries a genuine
// revision-two identity (batch 1). The artifact is read back by the normal
// schema-v2 reader and served through the single resident game path.
#pragma once

#include <bs/abstraction.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>

#include "artifact_internal.hpp"

namespace bs::artifacts::detail {

// Test/benchmark-only construction of an in-memory HeadsUpPolicy view. After
// RFC 0038 no artifact writer builds a HeadsUpPolicy, but the fixed-runout
// resident tests verify the resident against an in-process oracle; this access
// produces that oracle policy (identical rows, with any exact overrides
// applied). It never touches persistence.
class RekeyOracleAccess {
 public:
  static solver::HeadsUpPolicy with_overrides(
      const solver::HeadsUpPolicy& base,
      const std::map<solver::InformationKey, std::vector<double>>& overrides) {
    solver::HeadsUpPolicy out = base;  // value copy (game + rows)
    for (const auto& [key, probs] : overrides) {
      const auto it = out.rows_.find(key);
      if (it == out.rows_.end())
        throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                            "oracle override key absent from trained policy");
      if (probs.size() != it->second.actions.size())
        throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                            "oracle override action count mismatch");
      it->second.probabilities = probs;
    }
    return out;
  }
};

struct RekeyInput {
  const solver::HeadsUpGame& game;
  const solver::TrainingResult& result;  // trained in memory (fixed runout)
  // Optional exact published-probability overrides keyed by information key;
  // an override for a key absent from the trained policy is an error.
  const std::map<solver::InformationKey, std::vector<double>>* overrides = nullptr;
  // When set, only keys for which the predicate is true are transferred, so
  // the caller can publish an intentionally partial (root-only) artifact.
  bool (*include_key)(const solver::InformationKey&) = nullptr;
};

// Build the seat-generic, revision-two result that re-publishes an in-memory
// heads-up fixed-runout policy. Schema-v2 checkpoints store no raw training
// table, so only the immutable policy rows are transferred.
inline solver::SeatTrainingResult rekey_heads_up_result(const RekeyInput& in) {
  const solver::UnifiedGame unified = solver::to_unified_game(in.game);
  // unified.ranges is a fixed 10-seat array; the seat writer takes a vector
  // sized to player_count. Copy the two occupied seats.
  std::vector<std::vector<solver::WeightedHand>> seat_ranges;
  seat_ranges.reserve(unified.def.player_count);
  for (std::size_t seat = 0; seat < unified.def.player_count; ++seat)
    seat_ranges.push_back(unified.ranges[seat]);
  // The heads-up fixtures use the default sizing schedule; identity() with
  // cap 0 (uncapped) reproduces that menu exactly, so served menus do not
  // narrow.
  const abstraction::AbstractionId action_id = abstraction::identity_action_id();

  solver::SeatTrainingResult out;
  out.termination = solver::NSeatTerminationPhase::Complete;
  out.completed_iterations = in.result.completed_iterations;
  out.seed = in.result.seed;
  out.prng_state = in.result.prng_state;
  out.nodes = in.result.nodes;
  out.algorithm_revision = 2;
  out.batch_size = 1;
  // batch 1 -> one epoch per sample, so epochs equals iterations.
  out.completed_epochs = in.result.completed_iterations;
  out.action_id = action_id;
  out.terminal_depth = poker::TerminalDepth::River;

  SeatPolicyAssembler::set_identity(out.policy, unified.def, unified.sizes, seat_ranges, action_id,
                                    std::nullopt, unified.fixed_runout);
  for (const auto& [key, row] : in.result.policy.rows()) {
    if (in.include_key && !in.include_key(key))
      continue;
    std::vector<double> probabilities = row.probabilities;
    if (in.overrides) {
      if (const auto it = in.overrides->find(key); it != in.overrides->end()) {
        if (it->second.size() != row.actions.size())
          throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                              "rekey override action count mismatch");
        probabilities = it->second;
      }
    }
    SeatPolicyAssembler::add_row(out.policy, key,
                                 solver::SeatPolicyRow{row.actions, probabilities, 0});
  }
  if (in.overrides) {
    for (const auto& [key, probs] : *in.overrides) {
      const auto it = in.result.policy.rows().find(key);
      if (it == in.result.policy.rows().end())
        throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                            "rekey override key absent from trained policy");
      if (in.include_key && !in.include_key(key))
        throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                            "rekey override key excluded by partial predicate");
      (void)probs;
    }
  }
  out.information_sets = out.policy.rows().size();
  return out;
}

// Full-traversal provenance naming the publishing program; an empty algorithm
// string makes the writer derive the revision-two (batch-1) identity.
inline SeatCheckpointProvenance rekey_provenance(const std::string& engine_revision) {
  SeatCheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = engine_revision;
  return provenance;
}

}  // namespace bs::artifacts::detail
