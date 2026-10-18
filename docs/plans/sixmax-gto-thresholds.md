# Six-Max GTO Qualification Thresholds

Status: Proposed

This document specifies the numerical thresholds and error budgets required by
the [qualification proposal](sixmax-gto-qualification.md). It is not an
approved quality standard, a passing report, or a six-max completion claim.
The complete feature remains In Progress in the
[delivery register](sixmax-gto-delivery.md).

## Derivation Principles

The thresholds are derived from poker theory and statistical practice, not
from existing candidate performance. The qualification proposal explicitly
prohibits inventing a passing number from measured candidate results. The
justification for each threshold comes from:

1. **Poker AI literature**: exploitability conventions from published
   near-equilibrium poker systems.
2. **Statistical theory**: what precision is achievable with reasonable sample
   sizes and variance, with proper multiplicity control.
3. **Practical constraints**: what is measurable within the project's compute
   and time budget.

## Quality Thresholds

### `epsilon_seat`

**Proposed value: 0.05 BB/hand per seat (5 BB/100 hands).**

Justification: The poker AI literature uses sub-0.05 BB/hand exploitability
as the convention for "near-equilibrium" play. DeepStack (Moravčík et al.,
2017) reports heads-up exploitability below 0.05 BB/hand as its quality
target. For six-max play, the same per-seat threshold is a conservative
starting point: multiway games have more complex strategy spaces, but the
per-seat exploitability measure is directly comparable across seat counts.

This threshold is a convention, not a theoretical guarantee. It is the
maximum exploitability that still qualifies as "GTO-quality" rather than
"strongly exploitative." Strategies with lower exploitability are preferred;
this is a ceiling, not a target.

### `epsilon_sum`

**Proposed value: 0.15 BB/hand total across all six seats.**

Justification: The total exploitability is the sum of per-seat
exploitabilities against the joint best response (NashConv for enumerable
games, or summed learned-deviation gains for non-enumerable games). If each
seat meets `epsilon_seat = 0.05 BB/hand`, the naive sum is 0.30 BB/hand.

The threshold of 0.15 BB/hand is set at 3x `epsilon_seat`. In the worst
case, the total could approach 6x `epsilon_seat` (0.30 BB/hand) if all six
seats are independently exploitable at the maximum. However, in practice
the joint best response is weaker than the sum of independent per-seat
best responses because the responder must choose one action per information
set. The factor of 3 is an engineering tolerance that balances strictness
with practicality: it is strict enough to catch systematic quality issues
while acknowledging that six seats contribute independently. In the worst
case, the sum could approach 6x `epsilon_seat`; the 3x factor is chosen
because the learned-deviation method computes unilateral deviations (one
seat deviates against five frozen policies), where the joint best response
weakening does not apply, but the per-seat exploitability is typically
lower than the worst case.

### `delta_regression`

**Proposed value: 0.01 BB/hand (1 BB/100 hands) relative to a frozen control.**

Justification: When a new candidate policy is evaluated against a frozen
control (the previous qualified policy), the degradation must be small enough
that it does not materially affect the strategy's quality. A degradation of
0.01 BB/hand is 20% of `epsilon_seat`, which is within the noise of most
measurement campaigns. This threshold ensures that regressions are caught
without being so tight that normal iteration is blocked.

The comparison uses paired common random numbers (CRN) to reduce variance,
as required by the qualification proposal. The degradation is measured as the
candidate's signed gain minus the control's signed gain, with the confidence
interval computed from the paired differences.

### `interval_precision`

**Proposed value: Family-wise 95% confidence interval with half-width ≤ 0.01
BB/hand for per-seat measurements (per-seat 99.17% after Bonferroni
correction), and ≤ 0.02 BB/hand for the total (95%, single test).**

Justification: The confidence interval must be narrow enough to distinguish
"passes" from "fails" with reasonable certainty. If `epsilon_seat = 0.05`
and the interval half-width is 0.01, then a measured exploitability of 0.04
has a 99.17% CI of [0.03, 0.05], which is clearly below the threshold. A
measured exploitability of 0.06 has a CI of [0.05, 0.07], which is clearly
above. This gives a 0.01 BB/hand "gray zone" where the result is ambiguous.

For the total (`epsilon_sum = 0.15`), the half-width is 0.02, giving a
0.02 BB/hand gray zone. The total half-width is larger in absolute terms to
account for summed variance, but is tighter as a fraction of `epsilon_sum`
(13.3%) than the per-seat precision (20% of `epsilon_seat`).

The sample size required to achieve this precision depends on the variance
of the per-hand utility. Pilot measurements should estimate the variance and
compute the required sample size before the confirmation campaign.

## Statistical Rules

### Test type

**One-sided test.** The null hypothesis is `exploitability >= epsilon -
error_budget`; the alternative is `exploitability < epsilon - error_budget`.
Reject H0 to qualify. This formulation requires strong evidence that the
strategy is good (proven good), consistent with the pass condition
`measured + budget <= epsilon`.

### Multiplicity correction

**Bonferroni correction across all simultaneous tests.** The per-test
significance level is `alpha / N`, where `N` is the total number of
simultaneous tests across all seats and scenarios. With 6 seats and a single
scenario, `N = 6` and the per-seat significance level is `0.05 / 6 =
0.00833` (99.17% per-seat confidence). With 6 seats and 4 stack profiles,
`N = 24` and the per-test significance level is `0.05 / 24 = 0.00208`
(99.79% per-test confidence). The campaign must pre-declare the test count
before confirmation. For the total (one test), the confidence level remains
95%.

### Stopping rule

**Fixed sample size.** The sample size is computed from pilot variance
estimates before the confirmation campaign. The campaign runs until the
fixed sample size is reached, then stops. No optional stopping or
checkpoint-based selection is allowed.

If the pilot variance is too high to achieve the required precision with
an affordable sample size, the campaign reports this as a finding and
proposes either a variance reduction technique (e.g., more CRN pairing) or
a relaxed precision threshold (subject to independent review).

## Error Budgets

The error budget represents the gap between the measured exploitability in
the abstracted game and the full-game exploitability. The pass condition is:

```
measured exploitability + error budget <= epsilon_seat
```

The error budget is allocated as 40% of `epsilon_seat` (0.020 BB/hand),
leaving 60% (0.030 BB/hand) for the measured strategy's intrinsic
exploitability:

| Source | Budget (BB/hand) | Fraction of epsilon_seat |
| --- | --- | --- |
| Numerical (floating-point, iteration convergence) | 0.002 | 4% |
| Search (best-response approximation, depth limits) | 0.006 | 12% |
| Range (abstraction, bucketing, joint posterior) | 0.004 | 8% |
| Action abstraction (declared menu) | 0.008 | 16% |
| **Total** | **0.020** | **40%** |

Justification:
- **Numerical (0.002)**: CFR convergence at 100K iterations produces
  residual numerical error well below 0.002 BB/hand. Floating-point
  accumulation is negligible at this scale. This budget is verified by
  convergence studies (comparing 100K vs 200K iterations on representative
  fixtures).
- **Search (0.006)**: The depth-limited resolver with continuation values
  introduces approximation error at the depth boundary. This budget is an
  engineering tolerance to be verified by a convergence study comparing
  depth-1, depth-2, and depth-3 solves on the same fixtures (or depth-1
  against a much deeper solve on a subset of fixtures). It is not asserted
  from existing candidate performance.
- **Range (0.004)**: Range abstraction error must not exceed 8% of
  `epsilon_seat`. This budget is the maximum acceptable degradation from
  range abstraction, regardless of the current pipeline's characteristics.
  It is verified by comparing identity-abstraction vs bucketed-abstraction
  policies on representative fixtures.
- **Abstraction (0.008)**: Action abstraction error is the gap between the
  optimal strategy in the full integer-action game and the optimal strategy
  in the declared-menu game. This budget is the maximum acceptable
  degradation from action abstraction. It is verified by comparing
  declared-menu vs wider-menu strategies on representative fixtures.

### Action-translation error

Off-tree action mapping and rounding introduce error when the engine
encounters a legal action not in the declared menu. RFC 0025 explicitly
states that no certified bound exists for off-tree resolution error.

**No pre-assigned budget is claimed.** Instead, the qualification campaign
must:
1. Measure off-tree resolution error on held-out off-tree scenarios.
2. Report the measured error as a quality metric.
3. If the measured error exceeds 0.010 BB/hand (20% of `epsilon_seat`),
   report it as a finding and propose mitigation (e.g., wider declared
   menu, better off-tree mapping). Exceeding 0.010 BB/hand is not a
   campaign failure: the measured exploitability already captures off-tree
   effects when driving the production host. The threshold is a reporting
   trigger, not a pass/fail criterion.

This approach acknowledges the RFC 0025 limitation while still requiring
the campaign to quantify the error.

### Sampling error

The Monte Carlo variance in the learned-deviation measurement is distinct
from the confidence interval (which measures the sampling error of the
confirmation, not the deviation learning itself).

**The sampling error is subsumed by `interval_precision`.** The
confirmation campaign's sample size is computed to achieve the specified
confidence interval half-width, which accounts for the Monte Carlo
variance. No separate sampling error budget is needed.

## Coverage and Fallback Rules

### Uncovered rows

A strategy row (information set) is "uncovered" if the trained policy has no
action distribution for it. The acceptance rule is:

- **Zero uncovered rows** for information sets reachable by legal play in
  the full six-max game. Verification is by construction of the training
  pipeline, which produces a distribution for every reachable information
  set. For enumerable fixtures, this is verified by exhaustive enumeration;
  for non-enumerable games, it is verified by sampling.
- The declared action menu must cover all ordinary states (states reachable
  by legal play). If the declared menu is narrower than the full legal action
  space, the campaign must measure and report the coverage gap as a quality
  metric.

### Confirmation misses

A "confirmation miss" occurs when the learned deviation's confirmation
interval includes zero (no significant improvement found). The acceptance
rule is:

- Confirmation misses are **not failures**. They indicate that the current
  policy is already near-optimal for that seat/information set, which is
  expected for a converged CFR strategy.
- Any confirmation miss is reported. The campaign assesses whether the
  deviation search space is adequate. If more than 1 in 6 seats (17%) has
  a confirmation miss, the campaign reports this as a finding: it may
  indicate that the deviation search space is too narrow or that the policy
  has converged.

### Negative within-menu gains

Negative within-menu gains must be reported as-is, without clipping. A seat
with a statistically significant negative within-menu gain indicates an
incomplete deviation action space and is a finding, not a pass.

### Ordinary-state fallback

The engine's operational fallback (check/call/fold) is used when the engine
cannot produce a decision. The acceptance rule is:

- **Zero fallback decisions** in ordinary states (any state reachable by
  legal play in the full six-max game). A fallback in an ordinary state is
  a failure.
- Fallback in exceptional states (transport failure, process crash, timeout)
  is acceptable and must be reported separately. These are not strategy
  failures.

### Deadline acceptance

The engine must produce a decision within the platform's deadline. The
acceptance rule is:

- **99th percentile decision time ≤ 50% of `timeLeftMs`** on the target
  table clock, for the complete receipt-to-action pipeline (validation,
  reconstruction, range propagation, tree construction, loading/lookup,
  solve, extraction, response, adapter legality check).
- **100th percentile decision time ≤ `timeLeftMs - safetyMs`** where
  `safetyMs` is the runner's configured safety margin.
- Decisions that exceed the deadline are counted as fallback decisions and
  are subject to the ordinary-state fallback rule.

The River Club platform's deadline is `room.timeLeftMs`, which varies by
table. The CLAUDE.md documents clocks of 1.2-25+ seconds. The 50% threshold
ensures the engine has sufficient margin for network latency and adapter
overhead.

## Guarantee-to-Evidence Relation

The qualification proposal requires a clear relation between the guarantee
sought and the evidence supplied. The relation has two tiers, reflecting
the two measurement methods:

### Tier 1: Enumerable fixtures

For enumerable fixtures (small games where the pooled best-response oracle
can compute exact NashConv):

**Guarantee**: The strategy's per-seat NashConv is ≤ `epsilon_seat` and its
total NashConv is ≤ `epsilon_sum`.

**Evidence**: The measured NashConv from the pooled best-response oracle,
with confidence intervals at the specified `interval_precision`.

**Relation**: If the measured NashConv (upper confidence bound) + error
budget ≤ `epsilon_seat` per seat and ≤ `epsilon_sum` total, then the
strategy meets the guarantee for that fixture. The total's upper confidence
bound is computed from per-replica sums (not by adding per-seat interval
half-widths), per the qualification proposal's requirement. The total error
budget is the sum of per-seat error budgets (6 × 0.020 = 0.120 BB/hand).

### Tier 2: Non-enumerable games

For non-enumerable games (the full six-max game, where exact NashConv is
intractable):

**Guarantee**: The learned-deviation gain is statistically indistinguishable
from zero within the specified precision, meaning no opponent can find a
significant deviation against the strategy.

**Evidence**: The learned-deviation gain with confirmation interval, measured
on independent hands. The upper confidence bound of the learned-deviation
gain is reported.

**Relation**: If the upper confidence bound of the learned-deviation gain +
error budget ≤ `epsilon_seat` per seat and ≤ `epsilon_sum` total, then the
strategy meets the guarantee. The total's upper confidence bound is computed
from per-replica sums (not by adding per-seat interval half-widths), per the
qualification proposal's requirement. The total error budget is the sum of
per-seat error budgets (6 × 0.020 = 0.120 BB/hand). This is not a
certificate of the true exploitability: the confirmation interval measures
only the sampling error of the learned, frozen deviation, not the unknown
optimal best response. The qualification proposal explicitly warns that the
upper endpoint is not an upper certificate.

### Evidence validity

The evidence is valid only if:
1. The measurement uses the pooled best-response oracle (Tier 1) or the
   learned-deviation + confirmation method (Tier 2), not the per-deal walker.
2. The measurement drives the production host (not a seam fake or practice
   policy).
3. The measurement covers all six positions and all streets.
4. The measurement includes unequal stacks, all-ins, side pots, and legal
   nonstandard bet sizes.
5. The error budgets are not exceeded.
6. The multiplicity correction is applied.

## Campaign Budget

The qualification campaign requires:

1. **Training**: 100K CFR iterations per fixture.
2. **Measurement**: Pooled best-response oracle on enumerable fixtures,
   learned-deviation + confirmation on non-enumerable games.
3. **Deviation learning**: Learned deviation on a dedicated seed set,
   separate from confirmation and holdout.
4. **Confirmation**: Confirmation on an independent seed set, separate from
   deviation learning and holdout.
5. **Holdout**: Final holdout with predeclared seeds, not used for training,
   deviation learning, or confirmation.

### Fixture classes

The campaign must cover the full qualification scope, not just the T3
feasibility scan. The fixture classes include:

- **Seat counts**: 2, 3, 4, 5, 6 seats
- **Stack profiles**: 10 BB, 25 BB, 100 BB, and unequal stacks
- **Streets**: Preflop, flop, turn, river (whole-hand, not just subgame)
- **Special rules**: All-ins, side pots, short raises, legal nonstandard
  integer targets

The T3 feasibility scan (15 fixtures, turn-rooted, equal stacks, 10 hands
per seat) is a lower bound on the campaign scope, not an upper bound. The
full campaign requires significantly more fixtures and larger ranges.

### Compute estimate

The compute budget is not yet finalized. It depends on:
- The number of fixture classes (estimated 50-100+)
- The range size per seat (the T3 scan's 10 hands is a probe, not a
  qualification range; qualification ranges may require 100+ hands per seat)
- The whole-hand vs subgame measurement cost
- The confirmation/holdout hand counts with variance estimates from pilot
  runs

A pilot campaign should estimate the compute requirements before the full
campaign is authorized.

## Independent Review

This threshold proposal requires independent review before it can be frozen.
The reviewer must:
1. Verify that the thresholds are justified by poker theory and statistical
   practice, not by candidate performance.
2. Verify that the error budgets are realistic and that the pass condition
   (`measured + budget <= epsilon`) is mathematically satisfiable.
3. Verify that the coverage and fallback rules are complete and do not hide
   missing feature support.
4. Verify that the guarantee-to-evidence relation is sound and acknowledges
   the qualification proposal's warnings.
5. Verify that the statistical rules (test type, multiplicity, stopping) are
   complete.
6. Approve or reject the proposal with specific findings.

The reviewer must be a separately launched agent that did not author this
proposal. User confirmation is not required for threshold approval.

## Revision History

- 2026-10-17: Initial draft.
- 2026-10-17: Revised after first independent review (rejected with 1 blocker,
  7 major, 8 minor findings). Changes:
  - Restructured error budget to 40% of `epsilon_seat` (B1).
  - Replaced candidate-performance justifications with theoretical bounds
    and measurement plans (M1, m5, m8).
  - Replaced action-translation budget with measurement requirement (M2).
  - Restructured guarantee-to-evidence relation as two tiers (M3, M7).
  - Justified `epsilon_sum` independently (M4).
  - Added statistical rules: one-sided test, Bonferroni, fixed sample (M6).
  - Expanded campaign budget to full qualification scope (M5).
  - Defined "ordinary state" independently of declared menu (m1).
  - Added negative within-menu gain reporting (m3).
  - Cited poker AI literature for `epsilon_seat` (m4).
  - Justified confirmation-miss threshold (m6).
  - Tied deadline to `timeLeftMs` (m7).
- 2026-10-17: Second independent review: Approved with 12 minor findings.
  Editorial fixes applied:
  - Updated `interval_precision` to state family-wise 95%, per-seat 99.17%
    after Bonferroni (N1).
  - Restated hypothesis test direction to match pass condition (N2).
  - Renamed table line to "Action abstraction (declared menu)" (N3).
  - Justified off-tree measurement threshold and clarified pass/fail
    implication (N4).
  - Added replica-sum requirement for total's upper confidence bound (N5).
  - Specified Bonferroni counting rule for multi-scenario campaigns (N6).
  - Updated search budget verification to compare three depths (N7).
  - Strengthened `epsilon_sum` justification with worst-case analysis (N8).
  - Specified error budget application to total (N9).
  - Corrected "proportionally the same" claim for total precision (N10).
  - Specified "zero uncovered rows" verification method (N11).
  - Separated deviation-learning and confirmation seed sets (N12).
