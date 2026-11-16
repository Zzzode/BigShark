# RFC 0049 Stage 1 Gate 2 — Pre-Run Budget Estimate

Status: Current. Produced before any long Gate 2 solve, per the D280
directive. All figures are estimates from sealed throughputs, not
measurements; actual per-rung rows replace them as receipts land.

## Grounding

- Sealed cap-1 Coarse w1 throughput: 17.153 ms/iteration at 6 action
  nodes (D253 receipt, 5,146 ms / 300 iterations).
- Sealed cap-6 per-iteration cost: ~700 ms (D164, 1,000-iteration mean)
  at 1,924 action nodes (Gate 1 census, matching D164 exactly).
- Linear scaling by action-node count is used as the estimator; the
  O(kC^2) terminal showdown fold is node-count-independent, so the
  estimate is conservative for small trees and tight for wide ones.
- In-process wall budget: 1,800 s with a per-iteration stop predicate;
  outer OS timeout 1,900 s. Frozen rung ladder
  {100, 200, 400, 800, 1,500, 3,000, 6,000, 12,000, 20,000}.

## Per-candidate estimate

| Candidate (idx) | action nodes | est. ms/iter | iters inside 1,800 s | last rung before trip | full 20k wall (s) |
| --- | --- | --- | --- | --- | --- |
| coarse-cap1-control (0) | 6 | 17.2 | 104,938 | 20,000 (full ladder) | 343 |
| identity-cap2 (1) | 34 | 97.2 | 18,518 | 12,000 | 1,944 |
| identity-cap3 (2) | 106 | 303.0 | 5,940 | 3,000 | 6,061 |
| identity-cap4 (3) | 320 | 914.8 | 1,968 | 1,500 | 18,297 |
| identity-cap5 (4) | 878 | 2,510.1 | 717 | 400 | 50,201 |
| identity-cap6 (5) | 1,924 | 5,500.4 | 327 | 200 | 110,008 |
| wide-cap1 (6) | 12 | 34.3 | 52,469 | 20,000 (full ladder) | 686 |
| fine-cap1 (7) | 8 | 22.9 | 78,703 | 20,000 (full ladder) | 457 |

## Envelope decision

- Full 20,000-iteration ladders for every candidate would total about
  52.2 hours of single-instance wall time, which exceeds any one gate
  run and is therefore not how the campaign is executed.
- The 1,800 s in-process cap bounds every single process; the aggregate
  capped wall across all eight candidates is about 10,487 s (2.9 h),
  run one process at a time. Each process emits its flushed rung rows
  before the trip, so every candidate still reaches a typed verdict:
  the three small candidates (control, wide, fine) complete the full
  ladder; the five wide raise-cap candidates finish as
  BUDGET_NO_GO with a finite partial curve and a terminal slope if the
  gate is not crossed.
- No candidate is expected to cross e_embed <= 0.002 BB: D195 measured
  the perfect-widening fixed-policy floor at 0.0276 BB (13.8x over the
  gate) and the D253/D164 T^-0.52 law places 0.002 beyond the
  pre-registered iteration count. The campaign's designed result is
  therefore an honest FAIL/NO-GO with curves and resource
  extrapolation, never a threshold relaxation.
- Reach-bound scoring runs once per rung (nine walks per process, ~18 s
  each on the control per D253; wider trees cost more), which is
  included in the 1,800 s wall the predicate observes. If scoring
  itself pushes a candidate toward the budget, the predicate still
  latches cleanly at the next iteration poll and the last scored rung
  is already on disk.
