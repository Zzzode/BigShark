# Decision Support: Cap-4 Serving Menu and the Cap-3 Fallback

Status: Current
Date: 2026-11-19
Scope: D-380/D-382 decision support for the product-selected serving range
"support cap-3 to cap-4". All numbers come from bench receipts and offline
analysis tooling under `../plans/ablation/`; no engine solver, sealed
instrument, protocol, or existing contract number was changed, and nothing
is mounted, deployed, or pushed. The D-380 decisions are quoted, not
rewritten: certification rail unchanged at <=0.002 BB per decision;
serving rail at <=1.0 BB/100 table-level residual (target <=0.5); serving
menu cap-4 with cap-3 as fallback only; serving iteration count frozen at
20,000 (changing it requires re-measuring the menu term). D-382 closed
the unmeasured `other` river class with four aggressive-line anchors and
its pre-registered rule withdrew the serving release. Companion to
[cap1-table-level-expectation.md](cap1-table-level-expectation.md), whose
55.20 BB/100 figure is retained unchanged as the cap-1 comparison.

## 1. Two-track verdict (separate statements, not merged)

- **Certification rail: NOT PASSED.** Cap-4 converged e_embed is 0.0794 BB
  against the frozen 0.002 BB per-decision gate — about 40x over (39.7x at
  20,000 iterations; 39.0x at the 6,000-iteration floor). This sentence
  stands regardless of any serving-rail result.
- **Serving rail: RELEASE WITHDRAWN after the D-382 end-to-end test.** The
  measured check-to-river classes alone give 0.2216 BB/100 and pass, but
  the `other` river class — 28.83% of pooled dealt hands, reached after
  flop/turn aggression — measures **7.02 BB/100 (95% CI 6.58-7.47)**, seven
  times the 1.0 hard rail. The end-to-end table-level expectation is
  **7.25 BB/100**. Per the D-382 pre-registered rule (point estimate >1.0
  or CI upper bound clearly over 1.0 withdraws the D-380 conditional
  release and names the gap), cap-4 is not releasable on this evidence.
  The 0.5-target question is moot at a 7x overshoot.

## 2. Converged cap-3 / cap-4 quality on the check-to-river anchor

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
| Table-level BB/100, measured classes only (lower branch) | 1.0539 | 0.2216 | 0.2178 |
| Peak RSS | 35.1 MB | 63.7 MB | 63.5 MB |

cap-4 is 4.76x better than cap-3 on converged exploitability at 3.08x the
per-anchor solve cost; cap-3's own measured-class BB/100 (1.0539) already
exceeds the 1.0 serving hard rail, the numeric basis for its
fallback-only role. The 6,000-iteration cap-4 build is marginally better
than the 20,000 one on total exploitability at roughly one third of the
cost, but D-380 freezes the serving count at 20,000 (section 6); the
6,000 column is evidence, not a serving configuration.

## 3. Precise table-level BB/100 on the measured classes — method

The working estimate `(55.20 / 31.19) x 0.0794 ~= 0.14 BB/100` was
rejected for delivery: the 31.19 divisor is the cap-1 **union-reference**
value while 55.20 is built from the **T6 direct cap-0 reference**
per-seat limped mean (9.8861 BB), so the factor mixes scopes. The precise
calculation reuses the D-346 frequency machine unchanged:

```
per-hand loss = P(limp_ctr) x (e_embed / 2),  P(limp_ctr) = 134/2,400 = 0.0558333
BB/100        = 100 x per-hand loss
```

The pooled passive corpus has zero SRP check-to-river hands by
construction; the lower branch charges zero for the `other` class and
treats all-in runouts (14.75% of hands) as structurally zero, the same
discipline as the 55.20 certificate. The precise cap-4 result is
0.2216 BB/100 (95% interval 0.185-0.258); the crude factor understates it
by 1.58x — under the 2x D-380 audit threshold, which is why the 1.0 rail
was not revisited at the time. Full table:
`../plans/ablation/cap34-table-level/cap34-table-expectation.csv`.

## 4. The other river class, measured (D-382)

### 4.1 Anchor configuration and sample counts

The 692 pooled `other` hands (28.83% of the 2,400-hand passive pool) are
rivers reached after flop/turn aggression. Because the passive scripts
never raise preflop, every one of them belongs to the **limped** ledger
family; their empirical river-pot distribution spans chips 8-40 with the
median mass at pots 14-16. Four cap-4 anchors were run at 20,000
iterations, 28 threads, both completion arms, through the unchanged
`measure_cap_materiality` instrument (a new ledger-selecting bench probe
adds selection only; no solver or evaluator code changed):

| Anchor | Ledger (chips) | cap-4 action nodes | Bounded NC | Pure menu | e_embed Uniform | e_embed BrFav | Uniform wall |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A: srp-ledger-0 | pot 8, 4/4 | 412 | 0.00393070 | 0.44121011 | 0.44514081 | 5.34329845 | 2,023 s |
| L1: limped-ledger-1 | pot 8, 4/4 | 412 | 0.00393070 | 0.44121011 | 0.44514081 | 5.34329845 | 1,890 s |
| L2: limped-ledger-2 | pot 10, 5/5 | 390 | 0.00337816 | 0.51761726 | 0.52099541 | 5.75683226 | 1,761 s |
| L3: limped-ledger-3 | pot 16, 8/8 | 344 | 0.00489256 | 0.47138901 | 0.47628157 | 6.13934936 | 1,565 s |

Cross-validation: anchors A and L1 are different betting histories that
converge to the identical river GameDef (same five cards, pot,
contributions, and stacks); their receipts are bitwise identical on both
arms, validating the new probe against the sealed D164 binary. Every
process exited 0 with empty stderr; peak RSS stayed under 81 MB.

### 4.2 Bucket estimate and the pre-registered verdict

Each observed practice pot is mapped to its nearest measured limped
ledger (ties toward the smaller, more severe pot). Resulting weights over
the 692 hands: pot-8 ledger 7.51%, pot-10 ledger 29.62%, pot-16 ledger
62.86%. Frequency uncertainty is binomial on the 2,400-hand pool and
severities are treated as fixed, the same convention as the D-346
calculator:

| Quantity (Uniform) | Value |
| --- | --- |
| Weighted per-seat per-occurrence severity | 0.243594 BB |
| **Other-bucket point estimate** | **7.0236 BB/100** |
| 95% CI | **6.5822 - 7.4651** |
| Fixed-weight bracket (all hands charged best / worst ledger) | 6.4174 - 7.5110 |
| BrFavorable adversarial bucket | 86.01 BB/100 |
| End-to-end table level (0.2216 measured-class + 7.0236 other) | **7.2452 BB/100** (CI high 7.69) |

Pre-registered D-382 judgment: the point-estimate rule (≤1.0) FAILs at
7.02 and the CI-upper rule (≤1.0) FAILs at 7.47; the ≤0.5
threshold-maintenance rule is not reached. **The serving release is
withdrawn.** The result is robust to the ledger mapping: severities are
flat across the three ledgers (0.445-0.521 BB two-seat), so even the
fixed best-ledger floor (6.42 BB/100) is 6.4x over the hard rail.

Why the D-380 proxy understated: it applied the w1 check-to-river
per-seat severity (0.0397 BB) to `other` hands. Measured aggressive-line
severities are about 0.24 BB per seat — roughly six times larger per
occurrence — because the missing cap-4 raise actions matter more in pots
already swollen by flop/turn betting.

### 4.3 Disclosed extrapolation

42.8% of the 692 hands (296, practice pots 18-40) lie above the largest
measured ledger (pot 16) and are charged the pot-16 severity under
nearest-ledger mapping. No frozen decision-bearing ledger exists above
pot 16 except the terminal all-in roots (structural zero, no decision).
Deeper pots plausibly carry larger absolute residuals, so 7.02 is best
read as a central estimate with a downward floor of 6.42, not an upper
bound; the BrFavorable 86.0 row is the adversarial envelope.

## 5. Coverage-cost curve over the 134,459 canonical classes

Each canonical river board class is an orbit of concrete five-card boards
under the 24 suit permutations; its orbit size is its exact deal-frequency
mass under a uniform river deal, the same joint-deal normalization the
seedless engine uses. `board-orbit-coverage.mjs` re-derives every orbit
with the ctest-pinned canonicalization rule and passes three sealed
invariants: 134,459 classes, total mass C(52,5) = 2,598,960, and 1,287
monochrome classes each of orbit size 4. Only three orbit sizes exist:

| Orbit size (concrete boards per class) | Classes | Share of classes |
| --- | --- | --- |
| 4 | 2,301 | 1.71% |
| 12 | 48,503 | 36.07% |
| 24 | 83,655 | 62.22% |

Mass is close to uniformly spread — no small hot subset buys most of the
coverage. Single-ledger curve (one w1-ledger solve per class, 28 threads;
"100 slots" = 100 parallel 28-thread jobs, about 2,800 hardware threads):

| Deal-mass coverage | Classes | cap-3 20k mach-h / days | cap-4 20k mach-h / days | cap-4 6k mach-h / days |
| --- | --- | --- | --- | --- |
| 50% | 54,145 | 7,254 / 3.02 | 22,299 / 9.29 | 6,965 / 2.90 |
| 80% | 89,609 | 12,006 / 5.00 | 36,904 / 15.38 | 11,527 / 4.80 |
| 95% | 122,096 | 16,358 / 6.82 | 50,283 / 20.95 | 15,706 / 6.54 |
| 99% | 130,760 | 17,519 / 7.30 | 53,851 / 22.44 | 16,821 / 7.01 |
| 100% | 134,459 | 18,014 / 7.51 | 55,375 / 23.07 | 17,297 / 7.21 |

Multi-ledger reality (D-382 sync). A complete per-class strategy solves
all nine decision-bearing betting ledgers, not one. The five cap-4 20k
ledgers measured so far (w1 plus L1/L2/L3 plus srp-0) total 2.423 wall
hours per class — 5.88x the single-ledger line — which rescales the
curve to 131,186 / 217,110 / 295,822 / 316,814 / 325,776 machine-hours at
50/80/95/99/100% coverage (54.7 / 90.5 / 123.3 / 132.0 / 135.7 days on 100
slots). The four remaining SRP ledgers (pots 12/14/20/32) are unmeasured,
so these are partial lower bounds, not the full nine-ledger bill.

## 6. Iteration freeze at 20,000 and why

D-380 freezes the serving iteration count at 20,000; changing it requires
re-measuring the menu term. The structural term is non-monotone, so
iteration count is a quality-sensitive serving parameter rather than a
tunable budget:

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

## 7. Unmeasured items and risks

1. **Cap-4-conditional reach frequencies.** All frequencies come from a
   mounted cap-0 resolver in the 2,400-hand passive campaigns. A cap-4
   engine generates its own arrival distribution; qualitative direction
   (less fold pressure, higher river reach) is recorded in the cap-1
   document but no cap-4-conditional frequency has been measured.
2. **The pot-16 extrapolation hull.** 42.8% of `other` hands are mapped
   upward from practice pots 18-40; section 4.3 quantifies the floor but
   no direct measurement exists above pot 16.
3. **Four unmeasured SRP ledgers (pots 12/14/20/32).** They carry zero
   weight in the passive corpus but would bind under aggressive
   opponent styles, where single-raised pots occur naturally.
4. **Heads-up only.** Every severity and frequency is two-seat; no
   multi-way BB/100, orbit weighting, or menu residual exists.
5. **cap-5/cap-6 marginal benefit is unconverged.** Only truncated Gate
   2 rows remain (pure_menu 0.0402 BB at 562 iterations, 0.00959 at 227;
   D164 cap-6 0.00610 at 1,000); the conditional 30.6-hour cap-6 run was
   pre-closed by the D-370 branch-3 result.
6. **Single evaluator lineage.** The 200-3,000-iteration anchors use
   reach_bound while 6,000+ anchors use EmbeddedGame (same frozen
   identity, no shared-iteration bitwise equivalence receipt); all D-382
   rows are within-instrument EmbeddedGame.

## 8. Relationship to the cap-1 decision records

[cap1-table-level-expectation.md](cap1-table-level-expectation.md) is
not rewritten or re-scaled: its 55.20 BB/100 (95% interval 46.1-64.3)
remains the sealed cap-1 table-level certificate, and its raw campaign
CSVs are the direct frequency input to sections 3 and 4. The cap-4
end-to-end 7.25 BB/100 versus cap-1's 55.20 share one frequency machine
and one per-seat convention; cap-4 is about 7.6x better at the table
level but still far outside the serving rail once aggressive-line rivers
are priced. [cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md)
and [m2-certification-terminal-state.md](m2-certification-terminal-state.md)
keep their standing: nothing here satisfies restart criteria R1-R4, and
no `certified_bound` claim attaches to cap-3 or cap-4.

## 9. Receipts and reproducibility

- Severity receipts: `../plans/ablation/rfc0049/gate2/d370-*.csv`,
  `d371-*.csv` and `../plans/ablation/d382-other-river-class/d382-*.csv`
  (all RC 0, empty stderr; runner `*.log` files are git-ignored).
- BB/100 and other-bucket tooling:
  `../plans/ablation/cap34-table-level/` and
  `../plans/ablation/d382-other-river-class/d382-other-bucket.mjs`
  (deterministic; sealed frequencies and measured severities only).
- Coverage tooling: `../plans/ablation/cap34-table-level/board-orbit-coverage.mjs`.
- Frequency inputs: `../plans/ablation/cap1-table-level/{mounted,r2}-rows.csv`.
- Ledger-selecting bench probe:
  `../../engine/benchmarks/rfc0047/rfc0047_ledger_embed_probe.cpp`
  (manual, not a CTest; reuses `measure_cap_materiality` unchanged).
