// RFC 0039: range-conditioned continuation values at the turn depth-limit
// river boundary.
//
// This is a SEPARATE leaf-model seam from bs::gto::FrontierEvaluator
// (frontier.hpp). The frontier contract values ONE sampled joint deal:
// evaluate(board, hands, ledger) -> one net-chip scalar per seat, with no
// node identity and no ranges. A nested river subgame produces a different
// mathematical object: for boundary leaf L and seat p, the range-conditioned
// marginal
//
//   V_p(c) = E_{opponent deals ~ conditioned leaf ranges}[
//              U_p(c, .) under the inner subgame average strategy ]
//
// It is keyed by p's OWN combo c and is an expectation over opponent deals;
// it must never condition on the sampled opponents' exact holdings. The two
// contracts cannot share a shape:
//  - FrontierEvaluator has no frontier-node identity, so distinct turn
//    betting lines that terminate with the same five-card ledger would alias;
//  - its per-deal scalar is zero-sum for one joint deal, whereas the marginal
//    matrix is zero-sum only in the distributional sense:
//    sum_p sum_c P_p(c) * V_p(c) == 0. A per-deal zero-sum assertion on
//    marginals is WRONG and must not be added by callers.
//
// The seam has two surfaces because nested values enter the OUTER trainer
// during MCCFR sweeps, but nested solves run on a block cadence outside the
// sampled walk:
//  - refine(): the outer block driver solves/updates one leaf's inner river
//    subgame from exact conditioned leaf ranges and stores the matrix;
//  - lookup_scalar(): the outer trainer's TerminalFrontier dispatch consults
//    ONLY the traverser's own-combo entry V_traverser(c) for the sampled
//    deal, never performing a nested solve inside a sweep.
//
// Values are in CHIPS (1 BB = GameDef.big_blind chips; conversion to BB
// happens only in reports and gate checks).
//
// Stage 1 (RFC 0039) is measurement-only: the only producer is the nested
// M2 arm in engine/benchmarks/subgame_nash_conv.cpp. The production turn
// path stays turn_root_unserved; nothing in the engine host constructs a
// model.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>    // TerminalPayload, TreeNode
#include <bs/game_definition.hpp>  // GameDef, Chips
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace bs::solver {
struct WeightedHand;  // defined in heads_up_solver.hpp
class NSeatPolicy;    // defined in nseat_trainer.hpp
}  // namespace bs::solver

namespace bs::gto {

// Why a leaf request did not produce usable values.
//  - Solved:  a conditioned matrix is available.
//  - Miss:    this leaf has no stored matrix yet (outer sweep runs before the
//             first block refinement). The outer trainer must fall back to
//             its declared non-nested leaf path; Miss is never an error.
//  - Decline: a matrix was attempted but cannot be served under the accepted
//             predicates: nested budget/resource exhaustion, an action-
//             abstraction digest mismatch, or an inner best-response gap
//             that has never passed the absolute tolerance.
//  - Invalid: the leaf itself is malformed for a nested solve (every seat has
//             zero conditioned mass, an unsupported board/live-seat shape).
//             The outer candidate carrying this leaf is discarded.
enum class ContinuationStatus { Solved, Miss, Decline, Invalid };

// Wall/resource budget for ONE inner refinement. Mirrors the fields of
// solver::NSeatTrainerLimits that bound a nested train_nseat call. The
// conversion from the shared outer request envelope to these values uses the
// measured node/action/seat cost coefficients (RFC 0036 Stage 0), never the
// deals-based iteration_cap_for_budget projection.
struct ContinuationBudget {
  std::chrono::milliseconds wall{0};
  std::uint64_t added_iterations = 0;
  std::size_t max_nodes = 1'000'000;
  std::size_t max_information_sets = 1'000'000;
  std::size_t max_bytes = std::size_t{1} << 30;
  std::size_t max_visits = 1'000'000'000'000ULL;
};

// Full PUBLIC identity of one river-boundary leaf. It embeds:
//  - the outer tree identity (frontier node index under the outer
//    AbstractTree plus the outer action-abstraction digest), because the
//    48 river siblings share a chance parent and identical boards occur on
//    different turn action lines;
//  - the complete chip ledger from the leaf TerminalPayload (the
//    tree_shape_digest deliberately excludes terminal payloads);
//  - the ordered five-card board.
// Conditioned leaf ranges are NOT identity here: they drift between outer
// blocks and are an explicit refine() argument.
struct RiverBoundaryLeaf {
  // Node index of the TerminalFrontier node in the OUTER depth-limited tree.
  std::size_t outer_node_index = 0;
  // Outer abstraction identity (name/version/parameters/digest). The nested
  // tree must be built with the same value object and the digest asserted
  // equal before solving.
  abstraction::AbstractionId outer_action_id{};
  // Identity of the outer turn game (button/big_blind/ante live here).
  poker::GameDef outer_def{};
  tree::TerminalPayload ledger{};

  // Ordered five-card board (ledger.board[0..board_size)); board_size is 5.
  std::span<const int> board() const {
    return std::span<const int>(ledger.board.data(), ledger.board_size);
  }
};

// Construct the leaf identity from the outer tree, its TerminalFrontier node,
// and the node's stored payload. Throws std::invalid_argument if the node is
// not a TerminalFrontier leaf or the payload board is not five cards.
RiverBoundaryLeaf make_river_boundary_leaf(const tree::AbstractTree& outer_tree,
                                           std::size_t frontier_node_index);

// Build the street-start river-rooted GameDef nested behind one boundary
// leaf. Inherits button/big_blind/ante/variant from the outer game; takes
// per-seat stack, NET contribution, and folded flags from the leaf ledger;
// declares pot = sum of net contributions (folded dead money included); and
// resets every street-state field to its street-start value. The caller
// constructs the nested AbstractTree with the outer tree's ActionAbstraction
// (digest asserted) and DepthLimit::none(). Throws std::invalid_argument on a
// leaf with fewer than two live seats; poker::validate rejects every other
// malformed shape when the nested tree is built.
poker::GameDef nested_river_game(const RiverBoundaryLeaf& leaf);

// Per-seat sparse marginal values: [seat] maps combo_index (bs::comboIndex)
// to V_seat(combo) in CHIPS. Zero-mass combos are absent, never zero-filled.
using ContinuationMatrix = std::vector<std::map<int, double>>;

// How the last inner refinement ended (mirrors solver::NSeatTerminationPhase
// vocabulary at the seam boundary).
enum class ContinuationTermination { Complete, WallClock, ResourceLimit };

// One leaf's stored continuation result.
struct ContinuationValues {
  ContinuationMatrix marginals;  // [seat][combo] -> chips
  std::uint64_t inner_completed_iterations = 0;
  bool warm = false;
  ContinuationTermination termination = ContinuationTermination::ResourceLimit;
  // Last measured ABSOLUTE inner best-response gap in CHIPS (summed over
  // live seats). nullopt until an offline driver has measured one; the
  // acceptance predicate is applied by that driver, never assumed here.
  // Conversion to BB for the 0.01 gate is gap_chips / outer_def.big_blind.
  // Stored for diagnostics and the solve-scoped convergence tag; the model
  // never serves on a stall without a previously passing measurement.
  bool has_inner_gap = false;
  double inner_gap_chips = 0.0;
};

// The range-conditioned nested continuation-value model. One instance covers
// one bounded outer turn solve and owns its solve-scoped warm map; it is
// released with that solve. Implementations must be safe to use from the
// single-threaded serial train_nseat route only (RFC 0016's parallel route
// has no warm-state slot).
class ContinuationValueModel {
 public:
  virtual ~ContinuationValueModel() = default;

  // Stable model/abstraction identity string for diagnostics.
  virtual std::string identity() const = 0;

  // Solve (or warm-refine) the inner river subgame for one boundary leaf.
  // `conditioned_ranges` TRAIN the inner subgame; `root_support_ranges` are
  // the declared outer-root ranges that define the per-seat own-combo support
  // the marginal matrix must cover, INCLUDING combos with zero average-
  // strategy trunk reach to this leaf (ES-MCCFR evaluates every own deviation).
  // Opponents in a marginal stay distributed by the conditioned leaf ranges.
  // No exception may escape into an outer MCCFR sweep.
  virtual ContinuationStatus refine(
      const RiverBoundaryLeaf& leaf,
      const std::vector<std::vector<solver::WeightedHand>>& conditioned_ranges,
      const std::vector<std::vector<solver::WeightedHand>>& root_support_ranges,
      const ContinuationBudget& budget) = 0;

  // Look up the traverser's net-chip marginal for its OWN sampled combo at a
  // previously refined leaf. This is the ONLY function the outer trainer's
  // TerminalFrontier dispatch may call during a sweep; it performs no solve
  // and must not read other seats' sampled hole cards. Returns Miss when the
  // leaf has no matrix yet.
  virtual ContinuationStatus lookup_scalar(const RiverBoundaryLeaf& leaf, std::size_t traverser,
                                           std::array<int, 2> own_cards,
                                           double& value_chips) const = 0;

  // Read the stored result for an offline measurement driver (inner
  // policy/tree/ranges needed for the absolute best-response gap live with
  // the concrete NestedRiverValueModel, not on this base interface).
  virtual const ContinuationValues* values(const RiverBoundaryLeaf& leaf) const = 0;
};

// Default RFC 0039 Stage 1 implementation. One instance is scoped to one
// bounded outer turn solve and holds a const reference to the OUTER
// depth-limited tree (which must outlive the model); nested trees inherit
// that tree's ActionAbstraction value. The solve-scoped warm map keys on
// the frontier node index (unique within one outer tree) and retains the
// inner NSeatInitialState plus convergence tag between outer blocks.
//
// Warm state is a heuristic refinement start under drifting leaf ranges
// (the RFC 0012 bitwise guarantee holds only with unchanged ranges): a
// train_nseat validation rejection is caught and retried cold once. The
// model never accepts on stall; has_inner_gap is set only by an external
// offline measurement driver.
class NestedRiverValueModel final : public ContinuationValueModel {
 public:
  explicit NestedRiverValueModel(const tree::AbstractTree& outer_turn_tree,
                                 std::uint64_t master_seed = 0xC0FFEE);
  ~NestedRiverValueModel() override;

  std::string identity() const override;

  ContinuationStatus refine(
      const RiverBoundaryLeaf& leaf,
      const std::vector<std::vector<solver::WeightedHand>>& conditioned_ranges,
      const std::vector<std::vector<solver::WeightedHand>>& root_support_ranges,
      const ContinuationBudget& budget) override;

  ContinuationStatus lookup_scalar(const RiverBoundaryLeaf& leaf, std::size_t traverser,
                                   std::array<int, 2> own_cards,
                                   double& value_chips) const override;

  const ContinuationValues* values(const RiverBoundaryLeaf& leaf) const override;

  // Accessors for the offline absolute-gap driver. Two distinct
  // invalidation rules apply:
  //  - A refine() call clears the leaf's gap CERTIFICATION up front,
  //    including when that refine later fails (fail-closed).
  //  - The pointed-to Entry objects are replaced (and the old policy/tree/
  //    ranges/marginal storage freed) only by a SUCCESSFUL refine(); a
  //    failed refine leaves the previous Entry and these pointers alive
  //    (the old matrix is merely unserved). Callers must therefore (re)fetch
  //    after every successful refine() and must never cache the pointers
  //    across a refinement. The sweep-safe surface is lookup_scalar(),
  //    which returns a value by copy; nullptr before the first successful
  //    solve.
  const solver::NSeatPolicy* inner_policy(const RiverBoundaryLeaf& leaf) const;
  const tree::AbstractTree* inner_tree(const RiverBoundaryLeaf& leaf) const;
  const std::vector<std::vector<solver::WeightedHand>>* inner_ranges(
      const RiverBoundaryLeaf& leaf) const;

  // Record an externally measured absolute inner BR gap for one leaf
  // (offline driver only; CHIPS, summed over live seats) and whether it
  // passed the model's acceptance tolerance. A leaf whose gap has never
  // been recorded as passed is not served by lookup_scalar (the outer
  // trainer takes its declared non-nested fallback), so a stall without an
  // absolute passing measurement can never publish a candidate.
  void record_inner_gap(const RiverBoundaryLeaf& leaf, double gap_chips, bool passed);

 private:
  struct Entry;
  const tree::AbstractTree& outer_;
  std::uint64_t master_seed_;
  std::map<std::size_t, std::unique_ptr<Entry>> entries_;  // frontier node -> entry
};

}  // namespace bs::gto
