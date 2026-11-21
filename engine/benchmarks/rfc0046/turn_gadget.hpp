// RFC 0046 Stage-1 measurement-only turn-boundary resolving gadget.
//
// Generalizes the RFC 0005 terminal -x gadget to the full turn+river subtree:
// each non-hero seat (exactly one responder in the two-seat cells) gets one
// per-CONCRETE-COMBO -x infoset at the turn boundary, choosing TERMINATE at a
// locked margin b(c) or CONTINUE into the real cap-1 turn-rooted subtree. At
// two seats this is the exact Brown-Sandholm responder/safe-subgame
// construction. The regret solve is external-sampling MCCFR (one sampled
// blocker-aware joint deal per traverser per iteration, RM+ clipped once per
// iteration). Discount/weighting is deliberately split: the -x ROOT gadget
// infosets carry LINEAR CFR weight t (the RFC 0005 convention), while the
// subtree action infosets use vanilla RM+ with uniform iteration weight (the
// production n-seat trainer convention). The separate own-reach average
// traversal fans out both -x actions and ENUMERATES the fixed 48-card river
// chance (no sampling at the river, so every certifier-visited river row is
// converged); it accumulates concrete-keyed average strategies. All infoset
// keys come from ConcreteCardKeyer directly, never from a card bucket.
//
// Uniform-range precondition: the marginal/conditional sampling is unbiased
// only when combo weights are equal; run_turn_gadget rejects a weighted range.
//
// Benchmark-only: deleted after the gates under no-legacy.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/prng.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "concrete_keyer.hpp"

namespace bs::rfc0046 {

enum class GadgetStatus { Certified, SolveDeadline, ResourceExhausted, InvalidInput };

struct GadgetLimits {
  std::size_t max_nodes = 5'000'000;
  std::size_t max_information_sets = 1'000'000;
  std::size_t max_bytes = 1ull << 30;  // 1 GiB
  std::chrono::milliseconds wall{10'000};
};

// One served concrete-keyed subtree row.
struct GadgetRow {
  std::vector<poker::Action> actions;
  std::vector<double> probabilities;  // normalized average strategy
};

// Average {TERMINATE, CONTINUE} probability per responder concrete combo
// (dense ConcreteCards bucket on board4).
using MarginPolicy = std::map<std::uint32_t, std::array<double, 2>>;

struct GadgetResult {
  GadgetStatus status = GadgetStatus::InvalidInput;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;        // materialized tree nodes (tree.size())
  std::size_t walk_visits = 0;  // diagnostic: cumulative regret/average walk entries
  std::size_t information_sets = 0;
  std::size_t peak_bytes = 0;  // resident in-table byte estimate (charged at row creation), NOT RSS
  // Concrete-keyed average subtree policy: (concrete bucket, public node).
  std::map<bs::solver::NSeatInformationKey, GadgetRow> rows;
  // Per-combo -x average policy.
  MarginPolicy margin_policy;
};

// Locked per-combo margins indexed by responder ConcreteCards dense bucket.
using MarginTable = std::map<std::uint32_t, double>;

struct GadgetInput {
  const tree::AbstractTree* tree = nullptr;
  const ConcreteCardKeyer* keyer = nullptr;
  std::size_t hero = 0;       // served seat
  std::size_t responder = 1;  // the single non-hero seat at 2 seats
  // Boundary priors: weighted concrete combos per seat (board-blockers
  // removed). Joint deals are blocker-aware product-weighted pairs.
  std::vector<bs::solver::WeightedHand> hero_range;
  std::vector<bs::solver::WeightedHand> responder_range;
  MarginTable margins;  // b(c) per responder dense combo, chip units
  std::uint64_t seed = 0;
  std::uint64_t iterations = 0;
  GadgetLimits limits{};
};

// Runs the bounded augmented-game solve. A miss beyond nodes/infosets/bytes/
// wall returns the typed status with counts accumulated so far.
GadgetResult run_turn_gadget(const GadgetInput& input);

}  // namespace bs::rfc0046
