---
rfc: "0048"
subject: "Bounded Action-Menu Materiality and the Certified Menu-Widening Path"
status: "Draft"
authors: "Claude (zzzode agent session)"
created: "2026-11-13"
updated: "2026-11-13"
owners: "Engine rfc0047 benchmark harness (measurement); future production resolver/solver path gated by a later addendum"
supersedes: ""
superseded-by: ""
---

# RFC 0048: Bounded Action-Menu Materiality and the Certified Menu-Widening Path

## Summary

RFC 0047's G5 gate requires the bounded online action menu (`identity(1)`, one
full raise per street) to be menu-immaterial relative to the uncapped
`identity(0)` menu within `M = 0.002` BB. The RFC 0047 S3/T6 embedding
measurement proved the cap-1 menu is highly material (a ~20 BB pure menu term
on the deep 100 BB anchor). This RFC records the D164 parameterized cap-k
widening curve that searches for the smallest bounded cap whose embedded
exploitability clears M, establishes that no online-feasible bounded cap does
so inside the G1 timing/resource envelope, and selects the quality-preserving
direction (offline and/or reach-aware widening) with explicit gates. It changes
no production behavior: the measurement component and the 0.002 BB bar stay as
committed, and no production menu, resolver, or contract is modified by this
RFC.

## Motivation

The M2 production blueprint (RFC 0046) builds a bounded cap-1 turn trunk plus an
exact per-combo turn-rooted river endgame solved online. Its certified error
budget B0 = 0.010 BB already allocates 8.72e-3 BB (RFC 0047 G3 turn gap
1.06e-3 + propagated envelope 5.66e-3 + river R0 <= 2.0e-3). T6 showed the
cap-1 menu restriction contributes a ~20 BB (Uniform) / ~153 BB (BrFavorable)
pure menu term on the deep check-to-river anchor — four to five orders of
magnitude over M and un-absorbable into B0. M2 therefore cannot be certified
until the menu restriction is resolved. D164 was scoped to determine, with
bounded same-caliber measurements, whether simply raising the online cap
recovers the quality bar without breaking the G1 online envelope.

## Goals

- Quantify the exact embedded exploitability `e_embed(k)` for every practical
  bounded cap k = 1..6 on the binding deep anchor, under a fixed comparable
  training configuration, both completion arms, with online NashConv, pure menu
  term, node count, wall/CPU time, and RSS.
- Determine whether any bounded cap achieves a certifiable `e_embed(k) <= 0.002`
  BB within the G1 online envelope (p95 <= 10 s, max <= 12 s, RSS <= 1 GiB,
  nodes <= 5 M).
- Identify the quality-vs-latency trade-off boundary and the viable
  quality-preserving path, without lowering the quality bar.

## Non-Goals

- No production menu, resolver, composition-root, ABI, protocol, or persisted
  format change.
- No re-opening or weakening of the M2 quality contract or the 0.002 BB bar.
- No full-stratum or 432-528-root exhaustive batch under this RFC's measurement
  phase (single bounded anchor roots only; a per-stratum MAX remains a separate
  gate).
- No cap-0 equilibrium solve (the uncapped multi-hour solve stays out of scope;
  the embedding method needs no cap-0 equilibrium).

## Current State and Evidence

All evidence is benchmark-only from `engine/benchmarks/rfc0047`
(`cap_embedding.{hpp,cpp}`, `rfc0047_cap_materiality.cpp`), seedless and
bitwise reproducible. The frozen identity (independently reviewed in T6) is

```
e_embed(k) = e_Gk(sigma_k) + pure_menu_term(k),   both terms >= 0
```

where `e_Gk` is the bounded policy's own exact NashConv and the pure menu term
is the additional best-response value unlocked only by cap-0-only actions.

D164 cap-k curve, w1 deep check-to-river anchor (198 BB behind, pot 4),
300 cap-k iterations, 16 threads, Uniform primary arm, BB:

| cap | online nodes | online NC | e_embed | pure menu | wall (ms) | RSS peak (KB) |
|----|--------------|-----------|---------|-----------|-----------|---------------|
| 1 | 10 | 0.02395 | 19.4822 | 19.4582 | ~40000 | 30460 |
| 2 | 34 | 0.12661 | 3.4023 | 3.2757 | 22054 | 30460 |
| 3 | 106 | 0.09159 | 0.9970 | 0.9054 | 29938 | 34876 |
| 4 | 320 | 0.11082 | 0.2596 | 0.1488 | 53674 | 63428 |
| 5 | 878 | 0.07066 | 0.1089 | 0.0383 | 113493 | 163608 |
| 6 | 1924 | 0.12671 | 0.1289 | 0.00217 | 304047 | 351316 |

Convergence probe at cap-6, 1000 iterations:

| cap-6 iters | online NC | e_embed | pure menu | wall (ms) | RSS peak (KB) |
|-------------|-----------|---------|-----------|-----------|---------------|
| 300 | 0.12671 | 0.1289 | 0.00217 | 304047 | 351316 |
| 1000 | 0.06821 | 0.0743 | 0.00610 | 700438 | 351236 |

Verified facts:

1. The pure menu term falls roughly 4-6x per added raise but only approaches M
   at cap-6, and the 300-iteration near-crossing (0.00217) is a non-converged
   transient: at 1000 iterations the pure menu term rises to 0.00610 (3x over M)
   as the policy's own NashConv falls and the structural restriction is exposed.
2. The online NashConv converges slowly (~T^-0.52; 300->1000 iterations gave
   only 1.86x reduction). At cap-6 the 1000-iteration NC is still 0.0682 BB,
   34x over M; reaching 0.002 by extrapolation needs order 10^5-10^6 iterations.
3. Cost already violates G1 at cap-6: 700 s wall for 1000 iterations (70x the
   10 s p95), 351 MB RSS. The caps whose structural menu term approaches M
   (cap >= 5) cannot be converged online; the caps that converge fast enough
   (cap 1-2) carry a 3-19 BB menu error.
4. Correctness is independent of the convergence issue: the embedded EV root
   values match the bounded kernel's own CFV x 990 to < 1e-6 at both cap-1 and
   cap-2; serial/threaded results are bitwise identical; the 15-fixture native
   test is green.

Conclusion: no bounded cap achieves a certified `e_embed <= 0.002` inside any
online-feasible budget on the deep anchor. Uniform online menu widening is not
a viable standalone fix.

## Design Principles

- The certified quality bar (0.002 BB menu materiality, 0.010 BB B0) is
  immutable under this RFC; a design that cannot meet it is redirected, not
  excused by relaxing the bar.
- The offline boundary and seedless exact enumeration are preserved; any widened
  equilibrium must remain deterministic and independently certifiable.
- Blast radius stays inside the rfc0047 benchmark until a later addendum
  explicitly authorizes a production path.
- Occam's razor: prefer the smallest change that preserves the quality
  guarantee over a new abstraction.

## Proposed Design

This RFC proposes a direction and its gates, not a production implementation.
Two candidate paths preserve the quality bar; a third is rejected.

### Path A (primary candidate): move the widened equilibrium offline

Pre-solve the wide-menu (or cap-0) equilibrium for the binding deep roots as an
offline blueprint artifact, and have the online resolver consume certified
per-combo continuation values rather than re-solving a wide tree within the
request envelope. The online component retains a small bounded tree for
adaptation, but its menu error is bounded against the offline wide reference by
the same embedding measurement, now scored offline without a latency constraint.

- Ownership: offline solver/artifact builder (new addendum), existing BSNCV-style
  codec, online resolver read-only consumption.
- Open design item: artifact size and build cost for wide-menu deep roots, and
  how the offline wide reference composes with the RFC 0047 turn-CV oracle.

### Path B (secondary candidate): reach-aware selective widening

Widen the menu only at high-reach betting nodes, keeping the full cap-1 menu on
low-reach deep multi-raise lines, so the aggregate certified error clears M at a
fraction of the full-wide tree cost. This requires a NEW reach-weighted gate
(the current G5 gate is an exact per-root MAX with no reach weighting), so it
cannot be claimed under the existing contract and needs its own certified
bound derivation.

### Path C (rejected): re-open the quality contract with a menu-error term

Adding an explicit >= 0.006 BB menu-error line to B0 is infeasible: B0 = 0.010
BB already has 8.72e-3 allocated, and D164 explicitly forbids lowering or
re-opening the quality bar.

## Dependency Rules

- Measurement code stays in `bigshark_rfc0047_bench`, which remains forbidden
  from linking any production resolver or composition root (the configure-time
  rfc0047 link guard).
- Any future production widening addendum must depend on this RFC's accepted
  evidence and may not be implemented before independent approval.
- No dependency from production engine code onto the benchmark component.

## Compatibility and Migration

No runtime, protocol, persisted-format, or CLI contract changes in this RFC; the
only CLI touched is a non-installed manual benchmark binary, whose optional CAP
positional defaults to the prior cap-1 behavior. A later Path A/B addendum owns
its own migration and artifact-versioning plan.

## Security and Operational Impact

None for this measurement RFC: no credentials, network I/O, or game-platform
contact (the offline boundary is unchanged). Operational impact appears only if a
later addendum adopts Path A (offline build/storage cost) or Path B (a new
reach-weighted certification gate); both are out of scope here.

## Alternatives Considered

- Uniformly raise the online cap and accept the latency: rejected by the
  measured 70x G1 wall overrun and the un-converged NashConv at cap-6.
- Certify against the cap-1 equilibrium's own NashConv while ignoring the menu
  restriction: rejected as quality laundering; T6 established the menu term is
  the dominant error and the embedding gate exists precisely to catch it.
- Do nothing and ship cap-1: rejected; leaves M2 uncertifiable against B0.

## Risks

- The single-anchor curve is not a per-stratum MAX; shallower post-bet roots may
  clear M at smaller caps. Mitigation: a later bounded per-stratum measurement
  before any production claim; a single-root crossing is never a pass.
- The T^-0.52 convergence extrapolation is an estimate, not a certified iteration
  count. Mitigation: treat Path A/B gates as empirical stop conditions.
- Path A artifact footprint is unmeasured for wide deep-root trees. Mitigation:
  a feasibility spike (node/byte census) is the first Path A stage.

## Verification Plan

- The D164 native test `rfc0047_cap_embedding` (15 fixtures) pins cap-k mapping,
  fail-closed validation, cap-widening monotonicity, the EV-to-kernel-CFV
  oracle at cap-1 and cap-2, and bitwise determinism.
- The cap-k CSV curve and cap-6 convergence probe are the recorded evidence;
  any future Path A/B claim must reproduce the embedding measurement under the
  new design and show `e_embed <= 0.002` with the EV-oracle still < 1e-6.
- Full rfc0047 ctest family plus `check-docs`/`check-rfcs` remain the closure
  gates.

## Rollout Plan

1. Accept this RFC on the measured evidence (no code promotion).
2. Path A stage 1: offline wide-menu deep-root feasibility spike (size/build
   cost), stop if it exceeds the artifact envelope.
3. Path A stage 2 or Path B spike: produce a design whose embedding measurement
   clears M on the binding anchor; stop and re-plan if it cannot.
4. Only after a design clears M within its stated envelope does a separate
   production addendum reach review.

## Rollback Plan

This RFC ships no production behavior, so rollback is confined to abandoning the
benchmark measurement branch; the cap-1 production default and all committed
RFC 0047 serve/certification paths remain intact and are independently
revertible.

## Open Questions

- Is Path A (offline wide blueprint) feasible within the artifact envelope for
  the deep 100 BB roots, and how does it compose with the turn-CV oracle?
- Can a reach-weighted gate (Path B) be derived with a certified bound that the
  existing exact-MAX gate cannot?
- Do any shallower frontier roots clear M at a small cap, changing where
  widening is actually needed?

## Acceptance Criteria

- The cap-k widening curve and cap-6 convergence evidence are recorded and
  independently reviewed.
- A quality-preserving path (A or B) is selected with measurable gates, with
  Path C explicitly rejected.
- No production contract, menu, or quality bar is changed by this RFC, and the
  rfc0047 link isolation and native test suite stay green.

## Decision

Pending independent approval-agent review.
