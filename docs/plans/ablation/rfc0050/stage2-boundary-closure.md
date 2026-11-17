# RFC 0050 Stage 2 — Six-Anchor Boundary Closure

Status: Current. Two receipts: `stage2-boundary.csv` (the fixed-wall
six-anchor run) and `stage2-deep-rerun.csv` (the D316 fixed-rung rerun
for the two deepest anchors). Bench-only; no frozen constant, ladder,
wall, gate, or classifier changed.

## Why the fixed-wall run measured speed, not boundary

Every anchor ran under the same 1,800 s wall. Per-iteration cost grows
with the action-node count of the deeper ledger (6.1 to 27.2 s/iter
across the six anchors), so the number of iterations completed is set
by tree size, not convergence. The fixed-wall receipt therefore ranks
throughput; comparable convergence points require a fixed-rung rerun,
which D316 ordered for the two anchors that could not reach the first
rung.

## Per-anchor closure table (D316 row format)

| anchor | family | pot | fixed-wall verdict | iters | rungs | min e_embed (BB) | pure-menu measured? | pure-menu value | wall (s) | RSS (MB) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | srp | 32 | budget_no_go | 298 | 2 | 0.26548740 | yes | 0 | 1,822.5 | 615 |
| 1 | srp | 20 | budget_no_go | 160 | 1 | 0.44453217 | yes | 0 | 1,842.9 | 1,007 |
| 2 | limped | 16 | budget_no_go | 120 | 1 | 0.41925887 | yes | 0 | 1,847.8 | 1,276 |
| 3 | srp | 14 | budget_no_go | 100 | 1 | 0.43075423 | yes | 0 | 1,880.5 | 1,515 |
| 4 | srp | 12 | verdict pending (fixed-wall inconclusive) | 82 | 0 | n/a | **no (default 0, withdrawn)** | unmeasured | 1,879.7 | 1,515 |
| 5 | limped | 10 | verdict pending (fixed-wall inconclusive) | 69 | 0 | n/a | **no (default 0, withdrawn)** | unmeasured | 1,875.5 | 1,515 |

The fixed-wall `min_pure_menu_abs_bb=0.00000000` on anchors 4 and 5 was
an unmeasured default: the boundary probe's accumulator starts at zero
and is only written inside the scoring sink, which never ran because
no checkpoint was reached. That zero is explicitly NOT counted as a
zero-menu result; the zero-menu claim for those two anchors was
withdrawn and replaced by the fixed-rung evidence below.

## D316 fixed-rung rerun (target iteration 200, 5,400 s 3x cap)

Both deep anchors reached the target without hitting the cap, and every
row carries `pure_menu_measured=1`, so the zero is a scored
measurement, not a default:

| anchor | family | pot | rung 100 e_embed (BB) | rung 200 e_embed (BB) | pure-menu (measured) | phase wall (s) | reach |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 4 | srp | 12 | 0.46503812 | 0.20692830 | 0 (measured) | 4,606.6 | target_rung_reached |
| 5 | limped | 10 | 0.42518013 | 0.20231846 | 0 (measured) | 5,253.8 | target_rung_reached |

No instrumentation fault occurred (`fault=0`, all values finite, normal
completion). The first-rung threshold is iteration 100; the fixed-wall
runs fell short at 82 and 69 iterations respectively (18 and 31
iterations short), confirming the original zero-rung outcome was a
budget result, not an apparatus failure.

## Measured zero-menu scope, after the rerun

All six decision-bearing anchors now have a scored zero pure-menu term:
anchors 0-3 from the fixed-wall run, anchors 4-5 from the fixed-rung
rerun with the measured flag set. The exact-menu structural identity
generalizes across the full anchor set; the earlier withdrawal is
resolved by real measurements rather than assumed.

## Convergence regime across depths

The 100->200 doubling slope is steep on every anchor with both rungs:

- rank 0 srp(32): slope -1.004;
- rank 4 srp(12): slope -1.168;
- rank 5 limped(10): slope -1.071.

The deeper anchors do not converge more slowly; their early slopes are
at least as steep as the shallow anchor. Two-point power-law
extrapolation (not certified, only two rungs) puts a 0.002 BB crossing
near ~26,000 iterations for rank 0, ~10,600 for rank 4, and ~14,900
for rank 5 if the current law persists. The same uncertified
alternatives remain: an RFC 0043-style plateau, or a later decay toward
the ~T^-0.52 bounded-control law. The deeper anchors cost more wall per
iteration (~22.7 and ~25.9 s), so reaching their crossing iterations is
still a multi-hour-to-multi-day commitment even though the iteration
count is lower.

## Open classification item

The empty-row inconclusive ambiguity is documented in
`stage2-verdict-gap-amendment-candidate.md` and stays a reviewed
proposal. The deep rerun shows the practical remedy (a fixed-rung
target with a 3x cap and an explicit measured flag) produces clean
typed evidence without changing the frozen taxonomy; whether to encode
a separate budget-no-rung verdict is for the independent amendment
review, not an in-place classifier edit.
