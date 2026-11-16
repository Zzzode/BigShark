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

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bs::rfc0047 {

// RFC 0049 Stage 1 Gate 2: one scored checkpoint. Emitted to the optional
// sink only at pre-registered rungs; carrying the average-strategy snapshot
// so an external reach-bound instrument can score e_embed mid-solve.
struct RiverCheckpoint {
  std::uint64_t iteration = 0;
  double nash_conv_bb = 0.0;
  std::vector<std::vector<double>> avg_sigma;
};

// Sink invoked once per reached rung after that iteration's strategy is
// observable. Bench-only; absent by default.
using RiverCheckpointSink = std::function<void(const RiverCheckpoint&)>;

// Polled once per iteration at the top of the driver loop, OUTSIDE every
// traversal floating-point operation, with elapsed wall time in
// milliseconds. Returning true latches an early, orderly stop after zero
// traversal FP work has run for that iteration. Integer clock reads only,
// so no numeric result or FP ordering can change. Bench-only; absent by
// default (and when absent no clock call is made).
using RiverStopPredicate = std::function<bool(std::uint64_t iteration, double elapsed_wall_ms)>;

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
  // RFC 0048 Stage-2b (D184): optional explicit menu. When set, the solver
  // builds THIS abstraction and action_cap is ignored at tree construction;
  // the embedding caller passes the same object to the solver and the
  // separately built bounded AbstractTree so avg_sigma node indexing aligns.
  // Nullopt is the single construction site for identity(action_cap), which
  // keeps every existing numeric path bitwise unchanged (the seedless kernel
  // layout and RM+ fallback depend only on the tree).
  std::optional<bs::abstraction::ActionAbstraction> action_abstraction;
  // RFC 0048 D251: RM+ clips negative regrets after each sweep when true
  // (the historical default, byte-preserving); false keeps vanilla RM
  // regrets while rm() still applies the positive part, mirroring the turn
  // solver's rm_plus switch. Bench-only measurement lever; the default
  // leaves every existing numeric path unchanged.
  bool rm_plus = true;
  // RFC 0048 D252: Linear DCFR+ time discounting. When true, at the start
  // of iteration t every accumulated regret and average-strategy table is
  // scaled in place by t/(t+1) BEFORE the two traversals add their fresh
  // undiscounted contributions; RM+ clipping is governed separately by
  // rm_plus and stays on in the D252 candidate, so the beta (negative-
  // regret) factor is structurally inert. The false default makes this
  // field dead and keeps every existing numeric path byte-identical.
  // Bench-only measurement lever; no production solver reads it.
  bool dcfr = false;
  // RFC 0049 Stage 1 Gate 2 additive default-off seams. When tree_limits is
  // unset the RiverGame builds with the implicit production-default
  // TreeLimits exactly as before; the sink and stop predicate are empty
  // std::functions that disable themselves (no snapshot allocation, no
  // steady_clock call), leaving the default solve byte-identical.
  std::optional<bs::tree::TreeLimits> tree_limits;
  RiverCheckpointSink checkpoint_sink;
  RiverStopPredicate stop_predicate;
};

struct ComboNashConv {
  std::vector<double> seat0_br;  // per seat-0 combo best-response value (chips)
  std::vector<double> seat1_br;
  std::vector<double> seat0_ev;  // per combo equilibrium-game value
  std::vector<double> seat1_ev;
};

// RFC 0050 Stage 0 Step 0b: exact-menu construction-only allocation
// receipt. The probe builds the full RiverGame (tree, showdown matrix,
// per-action-node regret/average tables, per-node reach/value storage,
// scratch pool) under the supplied TreeLimits and runs ZERO CFR
// iterations, so no NashConv exists and the average-strategy snapshot is
// never built (sigma bytes are reported zero). Every resident table is
// itemized per the Amendment 1 review nit 3 rather than summed into one
// number. A construction failure is a typed memory NO-GO: the tree
// builder's envelope guard reports "tree_resource_exhausted" and an
// allocator failure reports "bad_alloc"; on failure every byte field is
// zero and the campaign gate is fail-closed. Bench-only under the link
// guard.
struct RiverAllocationReceipt {
  bool allocated = false;
  std::string failure;  // "none" | "tree_resource_exhausted" | "bad_alloc"
  std::size_t action_nodes = 0;
  std::size_t total_nodes = 0;
  std::size_t tree_accounted_bytes = 0;
  std::size_t showdown_matrix_bytes = 0;
  std::size_t regret_bytes = 0;
  std::size_t average_sum_bytes = 0;
  std::size_t reach_bytes = 0;
  std::size_t value_bytes = 0;
  std::size_t sigma_snapshot_bytes = 0;  // always 0 at zero iterations
  std::size_t scratch_bytes = 0;
  std::size_t bookkeeping_bytes = 0;
  std::size_t itemized_total_bytes = 0;
  long peak_rss_kb = -1;  // getrusage ru_maxrss; -1 if unavailable
  double construction_wall_ms = 0.0;
};

// Construct the exact-menu RiverGame once, allocate every table, run no
// iterations, and return the itemized allocation receipt. Never throws on
// the two memory failure modes; any other exception propagates.
RiverAllocationReceipt probe_river_allocation(const RiverSolveConfig& cfg);

// RFC 0050 D307 bench-only resumable river state. The raw regret and
// average-sum tables are the only quantities a seedless CFR continuation
// needs; the average-strategy snapshot is derived lazily and is not stored.
// Tables are parallel to tree nodes, empty at non-action nodes. The identity
// block lets a checkpoint validate that it is reloaded into the identical
// game and menu before any table byte is imported.
struct RiverResumableState {
  std::uint64_t completed_iterations = 0;
  bs::poker::GameDef def{};
  std::uint64_t action_id_digest = 0;
  std::size_t tree_size = 0;
  std::vector<std::vector<double>> regret;
  std::vector<std::vector<double>> average_sum;
};

// Run cfg.iterations CFR iterations from zeroed tables and capture the raw
// state for later resume. Bench-only; does not compute NashConv.
RiverResumableState capture_river_state(const RiverSolveConfig& cfg);

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
  // RFC 0049 Gate 2: number of iterations actually completed and whether the
  // stop predicate latched before cfg.iterations. Zero/default on every
  // pre-RFC-0049 path that ran to completion, where iterations_completed
  // equals the requested count.
  std::uint64_t iterations_completed = 0;
  bool stopped_early = false;
};

// Deterministic exact solve. Throws std::invalid_argument on a malformed
// (non-five-card / non-two-seat / non-river) config.
RiverSolveResult solve_river_subgame(const RiverSolveConfig& cfg,
                                     std::span<const std::uint64_t> checkpoints = {});

// Rebuild the identical game, import the captured tables (failing on any
// identity or shape mismatch), then run iterations seed.completed+1 through
// cfg.iterations and return the normal solved result with NashConv and
// average strategy. Numeric neutrality is a hard contract: the resumed
// result must be bitwise equal to an uninterrupted solve of the same total
// iteration count.
RiverSolveResult resume_river_solve(const RiverSolveConfig& cfg,
                                    std::span<const std::uint64_t> checkpoints,
                                    const RiverResumableState& seed);

// Best-response / equilibrium per-combo values of the current average
// strategy (also deterministic), kept separate for certified-error use.
ComboNashConv river_combo_values(const RiverSolveConfig& cfg, std::uint64_t iterations,
                                 bool linear_averaging);

}  // namespace bs::rfc0047
