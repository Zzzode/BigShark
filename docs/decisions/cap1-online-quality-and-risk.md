# Decision Support: What Shipping Cap-1 Actually Means Online

Status: Current
Date: 2026-11-18
Scope: read-only analysis supporting the product decision on option A
(ship the bounded cap-1 online policy with its structural residual
documented). Every number comes from sealed bench receipts; no new
computation was run and no code was changed. Companion to
[m2-certification-terminal-state.md](m2-certification-terminal-state.md),
which establishes that the cap-1 policy cannot be certified against the
frozen quality contract.

## 1. The short answer

A cap-1 engine would be fast, stable, and fully inside every runtime
resource limit — but in deep river spots it would play without the
re-raise weapon. Against an opponent who knows this, the measured cost
is on the order of **10-16 big blinds per hand in those spots** under
the standard measurement, with a constructed worst-case bound of
**77-108 big blinds per hand**. More iterations do not fix this: the
loss is structural (the re-raise actions do not exist in the menu), not
a training shortfall.

## 2. The actual online budget, and the quality it buys

Production limits for one online resolve (`ResolveLimits` in
`engine/include/bs/resolver.hpp`): p95 wall 10 s, max 12 s, RSS 1 GiB,
1,000,000 infosets. The production-shaped online probe
([online-river-w1-t24.csv](../plans/ablation/rfc0047/s3/online-river-w1-t24.csv),
24 threads, the measured socket-local sweet spot, 10-12 action nodes)
shows what the cap-1 river tree completes inside that envelope on the
binding deep anchor:

| iterations | wall (ms) | policy's own NashConv (BB) |
| --- | --- | --- |
| 2,500 | 8,953 | 0.00157435 |
| 3,000 | 10,106 | 0.00121431 |
| 3,500 | 11,748 | 0.00101136 |

So a real online serve runs roughly **2,500-3,500 iterations**, at which
the policy's own equilibrium error is about **0.001 BB** — small. The
dominant error is the structural pure-menu term, and the receipts show
it is essentially flat across the entire iteration range, so the online
budget does not improve it:

| Measurement instrument | At 100-300 iterations | At 2,000-3,000 iterations | At 20,000 iterations |
| --- | --- | --- | --- |
| T6 direct cap-0 reference, Uniform | e_embed 19.48 BB (menu 19.46) | e_embed 20.69 BB (menu 20.69) | not run |
| T6 direct cap-0 reference, BrFavorable bound | 152.92 BB | 156.85 BB | not run |
| Union reference (RFC 0049 frozen control) | e_embed 31.16 BB (menu 31.04) | 31.20 BB (menu 31.20) | 31.19 BB (menu 31.19; own NC 0.00006391) |

Receipts: T6 figures in
[docs/plans/0047-stage1-tracker.md](../plans/0047-stage1-tracker.md)
("T6 FIRST GATE DATA", "T6 SIX-STRATUM ANCHOR SCOPE RESULT"); the full
iteration curve in
[w1-control-cap1.csv](../plans/ablation/rfc0049/gate2/w1-control-cap1.csv).
The two instruments differ only in reference scope: the T6 direct
reference prices missing raise-cap actions; the stricter union
reference also prices missing bet-sizing actions. Both are valid frozen
identities; the truth lies in the band between them, and both miss the
0.002 BB contract by roughly four orders of magnitude.

**The online-budget quality is therefore: e_embed about 19.5-31.2 BB
per hand (two-seat sum), of which ~0.001 BB is solvable-away policy
error and essentially all of it is the structural menu term.**

## 3. How to read these numbers (glossary for non-specialists)

- **BB (big blind).** The table's standard betting unit. At a 99 BB
  deep cash table, a player starts with 99 BB; the river pot in the
  measured anchor is 2 BB. A loss quoted in BB is directly comparable
  across stake levels: 10 BB at any stake is ten big blinds of that
  stake.
- **Two-seat sum vs. per-seat loss.** The bench's `exploitability_bb`
  (`engine/benchmarks/rfc0047/cap_embedding.cpp`, comment "Summed-seat
  exploitability") sums the best-response gain over both seats and
  averages over all card deals reaching the spot. The amount one
  player actually concedes when the other plays a perfect best response
  is one half: **roughly 9.7-15.6 BB per hand (Uniform)** and
  **76.5-107.7 BB per hand (BrFavorable bound)** on this anchor.
- **Uniform vs. BrFavorable.** Two ways of completing the actions the
  cap-1 menu cannot represent. Uniform assumes the opponent splits
  attention evenly among unavailable lines — the standard, conservative
  certificate. BrFavorable lets the opponent pick the single most
  damaging unavailable line every time — a deliberately loose upper
  bound (the project's structural certificates run 9-16x looser still;
  see the D192 diagnostic). A real adaptive opponent sits somewhere
  between the two; an opponent who studies the engine and targets the
  missing re-raises moves toward the BrFavorable end.
- **Per occurrence, not per session.** These are expected extra losses
  *each time this specific river spot occurs*, averaged over the cards.
  They are not totals over a hundred hands.
- **Worked scale check.** A roughly 10-16 BB Uniform loss on a 2 BB pot
  is 5-8x the pot, which is possible because the missing actions
  include all-in branches that contest the full 99 BB stacks — the
  money is lost through future bets the engine cannot make, not the
  immediate pot.

## 4. Where the residual concentrates: one specific decision type

The D192 exact Lemma-1 allocation
([pathb-reach/w1-coarse-300.txt](../plans/ablation/rfc0047/s3/pathb-reach/w1-coarse-300.txt))
attributes the 31.24 BB union-reference total to individual decision
nodes:

| decision node | mean reach | contribution (BB) | share |
| --- | --- | --- | --- |
| restricted root node (seat 1) | 1.00 | 13.76 | 44.0% |
| restricted root node (seat 0) | 1.00 | 10.01 | 32.0% |
| restricted root node (seat 0) | 1.00 | 6.35 | 20.3% |
| restricted root node (seat 1) | 0.42 | 1.12 | 3.6% |
| two widened deep nodes | — | 0.00086 | ~0% |

In poker terms: cap-1 permits exactly one raise per street. At the
river's first decision points the game-theoretic menu (five edges:
passive actions plus multiple raises including jam) wants to **re-raise
and to jam**; the cap-1 policy can only **call or fold** there. Reach
1.00 means every hand that reaches this spot faces these decisions —
this is not a rare deep-line curiosity. Two concrete leak modes follow:

1. **Value under-extraction.** Strong hands cannot re-raise or shove for
   value, so opponents pay off less than game theory demands.
2. **No fold pressure.** Bluff re-raises and bluff jams are impossible,
   so the engine's call/fold-only response is face-up; opponents can
   value-bet relentlessly without facing escalation.

Two receipts prove it is the missing raise category itself, not the
precision of bet sizes: the Fine five-fraction sizing grid changes the
result by only +0.022 BB over the coarse grid with 99.81% of
exploitability still structural (D249,
[w1-fine-300.csv](../plans/ablation/rfc0047/s3/fine-grid/w1-fine-300.csv)),
and widening deep low-reach nodes recovers essentially nothing
(0.00086 BB above). The D195 perfect-root-widening experiment shows that
even an unattainably perfect fix at these nodes would still leave a
0.0276 BB policy floor — useful as proof that root raise decisions are
the whole story.

## 5. Operational risk profile

What does **not** degrade under cap-1: latency and stability. The
online tree has 10-12 action nodes; probe RSS peaks are 10-41 MB against
a 1 GiB budget; the 10 s p95 is met at ~2,500-3,000 iterations; all
existing fail-closed and offline-boundary behavior is unchanged. No
crash, timeout, or memory risk is introduced.

What does degrade, and how to judge tolerance:

- **Predictability at river escalation points.** Opponents who observe
  that the engine never re-raises the river can exploit it on every
  deep spot; the residual is concentrated at reach-1.0 nodes, so the
  exposure repeats whenever the spot arises rather than being diluted
  across rare lines.
- **Texture breadth.** The same order of magnitude was measured on all
  six frozen strata (Uniform 19.45-29.44 BB; BrFavorable 152.92-188.76
  BB, T6 six-stratum table), so this is not one freak board texture —
  every stratum's worst anchor fails at 10^4-10^5x the original
  contract.
- **Contractual status.** Shipping cap-1 means replacing the certified
  0.002 BB quality contract with "cap-1 plus documented structural
  residual". The numbers above are the documentation of that residual;
  they cannot be presented as a certified quality level.

## 6. What these measurements do NOT tell us

Honest limits of the evidence, so the decision is not over-weighted:

- No table-level BB/100 aggregate exists. All figures are per-occurrence
  values on deliberately chosen worst-anchor roots, not reach-weighted
  averages over a full session. Producing one requires a new measurement
  campaign, which was not authorized and was not run.
- No realized loss against human or adaptive opponents is measured.
  BrFavorable is a constructed bound, not a forecast; Uniform is the
  standard conservative certificate.
- The values are river-resolve figures on the deep 99 BB anchor.
  Shallower roots have smaller reference trees and were noted as
  potentially less severe, but the six-stratum anchor sweep shows no
  stratum escapes the order of magnitude.
- All numbers assume the current single-instance host and the measured
  thread configuration; they are reproducible bitwise but not
  transferable to different hardware without re-measuring.

## 7. One-sentence conclusion

Shipping cap-1 gives users a fast, reliable engine that, in deep river
spots, is structurally incapable of re-raising or jamming — a
documented, repeatable, decision-level leak worth roughly 10-16 big
blinds per such hand against an informed opponent (77-108 in the
adversarial bound), concentrated entirely at the first river raise
decisions, with no operational or latency benefit left on the table to
trade against it.

## 8. Evidence index

- Online timing and bounded NashConv:
  [docs/plans/ablation/rfc0047/s3/online-river-w1-t24.csv](../plans/ablation/rfc0047/s3/online-river-w1-t24.csv).
- Full 20,000-iteration cap-1 curve:
  [docs/plans/ablation/rfc0049/gate2/w1-control-cap1.csv](../plans/ablation/rfc0049/gate2/w1-control-cap1.csv).
- T6 six-stratum residuals and D192/D195/D249 narratives:
  [docs/plans/0047-stage1-tracker.md](../plans/0047-stage1-tracker.md).
- Node-level residual allocation:
  [docs/plans/ablation/rfc0047/s3/pathb-reach/](../plans/ablation/rfc0047/s3/pathb-reach/),
  [docs/plans/ablation/rfc0047/s3/patha-rootmenu/](../plans/ablation/rfc0047/s3/patha-rootmenu/).
- Dilemma and restart criteria:
  [m2-certification-terminal-state.md](m2-certification-terminal-state.md).
- Normalization definitions:
  [docs/reference/exploitability.md](../reference/exploitability.md).
