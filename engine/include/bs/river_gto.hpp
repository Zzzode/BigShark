// river_gto.hpp — public facade for the river Nash engine.
//
// Solves the heads-up one-bet/one-raise river subgame (check/bet, fold/call/
// raise, and the call/fold decision after a raise) for differentiated 1326-
// combo ranges, returning a behavior probability at every (combo, public node).
//
// Two backends are hidden behind this header:
//   * exact sequence-form LP (OpenSpiel infostate tree + HiGHS) when the deal
//     count fits the time budget (exploitability ~ 1e-5 pot);
//   * bounded full-tree DCFR+ otherwise (exploitability < 0.05% pot at the
//     default iteration count), which needs no external dependency.
//
// No OpenSpiel/HiGHS type appears here, so the live decision core does not
// inherit those include paths or ABI constraints.
#pragma once
#include <array>
#include <bs/range.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bs {
namespace gto {

struct RiverSolutionData;  // internal, defined in src/gto
// Public betting nodes of the river tree (see gto/river_game.cc).
//   Root   in-position first decision:           check / bet
//   AfterC out-of-position after a check:        check / lead
//   FaceBet  facing a bet (no raise yet):         fold / call / raise
//   FaceLead after we checked, facing a lead:    fold / call / raise
//   FaceRaiseI  our bet was raised:              fold / call
//   FaceRaiseO  our lead was raised:             fold / call
enum class RiverNode {
  kRoot,
  kAfterC,
  kFaceBet,
  kFaceLead,
  kFaceRaiseI,
  kFaceRaiseO,
};

struct RiverSolveOptions {
  int max_combos_per_side = 40;  // strength-stratified cap to bound solve time
  double time_budget_s = 1.5;
  int cfr_iterations = 100000;
  bool allow_exact = true;  // use the LP backend when feasible & available
};

struct RiverSolveResult {
  bool ok = false;
  bool exact = false;        // true = LP equilibrium, false = bounded CFR
  double value_to_ip = 0.0;  // equilibrium chip value to the in-position side
  double exploitability_pot = 1.0;
};

class RiverSolution {
 public:
  RiverSolution();
  ~RiverSolution();
  RiverSolution(RiverSolution&&) noexcept;
  RiverSolution& operator=(RiverSolution&&) noexcept;

  RiverSolveResult result() const;

  // Number of legal actions at a node (2 or 3); their order is the canonical
  // order given in the RiverNode doc (check/bet; fold/call/raise; fold/call).
  static int NumActions(RiverNode node);

  // Behavior probabilities for a private combo at a node, written to `out`
  // (must hold NumActions entries). Returns false if the combo is absent from
  // the solved range; caller should then fall back to its baseline policy.
  bool ProbsFor(int combo, RiverNode node, double* out) const;

  // Deterministic mixed-strategy action index in [0,NumActions). The same
  // (combo,node,seed) always yields the same index, so STALE re-decisions do
  // not flip a mixed action.
  int ChooseAction(int combo, RiverNode node, uint64_t seed) const;

 private:
  friend std::unique_ptr<RiverSolution> SolveRiver(std::vector<int> board, int pot, float bet_frac,
                                                   float raise_frac, const Range& ip_range,
                                                   const Range& oop_range,
                                                   const RiverSolveOptions& opts);
  std::shared_ptr<RiverSolutionData> data_;
};  // Solve the river subgame. Always returns a usable object (bounded CFR needs no
// external solver); inspect result().ok for quality.
std::unique_ptr<RiverSolution> SolveRiver(std::vector<int> board, int pot, float bet_frac,
                                          float raise_frac, const Range& ip_range,
                                          const Range& oop_range,
                                          const RiverSolveOptions& opts = {});

// Build a strength-stratified river range (1326 combos, board cards removed)
// capped to `cap` combos while preserving the weak..strong distribution. This
// is the action-line range constructor that keeps the solve inside the live
// time budget. A cap <= 0 returns every non-board combo.
Range BuildRiverRange(const std::vector<int>& board, int cap);

// Differentiated, action-line-tracked river ranges for both sides.
struct TrackedRangesOptions {
  int cap = 24;
  int preflop_raises = 0;  // 0 limped, 1 single raise/3bet, >=2 4bet+
  bool hero_was_aggressor = false;
};
struct TrackedRanges {
  Range ip;   // solver first actor (acts at root)
  Range oop;  // solver responder
};
// `flop_line`/`turn_line` are actor-tagged action codes from the orchestrator,
// e.g. "Hx,Ob,Hc" (H=hero, O=opponent; x check, c call, b bet, r raise).
// `hero_is_ip` maps hero to the solver first-actor side; hero's actual combo
// is always retained with weight 1.
TrackedRanges TrackRiverRanges(const std::vector<int>& board, int hero_combo, bool hero_is_ip,
                               const std::string& flop_line, const std::string& turn_line,
                               const TrackedRangesOptions& opt = {});

// Reports whether the exact sequence-form LP backend (HiGHS) is compiled in.
// The bounded DCFR backend is always available.
bool hasExactRiverLp();

}  // namespace gto
}  // namespace bs
