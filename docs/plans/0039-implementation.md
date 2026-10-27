# RFC 0039 Implementation Plan — Nested River Continuation Values

Status: Current
RFC: docs/rfcs/0039-nested-river-continuation-values.md (Accepted 2026-10-27)
Owning modules: engine resolver, engine tree, engine solver, engine gto/trainer (benchmark driver), apps/engine-host (default-deny wiring)

## Intended outcome

The turn solve path replaces the equity leaf at the river boundary with range-conditioned
nested river subgame values, gated by measured quality. Host routing for 3+ seats lands
default-deny and is enabled only per shape once M2/M1/latency gates pass.

## Acceptance-criteria map

| RFC AC | Implementation site | Evidence |
| --- | --- | --- |
| AC1 | `engine/include/bs/continuation_value.hpp` (new) + resolver | New seam + unit tests |
| AC2 | `NestedRiverValueModel` + `train_nseat` + M2 nested arm in `subgame_nash_conv.cpp` | M2 CSV, CI rows |
| AC3 | M1 rerun `--rfc0036-m1-envelope` + host replay latency | CSV row per shape |
| AC4 | warm validation, key-sensitivity, digest/abstraction/side-pot tests | native + ASan tests |
| AC5 | gate constants + default-deny host entry | host path + receipts unchanged |
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

## Stage 2 — Bounded entry, 4/5/6 seats, host wiring

1. **Default-deny bounded turn entry.** Add the RFC 0036 bounded-turn
   resolver entry and host gate table in `apps/engine-host/main.cpp` and
   `engine/src/protocol/envelope.cpp`, declining every 3+ live turn request
   initially (preserving `turn_root_unserved` labels and receipts). The
   bounded entry exercises the nested path from benchmark/host replay only.
2. **M1 envelope rerun.** `--rfc0036-m1-envelope` nested-backed trees at
   4/5/6 seats on the envelope-affordable iteration counts; record per-shape
   inner solve counts, warm hit/miss/refine, inner iterations, wall time.
3. **Latency gate.** Offline host replay through the existing
   envelope `solve_time_us` timing; receipt-to-action distribution on the
   1,600 ms envelope, nested solve wall time inclusive.
4. **Enablement.** Per-shape gate-table constant edits, each shipped with
   its CSV evidence row and only where M1 upper CI bound is within 0.15 BB
   summed and latency passes. Every other shape keeps declining.

Stop condition: one committed evidence row per shape; shapes that fail stay
declined (the expected 5/6-seat outcome per the Stage-0 arithmetic).

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
- `node bin/replay.mjs` (offline host replay rows only; no live traffic)
- Raw rows commit under `docs/plans/ablation/`; one CSV header version,
  seeds/menus/caps/CI declared in the file.

## Rollback

Revert the additive resolver/host-gate commit set; default-deny tables restore
current `turn_root_unserved` behavior. No artifact or wire-format change in
Stages 1-2.
