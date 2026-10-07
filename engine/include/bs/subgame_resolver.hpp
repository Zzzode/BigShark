// RFC 0011 Phase 1: turn-rooted subgame resolver.
//
// When a turn or river decision arises that the flop-terminal blueprint does
// not cover and the terminal-only resolver cannot handle (remaining opponent
// actions), this resolver:
//
//   1. Builds a River-terminal subtree rooted at the current turn/river state.
//      The subtree materializes the remaining action streets and the river
//      chance (44 cards for 2 seats), which is bounded and fast.
//   2. Derives prior ranges from the flop-terminal blueprint by walking the
//      observed flop action line. For each seat, the per-hand reach is the
//      product of blueprint action probabilities along the path.
//   3. Solves the subtree with CFR (train_nseat). River leaves are valued by
//      the existing settle_showdown settlement (exact equity, no new
//      evaluator).
//   4. Serves the candidate at `approximate` (no certification in Phase 1).
//
// Phase 1 is 2-seat only. A 3-seat turn/river decision is Ineligible until
// the N-way continuation-range export lands. The resolver is offline (no
// wire, no credentials) and thread-affine (call from one thread).
//
// The fallback chain is: blueprint -> terminal-only resolver -> subgame
// resolver -> operational fallback. A build failure, solve deadline, or
// coverage miss returns the typed status so the engine can fall through.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // InformationKey, PolicyRow, WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/resolver.hpp>  // BlueprintSource, ResolveLimits
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace bs::resolver {

// Status of a subgame resolve attempt (RFC 0011 Phase 1).
enum class SubgameResolveStatus {
  Solved,         // candidate produced
  Ineligible,     // node/root is not a supported subgame resolve
  SolveDeadline,  // wall time or iteration cap exhausted
  CoverageMiss,   // blueprint row missing for range derivation
  InvalidInput,   // malformed game, ranges, or identity
};

const char* to_string(SubgameResolveStatus status) noexcept;

// Result of one subgame resolve (RFC 0011 Phase 1). Phase 1 has no
// certification: all candidates serve at `approximate` with the SUBGAME
// source. A solve deadline or build failure falls back to the operational
// fallback; the engine never publishes an untrained candidate.
struct SubgameResolveResult {
  SubgameResolveStatus status = SubgameResolveStatus::Ineligible;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;
  std::size_t information_sets = 0;
  double root_pot = 0;
  std::size_t seat_count = 0;
  // Whole-range candidate at the subgame root (the current turn/river node),
  // keyed by the normal solver information_key(state, hero_cards). Every
  // positive-reach hero combo appears; probabilities are over the node's
  // ordered abstract actions.
  std::map<solver::InformationKey, solver::PolicyRow> candidate;
  // Ordered node abstract actions shared by every candidate row.
  std::vector<poker::Action> node_actions;
};

// RFC 0011 Phase 1: turn-rooted subgame resolver. One bounded, offline,
// whole-range resolve. A SubgameResolver is reusable and thread-affine.
class SubgameResolver {
 public:
  SubgameResolver();
  ~SubgameResolver();

  // Resolve the turn/river decision at `node` (whose actor is the hero).
  // The blueprint supplies the game identity, declared ranges, and the flop
  // policy for prior range derivation. `history` is the observed
  // public-action path from the blueprint's flop root to `node`.
  //
  // The resolver constructs, solves, and selects a single candidate for the
  // COMPLETE hero range before looking up the actual hero combination. Solve
  // inputs, seed, and iteration cap are independent of the hero's actual
  // cards, matching the terminal-only resolver's private-independent
  // contract.
  SubgameResolveResult resolve(const poker::GameState& node,
                               std::span<const poker::PublicAction> history,
                               const BlueprintSource& blueprint, const ResolveLimits& limits) const;

 private:
  // Build the River-terminal GameDef for the subgame rooted at `node`.
  // The subgame is a standalone game: stacks and pot come from the current
  // state, the board carries the observed turn/river cards, and the terminal
  // depth is River so the builder materializes the full chance subtree.
  static poker::GameDef build_subgame_def(const poker::GameState& node,
                                          const poker::GameDef& blueprint_def);

  // Reconstruct the ActionAbstraction from the blueprint's action_id and
  // sizes so the subgame tree uses the same menu as the blueprint. A
  // declared-coarse id rebuilds the DeclaredOnly menu; any other id rebuilds
  // the identity (MinAndCap) menu.
  static abstraction::ActionAbstraction reconstruct_action_abstraction(
      const abstraction::AbstractionId& action_id, const abstraction::SizeSchedule& sizes);

  // Derive prior ranges at the subgame root by walking the flop action line
  // in the blueprint policy. For each seat, the per-hand reach is the
  // product of blueprint action probabilities along the observed flop path.
  // The resulting per-seat marginal ranges are the prior ranges at the
  // subgame root.
  //
  // A missing blueprint row at any step sets `status` to CoverageMiss and
  // returns an empty vector. The caller never invents a uniform policy.
  std::vector<std::vector<solver::WeightedHand>> derive_prior_ranges(
      const poker::GameState& flop_root, std::span<const poker::PublicAction> flop_history,
      const BlueprintSource& blueprint, const poker::GameState& subgame_root,
      SubgameResolveStatus& status) const;
};

}  // namespace bs::resolver
