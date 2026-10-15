# Six-Max GTO Qualification Proposal

Status: Proposed

The user has no prescribed reference policy or error threshold and requested
an independently reviewed qualification proposal. This is that proposal,
not an approved quality standard, passing report, or six-max completion claim.
The complete feature remains In Progress in the
[delivery register](sixmax-gto-delivery.md).

## Qualification Object

Freeze the complete production decision pipeline, not one trainer: actual
rules and six-seat stack vector, position/blinds, policy SHA-256, abstraction,
training identity/batch, range propagation, off-tree handling, and runtime
limits. A change to any evidence-bearing component requires requalification.
Include every position and street, 2..6 live players within the dealt six-seat
hand, unequal stacks, folds/dead money, all-ins/refunds/side pots, short raises,
and legal nonstandard integer targets. No singleton/short-stack experiment
substitutes for ordinary six-max operation.

Use bb per hand for whole-hand quality, with the actual big blind, utility
rules and hand-sampling distribution fixed in the report. Report conditional subgame results
separately with their actual roots/ranges/pots; do not add them as whole-game
evidence or confuse a finite fraction menu with all legal integer sizes.

## Three Required Gates

### Complete functional coverage

Drive the production host, not a seam fake or easy/medium practice policy,
through complete hands in all six positions. Combine structural native cases,
rules-equivalence classes, and public-only adversarial hand sequences with
held-out randomized campaigns. Random zero failures alone do not establish
exhaustive coverage.

Illegal actions, chip/status/eligibility errors, private-information leakage,
guessed history, and false guarantee labels are deterministic failures. A
normal unsupported state returning operational fallback is an unclosed feature
gap, not a passing GTO decision. Exceptional transport failures must remain
safe and visible without being counted as strategy coverage.

### Independently justified strategy quality

First verify the evaluator on enumerable six-seat fixtures using a separate
information-set-consistent best-response oracle. Opponent worlds with the same
responder information must be pooled before choosing an action. Include more
than one holding, card conflicts, seat permutations, folded-card removal,
dead-money/side-pot examples, mixed strategies, and known weak positive
controls. These fixtures prove measurement mathematics, not complete normal
stack poker quality.

Existing reusable references are the pooled oracle in
`engine/src/stage6/exact_oracle.cpp` and the learn/confirm estimator in
`engine/src/stage6/br_estimator.cpp`. Their offline boundary stays intact;
they must not become another production strategy or gain a service dependency.

For non-enumerable games, learn one legal deviation per seat against the other
five frozen policies, then confirm on independent hands. Report each seat's
signed gain and interval, the worst seat, and per-replicate summed gains with
covariance retained. Summed learned-deviation gains are not true NashConv
(the sum of optimal information-set-consistent best-response gains).
Use paired common random numbers for candidate/control
comparisons and keep numerical, search, sampling, range and abstraction errors
distinct. Negative within-menu gain must not be clipped away to conceal an
incomplete deviation action space.

The confirmation interval measures only the sampling error of the learned,
frozen deviation. Its upper endpoint is not an upper certificate for the
unknown optimal best response. No significant deviation found, stabilized
learning, or positive self-play winnings proves exact six-player equilibrium.
The complete GTO claim cannot pass without an independently justified relation
between the actual guarantee sought and the supplied evidence. Reporting an
empirical approximate result is not permission to relabel it as exact GTO.

The previous `bigshark-subgame-nash-conv` walker is not an accepted oracle:
its current code maximizes separately under fixed joint private cards before
averaging, allowing hidden-information-dependent responses. Audit/correct or
rename that measurement under independent design review before any quality
campaign. Do not reuse its historical values as six-max NashConv truth.
Similarly, measurement-driver `--exact` denotes a declared finite fraction
menu, not every legal integer target.

#### Required information-restriction negative control

A small mathematical decision fixture makes the hidden-world maximization
defect non-vacuous. Keep the responder's own holding, public history and
information identical in two equally likely opponent worlds. Use fold/call
payoffs of `(-1, -2)` in the strong-opponent world and `(-1, +2)` in the weak
world. The responder cannot tell those worlds apart:

- One legal information-set response chooses the same action in both worlds.
  Fold has expected utility -1; call has expected utility 0, so the best
  response utility is 0.
- Maximizing separately in each world chooses fold against strong and call
  against weak, averaging to 0.5. That is hidden-information-dependent
  behavior, not a legal response at this information set.
- Against an always-fold baseline with utility -1, the legal gain is 1,
  while the erroneous world-wise calculation reports 1.5.

These numbers are exact mathematical expectations, not a measured poker
fixture, a six-seat bound, or a newly accepted threshold. The native oracle
regression must realize a legal declared game with that observation/payoff
structure, then retain the analogous six-dealt-player cases, folded-card
conditioning and seat permutations. It must fail if worlds are unpooled.
At multiple information sets, pooling must use opponents' counterfactual
reach weights and public/private information correctly; this one-node
example is not a replacement for the complete best-response algorithm.
Counterfactual reach includes chance probabilities and the other players'
reach, excluding the responder's own reach.

The current manual subgame tool also skips root-folded players when dealing
private hands and uses `{-1,-1}` placeholders, removing their cards from
runout exclusion. Unknown folded cards are not permission to remove dealt
blockers under arbitrary conditioned ranges. The qualification oracle must
integrate their compatible private assignments; any analytically equivalent
marginalization needs a separately justified model. Native ledger/payout
tests alone do not verify that range/chance distribution.

### Whole-decision resource and reliability

Measure validation, state/history reconstruction, range propagation, tree
construction, loading/lookup, solve, extraction, response, and adapter legality
checks inside the actual deadline. A training-only wall limit does not cover
construction. Measure cold/warm behavior, resource exhaustion, process restart,
and memory including resident policies, transient controls, and in-use objects.
Oversized or insufficient-evidence measurements cannot be reported as zero
deviation or as a pass.

## Range, Action, and Opponent Controls

- Independently enumerate small joint posteriors and compare both propagation
  and compatible sampling. Folded players remain dealt blockers. A bucket's
  averaged marginal ranges are not automatically its exact joint posterior.
- Treat uniform, chart-conditioned, and actual action-conditioned ranges as
  separate experiments. Missing public histories cannot supply the last one.
- Freeze controls/adversaries with opaque identities: existing chart baseline,
  uniform policy, previous policy, limp/call-heavy, tight, overfolding,
  aggressive, and legal nonstandard-size/all-in/short-raise/side-pot families.
  They are diagnostic controls, not independent GTO ground truth.
- Check the candidate's action support is contained in the deviation space or
  explicitly retain the original candidate as an available deviation. Never
  equate nearest-target projection with justified off-tree policy behavior.
- Rotate labels, button, stacks, ranges, statuses, pots and history together.
  Relative-position relabeling should preserve meaning; UTG and BTN need not
  play identical policies. Report every relative position separately.

## Predeclared Seeds and Statistical Rules

Separate training, engineering pilot, deviation learning, confirmation, and
final holdout seed sets; publish their identities/hashes. Confirmation/holdout
must not choose checkpoints or tune candidates. A revised candidate consumes
a fresh unspent holdout, not repeated trials against an already observed set.

Freeze confidence level, one-sided/two-sided tests, multiplicity across seats,
scenarios and candidates, and stopping rules before confirmation. Compute sum
intervals from each replica's sum, not by adding per-seat interval half-widths.
Repeated node visits are not independent hand samples.

## Threshold Review Before Training

Do not invent a passing number from existing candidate performance. The
independent standard must specify and justify:

- `epsilon_seat`: each seat's accepted quality/error criterion;
- `epsilon_sum`: whole-hand aggregate criterion;
- `delta_regression`: acceptable degradation relative to a frozen control;
- `interval_precision`: required statistical precision and confidence;
- numerical, search, range, abstraction and action-translation error budgets;
- uncovered rows, confirmation misses, ordinary-state fallback and deadline
  acceptance rules, without hiding missing normal feature support.

Pilot runs may choose variance estimates, sample counts and affordable search
budgets, but not move pass thresholds to accommodate the candidate. Every
threshold and its rationale must be frozen by an independent reviewer before
qualification training/evaluation. Until the guarantee-to-evidence relation,
thresholds and actual resource budget are resolved, this proposal remains
Proposed and cannot sign off the complete feature.

## Execution Ownership and Acceptance

Solver/benchmark owners supply independent math and bounded measurement;
protocol/River owners supply trusted public-state completeness; resident and
resolver owners supply qualified policies/ranges/deadlines; practice and
integration owners exercise the actual host. Existing native and Node
frameworks remain the primary gates; no disposable script counts as delivery.

Required next design checkpoints:

1. Audit/correct measurement semantics and independent oracle fixtures.
2. Define bounded learn/confirm stop semantics and incomplete-evidence outcomes.
3. Freeze the guarantee, justified thresholds, support matrix and budgets.
4. Review before campaign; run full pipeline on unseen cases and report all
   exclusions/failures rather than selecting favorable checkpoints.
5. Independent full-feature review only after every delivery task is closed.

Independent mathematical/source checkpoint by `/root/sixmax_runtime_audit`
on 2026-10-14: Approved for the negative control, hidden-world restriction,
folded-blocker interpretation and the explicit equivalence-marginalization
caveat. This approves the proposal's direction and accuracy only, not a
passing threshold, budget, benchmark implementation or six-max quality claim.
The proposal remains Proposed; actual native oracle fixtures and all earlier
qualification decisions remain required.

No live play, spending, large training, publication, deployment or changed
protocol/algorithm is authorized by this proposal. The evidence report must
separate proven invariants, empirical results, and unproven full-game claims.

## Independent Direction Review

Reviewer: `/root/sixmax_runtime_audit`, independently launched and not an
author of this proposal. Date: 2026-10-14. Outcome: direction Approved.
The reviewer accepted separate functional, quality and whole-decision gates,
information-set-consistent measurement, and explicit incomplete-evidence
outcomes. No blocking finding remained in that direction review.

This is not approval of numerical thresholds, the guarantee-to-evidence
relation, campaign budgets, policy quality, or full-feature completion. All
remain unresolved and this document stays Proposed. Empirical finite-menu
results cannot certify the complete integer-action game. Provider rules and
public-history completeness still require authoritative public evidence.
