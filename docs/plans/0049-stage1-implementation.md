# RFC 0049 Stage 1 Implementation Plan

Status: Current. Scope owner: `bigshark_rfc0047_bench` only. This plan
maps the accepted RFC 0049 Stage 1 gates to concrete artifacts and
records their measured outcomes; Stage 2 (BSFLIB04 pack/mount) and
Stage 3 (per-stratum certification and serving) remain non-authorized
addenda and do not appear here as work items.

## Gate 1 — offline tree census (closed 2026-11-16)

Objective: without solving, decide which pre-registered candidates fit
the frozen offline envelope and collect the structural rows Gate 3
needs.

Artifacts:

- `engine/benchmarks/rfc0047/offline_census.{hpp,cpp}` — bench-only
  census helper. Exposes the frozen constants
  `kOfflineCensusMaxNodes = 25,000,000`,
  `kOfflineCensusMaxDepth = 256`,
  `kOfflineCensusMaxBytes = 8 GiB`,
  `kOfflinePackMaxMenuActions = 32`, builds each full `AbstractTree`
  under `offline_census_limits()`, walks the materialized nodes, and
  converts a `tree_resource_exhausted` throw into a typed
  `fits = false` census-rejected row. A limits-parameter overload exists
  only for the tiny-limit native test.
- `engine/benchmarks/rfc0047/rfc0047_offline_census_probe.cpp` — manual
  single-instance probe with the candidate list hard-coded (no argv):
  the sealed Coarse cap-1 control, identity raise caps 2..6, and the
  Wide/Fine cap-1 sizing grids.
- `engine/tests/test_rfc0047_offline_census.cpp` — native behavioral
  ctest: frozen-envelope constants, count agreement with an
  independent tree walk, deterministic reruns, tiny-limit typed
  rejection, and the w1 control build.

Receipt: `docs/plans/ablation/rfc0049/gate1-w1-census.csv`, one run,
RC 0. Every candidate fits the offline envelope and every per-node menu
is at most 6 actions, inside the 32-action pack ceiling. Key rows:

| Candidate | total nodes | action nodes | max menu | projected infosets | accounted bytes |
| --- | --- | --- | --- | --- | --- |
| coarse-cap1-control | 15 | 6 | 3 | 6,486 | 9,664 |
| identity-cap2 | 99 | 34 | 6 | 36,754 | 77,024 |
| identity-cap3 | 315 | 106 | 6 | 114,586 | 186,624 |
| identity-cap4 | 957 | 320 | 6 | 345,920 | 621,792 |
| identity-cap5 | 2,631 | 878 | 6 | 949,118 | 1,498,208 |
| identity-cap6 | 5,769 | 1,924 | 6 | 2,079,844 | 3,015,168 |
| wide-cap1 | 33 | 12 | 6 | 12,972 | 23,040 |
| fine-cap1 | 21 | 8 | 4 | 8,648 | 11,584 |

The cap-6 action-node count 1,924 matches the sealed D164 receipt
exactly, cross-validating the census. As the RFC review predicted, no
Gate 1 node-count rejection occurs on this single anchor: all eight
candidates proceed to Gate 2, and the 32-action precheck is satisfied
with wide margin. The largest single-anchor projection (cap-6,
2,079,844 infosets / ~3.0 MB accounted) stays roughly 12x under the
online 1M-infoset resident figure and far under the 8 GiB offline
byte envelope; the >100M-node objects belong to the Stage 3 full
fan-out, not this anchor.

Verification: native ctest 0.01 s; targeted regression
rfc0047_link_isolation, rfc0047_tree_budget, rfc0047_river_solver,
rfc0047_offline_census 4/4 green in 119.09 s; full release build green;
clang-format clean; no production, schema, mount, protocol, or threshold
file changed.

## Gate 2 — offline convergence (not started)

Planned artifacts (per the accepted RFC, each RED-tested before
implementation):

1. Two additive default-off bench seams in `river_solver.{hpp,cpp}`:
   an optional `TreeLimits` field on `RiverSolveConfig` (the constructor
   currently builds with the implicit default at river_solver.cpp:56)
   and an optional checkpoint sink plus a per-iteration stop predicate
   `stop(iteration, elapsed_wall_ms) -> bool` polled at the top of the
   driver loop outside all traversal FP work. Default-off behavior must
   stay byte-identical (sealed control 0.02761552 BB /
   hash 1922761369290757593), with a native test pinning it.
2. A convergence probe iterating only the eight Gate-1 passing
   candidates, one single-instance process per candidate, rungs
   {100, 200, 400, 800, 1,500, 3,000, 6,000, 12,000, 20,000},
   1,800 s in-process wall, 1,900 s outer timeout, hard-coded constants
   with no argv override. Each rung row is scored by the frozen
   reach-bound instrument and flushed before the next rung.
3. Native tests for sink invocation and row flushing, predicate
   latching on a deliberately tiny budget (in-process trip before any
   outer stop), verdict routing PASS / CONVERGENCE_NO_GO /
   BUDGET_NO_GO / INCONCLUSIVE, and the single-rerun-to-BLOCKED rule.

Gate 2 exit is a typed verdict per candidate; the stage cannot be closed
while any candidate is BLOCKED.

## Gate 3 — packed-artifact projection (not started)

Runs only for Gate 2 PASS candidates. Computes probability-only BSFLIB
row math (per-node menu block, 8-byte bucket entries, 3-byte compact
actions, Codebook16 runs) with the 1.0249 factor explicitly carried as
an unverified flop-derived assumption that Stage 2 must re-measure.
Nothing is serialized and no format is modified.

## Stage exit

GO requires at least one Gate 2 PASS with a finite Gate 3 projection;
NO-GO means every candidate is census-rejected, CONVERGENCE_NO_GO, or
BUDGET_NO_GO; BLOCKED requires owner escalation and either an
instrument fix or an approved pre-registration amendment. The likely
outcome implied by the D195/D164 evidence (a 0.0276 BB best-case policy
floor, 13.8x over the gate, under a T^-0.52 law) is NO-GO; that is the
RFC's designed falsifiable result, not a defect.
