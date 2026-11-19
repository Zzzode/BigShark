# Decision Support: Serving Cost of the Cap-3 / Cap-4 Wide Menus

Status: Current
Date: 2026-11-19
Scope: read-only decision support for the product-selected serving range
"support cap-3 to cap-4". Every number comes from bench receipts taken on
this machine; no engine, sealed instrument, protocol, or quality contract
was changed, and nothing is mounted, shipped, or pushed. The certification
gate is not relaxed (section 4). Companion to
[m2-certification-terminal-state.md](m2-certification-terminal-state.md),
[cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md), and the
D-370 closure note
[../plans/ablation/rfc0049/gate2/d370-cap4-convergence-closure.md](../plans/ablation/rfc0049/gate2/d370-cap4-convergence-closure.md).

## 1. Headline

Both wide menus converge to a stable, non-negligible structural gap:

- **cap-4 at 20,000 iterations: e_embed 0.0794 BB**, i.e. **0.0397 BB per
  player per dealt hand, about 2.0% of the 2 BB anchor pot**;
- **cap-3 at 20,000 iterations: e_embed 0.3775 BB**, i.e. **0.1888 BB per
  player per dealt hand, about 9.4% of pot**.

cap-4 is 4.76x better than cap-3 on converged exploitability at 3.08x the
per-anchor solve cost. Neither is within 0.002 BB. On both caps the
policy's own NashConv keeps converging while the pure-menu structural term
bottoms and then rises: convergence reveals the menu restriction rather
than removing it.

## 2. Converged quality, side by side

Configuration: w1 deep check-to-river anchor (pot 4 chips, big blind 2,
full 1,081-combo ranges), identity(k) menu, seedless linear-averaging RM+,
28 threads, D164 `EmbeddedGame` evaluator, frozen identity
`e_embed = bounded NC + pure_menu`. All values BB, summed over both seats.

| Quantity (Uniform arm) | cap-3 @20,000 | cap-4 @20,000 | cap-4 @6,000 (measured floor) |
| --- | --- | --- | --- |
| Action nodes | 106 | 320 | 320 |
| Bounded NC (policy's own error) | 0.00340111 | 0.00714721 | 0.01205936 |
| Pure-menu structural term | 0.37410857 | 0.07224095 | 0.06596689 |
| e_embed (total exact exploitability) | 0.37750969 | 0.07938816 | 0.07802624 |
| e_embed vs M = 0.002 | 188.8x over | 39.7x over | 39.0x over |
| e_embed vs B0 margin 1.28e-3 | 294.9x over | 62.0x over | 61.0x over |
| Per-player loss per dealt hand (e_embed / 2) | 0.18875 BB | 0.03969 BB | 0.03901 BB |
| Share of the 2 BB anchor pot | 9.44% | 1.98% | 1.95% |
| Pure-menu component per hand | 0.18705 BB | 0.03612 BB | 0.03298 BB |
| BrFavorable arm e_embed (adversarial bound) | 15.8501 | 4.7261 | 4.7621 |
| Peak RSS | 35.1 MB | 63.7 MB | 63.5 MB |

Structural-term trajectory. cap-3's sealed reach_bound curve has pure_menu
0.3474 BB at 3,000 iterations; the 20,000-iteration EmbeddedGame reading is
0.3741 BB (+7.7%) while bounded NC falls 0.0273 -> 0.0034. cap-4's pure_menu
bottoms near iteration 1,500 (0.0640) and rises to 0.0722 at 20,000. These
are converged structural levels, not truncated transients. For cap-4 the
best measured total is the 6,000-iteration point (0.0780 BB); pushing to
20,000 slightly worsens e_embed while improving the policy's own NashConv.

Instrument caveat. The 200-3,000-iteration anchors were scored by
`reach_bound_from_snapshot`; the 6,000/20,000-iteration anchors by the
independent
`EmbeddedGame` evaluator. Both implement the same independently reviewed
frozen T6 identity, but no shared-iteration bitwise equivalence receipt
exists between the two evaluators. Within-instrument comparisons (the
6,000 -> 20,000 tail direction) are unaffected.

Context. Sealed cap-1 20,000-iteration e_embed readings are 20.69 BB (T6
direct cap-0 reference) to 31.19 BB (union reference). The cap-3/cap-4
levels here are roughly two orders of magnitude below cap-1, but they are
measured under the direct cap-0 evaluator and are not a like-for-like
subtraction.

## 3. Campaign arithmetic over 134,459 canonical classes

Per-anchor walls are measured process wall times at 28 threads; the
campaign multiplies them by the 134,459-class frontier with no learning,
deduplication, or queue overhead assumed. "100 slots" means 100 parallel
28-thread jobs, i.e. about 2,800 hardware threads running 24/7.

| Build | Wall per anchor (Uniform) | Machine-hours | Single box (8,760 h/y) | 100 slots |
| --- | --- | --- | --- | --- |
| cap-3, 20,000 iters | 0.1340 h (482 s) | 18,014 | 2.06 years | 7.5 days |
| cap-4, 20,000 iters | 0.4118 h (1,483 s) | 55,375 | 6.32 years | 23.1 days |
| cap-4, 6,000 iters | 0.1286 h (463 s) | 17,296 | 1.97 years | 7.2 days |

Each process also runs the BrFavorable completion arm (a second full
solve); counting both arms doubles the totals: cap-3 36,154 machine-hours
(4.13 years / 15.1 days), cap-4 20k 111,360 machine-hours (12.71 years /
46.4 days), cap-4 6k 34,462 machine-hours (3.93 years / 14.4 days).
Measured CPU-time is 15.4-15.8x wall time; Uniform-arm campaign CPU-hours
are about 277k (cap-3) and 875k (cap-4 20k).

The D-371 cross-check figure (cap-4 20k, 0.41 h/anchor -> 55,375 machine-
hours, 6.3 years single box, ~23 days on 100 slots) reproduces exactly.
The cap-4 6,000-iteration build is notable: it is the measured quality
floor (0.0780 BB, marginally better than 20k) at roughly one third of the
20k campaign cost.

## 4. Certification position — gate unchanged

The M = 0.002 BB river term and the B0 residual margin 1.28e-3 BB are
**not relaxed, amended, or reinterpreted** by this document. Neither cap
passes: cap-3 is 188.8x over M and cap-4 is 39.0-39.7x over M at their
best measured points. The numbers above are the measured practical price
of serving within the user-selected cap-3/cap-4 range — per-hand expected
loss, pot share, and build cost — not a certification, a quality-laundering
re-baselining, or evidence toward restart criteria R1-R4. No
`certified_bound` claim attaches to either menu.

## 5. Remaining unmeasured items

1. **Real opponent reach frequencies.** The existing table-level 55.20
   BB/100 figure (D-346) weights cap-1 per-occurrence losses by frequencies
   observed in two 2,400-hand passive-style campaigns under cap-0
   conditional arrival rates. No analogous reach-weighted BB/100 exists for
   cap-3 or cap-4; the per-hand figures in section 2 are exact
   per-anchor-occurrence rates, assuming the anchor is reached.
2. **Multi-way play.** Every measurement here is heads-up. Three-or-more-seat
   trees have different menus, reaches, and showdown normalizations; no
   cap-3/cap-4 multi-way number exists.
3. **Marginal benefit toward cap-6.** The only cap-5/cap-6 readings are
   truncated Gate 2 rows (pure_menu 0.0402 BB at 562 iterations and
   0.00959 BB at 227; D164 cap-6 0.00610 BB at 1,000), all unconverged.
   The direction indicates further widening shrinks the structural term,
   but no converged cap-5/cap-6 value exists, and the D-370 branch-3
   result pre-closed the conditional 30.6-hour cap-6 run. cap-6 also costs
   ~802 MB RSS and ~8 s per iteration versus cap-4's 64 MB and ~74 ms.
4. **Six-stratum and all-root coverage.** All D-370/D-371 numbers are the
   single w1 anchor root. The per-stratum MAX discipline requires repeating
   the build across the six frozen strata; the campaign arithmetic assumes
   the per-anchor cost is representative.

## 6. Receipts and traceability

- cap-4: `../plans/ablation/rfc0049/gate2/d370-w1-identity-cap4-embed-{20000,6000}.csv`
  and `d370-cap4-convergence-closure.md` (committed as D-370).
- cap-3: `../plans/ablation/rfc0049/gate2/d371-w1-identity-cap3-embed-20000.csv`
  (RC 0, empty stderr; the `*.log` runner file is git-ignored).
- Sealed comparison curves: `w1-identity-cap3.csv`, `w1-identity-cap4.csv`,
  `w1-identity-cap5.csv`, `w1-identity-cap6.csv` in the same directory.
