# Root-Cause Memo: The 7.02 BB/100 Other-River Residual

Status: Current
Date: 2026-11-19
Scope: technical root-cause analysis of the D-382 other-river-bucket
failure, under a <=20-minute-per-bench budget. All inputs are existing
sealed receipts plus four new equal-iteration cap-widening runs; no
production behavior changed and nothing is pushed. Companion to
[cap34-serving-decision.md](cap34-serving-decision.md).

## 1. Residual decomposition (covers 100% of the 7.0236 BB/100)

Additive split by mapped betting ledger (Uniform arm; frequency f_other =
692/2,400 = 0.2883):

| Ledger | Pot (chips) | Hand weight | BB/100 | Share | Cumulative |
| --- | --- | --- | --- | --- | --- |
| L3 limped-3 | 16 | 62.86% | 4.3163 | 61.5% | 61.5% |
| L2 limped-2 | 10 | 29.62% | 2.2251 | 31.7% | 93.1% |
| L1 limped-1 | 8 | 7.51% | 0.4822 | 6.9% | 100.0% |

The top two ledgers cover 93.1% of the residual (>=80% requirement met).
Within every ledger the frozen identity `e_embed = bounded NC + pure_menu`
splits the loss as **99.10% structural menu term, 0.90% solver
non-convergence** (structural share per ledger: L1 99.12%, L2 99.35%, L3
98.97%; the w1 check-to-river anchor is 91.0%).

## 2. Cause: menu/abstraction structural gap, not convergence

Three independent evidence lines:

1. **Composition.** At 20,000 iterations the policies' own NashConv is
   already 0.0034-0.0049 BB while the pure-menu term is 0.44-0.52 BB;
   running longer can only attack the 0.9% component.
2. **Convergence trajectory.** The cap-4 menu term is non-monotone and
   rising after its floor (w1: 0.0640@1,500 -> 0.0660@6,000 ->
   0.0722@20,000) while bounded NC keeps falling — convergence reveals the
   missing actions, it does not price them away.
3. **Widening response (this memo's new experiment, L3 pot-16, equal
   3,000 iterations, 28 threads, Uniform):**

| cap | action nodes | bounded NC | pure_menu | e_embed | step |
| --- | --- | --- | --- | --- | --- |
| 4 | 344 | 0.02912186 | 0.51976893 | 0.54889079 | — |
| 5 | 652 | 0.01860123 | 0.09134946 | 0.10995069 | 4.99x lower |
| 6 | 990 | 0.02656576 | **0.00000223** | **0.02656799** | 4.14x lower |

The structural term collapses 211,385x on L3 by cap-6 (to 2.23e-6, ~900x
below M = 0.002 but above the 1e-8 exact-zero predicate, so "near-zero",
not certified zero). Generalization check on L2 pot-10, cap-6 @3,000:
pure_menu 0.5176 -> 0.00588 (88x smaller), e_embed 0.05621249 with 89.5%
of the remainder now bounded NC — i.e. once the menu is wide enough the
residual's nature flips from structural to convergence, which iteration
count does fix. The cap-4 tree retains only 5.3-10.3% of the exact cap-0
action nodes on these ledgers; the missing raise lines are the cause.

## 3. Repairability: repairable by menu widening, not by more iterations

Minimal viable extension: **serve cap-6 on the aggressive-line limped
ledgers (L1/L2/L3), keeping cap-4 on check-to-river roots** — a
line-conditioned menu, the smallest change that targets the 93.1% mass
ledgers without widening the already-passing 0.22 BB/100 class. Measured
and projected effects (3,000-iteration receipts are conservative because
their NC is not yet converged; L3 NC falls 5.43x from 3k to 20k):

| Scenario | Other bucket BB/100 | End-to-end (with 0.2216) | Status vs rails |
| --- | --- | --- | --- |
| A: all three ledgers cap-6 (L1 by L2's measured ratio, conservative) | 0.5328 | 0.7544 | under 1.0 hard rail now; 20k convergence plausibly reaches the 0.5 target |
| B: only L2/L3 cap-6, L1 stays cap-4 | 0.9628 | 1.1844 | marginal; does not close cleanly |
| Current cap-4 everywhere | 7.0236 | 7.2452 | FAIL (sealed D-382) |

Cost. Measured cap-6 walls per 3,000-iter anchor: L3 676 s, L2 1,054 s
(RSS 184/276 MB, inside the 1 GiB envelope). Extrapolated 20,000-iter,
three-ledger build: ~5.2 wall hours per canonical class, i.e. ~693,000
machine-hours / ~289 days on 100 slots for the 134,459-class universe —
12.5x the single-ledger cap-4 line. L1 cap-6 is the one unmeasured cell
(its cap-0 tree is the largest at 7,824 nodes); no go decision should be
finalized without that receipt and 20,000-iteration cap-6 convergence
rows on L2/L3.

## 4. Go / no-go recommendation

**No-go for cap-4 as the serving menu on aggressive lines (stands);
conditional go for a line-conditioned cap-6 extension, gated on three
named measurements.** The failure is a repairable abstraction gap, not a
solver ceiling: two ledgers already show the structural term collapsing
under cap-6 within the memory envelope. Before any release decision:
(1) L1 cap-6 receipt (fills the 7.5% ledger and the largest tree);
(2) cap-6 @20,000 on L2/L3 to convert the NC-dominated remainder into
converged numbers; (3) re-run the pre-registered <=1.0 / <=0.5 judgment
on measured (not ratio-projected) values. The 0.002 BB certification rail
is untouched by any of this — even cap-6's near-zero menu term leaves the
per-decision e_embed an order of magnitude over 0.002 at 3,000
iterations, so certification still requires the R1-R4 path.

## 5. Receipts

`../plans/ablation/d382-other-river-class/rootcause-l{2,3}-cap{4,5,6}-3000-uniform.csv`
(four new runs, all RC 0, empty stderr; each process under 18 minutes)
and the D-382 twenty-thousand-iteration anchor receipts in the same
directory.
