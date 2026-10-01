// RFC 0005 Stage 9 bounded resolving gadget with independent best-response
// certification, generalized to two or three seats (RFC 0009 W2c-ii-c). Offline
// solver-domain component; it never opens a database, never touches the wire,
// and never changes v0/minor-0 behavior.
//
// The resolver re-solves a SINGLE, terminal-only postflop hero decision rooted
// at a reconstructed descendant of an immutable blueprint game. It builds a
// hero-vs-field zero-sum resolving gadget (Brown & Sandholm style), where the
// field is the separable sum of every non-hero seat:
//
//   - A synthetic chance root selects a compatible joint private deal (one hand
//     per seat) at the fixed public node with an UNNORMALIZED counterfactual
//     weight (see CounterfactualReach), proportional to each non-hero seat's
//     counterfactual mass m_s(I).
//   - At the gadget root each non-hero seat observes only its own two cards at
//     a synthetic "-x" information set, DISTINCT from every normal solver
//     information key. It chooses TERMINATE or CONTINUE. Each seat's choice is
//     independent (the field is a separable sum, so the gadget is zero-sum).
//   - TERMINATE pays the centered per-infoset margin b_s(I) to that seat and
//     -b_s(I) to the hero (zero-sum hero-vs-field). CONTINUE enters the real
//     subtree: the hero's current terminal decision, then exact engine
//     settlement through fold or the (possibly all-in) call and the
//     uniform/fixed public runout.
//   - Only the non-hero trunk choices (terminate/continue) and the hero's
//     current decision are re-solved. Everything before the node is locked to
//     the blueprint prefix; the terminal-only profile leaves NO later decision
//     to lock.
//
// m_s(I)/b_s(I) normalization (the highest-risk detail; auditable here):
//
//   For a joint deal d = (hero hand h, and a hand r_s for each non-hero seat s)
//   at the node, the unnormalized counterfactual weight is
//       w(d) = rangeW[hero](h) * prod_s rangeW[s](r_s) * pi^prefix_hero(h),
//   zero when any two hands share a card or a hand blocks the board.
//   pi^prefix_hero is the product of the LOCKED BLUEPRINT probabilities of
//   every prior HERO action on the observed path. Each non-hero seat's own
//   action reach is deliberately EXCLUDED (its -x infoset starts fresh), as is
//   every hero action at or after the current node. Already-dealt public cards
//   condition the support (blocker removal); their uniform chance probability
//   is a common constant across deals at the fixed observed board and is
//   therefore omitted without changing any ratio, the chance normalization, or
//   b_s(I).
//       m_s(c) = sum_{d: seat s holds c} w(d)   (recorded; zero kept zero)
//   The baseline locks the hero's CURRENT node to its blueprint row, so seat
//   s's locked-continuation value for deal d is
//       C_s(d) = sum_a blueprint_h[a] * U_s(leaf a; d),
//   and the centered margin is
//       b_s(c) = (sum_{d: s holds c} w(d) * C_s(d)) / m_s(c).  (never /0)
//   In the terminal-only profile no seat has an in-subtree decision, so this
//   locked-continuation expectation IS the baseline best response.
//
//   Augmented terminal bookkeeping: the chip terminal under CONTINUE is the
//   exact engine Settlement.net_utility (gross chips, refunds, ties already
//   handled), expressed per seat. The TERMINATE leaf is the constant b_s(I) in
//   the same chip units; CFR plays it zero-sum hero-vs-field. Chance at the
//   gadget root normalizes w(d)/sum w; because every -x infoset is weighted by
//   exactly its mass, the post-training margin check is scale-free.
//
//   Certification at n >= 3 is a PER-SEAT UNILATERAL NON-REGRESSION bound, NOT
//   a multi-player equilibrium claim and NOT a two-player bound. The independent
//   certifier recomputes, with a separately written enumeration and the
//   unchanged prefix weights, each non-hero seat's best response against the
//   WHOLE candidate:
//       BR_s(c) = max( b_s(c), (sum_{d: s holds c} w(d) C^cand_s(d)) / m_s(c) ),
//   and accepts only when BR_s(c) <= b_s(c) + 1e-9 * root_pot for every
//   positive-mass infoset of EVERY non-hero seat, with every zero-mass infoset
//   providing zero gadget chance. The tolerance is in root-pot chip units and
//   is never weakened. At two seats this reduces exactly to the RFC 0005
//   responder bound; the wire level stays certified_bound only when EVERY seat's
//   bound passes, and a multiway certification carries a diagnostic token.
#pragma once

#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/unified_game.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bs::resolver {

// Exact certification tolerance in units of the root pot (RFC 0005 line 262).
inline constexpr double kCertPotTolerance = 1e-9;
// Gadget equilibrium sanity tolerance for the small-game test oracle.
inline constexpr double kEquilibriumPotTolerance = 1e-8;
// Diagnostic token attached to a multiway (3-seat) certified resolve response,
// identifying the per-seat unilateral non-regression certification. The host
// attaches it when ResolveResult::seat_count >= 3; a two-seat resolve never
// carries it so the frozen two-seat wire bytes are unchanged.
inline constexpr std::string_view kMultiwayCertificationToken =
    "multiway-certified:per-seat-unilateral-non-regression";

enum class ResolveStatus {
  // Whole-range candidate passed independent bounds. At two seats this is the
  // RFC 0005 responder bound; at three seats it is a PER-SEAT UNILATERAL
  // NON-REGRESSION bound (every non-hero seat's BR_s <= b_s + 1e-9*root_pot),
  // NOT a multi-player equilibrium claim. A multiway certification carries a
  // diagnostic token on the wire so it is never read as the two-player meaning.
  Certified,
  CertificationRejected,  // bounds recomputed; the candidate fails non-regression
  SolveDeadline,          // budget/deadline exhausted while training the gadget
  CertifyDeadline,        // budget/deadline exhausted during independent certification
  Ineligible,             // node/root is not a supported terminal-only resolve
  CoverageMiss,           // a required blueprint row/identity was missing
  InvalidInput,           // malformed game, ranges, or identity
};

const char* to_string(ResolveStatus status) noexcept;

// One immutable blueprint row in solver-domain terms. The pointers reference
// storage valid for the call duration (resident compact rows in production).
struct BlueprintRowView {
  const bs::poker::Action* actions = nullptr;
  const double* probabilities = nullptr;
  std::size_t size = 0;
};

// Locked blueprint access during a resolve. Implemented by the resident layer
// over a ResidentPolicySet record; the resolver depends on no storage type.
// All calls happen on the single calling thread.
class BlueprintSource {
 public:
  virtual ~BlueprintSource() = default;
  // The validated artifact game (seat-generic root identity, declared ranges,
  // sizes, fixed runout). Must stay valid for the duration of the resolve.
  virtual const bs::solver::UnifiedGame& game() const = 0;
  // 64-lowercase-hex artifact digest used for identity and cache keys.
  virtual std::string_view artifact_digest() const = 0;
  // Blueprint row at an action state for one seat's own two cards. `history`
  // is the observed public-action path to `state` (the prefix span at the
  // cursor during a prefix replay). A missing row is a coverage miss; the
  // resolver never invents a uniform policy.
  virtual std::optional<BlueprintRowView> row(const bs::poker::GameState& state,
                                              std::span<const bs::poker::PublicAction> history,
                                              std::size_t player,
                                              std::array<int, 2> cards) const = 0;
};

// Key for a non-hero seat's "-x" gadget infoset: the seat index and the seat's
// own sorted two cards. Distinct from every normal solver information key.
struct GadgetKey {
  std::size_t seat = 0;
  std::array<int, 2> cards{};
  bool operator<(const GadgetKey& other) const noexcept {
    if (seat != other.seat)
      return seat < other.seat;
    return cards < other.cards;
  }
};

// Per-infoset diagnostic for one non-hero seat's -x infoset, tagged by seat
// and the seat's own sorted two cards. Zero-mass infosets are retained with
// mass zero and no division.
struct MarginRecord {
  std::size_t seat = 0;
  std::array<int, 2> cards{};
  double mass = 0;           // m_s(I), unnormalized
  double baseline = 0;       // b_s(I); zero when mass is zero
  double candidate = 0;      // candidate locked-continuation value / m_s(I)
  double best_response = 0;  // max(baseline, candidate) under the candidate
  double slack = 0;          // best_response - baseline - kCertPotTolerance*pot
  bool positive_mass = false;
};

// Resource/time budget for one resolve, mirroring the RFC 0004 trainer limits.
struct ResolveLimits {
  std::size_t max_nodes = 5000000;
  std::size_t max_information_sets = 1000000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;
  // Independent certification node cap. Zero means reuse max_nodes. A small
  // value forces a certification resource limit, which must always discard.
  std::size_t certify_max_nodes = 0;
  std::chrono::milliseconds time{2000};
  // Deterministic full-traversal iteration count for the gadget CFR. A timeout
  // or any resource limit before these complete discards the candidate.
  // Callers with a request-derived time budget must derive this from that
  // public budget (never from the hero hand) so the whole-range candidate
  // identity stays private-independent; see IterationCapForBudget.
  std::uint64_t iterations = 100000;
  // SplitMix64 domain seed. The candidate must depend only on public context;
  // the caller derives this from digest/root/ranges/history/utility/sizes.
  std::uint64_t public_seed = 0;
};

// Conservative per-unit cost of one full-traversal gadget iteration. One
// iteration walks every joint deal once per traverser (one sweep per seat) and
// touches every ordered node action, so the cost driver is deals x actions x
// seats rather than the iteration count alone. Measured release-build cost on
// the verified host is roughly 16 ns per (deal x traverser) walk step; this
// constant is about fifteen times larger so a derived cap cannot promise more
// work than the wall clock can deliver on any supported profile. It only
// scales the cap downward and never substitutes a heuristic candidate.
inline constexpr std::chrono::nanoseconds kGadgetIterationUnitCost{250};

// Deterministic iteration cap for a request-derived solve budget. The solve
// share of the budget (70 percent after the RFC-mandated return reserve, the
// same split Resolver::resolve applies between solve and certification) is
// divided by the conservative per-unit cost times the per-iteration unit count
// (one traverser sweep per seat over every deal and every ordered action), then
// the caller's configured cap is applied as an upper bound.
//
// Every input is public — budget, deal count, action count, seat count — so
// every counterfactual hero combination derives the identical cap and cache
// identity, as RFC 0005 requires. A zero result means the budget cannot pay
// for even one iteration: callers must discard the candidate rather than
// publish an untrained one.
inline std::uint64_t iteration_cap_for_budget(std::chrono::milliseconds budget,
                                              std::uint64_t configured_cap, std::size_t deals,
                                              std::size_t actions, std::size_t seats) noexcept {
  if (deals == 0 || actions == 0 || seats == 0 || budget.count() <= 0)
    return 0;
  const auto usable = budget - std::max(std::chrono::milliseconds(5), budget / 10);
  if (usable.count() <= 0)
    return 0;
  const auto solve_share = usable * 7 / 10;
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(solve_share).count();
  if (micros <= 0)
    return 0;
  const auto units = static_cast<std::uint64_t>(deals) * (actions + 1) * seats;
  const auto per_iteration_ns =
      units * static_cast<std::uint64_t>(kGadgetIterationUnitCost.count());
  if (per_iteration_ns == 0)
    return 0;
  const auto total_ns = static_cast<std::uint64_t>(micros) * 1000ULL;
  return std::min(total_ns / per_iteration_ns, configured_cap);
}

struct ResolveResult {
  ResolveStatus status = ResolveStatus::Ineligible;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;
  std::size_t information_sets = 0;
  double root_pot = 0;
  // Number of seats in the resolved game (2 or 3). The wire guarantee level is
  // certified_bound only when every non-hero seat's unilateral bound passes; a
  // multiway (3-seat) certification also carries a diagnostic token.
  std::size_t seat_count = 0;
  // Whole-range candidate at the hero's CURRENT node, keyed by the normal
  // solver information_key(state, hero_cards). Every positive-reach hero combo
  // appears; probabilities are over the node's ordered abstract actions.
  std::map<bs::solver::InformationKey, bs::solver::PolicyRow> candidate;
  // Ordered node abstract actions shared by every candidate row.
  std::vector<bs::poker::Action> node_actions;
  // Per-seat gadget diagnostics for every reachable non-hero -x infoset, plus
  // explicit zero-mass entries.
  std::vector<MarginRecord> margins;
  // Gadget average strategy at each non-hero -x infoset: probability of
  // TERMINATE then CONTINUE. Exposed for the independent equilibrium oracle.
  std::map<GadgetKey, std::array<double, 2>> gadget_terminate;
};

// One bounded, offline, whole-range resolve. A Resolver is reusable and
// thread-affine (call from one thread); it owns an in-process certified-policy
// cache keyed by the exact public resolve identity.
class Resolver {
 public:
  Resolver();
  ~Resolver();

  // Resolve the terminal-only hero decision at `node` (whose actor is the
  // hero). The blueprint supplies identity, declared ranges, prefix reach, and
  // the current-node baseline. `node` must be a descendant of the blueprint
  // game's flop root reached through `history` (the observed public-action
  // path from the root to `node`) and its board.
  ResolveResult resolve(const bs::poker::GameState& node,
                        std::span<const bs::poker::PublicAction> history,
                        const BlueprintSource& blueprint, const ResolveLimits& limits) const;

  // Drop the in-process certified cache (e.g. on process/session restart).
  void clear_cache() const noexcept;

 private:
  struct Cache;
  std::unique_ptr<Cache> cache_;
};

}  // namespace bs::resolver
