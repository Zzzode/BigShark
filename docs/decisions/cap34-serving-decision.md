# Decision Support: Cap-4 Serving Menu and the Cap-3 Fallback

Status: Current
Date: 2026-11-19
Scope: D-380 decision support for the product-selected serving range
"support cap-3 to cap-4". All numbers come from bench receipts and offline
analysis tooling under `../plans/ablation/cap34-table-level/`; no engine,
sealed instrument, protocol, or existing contract number was changed, and
nothing is mounted, deployed, or pushed. The D-380 decisions are quoted,
not rewritten: certification rail unchanged at <=0.002 BB per decision;
new serving rail at <=1.0 BB/100 table-level residual (target <=0.5);
serving menu cap-4 with cap-3 as fallback only; serving iteration count
frozen at 20,000 (changing it requires re-measuring the menu term).
Supersedes the D-371 version of this document with the precise BB/100
calculation and coverage-cost curve; companion to
[cap1-table-level-expectation.md](cap1-table-level-expectation.md), whose
55.20 BB/100 figure is retained unchanged as the cap-1 comparison.

## 1. Two-track verdict (separate statements, not merged)

- **Certification rail: NOT PASSED.** Cap-4 converged e_embed is 0.0794 BB
  against the frozen 0.002 BB per-decision gate — about 40x over (39.7x at
  20,000 iterations; 39.0x at the 6,000-iteration floor). This sentence
  stands regardless of the serving-rail result below.
- **Serving rail: PASSED ON MEASURED CLASSES, CONDITIONAL ON THE UNMEASURED
  RIVER CLASS.** The precise table-level expectation for cap-4 at 20,000
  iterations is **0.2216 BB/100** (95% interval 0.185-0.258) on the same
  lower-branch discipline as the cap-1 55.20 certificate — inside the
  0.5 target and far under the 1.0 hard rail. The proxy upper band that
  charges the unmeasured `other` river class at the w1 severity is
  1.37 BB/100, above the hard rail; the rail cannot be closed end-to-end
  until that class is measured (section 6).

## 2. Converged cap-3 / cap-4 quality side by side

Configuration: w1 deep check-to-river anchor (pot 4 chips, big blind 2,
full 1,081-combo ranges), identity(k) menu, seedless linear-averaging RM+,
28 threads, 20,000 iterations, D164 `EmbeddedGame` evaluator, frozen
identity `e_embed = bounded NC + pure_menu`. BB values are two-seat sums;
per-hand values divide by two for the single engine seat.

| Quantity (Uniform arm) | cap-3 @20,000 | cap-4 @20,000 | cap-4 @6,000 (measured floor) |
| --- | --- | --- | --- |
| Action nodes | 106 | 320 | 320 |
| Bounded NC (policy's own error) | 0.00340111 | 0.00714721 | 0.01205936 |
| Pure-menu structural term | 0.37410857 | 0.07224095 | 0.06596689 |
| e_embed (total exact exploitability) | 0.37750969 | 0.07938816 | 0.07802624 |
| e_embed vs 0.002 certification gate | 188.8x over | 39.7x over | 39.0x over |
| Per-player loss per dealt hand | 0.18875 BB | 0.03969 BB | 0.03901 BB |
| Share of the 2 BB anchor pot | 9.44% | 1.98% | 1.95% |
| **Precise table-level BB/100 (lower branch)** | **1.0539** | **0.2216** | **0.2178** |
| Proxy upper band BB/100 (other = w1 severity) | 6.4963 | 1.3661 | 1.3427 |
| BrFavorable adversarial BB/100 (lower branch) | 44.2482 | 13.1937 | 13.2942 |
| Peak RSS | 35.1 MB | 63.7 MB | 63.5 MB |

cap-4 is 4.76x better than cap-3 on converged exploitability at 3.08x the
per-anchor solve cost; cap-3's own lower-branch BB/100 (1.0539) already
exceeds the 1.0 serving hard rail, which is the numeric basis for its
fallback-only role under D-380. The 6,000-iteration cap-4 build is
marginally better than the 20,000 one on total exploitability and costs
roughly one third, but D-380 freezes the serving count at 20,000
(section 5); the 6,000 column is retained as evidence, not as a serving
configuration.

## 3. Precise table-level BB/100 — method and sources

The D-380 audit found the working estimate `(55.20 / 31.19) x 0.0794
~= 0.14 BB/100` unfit for delivery: it mixes two measurement scopes. The
31.19 BB divisor is the cap-1 **union-reference** w1 two-seat value (it
also prices missing bet-sizing actions), while 55.20 is built from the
**T6 direct cap-0 reference** per-seat limped mean (9.8861 BB). The
precise calculation reuses the D-346 frequency machine unchanged and
substitutes the cap-3/cap-4 per-occurrence severity at the same scope.

Method (identical identity to
[cap1-table-level-expectation.md](cap1-table-level-expectation.md)
sections 2.1 and 5.1):

```
per-hand loss = P(limp_ctr) x (e_embed / 2)
BB/100        = 100 x per-hand loss
```

- `P(limp_ctr) = 134 / 2,400 = 0.0558333`, the pooled passive frequency
  over the two independent 1,200-hand campaigns (sealed
  `both-summary.csv`; the pooled passive corpus has zero SRP
  check-to-river hands by construction, so no second term appears).
- `e_embed / 2` converts the two-seat summed exploitability to the
  per-engine-seat loss, exactly as table-expectation.mjs halves the T6
  values. The cap-3/cap-4 EmbeddedGame evaluator is a direct cap-0
  reference, matching the T6 scope.
- The lower branch charges zero for the unmeasured `other` river class
  and treats all-in runouts (14.75% of hands) as structurally zero, the
  same discipline as the 55.20 certificate.

Results and the crude-factor audit (full table in
`../plans/ablation/cap34-table-level/cap34-table-expectation.csv`):

| Build | Precise lower BB/100 | Crude-factor estimate | Ratio |
| --- | --- | --- | --- |
| cap-4 @20,000 | 0.2216 | 0.1405 | 1.58x |
| cap-4 @6,000 | 0.2178 | 0.1381 | 1.58x |
| cap-3 @20,000 | 1.0539 | 0.6681 | 1.58x |

The deviation is 1.58x — under the 2x re-examination threshold D-380 set,
so the 1.0 BB/100 rail does not require revisiting on this evidence. The
1.58x factor is structural, not sampled: the correct implicit conversion
of a direct-scope two-seat value is `100 x 0.055833 / 2 = 2.7917` per BB,
while the crude factor 55.20/31.19 = 1.7698 is the union-scope conversion
misapplied to a direct-scope number.

Bands. The BrFavorable completion scales the cap-4 lower branch to
13.19 BB/100 and cap-3 to 44.25 BB/100 — adversarial envelopes in the
same role as the cap-1 document's 438-601 BB/100 rows. The proxy upper
band applies the w1 per-seat severity to the `other` river class
(28.83% of pooled hands, no cap-3/cap-4 receipt exists) purely to bracket
the unknown; it is a labeled assumption, not a measurement.

## 4. Coverage-cost curve over the 134,459 canonical classes

The single full-universe number "134,459 classes = 55,375 machine-hours"
is correct but unactionable. Each canonical river board class is an orbit
of concrete five-card boards under the 24 suit permutations; its orbit
size is its exact deal-frequency mass under a uniform river deal, the
same joint-deal normalization the seedless engine uses. The analysis
tool `board-orbit-coverage.mjs` re-derives every orbit with the ctest-
pinned canonicalization rule and passes three sealed invariants:
134,459 classes, total mass C(52,5) = 2,598,960, and 1,287 monochrome
classes each of orbit size 4. Only three orbit sizes exist:

| Orbit size (concrete boards per class) | Classes | Share of classes |
| --- | --- | --- |
| 4 | 2,301 | 1.71% |
| 12 | 48,503 | 36.07% |
| 24 | 83,655 | 62.22% |

Mass is therefore close to uniformly spread across the class universe —
there is no small hot subset that buys most of the coverage. Ranking
classes by descending deal mass and applying the measured per-anchor
walls (one w1-ledger solve per class, 28 threads; "100 slots" means 100
parallel 28-thread jobs, about 2,800 hardware threads 24/7):

| Deal-mass coverage | Classes needed | Share of universe | cap-3 20k mach-h / days | cap-4 20k mach-h / days | cap-4 6k mach-h / days |
| --- | --- | --- | --- | --- | --- |
| 50% | 54,145 | 40.27% | 7,254 / 3.02 | 22,299 / 9.29 | 6,965 / 2.90 |
| 80% | 89,609 | 66.64% | 12,006 / 5.00 | 36,904 / 15.38 | 11,527 / 4.80 |
| 95% | 122,096 | 90.81% | 16,358 / 6.82 | 50,283 / 20.95 | 15,706 / 6.54 |
| 99% | 130,760 | 97.25% | 17,519 / 7.30 | 53,851 / 22.44 | 16,821 / 7.01 |
| 100% | 134,459 | 100.00% | 18,014 / 7.51 | 55,375 / 23.07 | 17,297 / 7.21 |

Direct answers: half of all river deal mass needs 40% of the classes and
9.3 days of 100-slot compute at the cap-4 20k serving configuration;
95% needs 91% of classes and 21.0 days. The marginal 50%->95% step alone
adds 27,984 machine-hours (67,951 further classes), while the final
99%->100% point adds 1,524 machine-hours (3,699 classes): mass is
concentrated enough that the last percent is cheap, but not enough that
any small subset reaches high coverage. The cap-4 6,000-iteration build
tracks cap-3 20k cost at every level while retaining the 4.76x quality
advantage. Scope: this weights board classes at one betting ledger (the
w1 campaign line); a full eleven-ledger build multiplies by the ledger
structure, whose distribution is the opponent-action unknown in
section 6.

## 5. Iteration freeze at 20,000 and why

D-380 freezes the serving iteration count at 20,000; changing it requires
re-measuring the menu term. The measured evidence supports the freeze
because the structural term is non-monotone, so iteration count is a
quality-sensitive serving parameter rather than a tunable budget:

| cap-4 point | Iterations | Pure-menu term (BB) |
| --- | --- | --- |
| sealed Gate 2 | 1,500 | 0.06397012 |
| D-370 | 6,000 | 0.06596689 |
| D-370 | 20,000 | 0.07224095 |

The policy's own bounded NashConv keeps falling across the same range
(0.0341 -> 0.0121 -> 0.0071) while the menu term bottoms and rises:
convergence reveals the restriction, it does not price it away. No
interpolation across iteration counts is valid; every serving-count
change needs a fresh same-instrument menu-term receipt.

## 6. Unmeasured items and risks

1. **Real opponent reach frequencies.** Both the 55.20 cap-1 figure and
   the 0.22 cap-4 figure use frequencies from a mounted cap-0 resolver in
   the 2,400-hand passive-style campaigns. A cap-4 engine generates its
   own arrival distribution; qualitative direction (less fold pressure,
   higher river reach) is recorded in the cap-1 document but no
   cap-4-conditional frequency has been measured. The 0.22 BB/100 is the
   best available estimate, not a cap-4-certified rate.
2. **The `other` river class (28.83% of pooled hands).** No cap-3/cap-4
   per-occurrence measurement exists for rivers reached after flop/turn
   aggression, and no raised-srp (pot-8) cap-3/cap-4 anchor has been run.
   The proxy upper band (1.37 BB/100 for cap-4) is the quantified
   consequence; closing it requires one pot-8 cap-4 anchor and an
   aggressive-line anchor, each roughly one hour of bench time at
   20,000 iterations.
3. **Heads-up only.** Every severity and frequency here is two-seat. No
   multi-way BB/100, orbit weighting, or menu residual exists; 3+ seat
   trees have different menus, reaches, and normalizations.
4. **cap-5/cap-6 marginal benefit is unconverged.** The only readings
   remain the truncated Gate 2 rows (pure_menu 0.0402 BB at 562
   iterations, 0.00959 at 227; D164 cap-6 0.00610 at 1,000). The D-370
   branch-3 result pre-closed the conditional 30.6-hour cap-6 run; cap-6
   also carries ~802 MB RSS and ~8 s per iteration versus cap-4's 64 MB
   and ~74 ms.
5. **Single anchor and single evaluator.** All cap-3/cap-4 values are
   the w1 root; the 200-3,000-iteration anchors use reach_bound while
   6,000/20,000 use EmbeddedGame (same frozen identity, no
   shared-iteration bitwise equivalence receipt). Six-stratum and
   all-root coverage is unmeasured, and the campaign arithmetic assumes
   the w1 per-anchor wall is representative.

## 7. Relationship to the cap-1 decision records

[cap1-table-level-expectation.md](cap1-table-level-expectation.md) is
not rewritten or re-scaled: its 55.20 BB/100 (95% interval 46.1-64.3)
remains the sealed cap-1 table-level certificate under the cap-0-
conditional frequencies, and its raw campaign CSVs are the direct input
to section 3. The cap-4 0.22 BB/100 and cap-1 55.20 BB/100 share one
frequency machine and one per-seat convention, so their ratio (~249x
less table-level expected loss) is internally consistent; they differ in
per-occurrence severity (0.0397 vs 9.8861 BB per seat), not in weighting.
[cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md) and
[m2-certification-terminal-state.md](m2-certification-terminal-state.md)
keep their standing: nothing here satisfies restart criteria R1-R4, and
no `certified_bound` claim attaches to cap-3 or cap-4.

## 8. Receipts and reproducibility

- Severity receipts: `../plans/ablation/rfc0049/gate2/d370-w1-identity-cap4-embed-{20000,6000}.csv`
  and `d371-w1-identity-cap3-embed-20000.csv` (all RC 0, empty stderr).
- BB/100 tooling and output:
  `../plans/ablation/cap34-table-level/cap34-table-expectation.mjs`
  and `.csv` (deterministic; sealed frequencies and D-370/D-371
  severities only).
- Coverage tooling and output:
  `../plans/ablation/cap34-table-level/board-orbit-coverage.mjs`
  and `.csv` (~17 s, three sealed invariants asserted at run time).
- Frequency inputs: `../plans/ablation/cap1-table-level/both-summary.csv`.
