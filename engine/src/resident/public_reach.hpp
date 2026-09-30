// Hero-card-independent public belief propagation (RFC 0005 Stage 6).
//
// Starting from the artifact's declared pair of weighted ranges, the model
// replays the observed public path to a query node:
//   - observed action by actor a: each live a-combo reach is multiplied by
//     that node's policy probability for the action (kind plus exact target);
//   - public card: combinations sharing the card are removed for both
//     players, which also removes every joint deal sharing that card, and the
//     legal joint distribution is renormalized.
// The output is the normalized joint distribution over card-compatible deals,
// exposed as per-player per-combo marginals. The model never takes a hero
// hole-card argument; the hero-private blocker filter is a separate step.
#pragma once

#include <array>
#include <bs/range.hpp>
#include <bs/resident_policy.hpp>
#include <bs/unified_game.hpp>
#include <cstddef>

namespace bs::resident {

class ReachModel {
 public:
  explicit ReachModel(ResidentScratch& scratch);

  // Install the declared range weights (bs::comboIndex indexed), remove
  // combinations blocked by the root flop, and normalize the legal joint
  // distribution to mass one. Returns false when no positive
  // card-compatible joint deal exists; the scratch is then fail-closed.
  // W2c-ii-b: the belief model is exact for two and three seats. A 4..10-seat
  // game loads and advertises, but its belief cannot be conditioned exactly
  // (the per-node marginal is a declared coverage limitation), so it returns
  // false and the caller misses declared.
  bool initialize(const solver::UnifiedGame& game);

  // Remove every combination holding the dealt public card and renormalize.
  // Returns false when the conditioned joint mass is zero.
  bool observe_card(int card);

  // Multiply the actor's per-combo reach by the observed action probability
  // and renormalize. Zero-probability entries remove that combination.
  // Returns false when the conditioned joint mass is zero (the observed
  // action has zero probability for every live deal).
  bool observe_action(std::size_t actor, const std::array<double, bs::N_COMBOS>& probability);

  // W2c-ii-b: cache the partner mass J_{-seat}(combo) of every combo of
  // `seat` so has_positive_partner answers in O(1) on the three-seat path.
  // The two-seat path answers in O(1) per combo directly, so this is a no-op
  // there. Must be called after the latest mutation and before the matching
  // has_positive_partner calls for a three-seat seat.
  void prepare_partner_mass(std::size_t seat);

  // Whether `combo` of `player` currently has at least one opponent
  // combination with positive conditioned joint mass (an opponent combo that
  // shares neither card). An orphan combination (positive raw reach but no
  // positive compatible partner) never has a row in a complete artifact.
  bool has_positive_partner(std::size_t player, int combo) const;

  // Normalized per-player marginals, each summing to one. Returns false and
  // leaves the marginal buffers unchanged when the joint mass is not finite
  // and positive, so the caller can fail closed instead of dividing by zero.
  bool write_marginals();

  // Copy the opponent marginal and remove combinations holding a hero (or
  // board) card. Sets fully_blocked when no opponent combination survives.
  void opponent_private_view(std::size_t opponent, std::array<int, 2> hero_cards,
                             bool& fully_blocked);

 private:
  // Recompute per-card summed mass and the card-compatible joint mass.
  double recompute_joint_mass();
  bool renormalize();
  // W2c-ii-b three-seat path.
  void rebuild_totals();
  double compatible_mass(std::size_t seat, const std::array<int, 4>& blocked,
                         std::size_t n_blocked) const;
  double recompute_joint_mass_three();
  void prepare_partner_mass_three(std::size_t seat);
  bool write_marginals_three();

  ResidentScratch& scratch_;
  std::size_t seat_count_ = 0;
  // Per-seat summed raw mass, rebuilt by rebuild_totals (three-seat path).
  std::array<double, poker::kMaxUnifiedSeats> total_{};
};

// Exact root joint mass: the sum of declared range weights over assignments of
// one combination per seat that share no card with each other or the ordered
// root flop. Exact for every seat count by backtracking (most-constrained seat
// first); per-player max normalization is deliberately not applied because the
// only property tested here, positivity, is invariant under positive rescaling.
double root_joint_mass(const solver::UnifiedGame& game);

}  // namespace bs::resident
