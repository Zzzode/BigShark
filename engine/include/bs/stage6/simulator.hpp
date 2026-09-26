// stage6/simulator.hpp — RFC 0008 stage 6 full-hand N-seat simulator.
//
// Drives ONE no-limit hold'em hand from a preflop GameDef to an exact terminal
// settlement, querying one BehaviorPolicy per seat at every action. It owns no
// strategy: the per-seat policies (pinned baseline, fixed uniform, or the
// composed candidate) supply a concrete action distribution, and the caller
// supplies the joint hole-card deal and the five board cards (the R5 dealer
// samples both conditioned on zero duplication). Runout cards are therefore
// fixed BEFORE the first action, matching the game's chance distribution: all
// 2N hole cards and the board are removed from the deck at deal time.
//
// Terminal payoffs come from the exact L1 GameState settle_fold /
// settle_showdown paths (contribution-layer settlement, exact side pots); the
// simulator never scores or awards chips itself. It asserts zero-sum chip
// conservation at every terminal and treats a policy that emits an illegal
// action as a typed harness failure (never a clamp).
//
// Offline-only: this lives in bigshark_stage6_eval and is never linked by the
// decision service or either protocol.
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/geometry.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace bs::stage6 {

// A simulator harness failure, distinct from a legal poker outcome: a policy
// returned a non-distribution or an illegal action, the predetermined board
// was inconsistent, or settlement violated conservation. These abort the
// measurement; they are never folded into a payoff.
class stage6_sim_error : public std::runtime_error {
 public:
  explicit stage6_sim_error(const std::string& what) : std::runtime_error(what) {}
};

// The terminal result of one simulated hand.
struct SimResult {
  // Exact chip utility (awards + refunds - gross contributed) per seat, in
  // seat order; zero-sum across seats.
  std::array<double, 10> utility{};
  // True when the hand ended by folds to one live player; false on showdown.
  bool folded = false;
};

// Runs full hands for 2..10 seats. `policies` has one entry per seat and must
// outlive the simulator. `coverage`, when non-null, is the frozen candidate
// geometry matrix: the first time a hand reaches a three-card flop with at
// least two live seats, its exact signature MUST be covered or the run throws
// stage6_geometry_uncovered. It is null for reference profiles (uniform) that
// need no candidate artifact.
class HandSimulator {
 public:
  HandSimulator(std::vector<const BehaviorPolicy*> policies, const GeometryCoverage* coverage,
                bs::SplitMix64& rng);

  // Plays one hand. `def` must be a valid preflop GameDef for policy_count()
  // seats; `holes` gives one pair per seat; `board` gives the five public cards
  // in deal order (flop, turn, river), mutually distinct and disjoint from
  // every hole card. `hand_id` is the deterministic, caller-assigned hand
  // index used (with the per-hand decision counter) to build each policy's
  // decision seed, distinct from the sampling RNG stream. Throws
  // stage6_sim_error on any harness inconsistency and
  // stage6_geometry_uncovered on an uncovered candidate flop.
  SimResult run(const poker::GameDef& def, const std::vector<HoleCards>& holes,
                const std::array<int, 5>& board, std::uint64_t hand_id = 0);

  std::size_t policy_count() const noexcept { return policies_.size(); }

 private:
  std::vector<const BehaviorPolicy*> policies_;
  const GeometryCoverage* coverage_;
  bs::SplitMix64& rng_;
};

}  // namespace bs::stage6
