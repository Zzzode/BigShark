# G3 independent re-review round 2 (2026-11-07)

After the production spike box (frame MAX 8.19e-4), the same non-author
reviewer returned CHANGES-REQUESTED (narrow, ~85% closed): BLOCKING 2 and
MAJOR 4 RESOLVED; BLOCKING 1 largely resolved but the spike family was a
restricted per-group/per-seat positive-tail water-fill, leaving three gaps:

- F1 (MAJOR): the box is global; cross-group/cross-river-card concentration
  and the seat split were hard-coded. Closure chosen: a shape-free analytic
  global water-fill bound (no new solves), plus measured corroboration.
- F2 (MAJOR): negative-w tail (delta=-E on w<0) never probed; analytic bound
  uses sign(w) over |w| and covers both tails.
- F3 (MAJOR): full 240-group census production emax_real = .0219/.0216/.0232
  exceeds the frame12-sample E=.01837 for 3/6 strata; evaluator ignored the
  column. FINAL design (superseding the interim rescale): certify each
  stratum at its OWN measured envelope; E_env is reported only, not
  re-applied (the bound is piecewise-linear in E, not globally linear).
- convexity caveat: the fixed-strategy telescoping gap is CONVEX in leaf
  payoffs, so sum|w|delta is a first-order bound; each injection row carries
  analytic_bound_bb, required to dominate measured delta empirically (holds at
  all 24 points), and an exact-corner d* injection (review R1) tests the box
  maximizer itself.

Commit 22a9441 implements the analytic water-fill + E_env envelope (27 node
tests); a candidate r6000 + reference r20000 "full" campaign with four fine
spikes per stratum and per-row analytic bounds (task bkvmi82q2) produces the
final measured-vs-analytic dominance check and the envelope number.
