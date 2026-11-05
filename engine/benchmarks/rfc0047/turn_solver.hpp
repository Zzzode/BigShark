// RFC 0047 S2/S3 two-street depth-limited turn solver (measurement-only).
//
// Range-vectorized two-seat CFR over the turn-rooted cap-1 public tree whose
// post-chance river frontier is terminated by externally supplied per-leaf
// continuation matrices (the five-card river deal-value matrices produced by
// the deterministic river kernel, optionally perturbed for G3). Turn combos
// are C(48,2)=1,128 on the four-card board; at each of the 48 river chance
// children a turn combo that does not block the dealt card maps through
// holding_for_dense/dense_pair_index to its 1,081 five-card dense index and
// indexes the leaf matrix. Chance is uniform 1/44 over the 44 river cards
// legal for a concrete joint deal (52 minus the 4 board and 4 hole cards;
// blocked cards contribute 0 through the combo mask), so values are exact
// per-deal expectations with no public-card rescale.
//
// This is the online turn-solve core reused by S3 and the G3 perturbation
// harness; it solves ONLY the turn street (river action is folded into the
// supplied leaf matrices). Benchmark-only under the rfc0047 link guard.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "strata.hpp"

namespace bs::rfc0047 {

inline constexpr std::uint32_t kTurnC = 1128;       // C(48,2) combos on 4-card board
inline constexpr std::uint32_t kRiverC = 1081;      // C(47,2) combos on 5-card board
inline constexpr std::uint32_t kTurnCompat = 1035;  // C(46,2): opponent combos
inline constexpr std::uint32_t kRiverCards = 48;

// One post-chance frontier leaf: the direct child of a turn chance node.
// `child_node` indexes the turn AbstractTree node where river action begins;
// `matrix` is a 1081*1081 row-major seat-0 deal-value table for that five-card
// board/ledger. G3 perturbs the table contents in place between solves.
struct TurnLeafValue {
  std::size_t child_node = 0;
  std::array<int, 5> board5{};
  const double* matrix = nullptr;
};

// Per-leaf counterfactual reach exported after the turn equilibrium solve;
// rho[seat] is indexed by five-card dense combo (1081) for that leaf's board.
struct TurnLeafReach {
  std::size_t child_node = 0;
  std::array<std::vector<double>, 2> rho;
};

struct TurnSolveConfig {
  bs::poker::GameDef def;  // four-card turn root, terminal=River, cap 1
  std::uint64_t iterations = 0;
  bool linear_averaging = true;
  std::span<const TurnLeafValue> leaves;
  // Iteration checkpoints at which to record turn NashConv (BB).
  std::span<const std::uint64_t> checkpoints;
  // Worker threads for the 48-child river chance GEMVs; 0 = hardware count.
  std::size_t threads = 0;
  // RM+ (clip negative regrets each sweep, default) vs vanilla regret
  // matching with linear averaging. The depth-limited turn game is heavily
  // mixed (matrix-valued leaves); vanilla has a faster mixed-strategy tail.
  bool rm_plus = true;
};

struct TurnSolveResult {
  std::uint64_t iterations = 0;
  std::size_t turn_action_nodes = 0;
  double nash_conv_chips = 0.0;
  double nash_conv_bb = 0.0;
  std::vector<double> history_nash_conv_bb;  // one per checkpoint
  // Average action strategy per TURN action node only (vectors empty
  // elsewhere): turn combos x menu probabilities.
  std::vector<std::vector<double>> sigma;
  std::size_t tree_size = 0;
  // One entry per config leaf (same order): per-five-card-combo counterfactual
  // reach of seat p at that post-chance leaf under the average strategy,
  // rho(p,i5) = P_p(ti) P_q(tj-compatible-sum) / 44, summed over the live
  // turn combos mapping to i5 and divided by kTurnC*kTurnCompat so that the
  // sum over leaves and combos is the per-joint-deal river-arrival probability
  // (<= 1; turn folds carry no leaf mass). This is the frozen G3 e_l2 weight.
  std::vector<TurnLeafReach> leaf_reaches;
};

// Independent telescoping exact-best-response hybrid NashConv of a FIXED turn
// average strategy (node_sigma, as returned in TurnSolveResult::sigma) against
// the supplied (typically true, unperturbed) leaf matrices:
//   turn_gap  = sum over turn action infosets I and own combos c of
//               pi_prefix(c) (max_a PV_BR(a;c) - sum_a sigma(a;c) PV_BR(a;c))
//               / (kTurnC*kTurnCompat), in chips;
//   river_term = reach-weighted per-leaf river subgame NashConv
//               (leaf_river_nc_chips, per joint deal, summed over both seats),
//               the G3 t_river term.
// Reimplemented independently of the TurnGame CFR/evaluate path (its own tree
// walk, matrix folds, and dense mappings) so the zero-perturbation identity
// turn_gap == TurnSolveResult::nash_conv_chips is a real cross-check.
struct HybridNashConv {
  double turn_gap_chips = 0.0;
  double river_term_chips = 0.0;
  double turn_gap_bb = 0.0;
  double river_term_bb = 0.0;
};

HybridNashConv turn_hybrid_nash_conv(const TurnSolveConfig& cfg,
                                     std::span<const std::vector<double>> node_sigma,
                                     std::span<const double> leaf_river_nc_chips);

TurnSolveResult solve_turn_subgame(const TurnSolveConfig& cfg);

}  // namespace bs::rfc0047
