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

## Gate 2 — offline convergence (in progress)

Implemented seams (both additive and default-off, native-tested):

1. `RiverSolveConfig::tree_limits` (optional `TreeLimits`, nullopt
   preserves the implicit construction) plus `checkpoint_sink` and
   `stop_predicate` (`RiverCheckpointSink` / `RiverStopPredicate` in
   river_solver.hpp). With both empty no `steady_clock` call and no
   snapshot allocation occur; the default solve is byte-identical,
   asserted by test_rfc0047_offline_convergence against repeated
   default solves and an explicit-limits solve.
2. The driver polls the predicate once per iteration at the loop top,
   outside every traversal FP operation, latches
   `stopped_early` / records `iterations_completed`, and invokes the
   sink at each reached rung.
3. `offline_convergence.{hpp,cpp}` classifies flushed rows into PASS /
   CONVERGENCE_NO_GO / BUDGET_NO_GO / INCONCLUSIVE, with non-finite
   instrumentation forced to INCONCLUSIVE and a post-first-rung wall
   trip never retried.
4. The manual probe `rfc0047_offline_convergence_probe` takes only a
   validated candidate index; rungs, the 1,800 s wall, and the
   0.002 BB gate (`kRfc0049GateBb`) are hard-coded with no argv
   override. Each rung is scored through the frozen reach-bound
   instrument and flushed before the next.

Control receipt (w1 coarse cap-1, full nine-rung ladder,
`gate2/w1-control-cap1.csv`; the t=100 bounded NC 0.11621217 matches
the D253 curve bitwise): bounded NashConv converges from 0.1162 at
100 iterations to 0.00006391 BB at 20,000 — already far below the
0.002 gate by itself — while the pure-menu structural term stays
inside 31.0443-31.2192 BB and e_embed never drops below 31.1606 BB
(15,580x over the gate). Verdict: CONVERGENCE_NO_GO. This is direct
in-anchor evidence that even a well-converged bounded policy cannot
move the structural floor, matching the RFC 0049 closure prediction.
The remaining seven candidates run one process each under the
pre-registered budget estimate in `gate2/budget-estimate.md`.

### Gate 2 closure (all eight candidates, 2026-11-16)

Every process returned RC 0 with an empty stderr; no INCONCLUSIVE and no
BLOCKED occurred, so the per-iteration stop predicate delivered a typed
verdict for every candidate. Zero candidates crossed e_embed <= 0.002 BB.

| Candidate | verdict | rungs | completed / requested | min e_embed (x over gate) | min bounded NC | last pure-menu term | peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| coarse-cap1-control | convergence_no_go | 9 | 20,000 / 20,000 | 31.1606 (15,580x) | 0.00006391 | 31.1851 | 46.9 MB |
| fine-cap1 | convergence_no_go | 9 | 20,000 / 20,000 | 31.3231 (15,662x) | 0.000044 | 31.4230 | 47.0 MB |
| wide-cap1 | convergence_no_go | 9 | 20,000 / 20,000 | 22.2180 (11,109x) | 0.000078 | 23.7952 | 50.8 MB |
| identity-cap2 | budget_no_go | 8 | 15,496 / 20,000 | 2.1223 (1,061x) | 0.004041 | 2.2098 | 47.1 MB |
| identity-cap3 | budget_no_go | 6 | 4,916 / 20,000 | 0.3747 (187x) | 0.027304 | 0.3474 | 79.4 MB |
| identity-cap4 | budget_no_go | 5 | 1,558 / 20,000 | 0.0980 (49x) | 0.034051 | 0.0640 | 171.5 MB |
| identity-cap5 | budget_no_go | 3 | 562 / 20,000 | 0.0911 (46x) | 0.050865 | 0.0402 | 398.0 MB |
| identity-cap6 | budget_no_go | 2 | 227 / 20,000 | 0.1908 (95x) | 0.181198 | 0.0096 | 802.1 MB |

Two independent walls are now measured, not extrapolated from other
RFCs:

1. Structural menu wall. The cap-1/grid policies cannot shrink the
   pure-menu term below ~22-31 BB; the smallest value observed anywhere
   is cap-6 at 0.00959 BB, which by itself is still 4.8x over the
   0.002 gate. No measured policy reaches a representable policy whose
   embedded exploitability is within the contract.
2. Convergence wall on the wide caps. Even setting the menu term aside,
   local log-log extrapolation of the partial curves puts bounded NC
   <= 0.002 at roughly 7,200-98,000 further iterations, i.e. 6.6-26
   hours of single-instance solve per candidate (cap-3 ~10.4 h, cap-4
   ~11.6 h, cap-5 ~6.6 h, cap-6 ~26 h) — all far beyond the 1,800 s
   offline process budget and the G1 online envelope.

The control's bounded-NC curve matches D253 bitwise at the shared
t=100 checkpoint (0.11621217), cross-validating the new sink path. The
R2-1 design concern is resolved in practice: the slowest candidate
(cap-6, ~8 s/iteration) still flushed two finite rungs before latching
at 1,800 s and closed as a clean BUDGET_NO_GO rather than an OS-killed
INCONCLUSIVE.

### Gate 2 independent review (Approved)

A separate non-author agent reviewed the seams, receipts, and scope and
returned Approved with no blocker/major and three minor plus two nit
findings, all resolved:

- Minor 1 (RFC R3-1 poll measurement): the target-host steady_clock
  poll cost is now measured in the native ctest at 22.8 ns per read on
  this machine, inside the vDSO 15-30 ns range the RFC review cited and
  about 1.3e-6 of the shortest sealed 17.153 ms iteration. The integer
  read stays outside all FP work, so no numeric path changes.
- Minor 2 (sealed-value native pin): test_rfc0047_offline_convergence
  now pins the w1 Coarse cap-1 300-iteration control natively — policy
  hash 1922761369290757593 exactly and NashConv 0.02761552 within half
  the sealed 8-decimal digit (a raw double cannot equal the decimal
  literal at the ULP), so byte-identity is asserted on the real anchor
  in addition to the shallow fixture.
- Minor 3 (zero clock/alloc proof): covered structurally by the
  conditional `has_stop` construction and empty-function guards; no
  instrumentation test is added because the property is compile-time.
- Nit 4: the missing control `.err` sidecar was added; all eight
  processes now have a zero-byte stderr receipt.
- Nit 5: `std::exit(3)` on a non-finite rung is retained for the manual
  probe (rows are flushed first and there is no state to destruct).

## Gate 3 — packed-artifact projection (not applicable)

Gate 3 runs only for a Gate 2 PASS candidate. No candidate passed, so
no projection is produced and no BSFLIB04 sizing input is generated;
this is the RFC's designed NO-GO path, not a skipped gate. The
flop-derived 1.0249 overhead assumption remains unverified and stays
documented as such for any future campaign.

## Stage exit

NO-GO. Every candidate is CONVERGENCE_NO_GO or BUDGET_NO_GO with finite
curves and no BLOCKED state. Path A — pre-solving a widened cap-1/grid
equilibrium on the binding w1 anchor to e_embed <= 0.002 BB inside the
offline process envelope — is infeasible on measured evidence. The
surviving direction is the Stage 2/3 addendum scope (a different packed
river/turn format and a per-stratum, multi-anchor campaign), which
remains un-authorized and would need a new RFC grounded in these
receipts.
