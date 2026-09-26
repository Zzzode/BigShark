// stage6/br_estimator.hpp — RFC 0008 stage 6 R12 step 5: the two-phase
// (learn/confirm) Monte Carlo deviation-gain estimator.
//
// It estimates, for each seat i in turn, the unilateral best-response gain
// against the N-1 fixed opponent BehaviorPolicies over the DECLARED finite R8
// deviation menu (declared_behavior_menu), then forms the general-sum
// NashConv replicate Y_s = sum_i g_{i,s} per confirm seed.
//
//   Phase 1 (LEARN, learn-only seeds): policy-iteration epochs over the SAME
//   keyed seeds. At a traverser node every menu action is enumerated and its
//   continuation value is accumulated into ONE row per INFORMATION SET
//   (InfosetKey = public history + the traverser's OWN two cards, never an
//   opponent holding or an undealt card). One argmax action per infoset is
//   frozen after the table reaches a fixed point. Enumerating the menu and
//   pooling every learn seed into one row is what keeps the estimate on the
//   pooled side of max_a E_z[Q] <= E_z[max_a Q]: a per-deal max would be the
//   omniscient quantity exact_omniscient_deviation_utility names.
//
//   Phase 2 (CONFIRM, disjoint seeds): paired best/false rollouts share every
//   deal, runout card and opponent draw through content-addressed CRN streams
//   (crn_streams.hpp), diverging only at traverser actions and re-pairing on
//   reconvergence. The frozen table is independent of the confirm seeds, so
//   the paired difference is an exactly unbiased estimate of the frozen
//   policy's gain; learn-seed consistency drives it toward the true pooled BR.
//
// Everything is deterministic for fixed inputs, fails closed with typed
// errors (an illegal policy action, non-distribution, infoset miss or
// non-stabilization is never clamped), and lives entirely in the offline
// bigshark_stage6_eval target. Published numbers are declared-menu
// general-sum NashConv in chips/hand, an ESTIMATE with no convergence
// guarantee; zero-sum exploitability/2 and equilibrium wording do not apply.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/crn_streams.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/infoset_key.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::stage6 {

// --- Typed harness failures -------------------------------------------------

class stage6_br_error : public std::runtime_error {
 public:
  explicit stage6_br_error(const std::string& what) : std::runtime_error(what) {}
};

// A confirm rollout reached an information set the learn phase never froze.
class stage6_br_infoset_miss : public stage6_br_error {
 public:
  explicit stage6_br_infoset_miss(const std::string& what) : stage6_br_error(what) {}
};

// The learn table had not reached a fixed point when require_stabilization is
// set. Phase 2 stays unbiased for whatever table froze; only BR optimality is
// unproven, so when stabilization is not required the run continues and
// reports stabilized == false.
class stage6_br_not_stabilized : public stage6_br_error {
 public:
  explicit stage6_br_not_stabilized(const std::string& what) : stage6_br_error(what) {}
};

// --- Frozen best response ---------------------------------------------------

// Accumulated Q estimate for one menu action at one information set.
struct QCell {
  double sum = 0.0;
  std::uint64_t visits = 0;
  double mean() const noexcept { return visits == 0 ? 0.0 : sum / static_cast<double>(visits); }
};

// One frozen deviation table for one traverser.
struct FrozenBestResponseData {
  std::size_t traverser = 0;
  // One chosen action per information set.
  std::map<InfosetKey, poker::Action> actions;
  // The declared menu each action was chosen from, stored for the legality
  // proof: a consumer re-checks every action against that node's legal set.
  std::map<InfosetKey, std::vector<poker::Action>> menus;
  // Final-epoch Q rows, diagnostics only.
  std::map<InfosetKey, std::vector<QCell>> q;

  std::uint64_t menu_identity_hash = 0;
  std::uint64_t learn_seed_list_hash = 0;
  std::size_t learn_seed_count = 0;
  std::size_t epochs_run = 0;
  bool stabilized = false;

  // FNV-1a digest over the key->action rows plus every declared identity
  // field; frozen into the run lock. Equality of tables is map equality, not
  // this hash.
  std::uint64_t content_hash() const noexcept;
};

// A frozen table served as a BehaviorPolicy: distribution() is a unit point
// mass on actions[key], and the action is re-verified with
// LegalActions::contains on every query. An unknown information set throws
// stage6_br_infoset_miss: a point-mass policy never invents an action. (The
// estimator's confirm recursion implements the declared RecordMiss merge
// itself; this class is the fail-closed exact-evaluation wrapper.)
class FrozenBestResponsePolicy final : public BehaviorPolicy {
 public:
  explicit FrozenBestResponsePolicy(FrozenBestResponseData data) : data_(std::move(data)) {}

  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;

  const FrozenBestResponseData& data() const noexcept { return data_; }

 private:
  FrozenBestResponseData data_;
};

// How the confirm phase treats an information set absent from the frozen
// table. Throw is the validation default; RecordMiss makes the best leg play
// the profile's own sampled action at that node (a declared, conservative
// "no deviation where uncovered" rule) and counts the miss in the result.
enum class FrozenMissPolicy { Throw, RecordMiss };

// The action space the deviating best response may use.
//
//   Declared5Fraction: declared_behavior_menu — fold/check/call plus min and
//     the five pot-fraction aggressions {1/3,1/2,3/4,1,3/2}. This is the full
//     R8 declared menu; the frozen table's menu_identity_hash is the declared
//     menu hash.
//
//   CoarseAbstraction: the SAME coarse action abstraction the MCCFR trainer
//     solved (bs::tree::abstract_node_menu with the supplied
//     ActionAbstraction — today {half-pot bet, pot raise}). The measured scalar
//     is then a COARSE-GAME general-sum NashConv: the deviator may only
//     exploit within the abstraction the artifact was trained in, which is the
//     only equilibrium claim an artifact that carries zero mass off its coarse
//     menu can honestly make. Robustness against exact sizings is a separate
//     fail-closed question and is NOT measured here. Opponent seats still play
//     their native BehaviorPolicy untouched. Both profiles in an R11
//     comparison must be measured under the SAME space for the paired
//     difference to be meaningful.
enum class DeviationSpace { Declared5Fraction, CoarseAbstraction };

// --- Configuration / output -------------------------------------------------

struct BrEstimatorConfig {
  std::size_t max_epochs = 32;
  bool require_stabilization = true;
  FrozenMissPolicy on_confirm_miss = FrozenMissPolicy::Throw;
  // Optional total flop-geometry lookup for later candidate runs; null in
  // step 5, which measures reference profiles only.
  const GeometryCoverage* coverage = nullptr;
  std::size_t dealer_max_attempts = 100000;
  // Deviation action space. CoarseAbstraction requires coarse_action below.
  DeviationSpace deviation_space = DeviationSpace::Declared5Fraction;
  // Used only when deviation_space == CoarseAbstraction; must outlive the
  // estimator call and equal the abstraction the bound artifact was trained
  // with (referenced, like coverage, because ActionAbstraction is held by
  // identity elsewhere and is not default-constructible).
  const bs::abstraction::ActionAbstraction* coarse_action = nullptr;
};

struct BrEstimatorCounts {
  std::uint64_t learn_rollouts = 0;
  std::uint64_t confirm_rollouts = 0;
  std::uint64_t learn_action_nodes = 0;
  std::uint64_t confirm_action_nodes = 0;
  std::uint64_t card_draws = 0;
  std::uint64_t deal_restarts = 0;
  std::uint64_t paired_same_action_merges = 0;
  std::vector<std::size_t> epochs_per_traverser;
  std::vector<std::size_t> infosets_per_traverser;
  std::vector<std::size_t> confirm_misses_per_traverser;
};

struct BrEstimatorResult {
  std::vector<FrozenBestResponseData> frozen;  // exactly N entries
  std::vector<std::vector<double>> gain;       // gain[traverser][seed], chips/hand
  std::vector<double> nashconv_replicates;     // Y_s = sum_i g_{i,s}
  std::array<double, 10> mean_gain{};
  double mean_nashconv = 0.0;
  std::uint64_t learn_seed_list_hash = 0;
  std::uint64_t confirm_seed_list_hash = 0;
  BrEstimatorCounts counts;
};

// One recorded content-addressed draw with its full coordinates, so a test can
// prove the paired legs share draws on a common public spine and re-pair after
// divergence: equal coordinates (node token, purpose, seat, counter) must map
// to one value.
struct CrnDrawEvent {
  std::uint64_t node_token = 0;
  BrStreamPurpose purpose = BrStreamPurpose::Deal;
  std::size_t seat = 0;
  std::uint64_t counter = 0;
  std::uint64_t value = 0;
};

// One paired confirm rollout: the two terminal utility vectors, each leg's
// CRN draw log, and the joint-deal fingerprint (the first value of the seed's
// deal RNG, identical for both legs and for every per-traverser run).
struct ConfirmPairOutput {
  std::array<double, 10> best_utility{};
  std::array<double, 10> profile_utility{};
  std::vector<CrnDrawEvent> best_draw_log;
  std::vector<CrnDrawEvent> profile_draw_log;
  std::uint64_t deal_fingerprint = 0;
  std::size_t misses = 0;
  bool diverged = false;
};

// Phase-1 Monte Carlo learn driver: returns the frozen deviation table for one
// traverser from the learn seeds (policy-iteration epochs per config). `counts`
// is optional cumulative instrumentation.
FrozenBestResponseData mc_learn_frozen_best_response(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
    std::span<const std::uint64_t> learn_seeds,
    const bs::solver::JointDealTable* enumerated = nullptr, const BrEstimatorConfig& config = {},
    BrEstimatorCounts* counts = nullptr);

// Phase-2 paired driver for one traverser and one confirm seed against an
// already frozen table.
ConfirmPairOutput mc_confirm_pair(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser, std::uint64_t seed,
    const FrozenBestResponseData& frozen, const bs::solver::JointDealTable* enumerated = nullptr,
    const BrEstimatorConfig& config = {}, BrEstimatorCounts* counts = nullptr);

// Primary entry point. `policies` has one BehaviorPolicy per seat and must
// outlive the call. learn_seeds/confirm_seeds are disjoint nonempty lists;
// overlap or emptiness throws std::invalid_argument. When `enumerated` is
// non-null (the enumerable R9 fixtures) joint deals come from
// sample_joint_deal over that exact JointDealTable; otherwise
// sample_scalable_joint_deal draws over `ranges` under the product-
// conditional joint measure.
BrEstimatorResult estimate_deviation_gains(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::span<const std::uint64_t> learn_seeds,
    std::span<const std::uint64_t> confirm_seeds,
    const bs::solver::JointDealTable* enumerated = nullptr, const BrEstimatorConfig& config = {});

// Exact-freeze validation driver: runs the approved exact pooled
// best-response walk and exports its per-infoset choices as a frozen table
// (the oracle walk stays the single source of pooling truth). Wrap the result
// in FrozenBestResponsePolicy and exact_oracle_utility must equal
// exact_best_response_utility.
FrozenBestResponseData learn_exact_frozen_best_response(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser);

}  // namespace bs::stage6
