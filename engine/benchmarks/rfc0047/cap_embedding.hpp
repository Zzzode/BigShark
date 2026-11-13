// RFC 0047 S3/G5 bounded-menu vs cap-0 menu materiality (RFC 0046 Gate 3
// method), benchmark-only measurement component. No solver/certifier/serve API
// changes. The online cap is PARAMETERIZED (D164): identity(k) for any bounded
// raise cap k >= 1 is measured against the uncapped identity(0) reference so a
// menu-widening RFC can locate the smallest k whose materiality clears M. The
// production online menu stays cap-1; this header never changes that default.
//
// FROZEN MEASUREMENT (formula fixed before any gate data, per RFC 0047
// addendum discipline). On the SAME five-card river root (same pot, full
// 1,081-combo ranges, same chip unit), let Gk = identity(k) (a bounded online
// menu, k = max raises per street), G0 = identity(0) (uncapped exact full
// menu), and sigma_k the seedless cap-k average policy embedded into G0:
//
//   V_s(sigma_k_0)  expected value under the embedded policy,
//   BR_s(G0;sigma_k_0)  exact best response over the FULL cap-0 menu at seat s,
//                      opponent fixed to the embedded policy.
//   e_embed(k) = sum_s (BR_s - V_s) / (kC*kCompat) / big_blind   (BB, summed)
//
// Exact identity (independent review): e_embed(k) = e_Gk(sigma_k) + pure menu
// restriction term, both non-negative, because the cap-0-only actions take zero
// embedded probability (identical support tree) and BR over a superset menu is
// weakly larger. Certifying e_embed(k) <= M = 0.002 BB is therefore a
// SUFFICIENT conservative Gate-3 certification and needs NO cap-0 equilibrium
// and no NC_cap0 subtraction (subtracting NC_cap0 >= 0 would be
// anti-conservative). The seedless exact enumeration has zero sampling
// half-width, so the gate is the per-stratum MAXIMUM of e_embed over all
// reached river roots, not a UCB. A single-root crossing is evidence a cap is a
// CANDIDATE, never a global MAX or a Certified/M2 pass.
#pragma once

#include <bs/abstract_tree.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "river_solver.hpp"
#include "strata.hpp"

namespace bs::rfc0047 {

inline constexpr double kCapMaterialityGateBb = 0.002;  // RFC 0047 G5 M

// How an opponent behaves at a cap-0 node that the embedded bounded policy
// cannot reach (a cap-0-only raise subtree). The BR walker necessarily enters
// these.
enum class CapExtensionCompletion {
  // Non-anticipatory, combo-independent UNIFORM distribution over the node's
  // menu (the rfc0046 partition-walker precedent). Primary frozen convention.
  Uniform,
  // Robustness arm: the opponent takes the action most favorable to the
  // best-responding seat (a single max walk, no CFR). This bounds e_embed over
  // EVERY completion; if it also passes M the certification holds regardless of
  // how the extension subtree is actually played.
  BrFavorable,
};

struct CapMateriality {
  // The bounded online raise cap k whose policy was embedded (identity(k)).
  std::uint8_t online_cap = 1;
  std::size_t cap0_action_nodes = 0;
  std::size_t online_action_nodes = 0;
  // Exact exploitability of the embedded cap-k policy inside the cap-0 game.
  double embedded_exploitability_bb = 0.0;
  // The cap-k policy's OWN exact NashConv inside its cap-k game, so the frozen
  // decomposition e_embed = e_Gk + pure menu term is directly readable:
  // pure menu term = embedded_exploitability_bb - online_nash_conv_bb >= 0.
  double online_nash_conv_bb = 0.0;
  // Conservative Gate-3 verdict: e_embed <= M (sufficient; no cap-0 equilibrium).
  bool gate_pass = false;
  // Paired-DFS map coverage. A mapped node is a cap-0 action node whose public
  // action path is also a cap-k path. invariant: every node with nonzero
  // EMBEDDED-POLICY (EV-walk) prefix reach is mapped; this counts only that
  // EV-walk notion. The BR walk is EXPECTED to enter unmapped extension nodes
  // and is handled by the completion rule, never counted as a failure here.
  std::size_t mapped_nodes = 0;
  // Always 0 and enforced at runtime: the EV walk throws std::logic_error if it
  // ever reaches an unmapped node (it follows only positive embedded-mass
  // edges, so this is a structural impossibility, made fail-closed rather than
  // merely asserted). The BR walk entering extensions is expected and handled by
  // the completion rule, never counted as a failure here.
  std::size_t unmapped_ev_reachable_nodes = 0;
};

// Build the bounded-menu -> cap-0 public action-path map by PAIRED DFS from the
// roots (a flat Action lookup is forbidden: Check/Call/Fold all carry
// target_total 0 and compare equal across states). Throws std::invalid_argument
// unless:
//   - both trees are built from the identical GameDef (root paired to root);
//   - each paired pair has matching node kind;
//   - each mapped pair has |A_bounded| <= |A0| with the first |A_bounded|
//     Actions element-wise equal, and every cap-0-only suffix action is a
//     Bet/Raise (no passive edge may be cap-0-only).
// Returns, parallel to cap-0 tree node indices: the mapped bounded-tree node
// index, or size_t(-1) for an unmapped (cap-0-only extension) node.
std::vector<std::size_t> map_bounded_to_cap0(const bs::tree::AbstractTree& cap0,
                                             const bs::tree::AbstractTree& bounded);

// One exact cap-0-game evaluation of the embedded cap-k policy for one
// five-card river root, using a DEDICATED LIGHTWEIGHT fixed-policy evaluator.
// It builds the cap-0 AbstractTree alone (the 33,693-node tree is under the
// byte cap) plus the showdown a0_/fold/stake tables, the paired-DFS map, and
// depth-scoped 1081-wide scratch; it never constructs a cap-0 RiverGame (whose
// full regret/sum layout would exceed the 1 GiB envelope). No cap-0 CFR is run.
//
// online_cap >= 1 selects identity(k) and solves that bounded policy
// (online_cap == 0 is rejected: the uncapped cap-0 equilibrium is explicitly
// out of scope). online_iters solves the cap-k policy under a configuration
// comparable across caps (same root, linear averaging, thread count). The
// embedded per-node distributions must be non-negative, sum to 1 within 1e-12,
// and assign exactly 0.0 to cap-0-only actions; a violation throws.
CapMateriality measure_cap_materiality(const bs::poker::GameDef& river_def, std::uint8_t online_cap,
                                       std::uint64_t online_iters, std::size_t threads,
                                       CapExtensionCompletion completion);

// Per-stratum gate: run measure_cap_materiality over EVERY unique reached river
// root the frontier enumerator yields and return the maximum e_embed (the
// binding value) and the count of roots measured. A single-def result is not a
// gate; this is the per-stratum evidence shape Gate 3 requires.
struct StratumCapResult {
  double worst_embedded_exploitability_bb = 0.0;
  std::size_t roots_measured = 0;
  bool gate_pass = false;
};
StratumCapResult measure_stratum_cap_materiality(const StratumSpec& spec, std::uint8_t online_cap,
                                                 std::uint64_t online_iters, std::size_t threads,
                                                 CapExtensionCompletion completion);

// Diagnostic oracle for the lightweight evaluator. Raw (not divided by the 990
// compatible-deal normalization) per-combo root chip sums of the embedded
// cap-k policy over the cap-0 tree. The EV walk follows only positive
// embedded-mass edges and therefore NEVER enters a cap-0-only extension
// subtree: its root values are completion-independent and must reproduce the
// cap-k kernel's own root CFV times 990 to floating-point precision. The BR
// walk covers the full cap-0 menu and is completion-dependent.
// Benchmark/test-only.
struct EmbeddedRootValues {
  std::vector<double> ev_seat0;
  std::vector<double> br_seat0;
  std::vector<double> ev_seat1;
  std::vector<double> br_seat1;
};
EmbeddedRootValues embed_root_values(const bs::poker::GameDef& river_def, std::uint8_t online_cap,
                                     std::uint64_t online_iters, std::size_t threads,
                                     CapExtensionCompletion completion);

}  // namespace bs::rfc0047
