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
candidate next action was the L1 cap-7 discriminator in section 4; its
first run is wall-inconclusive under the 40-minute guard (section 6), so
the live choice is authorizing the 3,600 s rerun named there or deciding
among the priced options in sections 2-4. The
product-direction choice among full, narrowed, and retired remains with
the product owner; this page only attaches measured prices to each.

## 6. L1 cap-7 discriminator: wall-budget gap, no verdict forced

The section-4 minimal validation was pre-registered
(`../plans/ablation/d390-scenario-a/l1-cap7-discriminator-prereg.md`,
rules written before launch) and run once:
`ledger-embed-probe limped 1 3000 18 7 uniform` under a 2,400 s (40 min)
outer guard with a 1 GiB RSS hard line. Result: the guard tripped
(exit 124) with only the probe header written, no data row, and empty
stderr. Per the standing "stop and report the gap, do not force it, do
not add iterations" instruction, no rerun was launched.

Measured facts (cost axis only; no quality reading exists):

- The cap-7 L1 tree builds and the process enters the solve; build-phase
  RSS observed around 362 MB (cap-6 peak was 335 MB), so memory is not
  the binding constraint and the 1 GiB envelope is not threatened.
- The 3,000-iteration solve does not finish in 2,400 s, so cap-7
  per-iteration wall is at least 800 ms versus cap-6's measured
  648 ms/iter at the same 18 threads — at least 1.24x the per-solve
  cost, possibly higher (how far the run progressed is unobservable;
  the embed path emits no intermediate checkpoints).
- Pre-registered rule A needed pure_menu <= 0.00234812 and rule B
  needed >= 0.00788967; with no pure_menu reading neither is
  confirmable. The honest verdict is **inconclusive / unmeasured at
  this budget**, not A and not B.
- Estimate correction: the original "20-30 minute" expectation was
  anchored on the small L3 tree (cap-6 3k = 676 s at 28 threads). L1
  is the largest tree (7,824 cap-0 nodes, 1,832 cap-6 online nodes);
  its cap-6 3k-equivalent at 18 threads is already about 1,943 s
  (32.4 min), so the 40-minute guard was tight even for cap-6.

Consequence for the fourth path. The cost-axis evidence points against
"uniform widening is cheap" — cap-7 already fails a wall budget that
cap-6 barely fits — but the quality-axis question (does the structural
term keep shrinking 4-5x or plateau at 0.84x?) remains open, so neither
uniform stacking nor a selective-abstraction RFC is confirmed. The
smallest unblocking measurement, proposed but not launched, is the
identical configuration with a 3,600 s guard (estimated need 2,525-3,115 s
if the per-iteration premium is 1.3-1.6x); it remains a single local run
inside the memory envelope. The 172.2-day full-vs-narrow-b price
difference therefore stays an open structural question pending that
authorization.

## 7. Reproducibility and receipts

- Calculator and output:
  `../plans/ablation/d390-scenario-a/options-table.mjs` and
  `options-table.csv` (deterministic; reads D-382/D-390/D-370 CSVs;
  the cap-7 GAP row is labeled discriminator-single-point-GAP-RC124).
- Source receipts: `../plans/ablation/d382-other-river-class/`,
  `../plans/ablation/d390-scenario-a/`,
  `../plans/ablation/rfc0049/gate2/d370-w1-identity-cap4-embed-20000.csv`.
- No cell in this page uses a projected severity; projected labels apply
  only to the section-4 design proposal and the mixed-thread cost caveat.
