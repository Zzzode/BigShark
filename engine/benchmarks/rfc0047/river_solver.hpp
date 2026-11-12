// RFC 0047 S2 deterministic full-deal river solver (the V3 kernel).
//
// Solves a ZERO-sum two-player RIVER-ROOTED subgame with a fixed five-card
// board and uniform per-seat ranges, EXACTLY and SEEDLESSLY: every CFR
// traversal enumerates every compatible joint deal (no Monte Carlo card or
// deal sampling), so the result is bitwise reproducible and converges under a
// deterministic (not N^-0.5 sampled) law. Betting actions are public; each
// action node carries one regret/average vector per concrete own combo over
// the C(47,2)=1081 hands live on the five-card board.
//
// This TU produces, per combo, the equilibrium counterfactual continuation
// value and, via an exact full best response, the solution exploitability in
// chip and summed-BB units. It is the builder primitive for the five-card
// frontier rows and the reference used for certified per-entry error.
// Benchmark-only under the rfc0047 link guard.
#pragma once

#include <bs/game_definition.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace bs::rfc0047 {

struct RiverSolveConfig {
  bs::poker::GameDef def;        // five-card board, terminal=River, 2 seats
  std::uint64_t iterations = 0;  // CFR iterations (one per traverser sweep)
  bool linear_averaging = true;  // linear t weighting of average strategy
  // Also return the per-joint-deal seat-0 continuation matrix
  // (combos*combos row major) under the average strategy - the post-chance
  // frontier leaf value the G3 turn solver consumes. ~9 MB/leaf, off by
  // default.
  bool compute_deal_matrix = false;
  // Worker threads for the O(kC^2) terminal showdown folds and the deal
  // matrix. 0 = hardware count; 1 = deterministic serial. Results are
  // bitwise identical across thread counts (each own-combo row is an
  // independent fork-join task with a serial inner reduction).
  std::size_t threads = 1;
  // Max full raises per street in the action menu (ActionAbstraction::identity).
  // Default 1 is the bounded online cap-1 menu; 0 means no cap (the exact
  // cap-0 reference used only by the menu-materiality measurement). The
  // seedless solve stays exact for whichever menu is selected.
  std::uint8_t action_cap = 1;
};

struct ComboNashConv {
  std::vector<double> seat0_br;  // per seat-0 combo best-response value (chips)
  std::vector<double> seat1_br;
  std::vector<double> seat0_ev;  // per combo equilibrium-game value
  std::vector<double> seat1_ev;
};

struct RiverSolveResult {
  std::uint64_t iterations = 0;
  std::size_t action_nodes = 0;
  std::size_t combos_per_seat = 0;  // 1081
  // Exploitability.
  double nash_conv_chips = 0.0;              // summed over both seats, chips
  double nash_conv_bb = 0.0;                 // summed over both seats / big blind
  double nash_conv_pot = 0.0;                // summed / root pot
  std::vector<double> history_nash_conv_bb;  // one entry per logged checkpoint
  // Equilibrium per-combo counterfactual values at the root (chips), keyed by
  // dense five-card combo, for the two seats - the CV oracle primitive.
  std::vector<double> root_cfv_seat0;
  std::vector<double> root_cfv_seat1;
  // Per-combo exact best-response values at the root (chips) under the same
  // average strategy; br - cfv is the per-combo exploitability residual used
  // to certify the oracle reference's own accuracy.
  std::vector<double> root_br_seat0;
  std::vector<double> root_br_seat1;
  // Optional per-joint-deal seat-0 continuation matrix under the average
  // strategy (combos_per_seat^2 row major; 0 on blocker-incompatible pairs).
  // Populated only when config.compute_deal_matrix is set.
  std::vector<double> deal_value_seat0;
  // Node-indexed average strategy: one flat combos*menu row-major vector per
  // ACTION node (empty at non-action indices), menu = that node's action count.
  // Populated on every solve so benchmark-only fixed-policy consumers (the
  // cap-0 menu-materiality embedding) can evaluate the solved policy without
  // touching the kernel's internal regret/sum tables or re-running CFR.
  std::vector<std::vector<double>> avg_sigma;
};

// Deterministic exact solve. Throws std::invalid_argument on a malformed
// (non-five-card / non-two-seat / non-river) config.
RiverSolveResult solve_river_subgame(const RiverSolveConfig& cfg,
                                     std::span<const std::uint64_t> checkpoints = {});

// Best-response / equilibrium per-combo values of the current average
// strategy (also deterministic), kept separate for certified-error use.
ComboNashConv river_combo_values(const RiverSolveConfig& cfg, std::uint64_t iterations,
                                 bool linear_averaging);

}  // namespace bs::rfc0047
