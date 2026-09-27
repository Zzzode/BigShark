// stage6/translator.hpp — RFC 0008 stage 6 R12 step 6: the deterministic
// exact<->coarse action translator (R7).
//
// The stage-6 candidate trains and stores rows over the FINITE coarse action
// menu; the real L1 GameState offers a continuum of integer raise totals. The
// translator is the single declared bridge:
//
//   * coarse -> exact: a coarse PolicyAction distribution is projected onto
//     the concrete legal actions at one GameState action node. Passive actions
//     pass through when legal; an aggressive total snaps to the nearest legal
//     integer in the inclusive [minimum, maximum] interval with ties to the
//     SMALLER total; masses that project to one concrete action merge; when no
//     aggressive action is legal, aggressive mass follows the declared
//     call -> check -> fold fallback. Fold mass is never made aggressive.
//   * exact -> coarse: an exact action observed while the candidate acts as an
//     OPPONENT in an exact traversal is mapped back to the nearest coarse menu
//     index, so the traversal can locate the coarse node for its row lookup.
//
// Nothing here clamps an illegal passive action or an inconsistent
// distribution: malformed/inconsistent input throws std::runtime_error, while
// argument errors (unknown id, wrong phase, non-finite wanted total) throw
// std::invalid_argument, which derives from std::logic_error rather than
// std::runtime_error — a caller must catch std::exception to cover both. This
// matches the harness-error convention in behavior_policy.hpp. The translator is deterministic for
// fixed inputs and owns no RNG or policy; it lives in bigshark_stage6_eval and is never linked by
// the service, either protocol, or the engine host.
#pragma once

#include <bs/behavior_policy.hpp>       // PolicyAction
#include <bs/game_definition.hpp>       // poker::GameState
#include <bs/heads_up.hpp>              // poker::Action / LegalActions
#include <bs/stage6/translator_id.hpp>  // the single shared TranslatorId (core)
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bs::stage6 {

// The declared v1 translator identity, with the eval-side digest filled. This
// is the SAME type the trainer's frozen manifest records (translator_id.hpp in
// bigshark_stage6_core); the eval side owns the rule text and the digest.
const TranslatorId& declared_translator_id();

// Snaps an arbitrary non-negative wanted total (half-integers included) to
// the nearest integer chip total inside the inclusive legal interval
// [minimum, maximum]. An exactly equidistant tie chooses the SMALLER integer.
// Every integer in the interval is a legal wager (LegalActions::contains tests
// only the interval), so for an integer-valued coarse total inside the
// interval the result is the total itself; the half-integer handling exists to
// make the declared tie rule explicit and testable. Throws
// std::invalid_argument for a non-finite/negative wanted total or an inverted
// interval.
poker::Chips nearest_legal_target(double wanted, poker::Chips minimum, poker::Chips maximum);

// The declared destination for aggressive mass at a node with no legal
// aggressive action: call when legal, else check, else fold. The returned
// action carries target_total 0 and passes LegalActions::contains. Throws
// std::runtime_error when no passive action is legal at all (a state the L1
// machine never emits; the rule stays total).
poker::Action aggressive_fallback_action(const poker::LegalActions& legal);

// Projects a coarse abstract action distribution onto the concrete legal
// actions at `state` for acting `seat`. Rules, each validated against
// state.legal():
//  * fold/check/call pass through unchanged when legal (Call carries
//    target_total 0); a positive-mass passive action that is not legal is an
//    unrecoverable inconsistency and throws, never a silent clamp;
//  * each bet/raise snaps per nearest_legal_target to the legal interval and
//    is emitted with the LEGAL aggressive type (Bet or Raise), even when the
//    coarse entry named the other;
//  * when no aggressive action is legal, all aggressive mass moves via
//    aggressive_fallback_action; fold mass stays fold;
//  * coarse entries projecting to the same concrete action merge their masses
//    in stable first-seen order.
// The returned distribution is finite and non-negative, every action passes
// LegalActions::contains, and the masses sum to 1 within 1e-9. Throws
// std::invalid_argument (a std::logic_error, not a std::runtime_error) when
// this is not `seat`'s action phase or the id is unknown, and
// std::runtime_error on any malformed input or unrecoverable inconsistency.
// `id` is carried as the projection identity; the default is the frozen v1
// rule and results do not depend on it beyond identity keying.
std::vector<PolicyAction> translate_coarse_to_exact(const poker::GameState& state, std::size_t seat,
                                                    const std::vector<PolicyAction>& coarse_dist,
                                                    const TranslatorId& id = {});

// Maps one EXACT action back onto a coarse menu for the row lookup performed
// when the candidate acts as an opponent inside an exact traversal:
//  * fold/check/call must match a menu entry exactly (type and target_total 0)
//    or the result is -1;
//  * a bet/raise maps to the menu action of the SAME aggressive type whose
//    target total is nearest; an equidistant tie chooses the SMALLER target
//    (the lower menu index for the ascending declared menus). Returns -1 when
//    the menu has no action of that type.
int project_exact_to_coarse_index(const std::vector<poker::Action>& coarse_menu,
                                  const poker::Action& exact_action);

// EDGE variant for exact-game NashConf traversals. Same passive rule (exact
// match or -1). For an aggressive exact action it matches the nearest
// aggressive menu entry WITHOUT regard to Bet vs Raise type, ties to the
// smaller total, and returns -1 only when the menu has no aggressive edge.
//
// Empirically (stage-6 R11 item 3 probes) the type-agnostic arm is currently
// DEFENSIVE BREADTH that never fires on a reachable line: every observed
// aggression is replayed into the reduced shadow as a coarse aggression, so
// aligned actors imply the street's bet/raise label is aligned too — chip
// overshoot changes amounts, not the type. The materially different piece of
// exact-game handling is the candidate's off-tree fallback (a shadow node with
// no aggressive edge at all), not this matcher. Kept type-agnostic so the
// addressing rule stays a pure "nearest coarse edge" invariant and is pinned
// by translator unit tests.
int project_exact_to_coarse_edge(const std::vector<poker::Action>& coarse_menu,
                                 const poker::Action& exact_action);

}  // namespace bs::stage6
