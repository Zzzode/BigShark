# Decision Options: Full Build, Narrowed Build, or Retirement

Status: Current
Date: 2026-11-20
Scope: quantified option table for the one-time product decision on the
cap-3/cap-4/cap-6 serving lineage, requested after D-390 acceptance. This
page selects nothing; every option is priced from measured receipts so the
product owner can decide once. No engine source, cap configuration,
deployment, or push is involved, and no new long measurement was run.
Companion records:
[d390-scenario-a-cap6-receipts.md](d390-scenario-a-cap6-receipts.md)
(measured cap-6 cells),
[cap34-serving-decision.md](cap34-serving-decision.md) (D-380/D-382 rails),
[other-bucket-rootcause.md](other-bucket-rootcause.md) (structural cause).
All arithmetic is reproducible by
`../plans/ablation/d390-scenario-a/options-table.mjs`, which reads the
committed CSVs directly.

## 1. Frozen inputs and labeling rules

- Weights over the 692 pooled "other" hands (D-382 empirical): L1 0.0751,
  L2 0.2962, L3 0.6286; f_other = 692/2,400 = 0.288333; check-to-river
  lower branch 0.2216 BB/100.
- Every severity cell below is **measured** (20,000 iterations, Uniform
  arm): cap-4 from the D-382 receipts, cap-6 from the D-390 receipts.
- Cost cells are **measured-component sums**: per-ledger walls are
  measured, but cap-4 walls were taken at 28 threads and cap-6 at 18, so a
  homogeneous-thread campaign number is an unmeasured adjustment (the
  mixed-thread total is the best available and labeled accordingly).
- Campaign = per-class wall x 134,459 canonical classes; "100 slots" are
  parallel 24/7 jobs. The w1 check-to-river cap-4 component (0.4118 h/class)
  is common to every serving option and included in the totals.
- Cells with no measurement are labeled **unmeasured** rather than filled.

## 2. The options table (all quality cells measured)

| Option | L1 / L2 / L3 caps | Other bucket (BB/100) structural / convergence | End-to-end (BB/100) | <= 0.5 target | <= 1.0 hard rail | Per-class wall (h) | Campaign (machine-h / days @100 slots) | Saving vs full |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Full cap-6 | 6 / 6 / 6 | 0.1112 (0.0512 / 0.0600) | **0.3328** | PASS | PASS | 8.920 | 1,199,375 / 499.7 | - |
| Narrow b: L2+L3 cap-6 | 4 / 6 / 6 | 0.5801 (0.5198 / 0.0604) | **0.8017** | FAIL | PASS | 5.847 | 786,190 / 327.6 | 36.1% |
| Narrow a: L3 cap-6 only | 4 / 4 / 6 | 2.7485 (2.6921 / 0.0564) | **2.9701** | FAIL | FAIL | 3.375 | 453,840 / 189.1 | 65.2% |
| Baseline: all cap-4 | 4 / 4 / 4 | 7.0229 (6.9599 / 0.0630) | 7.2445 | FAIL | FAIL | 1.861 | 250,204 / 104.3 | 83.0% |
| Retire | no serving build | not applicable | not applicable | not applicable | not applicable | 0 incremental | 0 incremental | 100% |

The baseline row's 7.2445 reproduces the sealed D-382 end-to-end 7.2452 to
display precision (7e-4 rounding difference), cross-validating the
calculator. The D-390 three-ledger-only figure (8.508 h/class,
1,144,000 machine-h / 476.7 days) is the same full option without the
common w1 component; both conventions appear in the receipts.

### 2.1 Per-ledger structural / convergence detail (BB/100 contributions)

| Option | L1 structural / convergence | L2 structural / convergence | L3 structural / convergence |
| --- | --- | --- | --- |
| Full cap-6 | 0.0091 / 0.0039 | 0.0380 / 0.0184 | 0.0041 / 0.0377 |
| Narrow b | **0.4777** / 0.0043 | 0.0380 / 0.0184 | 0.0041 / 0.0377 |
| Narrow a | **0.4777** / 0.0043 | **2.2103** / 0.0144 | 0.0041 / 0.0377 |
| All cap-4 | 0.4777 / 0.0043 | 2.2103 / 0.0144 | 4.2719 / 0.0443 |

## 3. Option narratives

### 3.1 Full cap-6 (measured pass of both rails, highest cost)

The only measured configuration under the 0.5 target. Headroom is 0.1672
BB/100 to the target and 0.6672 to the hard rail (95% CI upper bound
0.3398, D-390). Marginal-value question the product owner must price:
versus narrow b, the extra 413,185 machine-hours (172.2 days on 100
slots) buys 0.4689 BB/100 of end-to-end quality (0.8017 -> 0.3328),
almost entirely the elimination of L1's cap-4 structural term. At full
cap-6 the residual is nearly balanced between structural (0.0512) and
convergence (0.0600) components, so unlike cap-4 it is not purely
structure-bound; further iteration is plausibly still useful. All eight
D-390 open items (single leaf, no BrFavorable cap-6, cap-0-conditional
frequencies, four SRP ledgers, pot-16 extrapolation hull, heads-up only,
iteration freeze, no online timing) qualify the pass.

### 3.2 Narrow b: L2+L3 cap-6, L1 stays cap-4 (measured hard-rail pass)

End-to-end 0.8017 passes the 1.0 hard rail with 0.1983 margin but fails
the 0.5 target by 0.3017. The failure is a single, named cell: L1's
cap-4 structural contribution 0.4777 BB/100 (91.9% of this option's
structural total); its convergence component is negligible (0.0043).
Cost 786,190 machine-h / 327.6 days, 36.1% under full. This is the
cost/quality knee if the 1.0 hard rail is the binding contract and 0.5 is
treated as a stretch target; it is non-compliant if 0.5 is binding.

### 3.3 Narrow a: L3 cap-6 only (measured failure of both rails)

End-to-end 2.9701, 3.0x over the hard rail. L2's untouched cap-4
structural term (2.2103 BB/100) dominates. The 65.2% saving does not buy
a releasable configuration; retained in the table only to show the
boundary.

### 3.4 Retirement (no new build; conclusions retained at zero marginal cost)

No incremental campaign machine-hours; the sunk D-370..D-390 bench work
stays as evidence. The certification-rail research line remains exactly
as the terminal-state record states: R1-R4 restart criteria open with no
candidate, the RFC 0047-0050 campaigns sealed no-go, and no priced
research path exists. External commitment changes: (1) no online
engine-serving quality promise is made on this lineage; (2) the
"certification rail not passed" wording is unchanged and still applies;
(3) the offline engine, artifacts, and practice table continue with no
GTO online-opponent claim; (4) the measured BB/100 evidence remains
available for any future re-opening without re-running the probes.

## 4. Suggested fourth path (recommendation input, not a product decision)

**Targeted structural engineering on L1 plus coverage-sequenced rollout,
instead of choosing one uniform cap for every ledger.**

Premise from the measured table: L1 cap-6 consumes 42.3% of the
aggressive-line wall (3.598 of 8.508 h/class) for only 0.0130 BB/100 of
residual, while it is the sole blocker preventing narrow b from reaching
0.5. Uniform cap stacking charges a 6.85x wall premium on L1
(cap-4 0.525 h -> cap-6 3.598 h) to remove a structural term that the
RFC 0048 Path B evidence expects to concentrate at reach-1.0 root nodes
(96% of removable menu value at roots, sealed on the cap-1 lineage). A
root-selective abstraction could in principle capture L1's structural
shrink at far fewer than 1,832 online nodes. Independently, the orbit
coverage curve lets the same build ship in deal-mass order: full cap-6 at
50% coverage is 482,988 machine-h / 201.2 days, at 80% 799,264 / 333.0
days, at 95% 1,089,152 / 453.8 days, each stage with identical per-class
quality and disclosed scope.

Unknowns, stated honestly:

1. No selective-abstraction instrument exists on these ledgers; building
   one is an abstraction-design change requiring its own RFC, not a bench
   flag. The cautionary sealed evidence is D195's 0.0276 BB fixed-policy
   floor even under perfect root widening (cap-1 era, different baseline).
2. Whether L1's structural term continues to shrink past cap-6 is
   unmeasured; the w1 D164 curve showed the cap-5 -> cap-6 margin
   collapsing to 0.84x on the check-to-river ledger, while the aggressive
   ledgers still showed 4-5x steps at 3,000 iterations — the two lineages
   disagree, so no extrapolation is defensible without a data point.
3. Coverage sequencing inherits every D-390 open item per served class
   and adds a partial-coverage communication obligation.

Minimal validation (proposed, not launched; no long run authorized here):
one single-arm L1 cap-7 probe at 3,000 iterations through the existing
ledger-embed probe, expected ~20-30 minutes and within the 1 GiB envelope
(cap-6 peak was 335 MB). It discriminates between "uniform cap
continuation closes L1 cheaply" (then full cap-6 is simply built) and
"the margin plateaus" (then a selective-abstraction RFC is the only route
to narrow b's cost at full cap-6 quality).

## 5. Recommendation input (advice, not the decision)

If the 0.5 target is contractually binding, full cap-6 is currently the
only measured compliant option and the choice reduces to accepting its
499.7-day, 1.2M machine-hour campaign or retiring. If 1.0 is the binding
gate, narrow b is the measured cost/quality knee at 327.6 days. The
L1 cap-7 discriminator in section 6 has now returned Rule A: uniform
widening structurally closes L1, so no selective-abstraction RFC is
needed for feasibility, but cap-7 costs 1.364x per iteration and gives
no serving-rail benefit over the already-passing full cap-6. The live
decision is therefore among the priced options in sections 2-4 plus the
section-6.4 finding that the menu-width question is engineering, not
fundamental. The product-direction choice among full, narrowed, and
retired remains with the product owner; this page only attaches measured
prices to each.

## 6. L1 cap-7 discriminator: Rule A confirmed on the authorized rerun

The section-4 minimal validation was pre-registered
(`../plans/ablation/d390-scenario-a/l1-cap7-discriminator-prereg.md`,
rules and the second-timeout contingency written before any run). The
first attempt under a 2,400 s guard tripped RC=124 with header-only output
(receipts retained as `*-attempt1-rc124.*`); one rerun was then authorized
with the identical configuration and a 4,200 s guard, which completed
(RC=0, empty stderr, 44.2 min wall, 37% guard headroom).

### 6.1 Measured cap-7 row (L1, 3,000 iterations, 18 threads, Uniform)

| Quantity | cap-6 @3k (same-iteration baseline) | cap-7 @3k (measured) | Movement |
| --- | --- | --- | --- |
| Online action nodes | 1,832 | 2,760 (35.3% of cap-0) | +50.7% |
| Bounded NC | 0.05480831 | 0.05475901 | -0.09% (flat) |
| Pure-menu structural term | 0.00939246 | **0.00001349** | **696.3x smaller** |
| e_embed | 0.06420077 | 0.05477250 | 1.172x smaller |
| Structural share of e_embed | 14.6% | 0.025% | composition flip |
| Peak RSS | 335 MB | 491 MB | +156 MB, 533 MB headroom under 1 GiB |
| Wall per iteration (18 threads) | 647.6 ms | 883.5 ms | 1.364x premium |

The frozen identity holds exactly (NC + pure_menu = e_embed).

### 6.2 Pre-registered verdict: Rule A, not B, not the middle zone

r = pure_menu(cap7) / pure_menu(cap6) = 0.001436, decisively below the
Rule-A threshold r <= 0.25: the aggressive-line 4-5x per-step shrink
repeats and is vastly exceeded at 696x. Rule B (r >= 0.84, plateau) is
not met, and the middle zone is not entered. The cap-7 pure_menu is 148x
below M = 0.002 but 1,349x above the 1e-8 exact-zero predicate, so the
correct label is **near-zero structural term, not a certified exact
menu**. The residual's nature flips on this tree too: 99.975% of cap-7
e_embed is bounded NashConv, a convergence problem iteration count
attacks, not a menu problem.

### 6.3 Same-ruler marginal comparison

Quality (L1 structural bucket, equal 3k): 0.010169 -> 0.000015 BB/100,
removing 0.010155 BB/100 of structural loss. Cost: the matched-thread
premium is 1.364x per iteration, inside the anticipated 1.3-1.6x band; a
linearly projected 20k L1 cap-7 build is about 4.91 h/class versus
cap-6's measured 3.60 h. Beside the section-2 trade (0.4689 BB/100 per
413,185 machine-hours for L1 cap-4 -> cap-6), cap-7 buys structural
exactness rather than serving-rail economy: a narrow-b-plus shape (L1
cap-7, L2/L3 cap-6) projects to 10.23 h/class / 1.38M machine-hours /
573.2 days, which is 14.7% MORE expensive than uniform full cap-6
(8.92 h/class), because the premium lands on the largest tree.

### 6.4 Consequence for the fourth path and the two rails

- A selective-abstraction RFC is **not required to close L1
  structurally**: uniform widening one step past cap-6 collapses the term
  to near-zero, so the 172.2-day full-vs-narrow-b difference is a
  menu-width cost choice, not evidence of a fundamental abstraction
  limitation. The RFC would only be revisited if campaign cost, not
  structural feasibility, becomes the binding objection.
- Serving rail: unchanged. Full cap-6 already passes at 0.3328 BB/100;
  cap-7 adds no measured serving-rail benefit and costs more.
- Certification rail: still failed, and cap-7 does not rescue it. At 3k
  its e_embed is 0.0548 (27.4x over M), all convergence. Applying the
  measured cap-6 L1 15.2x 3k -> 20k NashConv improvement as a labeled
  projection puts cap-7 20k e_embed near 0.0036, still about 1.8x over
  0.002. Certification remains governed exclusively by R1-R4.
- The 1,500-iteration paired short-run contingency in the pre-registration
  was not triggered: the rerun produced a full measured row, so no third
  long run and no contingency instrument were launched.

## 7. Pot>16 extrapolation hull: measured 20k restores the pass, with a correction

The D-390 headline flagged 42.8% of the other bucket (296 hands at
practice pots 18-40) as an extrapolation charged at the pot-16 severity.
Two pre-registered de-risk runs target the largest decision-bearing
ledger above the hull (SRP family_index 4, pot 32, cap-6, 18 threads,
Uniform); the rules in `../plans/ablation/d390-scenario-a/pot-gt16-prereg.md`
were locked before any run and never edited.

### 8.1 Arithmetic correction (logged before the 20k launch)

The first remap dropped 139 hands: L3's correct post-remap count is 308
(139 hands at practice pots <=16 plus 169 of the beyond-16 hands), not
169. Correct weights over the 692 other hands are L1 52 / L2 205 /
L3 308 / pot-32 127. The earlier raw-3k end-to-end of 0.6270 is
superseded; with corrected weights the 3k-severity end-to-end is
**1.0214**, which fails BOTH rails. This correction stands independently
of the 20k result and is part of the audit trail.

### 8.2 The two measured points and the locked Rule B

| Point | Iterations | bounded NC | pure_menu | e_embed | vs locked band [0.004030, 0.004738] | Rule B (>0.007107) |
| --- | --- | --- | --- | --- | --- | --- |
| pot-32 3k | 3,000 | 0.12080013 | 0.00002653 | 0.12082666 | 25.5x over upper edge | **triggered** (17.0x threshold) |
| pot-32 20k | 20,000 | 0.00319828 | 0.00002709 | **0.00322537** | outside, 0.80x the LOWER edge | not triggered |

At 20,000 iterations e_embed lands just below the pre-registered band and
well under the Rule-B threshold, so the correct reading is **convergence
closure of a 3,000-iteration outlier**, not a threshold relaxation: the
band and rules were unchanged. Both structural terms are near-zero
(0.84% of the 20k e_embed), consistent with the cap-7 L1 result — there
is no menu wall on this ledger at any iteration count.

### 8.3 Family convergence non-transfer, now a modeling premise

Pot-32's bounded NashConv improves 37.77x from 3k to 20k (e_embed 37.46x
overall), versus the measured limped-family range of 6.39-15.17x. The
limped-only log-linear model failed specifically because SRP trees start
less converged at 3k and then improve faster; the non-transfer is real
and directional. **Residual models must therefore carry family-specific
convergence factors as a premise** — a single cross-family iteration
curve is not defensible, and any future band fit needs at least one SRP
training point.

### 8.4 Two end-to-end conventions and the final rail position

- Measured 20k replacement (corrected weights): other bucket 0.1076,
  **end-to-end 0.3292 BB/100** — passes the 0.5 target with 0.1708
  margin and the 1.0 hard rail; this is a fully measured cell, not a
  projection. The D-390 "conditional" label on the 0.3328 headline is
  therefore removed and replaced by this slightly lower measured value.
- Retained 3k severity convention (corrected weights): end-to-end
  1.0214, failing both rails. It is retained only as the evidence that
  the serving count 20,000 is load-bearing on this ledger; it is not a
  serving configuration.
- The residual unmeasured hull is pot 40 alone (37 hands, 5.3% of the
  other bucket); every other cell is now measured at 20,000 iterations.

### 8.5 Final route evaluation

- **Full cap-6:** restored to a measured pass at 0.3292 BB/100; campaign
  cost is unchanged from section 5 because the pot-32 ledger was already
  inside the eleven-ledger build scope. The route's remaining open items
  are the eight D-390 list entries minus this hull, now six.
- **Narrow b:** still fails 0.5 on its L1 cap-4 cell (0.8017); the pot-32
  result neither rescues nor worsens it.
- **Retirement:** loses the marginal support the provisional hull gave
  it: the largest-pot measured cell converges inside both rails, so no
  measured ledger now exhibits an unbounded or structural-only gap.

## 8. Pot-40 final hull: no ledger exists, residual bounded by proxy

The last named hull is 37 practice hands at river pot 40 (1.542% of
dealt hands, 5.35% of the other bucket). A pre-registered classification
framework (`../plans/ablation/d390-scenario-a/pot40-availability.md`,
rules written before checking availability) first asked whether a
runnable ledger exists. It does not: the sealed eleven-ledger census has
no pot-40 decision-bearing entry — only the two pot-400 terminal all-in
ledgers with zero action nodes — and the ledger probe can only address
enumerated family/index pairs, so no probe is launched and the pot-32
value is not substituted.

The 37 hands are live decisions (all 37 are `all_in = 0`: 32 showdown,
5 fold), so structural-zero treatment does not apply. They are an
enumeration-coverage gap, not a measured cell. Their current weight in
the 0.3292 headline is a disclosed nearest-ledger (pot-32) proxy worth
0.00249 BB/100. Labeled conservative bounds, all built from measured
severities, keep both rails: excluding the hands gives 0.3267; charging
them at the worst measured 20k cell gives 0.3369; at the worst measured
3k cell gives 0.4198, still 0.0802 inside the 0.5 target. No true
structural ceiling is derivable without extending the frontier
enumeration, an engine/harness change outside the current boundary.

Final scope statement: the 0.3292 BB/100 result is fully measured over
655 of 692 other hands (94.65%); the remaining 5.35% is unmeasured,
non-terminal, and proxy-bounded with every measured proxy passing both
rails. The single open instrument item is a pot-40 decision-bearing
ledger, which requires harness work rather than another bench run.

## 9. Reproducibility and receipts

- Calculator and output:
  `../plans/ablation/d390-scenario-a/options-table.mjs` and
  `options-table.csv` (deterministic; reads D-382/D-390/D-370 CSVs;
  the cap-7 row is the measured discriminator-single-point-MEASURED-3k
  cell, with the RC=124 attempt retained as separate attempt-1 files).
- Source receipts: `../plans/ablation/d382-other-river-class/`,
  `../plans/ablation/d390-scenario-a/` (including
  `l1-cap7-3000.csv`, the `*-attempt1-rc124.*` evidence, and
  `l1-cap7-discriminator-prereg.md`),
- Pot>16 hull evidence: `../plans/ablation/d390-scenario-a/pot-gt16-prereg.md`,
  `pot32-srp4-cap6-3000.csv` (Rule B at 3k) and
  `pot32-srp4-cap6-20000.csv` (measured closure), with runner time files.
- Final hull availability and bounds: `../plans/ablation/d390-scenario-a/pot40-availability.md`.
  `../plans/ablation/rfc0049/gate2/d370-w1-identity-cap4-embed-20000.csv`.
- No cell in this page uses a projected severity; projected labels apply
  only to the section-4 design proposal and the mixed-thread cost caveat.
