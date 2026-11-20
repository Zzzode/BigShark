# D-390 Scenario A Cap-6 Receipts and Two-Rail Judgment

Status: Current
Date: 2026-11-20
Scope: offline measurement completion for the three named cap-6 receipts in
[other-bucket-rootcause.md](other-bucket-rootcause.md) section 4. No engine
source, production behavior, cap configuration, or deployment is changed;
nothing is pushed. This record converts that memo's projected Scenario A
cells into measured cells and states the two D-380 rails separately.
Committed receipts and the deterministic aggregator live under
`../plans/ablation/d390-scenario-a/`; the campaign scratch directory
`tmp/d390-scenarioA-20261120/` holds the same files plus runner logs and is
untracked.

## 1. What was measured and what was reused

Four uniform-arm probes were required; the probe prints
`family_index/pot/leaf_count` on its first line, so ledger identity is
self-attested (L1 pot 8, L2 pot 10, L3 pot 16).

| cell | ledger | iters | threads | status |
| --- | --- | --- | --- | --- |
| L1 cap-6 3,000 | limped/1 pot 8 | 3,000 | 28 | measured (this campaign) |
| L1 cap-6 20,000 | limped/1 pot 8 | 20,000 | 18 | measured (this campaign) |
| L2 cap-6 20,000 | limped/2 pot 10 | 20,000 | 18 | measured (this campaign) |
| L3 cap-6 20,000 | limped/3 pot 16 | 20,000 | 18 | measured (this campaign) |

The L2/L3 3,000-iteration receipts already existed
(`rootcause-l{2,3}-cap6-3000-uniform.csv`, committed under
`../plans/ablation/d382-other-river-class/`) and are reused unchanged per
the task instruction. The 20,000-iteration runs used 18 threads rather than
28; numerics are bitwise identical across thread counts by the kernel's
fork-join design, so only wall times are thread-count-specific and the
20k walls are compared only within this campaign. RSS and wall come from
the probe's own `getrusage(ru_maxrss)` and steady-clock columns;
`/usr/bin/time` is absent on this host, and these are the same kernel
source GNU time would report. Every process exited 0 with empty stderr.

## 2. Measured receipts

### 2.1 Three-thousand-iteration envelope check (all measured)

| ledger | pot | cap-0 nodes | online nodes | bounded NC | pure_menu | e_embed | wall (s) | RSS peak (MB) | source |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| L1 | 8 | 7,824 | 1,832 | 0.05480831 | 0.00939246 | 0.06420077 | 1,286.4 | 327.3 | measured |
| L2 | 10 | 5,720 | 1,498 | 0.05032803 | 0.00588446 | 0.05621249 | 1,054.2 | 269.1 | reused D-382 |
| L3 | 16 | 3,344 | 990 | 0.02656576 | 0.00000223 | 0.02656799 | 676.2 | 179.3 | reused D-382 |

Envelope decision: L1's 327 MB peak is far below the 1 GiB hard line, and RSS
is driven by tree size (identical tree at 3k and 20k), so the 20k run is
cleared on memory. The largest L1 3k wall was 21.4 minutes; the 20k
extrapolation stays under the four-hour per-ledger limit.

### 2.2 Twenty-thousand-iteration convergence rows (all measured)

| ledger | pot | cap-0 nodes | online nodes | bounded NC | pure_menu | e_embed | wall (h) | RSS peak (MB) | label |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| L1 | 8 | 7,824 | 1,832 | 0.00361227 | 0.00839753 | 0.01200981 | 3.598 | 327.1 | measured |
| L2 | 10 | 5,720 | 1,498 | 0.00432029 | 0.00889398 | 0.01321427 | 2.961 | 268.9 | measured |
| L3 | 16 | 3,344 | 990 | 0.00415761 | 0.00045071 | 0.00460831 | 1.949 | 179.3 | measured |

The frozen identity `e_embed = bounded NC + pure_menu` holds on every row to
display precision (largest residual 1e-8). Uniform arm, 18 threads.

### 2.3 Non-monotone structural term — recorded without smoothing

The 3,000 -> 20,000 comparison overturns any "more iterations always improves
the menu term" reading:

| ledger | bounded NC 3k -> 20k | pure_menu 3k -> 20k | e_embed 3k -> 20k |
| --- | --- | --- | --- |
| L1 | 0.05480831 -> 0.00361227 (-93.4%) | 0.00939246 -> 0.00839753 (-10.6%) | 0.06420077 -> 0.01200981 (-81.3%) |
| L2 | 0.05032803 -> 0.00432029 (-91.4%) | 0.00588446 -> 0.00889398 (**+51.1%**) | 0.05621249 -> 0.01321427 (-76.5%) |
| L3 | 0.02656576 -> 0.00415761 (-84.3%) | 0.00000223 -> 0.00045071 (**+20,111%**) | 0.02656799 -> 0.00460831 (-82.7%) |

Total e_embed still falls 4.3-5.8x because the bounded-NC collapse dominates,
but the structural pure-menu term **rises** on two of the three ledgers — the
same convergence-reveals-the-restriction mechanism first measured on cap-4 in
D-370. Two consequences stated plainly:

1. L3's 3,000-iteration "near-zero" 2.23e-6 reading was a non-converged
   transient, not an exact-menu certificate; at 20,000 it reads 4.51e-4.
2. At convergence, L1/L2 structural terms (0.00840 / 0.00889 BB) are each
   4.2-4.45x over M = 0.002 and still supply 69.9% / 67.3% of the residual.
   cap-6 is effectively exact only on the smallest L3 tree (structural share
   9.8%, term 0.23x M); it is not an exact menu on the two larger trees.

## 3. Scenario A end-to-end recomputation (measured vs projected)

The D-382 empirical weights (L1 0.0751 / L2 0.2962 / L3 0.6286 over 692
"other" hands; f_other = 692/2,400 = 0.288333) and the 0.2216 BB/100 D-380
lower branch are frozen inputs. Other bucket =
100 x f_other x sum(weight x e_embed/2); end-to-end = 0.2216 + bucket.

| Scenario | Cell status | Weighted per-seat (BB) | Other bucket (BB/100) | End-to-end (BB/100) | Label |
| --- | --- | --- | --- | --- | --- |
| A @3,000 | all three measured | 0.019086 | 0.5503 | 0.7719 | measured |
| A @20,000 | all three measured | 0.003856 | **0.1112** | **0.3328** | measured |
| Rootcause memo row | L1 by L2's 3k ratio | — | 0.5328 | 0.7544 | projected (superseded) |

The independent recomputation reproduces the cross-check value 0.332793
exactly under the frozen rounded weights (0.3328 to four decimals). Using
exact fractional weights (52/205/435 over 692) gives 0.332809, a 1.6e-5
display-level difference; the rounded-weight convention is retained for
continuity with `d382-other-bucket.mjs`. Binomial 95% CI on the frequency
term (SE 0.009247 over 2,400 hands; severities treated as fixed, the D-346
convention): other bucket [0.1042, 0.1182], **end-to-end [0.3258, 0.3398]**.
The 3,000-iteration end-to-end CI is [0.7373, 0.8065].

The measured 20k result is 2.3x better than the memo's 0.7544 projection
because both of its conservative ingredients were replaced: L1 is now
measured rather than ratio-projected, and 20k convergence removes most of
the bounded NC. Every numeric cell above is measured; no projected
severities remain in the 20k row.

## 4. Two-rail judgment (D-380, kept separate)

- **Certification rail (<= 0.002 BB per decision point): NOT PASSED.** At
  20,000 iterations every per-decision e_embed remains over M: L1 0.01201
  (6.00x), L2 0.01321 (6.61x), L3 0.00461 (2.30x), and the L1/L2 structural
  terms alone exceed M 4.2-4.45x. This rail is governed exclusively by the
  R1-R4 restart criteria in
  [m2-certification-terminal-state.md](m2-certification-terminal-state.md);
  no serving-rail result below can substitute for them, and no
  `certified_bound` claim attaches to cap-6.
- **Online serving rail (<= 1.0 BB/100 hard; 0.5 keep target): PASSED ON
  MEASURED CELLS.** End-to-end 0.3328 BB/100 with 95% CI upper bound 0.3398:
  hard-rail margin 0.6672 (CI upper still 0.6602 clear), and the point is
  itself below the 0.5 target by 0.1672 with the full CI under 0.5, so the
  D-380 "point <= 0.5 keeps the threshold unchanged" rule is met on this
  corpus. The D-382 withdrawal on cap-4 is therefore resolved for the
  measured three-ledger Uniform configuration; it is not a universal release
  while section 6 items stay open.

The two rails are not interchangeable: a serving-rail pass says nothing about
certification, and the certification rail remains governed by R1-R4
regardless of this serving result.

## 5. Cost table update

Measured 20,000-iteration walls (18 threads, Uniform; projected cells marked):

| Scope | Wall per class | Campaign over 134,459 classes | Label |
| --- | --- | --- | --- |
| L1 cap-6 20k | 3.598 h | 483,782 machine-h / 201.6 days on 100 slots | measured |
| L2 cap-6 20k | 2.961 h | 398,113 machine-h / 165.9 days | measured |
| L3 cap-6 20k | 1.949 h | 262,105 machine-h / 109.2 days | measured |
| **Three-ledger total** | **8.508 h** | **1,144,000 machine-h / 476.7 days** | measured |
| Rootcause memo estimate | 5.2 h/class | ~693,000 machine-h / 289 days | projected (28-thread 3k linear extrapolation; superseded) |

"100 slots" means 100 parallel 18-thread jobs (~1,800 hardware threads)
running 24/7. The measured bill is 1.65x the memo's projection; the
projection assumed 28-thread throughput and linear 3k -> 20k scaling, while
the campaign ran at 18 threads. Peak RSS stayed at 179-335 MB across all
ledgers, inside the 1 GiB envelope. This covers the three limped aggressive
ledgers only; adding SRP ledgers or all 96 leaves per ledger grows the bill.

## 6. Remaining unmeasured items and evidence

1. **Single leaf per ledger.** Each cell solves the DFS representative leaf
   (one of 96 river leaves); the six-stratum, all-root per-stratum MAX
   discipline has not been run, so the measured numbers are anchor evidence,
   not a campaign-wide certificate.
2. **Uniform arm only.** No BrFavorable cap-6 receipt exists; the
   adversarial completion envelope for cap-6 is unmeasured (the cap-4
   pattern was roughly an order of magnitude above Uniform).
3. **Cap-0-conditional frequencies.** The 692-hand arrival distribution comes
   from a mounted cap-0 resolver; a cap-6 engine generates its own
   distribution, whose directional bias is unmeasured.
4. **Four SRP ledgers (pots 12/14/20/32).** Zero weight in the passive corpus
   but binding under aggressive opponent styles; no cap-6 receipt exists.
5. **Pot-16 extrapolation hull.** 42.8% of the 692 other hands (practice
   pots 18-40) are still charged the pot-16 severity by nearest-ledger
   mapping; no decision-bearing frozen ledger exists above pot 16.
6. **Heads-up only.** No multi-way BB/100, weighting, or menu residual.
7. **Iteration-freeze discipline.** The non-monotone structural term
   (section 2.3) means 20,000 is a frozen serving count with mandatory
   re-measurement on any change, not an observed asymptote.
8. **No online timing claim.** These are offline solves for a solve-once /
   table-lookup architecture; memory is inside envelope, but the G1 online
   p95 wall was not re-measured for cap-6 trees.
