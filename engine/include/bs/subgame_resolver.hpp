// RFC 0011: turn/river subgame resolver.
//
// When a turn or river decision arises that the flop-terminal blueprint does
// not cover and the terminal-only resolver cannot handle (remaining opponent
// actions), this resolver:
//
//   1. Builds a River-terminal subtree rooted at the turn state. The subtree
//      materializes the remaining action streets and the river chance (44
//      cards for 2 seats, 42 for 3), which is bounded and fast.
//   2. Derives prior ranges from the flop-terminal blueprint by walking the
//      observed flop action line. For each seat, the per-hand reach is the
//      product of blueprint action probabilities along the path.
//   3. Solves the subtree with CFR (train_nseat). River leaves are valued by
//      the existing settle_showdown settlement (exact equity, no new
//      evaluator).
//   4. Serves the candidate at `approximate` (no certification in Phase 1).
//
// Supports 2..3 seats. For a river decision the resolver still builds a
// turn-rooted subgame (the blueprint is flop-terminal and has no turn rows),
// solves it once, then walks the solved tree — following the observed turn
// actions and the observed river card — to extract the candidate at the
// river action node.
//
// The resolver is offline (no wire, no credentials) and thread-affine (call
// from one thread). Folded seats are carried through the GameDef folded
// bitmap: the GameState constructor seeds players_[p].folded, and the
// existing fold-aware machinery (next_actor, refresh_live, settlement,
// trainer) handles them correctly. Folded seats keep their gross
// contributions as dead money and never become the actor.
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

// RFC 0011: turn/river subgame resolver. One bounded, offline, whole-range
// resolve. A SubgameResolver is reusable and thread-affine.
class SubgameResolver {
 public:
  SubgameResolver();
  ~SubgameResolver();

  // Resolve the turn/river decision at `node` (whose actor is the hero).
  // The blueprint supplies the game identity, declared ranges, and the flop
  // policy for prior range derivation. `history` is the observed
  // public-action path from the blueprint's flop root to `node`.
  //
  // For a turn decision (board_size=4) the subgame is rooted at the turn and
  // the candidate is extracted at the root. For a river decision
  // (board_size=5) the subgame is still rooted at the turn (the blueprint is
  // flop-terminal and has no turn rows); the solver walks the solved tree
  // along the observed turn actions and river card to extract the candidate
  // at the river action node.
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
  // Build the River-terminal GameDef for the subgame rooted at the turn state.
  // The subgame is a standalone game: stacks and pot come from the current
  // state, the board carries the observed turn card, and the terminal depth
  // is River so the builder materializes the full chance subtree. The button
  // is set so `hero` (the decision node's actor) is the first to act
  // postflop. The hero is passed explicitly because the turn_root cursor is
  // in Frontier phase (the blueprint is flop-terminal), so its actor() is
  // nullopt.
  static poker::GameDef build_subgame_def(const poker::GameState& turn_root, std::size_t hero,
                                          const poker::GameDef& blueprint_def);

  // Reconstruct the ActionAbstraction from the blueprint's action_id and
  // sizes so the subgame tree uses the same menu as the blueprint. A
  // declared-coarse id rebuilds the DeclaredOnly menu; any other id rebuilds
  // the identity (MinAndCap) menu.
  static abstraction::ActionAbstraction reconstruct_action_abstraction(
      const abstraction::AbstractionId& action_id, const abstraction::SizeSchedule& sizes);

  // Derive prior ranges at the turn root by walking the flop action line in
  // the blueprint policy. For each seat, the per-hand reach is the product
  // of blueprint action probabilities along the observed flop path. The
  // resulting per-seat marginal ranges are the prior ranges at the turn
  // root. Also outputs the turn-root GameState (the cursor after replaying
  // the flop line and dealing the turn card) so the caller can build the
  // subgame def from it.
  //
  // A missing blueprint row at any step sets `status` to CoverageMiss and
  // returns an empty vector. The caller never invents a uniform policy.
  std::vector<std::vector<solver::WeightedHand>> derive_prior_ranges(
      const poker::GameState& flop_root, std::span<const poker::PublicAction> flop_history,
      const BlueprintSource& blueprint, const poker::GameState& subgame_root,
      poker::GameState& turn_root_out, SubgameResolveStatus& status) const;

  // Walk the solved tree from the turn root, following the observed turn
  // actions and the observed river card, to reach the river action node.
  // Returns the river node's index, or -1 on any mismatch (action not in
  // tree menu, wrong phase, chance ordinal out of range). The cursor is
  // constructed from `subgame_def` (River-terminal, so it opens in Action
  // phase) rather than from the blueprint-derived turn root (which is
  // Frontier because the blueprint is flop-terminal).
  static std::size_t walk_to_river_node(const tree::AbstractTree& tree, std::size_t root_index,
                                        std::span<const poker::PublicAction> turn_history,
                                        int river_card, const poker::GameDef& subgame_def);
};

}  // namespace bs::resolver
