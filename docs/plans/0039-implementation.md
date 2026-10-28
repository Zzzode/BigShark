# RFC 0039 Implementation Plan — Nested River Continuation Values

Status: Current (Stage 1 complete at its design-level-failure stop branch; amendment D1-D5 independently re-approved 2026-10-28)
RFC: docs/rfcs/0039-nested-river-continuation-values.md (Accepted 2026-10-27; returned to Proposed; amendment re-Approved 2026-10-28)
Owning modules: engine resolver, engine tree, engine solver, engine gto/trainer (benchmark driver); apps/engine-host instrumentation-only (no enablement per addendum D3)

## Intended outcome

The turn solve path replaces the equity leaf at the river boundary with range-conditioned
nested river subgame values, gated by the amended two-number quality scheme (abstract-game
bucket gap as the leaf serve predicate; exact-hand gap plus composed M2 delta retained as
deployment evidence). Stage 1 evidence showed direct nested solving cannot fit the live
envelope, so per-shape host enablement is WITHDRAWN from this RFC (addendum D3); any
serving of real turns is deferred to a separate Stage 3 (persisted CV) or Stage 4
(learned estimator) RFC.

## Acceptance-criteria map

| RFC AC | Implementation site | Evidence |
| --- | --- | --- |
| AC1 | `engine/include/bs/continuation_value.hpp` (new) + resolver | New seam + unit tests |
| AC2 | `NestedRiverValueModel` + `train_nseat` + M2 nested arm in `subgame_nash_conv.cpp` | inner-series/identity-control/M2 CSVs, independent-oracle ctest (gate phrasing amended per addendum D2) |
| AC3 | AMENDED per addendum D2/D3: leaf serve predicate uses the bucket gap; 4/5/6 M1 rerun moved to the Stage 3 RFC | bucket-gap CSVs (this RFC); M1 owned later |
| AC4 | warm validation incl. failed-rerefine invalidation, key-sensitivity, digest/abstraction/side-pot tests | native + ASan tests |
| AC5 | SUPERSEDED per addendum D3: no routing changes; default-deny / `turn_root_unserved` behavior and receipts unchanged | grep + unchanged envelope labels |
| AC6 | raw rows under `docs/plans/ablation/rfc0039-*.csv` | check-rfcs + check-docs |
| AC7 | docs/design/gto-engine.md, RFC 0036 status, delivery plan | same change |

## Stage 1 — Seam, nested model, warm M2 (2/3 seats)

Measurement only; production routing stays `turn_root_unserved`.

1. **Continuation-value seam.** Add `ContinuationValueModel` in
   `engine/include/bs/continuation_value.hpp` with `RiverBoundaryLeaf`,
   `ContinuationBudget`, and MISS/DECLINE/INVALID statuses. Keep
   `FrontierEvaluator` unchanged. New code only under `engine/src/resolver/`,
   `engine/include/bs/`, `engine/src/tree/` (boundary reach hooks).
2. **Exact conditioned ranges.** Reuse the `continuation_range` export path
   (`engine/include/bs/continuation_range.hpp`) for per-sweep trunk CF reaches;
   reuse the RFC 0013 `extract_root_value` walk kernel for marginal extraction.
   No empirical belief window.
3. **Nested river model.** `NestedRiverValueModel` builds a river-rooted
   `GameDef`/`AbstractTree` from the outer tree's own `ActionAbstraction`
   (digest asserted equal), conditioned leaf ranges, RFC 0012
   `NSeatInitialState`, solves with `NSeatTrainer` passing
   `NSeatInitialState`; extracts per-combo marginal EV via the walk kernel.
4. **Absolute-gap gate and budget.** Inner BR gap measured by the pooled
   walker on nested trees (enumerable fixtures) or the RFC 0036 sampled BR
   estimator plus CI bound (full ranges); acceptance only after a passing
   absolute gap (0.01 BB summed fixture value, Stage 1). Budget shares one
   envelope derived from RFC 0036 Stage-0 node/action/seat coefficients;
   decline maps to the existing Ineligible outcome.
5. **Solve-scoped warm map.** Leaf identity -> `NSeatInitialState`,
   released with the solve; revision/batch/row-shape validation, cold start
   on mismatch; cross-drift warm reuse carries no identity guarantee.
6. **M2 nested arm.** Extend `bigshark-subgame-nash-conv --rfc0036-m2`
   with a nested leaf arm on the existing 2/3-seat 10-hand sub-ranges:
   converged nested marginals must match a direct full solve within 0.005 BB
   per combo, and nested turn values must pass the frozen 0.01/0.03 BB M2
   thresholds over seeds 101/202/303.

Stop condition: M2 passes at 2/3 seats, or a documented design-level failure
returns the RFC to review (the 0.01 BB band proves unreachable on the pinned
fixtures). Fixture-level convergence-series test records inner gap versus
inner iterations.

### Stage 1 implementation notes (2026-10-27)

Block exact policy iteration is the chosen outer cadence (independent review
finding: a nested solve per MCCFR sweep per leaf is infeasible — 240/1,344
TerminalFrontier leaves at 2/3 seats, counted empirically: 357-node 2-seat
and 1,864-node 3-seat turn trees). Each outer block: export exact conditioned
leaf ranges from the current sealed average policy (uniform for block 0),
warm-refine every reachable one-street river subgame, then run an outer
iteration block with `train_nseat` continuation hook returning
`V_traverser(own combo)`; iteration resumes bitwise across blocks. This
preserves the RFC essentials (exact non-empirical ranges, solve-scoped warm,
absolute-gap gate) and changes only the refinement cadence. The model TUs
live in `bigshark_solver` (`engine/src/gto/`) because the trainer leaf
dispatch is solver-local; resolver linkage would cycle. Inner absolute BR
gap uses the benchmark's pooled information-set-consistent walker directly
on each solved river tree (0.01 BB summed tolerance), avoiding the stage6
offline link guard.

Observed 2-seat composed M2 rows (`--rfc0039-m2`, 10-hand fixture;
FORCE_SERVED means every solved leaf feeds the outer trainer regardless of
the inner gate, a measurement-only escape hatch in the benchmark):

| outer/inner/blocks | gating | seed | ref NC BB | composed BB | delta BB |
| --- | --- | --- | --- | --- | --- |
| 100k/40k/3 | gate-honest | 101 | 2.7511 | 3.1422 | 0.391 (= equity fill) |
| 100k/40k/3 | force-served | 101 | 2.7511 | 2.8543 | 0.103 |
| 100k/40k/3 | gate-honest | 202 | 2.7094 | 3.2054 | 0.496 |
| 100k/40k/3 | force-served | 202 | 2.7094 | 3.2046 | 0.495 |
| 100k/40k/3 | gate-honest | 303 | 2.7100 | 3.0303 | 0.320 |
| 100k/40k/3 | force-served | 303 | 2.7100 | 3.1601 | 0.450 |
| 100k/200k/3 | force-served | 101 | 2.7511 | 2.9392 | 0.188 |

The early force-served rows are NOT a convergence series (the outer
iteration count changes between them); the 0.20 -> 2.44 -> 0.103 deltas
from the seed-101-only first pass were non-monotone and must not be
described as one. The controlled multi-seed comparison at identical
100k/40k/3 settings shows serving 40k inner matrices is seed-noise
dominated: seed 101 improves by 0.29 BB over the equity fill, seed 202 is
unchanged, seed 303 gets 0.13 BB worse (mean improvement ~0.05 BB inside a
~0.22 BB spread), and seed 101 at 200k inner (0.188) is worse than at 40k
(0.103). Every delta is far above the frozen 0.03 BB summed M2 gate
regardless. The decisive remaining question is whether CONVERGED inner
matrices (bucket-gap-passing, ~10M/leaf) yield a reliably small composed
delta across seeds; the force-served inner-1M cell and a six-block
cadence cell are pending in `docs/plans/ablation/rfc0039-m2.csv`.

### Stage 1 adversarial review (2026-10-27, workflow wf_89b34872)

Five max-effort dimension reviewers plus per-finding independent
verification confirmed 3 blockers and several majors; the load-bearing ones
are fixed:

- **FIXED (blocker):** the outer block train now passes BOTH the nested
  model and the EquityFrontierEvaluator; a Miss/Decline/Invalid leaf takes
  the declared non-nested equity leaf instead of throwing into the MCCFR
  sweep (verified: the small run that printed INFEASIBLE now trains).
- **FIXED (blocker, fail-closed):** the 0.01 BB absolute inner gap is now a
  real gate, not a counter — `record_inner_gap(leaf, gap, passed)` carries
  the decision, `lookup_scalar` returns Miss unless a gap was recorded as
  passing on the CURRENT ranges (the flag is invalidated on every refine),
  and `--rfc0039-m2` emits GATE_FAIL with a nonzero exit code unless every
  refined leaf passed. A new re-refine invalidates the acceptance until the
  driver re-measures.
- **FIXED (major):** the leaf-range differential test now uses a
  deterministic non-uniform (a+2 weighted) policy and independently looks
  up each path edge's bucket/probability, so it can detect wrong
  edge/actor/bucket addressing (the uniform version was tautological);
  warm-resume assertions pin exact cumulative iteration totals.

Outstanding before the Stage 1 stop condition can be claimed:

- **(major, FIXED in round 2)** a failed re-refine (Decline/Invalid under
  drifted leaf ranges) left the PREVIOUS block's gap-certified matrix
  stored and served, silently bypassing the equity fallback. `refine()` now
  invalidates the prior certification before any failure can return; a
  regression test covers Solved -> certified -> Decline -> Miss ->
  re-Solved -> re-certified.
- **(blocker B, closed in round 2 with a noted scope gap)** a native ctest
  (`test_nested_marginals_match_independent_full_solve`) compares nested
  per-combo marginals to an INDEPENDENT direct full DepthLimit::none()
  solve. Both sides average three independent seeds at 2M iterations on a
  shallow 2-BB-deep 2-seat fixture (worst per-combo delta 0.00467 BB);
  the AC2 text pins the 100 BB M2 fixture and mentions 2/3 seats, so this
  is strong wiring/identity evidence but a shallower substitute — the
  100 BB per-combo comparison exists in the M2 driver (first 16 leaves)
  only as a reported column, not a gate, and needs a converged inner run.
- **(major, FIXED in round 2)** the inner-gap measurement now uses exact
  per-seat weighted joint deals (`compute_nash_conv_weighted`): product
  weights, board/folded handling, total-mass normalization on both
  strategy and BR sides. The driver gates on this weighted measure.
- **(major, FIXED in round 1)** the structural isomorphism test compares
  the terminal ledger (contributed/refunded/stack/folded/live_order/board)
  as well as topology/menus.
- **(economics, confirmed in round 2)** the standalone cap-1 river tree
  needs ~10M inner iterations per leaf (~138.6 s cumulative cold-start
  wall) for a sub-0.01 BB gap under the bucket measure; the exact-hand
  measure does not converge below ~0.26 BB at all (abstraction floor).
  Against 240/1,344 leaves at 2/3 seats the live 1,600 ms envelope is
  unreachable by direct solving; this drives the Stage 3/4 pivot.


### Stage 1 stop-condition evidence (2026-10-27, inner-gap convergence series)

The single-leaf absolute inner BR-gap was measured on the cap-1 river
subtree behind the first checked-through turn frontier leaf (100 BB,
10-hand representative fixture, exact conditioned leaf ranges, bitwise
warm-resume trajectory). Raw rows:
`docs/plans/ablation/rfc0039-inner-gap-series.csv`.

| cumulative inner iters | exact-hand gap avg BB | current-strategy BB | bucket-BR BB | wall ms |
| --- | --- | --- | --- | --- |
| 100k | 0.2947 | 0.3930 | 0.1271 | 1,052 |
| 300k | 0.2591 | 0.2817 | 0.0598 | 2,842 |
| 1M | 0.2650 | 0.3409 | 0.0293 | 10,157 |
| 3M | 0.2794 | 0.3018 | 0.0161 | 28,099 |
| 10M | 0.2872 | 0.5368 | **0.0081 PASS** | 95,708 |

3-seat (88 nodes, 117 info sets): exact gap 0.360 BB at 1M/0.356 at 3M;
bucket gap 0.115 BB at 300k -> 0.0556 at 1M -> 0.0287 at 3M, still
falling (log-log slope ~-0.62; 0.01 crossing extrapolated to ~16-25M
iterations per leaf, ~15-24 min/leaf at ~57 us/iter).

Two distinct findings:

1. **The exact-hand pooled-BR gap floors at ~0.26 BB independent of
   iterations.** The sealed average strategy sits at 0.259-0.295 BB from
   300k to 10M; the RM+ current strategy is in the same band but noisier
   (0.282 best, 0.537 at 10M), so it is a corroborating signal, not an
   identical twin. The finding is not "both strategies measure exactly
   the same number"; it is that NEITHER drives the exact-hand gap toward
   zero while the bucket gap converges. The responder in the exact
   measure groups by exact hole cards while the policy acts on the
   9-bucket `kNSeatCardKind` abstraction: the floor is the
   **card-abstraction exploitability** of exact-card best response
   against a bucketed strategy (the RFC 0031 loss family), not solver
   non-convergence. A bucket-restricted BR (one action per bucket, the
   granularity the trainer actually solves) converges at ~1/sqrt(N):
   0.060 (300k) -> 0.029 (1M) -> 0.016 (3M) -> 0.0081 (10M, passes
   0.01 BB).

   **Causal control (`--rfc0039-identity-control`, raw rows
   `rfc0039-identity-control.csv`).** On As Ks Qs Js 2d a seven-combo
   range realizes seven distinct buckets (straight flush through ace
   high) on fourteen pairwise-disjoint cards, so each production bucket
   contains exactly one combo and the exact/bucket groupings coincide.
   The two measures then agree to machine epsilon (delta <= 4e-16) at
   every level and the gap converges with NO floor: 2.59 -> 0.229 (100k)
   -> 0.056 (1M) -> 0.030 (3M) -> 0.0157 BB (10M). The floor therefore
   requires within-bucket card discrimination and is isolated to the
   card abstraction; it cannot be blamed on the walker or the solver.
2. **Even under the abstract-game measure the economics are structural.**
   One representative 2-seat leaf needs ~10M inner iterations for 0.01 BB;
   the cumulative cold-start wall to 10M is ~138.6 s (the 95.7 s row is
   only the 3M->10M increment), i.e. ~87x the 1,600 ms envelope for one
   leaf alone. A turn solve has 240 (2 seats) / 1,344 (3 seats) frontier
   leaves and block policy iteration re-refines every reachable leaf per
   block: ~9 hours serial per 2-seat turn solve at ~13.7 us/iter, and the
   3-seat extrapolation is days (~14 days/solve at the ~16M crossing).
   This covers only the cheapest checked-through 1-BB-pot leaf on one
   board; bet/raised lines carry bigger pots and converge slower per BB,
   so these are optimistic bounds.

The frozen RFC 0039 gate text ("0.01 BB summed absolute inner BR-gap",
"exact pooled walker") predates this decomposition: independent review
(workflow wf_0d45b1b0, 2026-10-27) confirmed the accepted wording is the
EXACT-hand measure, and that the 0.01/0.03 band was imported from the
RFC 0036 M2 model-delta context (where the abstraction floor cancels to
first order) into an absolute-gap context (where it cannot). Under the
exact measure the band is structurally unreachable by any bucketed
strategy — including the full-solve reference — for reasons independent
of nested continuation values; under the abstract-game (bucket) measure
it is reachable only offline at 2 seats with heavy compute. This is the
Stage 1 stop-condition branch "a documented design-level failure returns
the RFC to review". Review must decide, and must NOT "amend and pass":
even the abstract-game evidence does not pass the frozen composed M2
band (0.103 BB force-served at 40k inner vs 0.03; force-served
high-inner rows pending). The disposition under consideration: (a) redefine
the inner convergence gate as the abstract-game bucket gap with explicit
lifted-game semantics while KEEPING the exact-hand number as deployment
evidence budgeted under RFC 0031 (reporting only 0.008 while hiding 0.26
would be quality laundering — both numbers answer different questions and
stay on record); (b) pivot to Stage 3 (persisted CV reuse) / Stage 4
(learned estimator) as the only envelope-compatible producers, with Stage 2
latency gated on their outputs. No host wiring or enablement has shipped;
production stays `turn_root_unserved` (grep-verified zero references
outside the benchmark, the new TUs, the header, and the ctest).

Independent oracle validation (AC2 blocker B) exists as a native ctest:
`test_nested_marginals_match_independent_full_solve` compares the model's
per-combo marginals against a separately solved `DepthLimit::none()` river
tree. Single MCCFR runs are dominated by fixed-seed worst-of-combo tail
noise (the statistic is non-monotone in iterations for one run), so both
sides average three independent seeds at 2M iterations each on a shallow
2-BB fixture; the worst per-combo delta is 0.00467 BB (gate 0.005 BB).

### Stage 1 round-2 adversarial review (2026-10-27, workflow wf_0d45b1b0)

Four max-effort dimension reviewers (measure math, model correctness,
gate/RFC validity, economics/completeness) plus independent verification
of every blocker/major, 14 agents, 1.46M tokens. Outcomes:

- Measurement math: no defect; the exact/bucket decomposition is sound
  (the restricted BR strategy set is a strict subset, gap_exact >=
  gap_bucket holds on every CSV row), the weighted joint measure matches
  the trainer's own dealing normalization, and the fail-closed loop is
  real. One requirement: keep BOTH numbers on record (internal
  convergence and real-game exploitability answer different questions).
- Model correctness: one CONFIRMED major (stale matrix after failed
  re-refine), fixed with a regression test; no sign/unit/zero-sum/key
  aliasing defects; ledger reconstruction verified.
- Gate validity: CONFIRMED the frozen gate is exact-hand and the
  0.01 number was mis-imported from a model-delta band; the bucket
  redefinition is legitimate only as a convergence gate paired with
  retained exact-card deployment evidence, and an RFC amendment cannot by
  itself declare Stage 1 passed.
- Economics/completeness: CONFIRMED the per-leaf wall arithmetic (with
  the cumulative 138.6 s / 87x correction) and flagged missing evidence:
  the identity control (since added), multi-seed bucket series, the
  per-leaf gap distribution beyond the cheapest check-through leaf, and
  the 3-seat 0.01 crossing. The composed-M2 proposition has been
  measured only at 40k inner (0.103 BB force-served); force-served
  high-inner rows are the remaining decisive evidence before RFC review.

## Stage 2 — Withdrawn enablement; instrumentation-only (amended by the 2026-10-27 Review Addendum D3)

The Stage 1 evidence showed direct nested solves cannot pass the latency
gate (~138.6 s cumulative cold-start per 2-seat leaf for the 0.01 BB
abstract-game gap, ~87x the 1,600 ms envelope; days per 3-seat solve), so
the enablement half of the original Stage 2 plan is withdrawn by the RFC
addendum and these items no longer gate this RFC:

- **M1 envelope rerun at 4/5/6 seats** and the **latency/1,600 ms
  enablement gate** move to the future Stage 3 RFC (persisted CV reuse) as
  evidence for whether precomputed continuation values can serve shapes.
- **Per-shape gate-table enablement is deleted** from this RFC: the gate
  table stays default-deny for every shape, and no enablement CSV row is
  required or accepted here.

The only Stage 2 work this RFC still permits is optional, observable-
behavior-preserving instrumentation:

1. **Default-deny bounded turn entry (instrumentation only).** The RFC
   0036 bounded-turn resolver entry and host gate table may be added in
   `apps/engine-host/main.cpp` and `engine/src/protocol/envelope.cpp`
   declining every 3+ live turn request, preserving today's
   `turn_root_unserved` labels and receipts. No shape is ever enabled.
2. **Offline measurement hooks.** The benchmark/host timing path may
   record what a nested solve WOULD cost (refine counts, inner
   iterations, wall time) without serving the result to a live decision.

Stop condition: zero observable routing change; the Stage 3 RFC owns any
future enablement with its own independent review.

## Stage 3 — Trigger-gated follow-ons (separate RFCs)

- Persisted/process CV reuse: new RFC only when measured miss frequency and
  latency distributions support it; format owned as an additive BSFLIB04-style
  section, never by amending this RFC after acceptance.
- Learned estimator: separate RFC with data provenance, C++ inference contract,
  RFC 0031-style abstraction-loss measurement.

## Verification gates per stage

- `cmake --preset release && cmake --build --preset release --target format`
- `cmake --build --preset release`; focused + full `ctest --preset release`
- `ctest --preset asan` on touched surfaces (nested solver, reach, warm map)
- `npm run check`, `npm run proto:check`, `node bin/check-rfcs.mjs`,
  `node bin/check-docs.mjs`
- `cmake --build --preset release --target benchmark-multistreet`
- Raw rows commit under `docs/plans/ablation/`; one CSV header version,
  seeds/menus/caps/CI declared in the file.

## Rollback

Revert the additive resolver/host-gate commit set; default-deny tables restore
current `turn_root_unserved` behavior. No artifact or wire-format change in
Stages 1-2.
