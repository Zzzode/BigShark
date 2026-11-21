# RFC 0041 Implementation Plan — Learned River Continuation-Value Network

Status: **Historical** — RFC closed 2026-10-29 at Stage 1 with a recorded
FAILURE (Amendment 2); no Stage below was implemented. Kept as the
sequencing/failure reference. Amendment 1 (10,000 ms decision envelope)
was approved independently and remains in force.
RFC: docs/rfcs/0041-learned-river-continuation-value-network.md
Owning modules: engine resolver (bounded turn entry + production block refinement driver), engine gto/solver (NeuralContinuationValueModel + BSNCV01 codec), engine benchmarks (label generator + gates), tools/neural-cv (offline trainer), apps/engine-host (default-deny mount)

## Intended outcome

A feed-forward f32 value network replaces infeasible per-solve river MCCFR at turn
depth-limit leaves by implementing the RFC 0039 `ContinuationValueModel` seam:
offline exact-solver labels, offline-only Python training, a digest-pinned
BSNCV01 artifact, C++ in-process inference, and default-deny host enablement
behind the RFC's frozen AC4/AC5 gates. Any failure is recorded honestly and the
engine stays on the current continuation-null path.

## Acceptance-criteria map

| RFC AC | Implementation site | Evidence |
| --- | --- | --- |
| AC1 | `engine/include/bs/neural_continuation_model.hpp` + `src/gto/neural_continuation_model.cpp`; additive §3a fields/enum in `continuation_value.hpp` | native ctests incl. zero-sum Decline, Inferred, determinism |
| AC2 | `engine/include/bs/neural_cv_codec.hpp` + `src/gto/neural_cv_codec.cpp` | codec ctests (128-byte table, tensor blob, every rejection, scope) |
| AC3 | `engine/benchmarks` label generator; `tools/neural-cv/` trainer | label manifest, training report JSON, tolerance-defined reproduction |
| AC4 | benchmark neural M2 arm + exact-hole D1 column | `rfc0041-*.csv`, bootstrap intervals, strict adoption rule |
| AC5 | host timing path + per-decision aggregate benchmark; full-range exporter cost (G8) | latency CSV, serve-rate, 16 MiB budget check |
| AC6 | gate table in resolver/host + envelope receipt tokens; conformance | unchanged non-enabled behavior pinned |
| AC7 | build contract + docs/README indexes + RFC 0036/0039 cross-refs | release/asan ctest, npm, proto, check-docs |

## Prerequisite: RFC 0039 E4 sequencing

Stage 1 below starts only after these land on the RFC 0039 code line:
1. benchmark serve predicate switched to the D1 bucket gap while still
   recording the exact gap and composed M2 delta;
2. a converged 100 BB per-combo oracle row produced in the release
   configuration (the ctest oracle currently uses a shallow 2-BB fixture);
3. `subgame_nash_conv.cpp` shared driver rebased so the exact and neural
   producers both fit (this is also G7's benchmark-generalization work).

## Stage 1 — Exact label generator (no serving, no network)

Frozen pre-generation plan: [0041-label-cells.md](0041-label-cells.md)
(board-class universe and 300/60 split, line quotas, range profiles,
seed/iteration scheme, storage and compute budget). Empirical inputs:
`docs/plans/ablation/rfc0041-leaf-inventory.csv` and
`rfc0041-leaf-cost.csv`.

1. Add `--rfc0041-genlabels` to the subgame nash-conv benchmark (or a sibling
   binary if it grows): enumerate canonical river-boundary leaves over the
   committed fixed cell plan — board classes (canonical suit classes), line
   quotas with mandatory bet/raised leaves, stack/SPR grid, 2 seats first,
   1,326-combo full ranges as well as the 10-hand fixture.
2. Per leaf: export exact conditioned ranges; solve with NestedRiverValueModel
   at the predeclared label scheme (fixed high iteration + >=3 independent
   seeds averaged); measure per-combo label disagreement (<=0.003 BB gate) and
   bucket/exact gaps.
3. Emit shards + manifest (feature vectors, V_p(c) labels, seed/iter/gap
   metadata, per-shard sha256); bulk shards gitignored, manifests committed.
Stop: reproducible labels with measured noise <=0.003 BB and recorded
per-class compute/storage budget.

## Stage 2 — Trainer, codec, C++ inference (offline only)

1. `tools/neural-cv/`: hash-pinned requirements (PyTorch CPU), isolated from
   CMake/ctest/npm CI; loader, training under a committed hyperparameter file,
   fixed framework-neutral weight export (the §6 tensor order), training
   report JSON with revisions/seeds/metrics.
2. `BSNCV01` codec: exact 128-byte header, 4096 alignment, layered f32 blob
   reader/writer, scope table + scope_digest, manifest digest, atomic publish
   and .sha256 sidecar; no SQLite.
3. `NeuralContinuationValueModel` in bigshark_solver: scalar f32 GEMV (SIMD
   later behind the same architecture id), mmap/read-only weights, reusable
   pre-sized scratch, feature builder, optional zero-sum projection, §3a
   Solved/Invalid/Decline rule with the always-on weighted residual
   (total_pot = ledger leaf pot, tolerance pinned this stage).
4. Pin architecture widths inside the 16 MiB byte ceiling; stamp
   ContinuationTermination::Inferred.
Stop: reproducible artifact round-trips; native ctests green; no host change.

## Stage 3 — Offline gates (the decision point)

1. AC4: board-class x seed bootstrap protocol (>=60 held-out classes, >=3
   seeds, 10k bootstrap, range-mass weighted); per-combo 0.01 BB mean CI and
   0.05 BB p95; production-semantics composed-M2 arm (mixed served/declined)
   with the strict hi(neural)<lo(equity) rule, <0.03 band, and the exact-hole
   D1 column vs the RFC 0039 baseline.
2. AC5: whole-decision aggregate T=export+features+inference+outer MCCFR over
   leaves x blocks (p95 <=1,600 ms, 28-core reference, >=200 decisions),
   inference+feature subtotal <=400 ms, model <=16 MiB, serve rate >=99%.
3. G8: full-range conditioned-range export cost measured; optimize/memoize
   or add a bounded sampled exporter before enablement if it breaches budget.
Stop: documented PASS (continue) or FAILURE/INCONCLUSIVE (RFC closes, engine
unchanged). Rows under docs/plans/ablation/rfc0041-*.csv with frozen headers.

## Stage 4 — Default-deny host path and shadow

1. Production bounded cap-1 streets(1) turn entry in the resolver; production
   outer block refinement driver within request wall budget; model
   mount/construction from a digest-pinned BSNCV01 at resolver setup; gate
   table initially with no enabled shape.
2. Receipt tokens: SOLVER_SOURCE_RESOLVING + guarantee_level=approximate +
   artifact_sha256 + reason_code neural_cv_served/neural_cv_declined.
3. Shadow run on bigshark-practice, >=10,000 decisions, actions unchanged:
   serve/decline rate, per-share timings, zero-sum residual.
Stop: zero observable behavior change and shadow metrics consistent with
Stage 3; full conformance suite unchanged.

## Stage 5 — Conditional per-shape enablement

Enable exactly the 2-seat stack/line classes whose AC4/AC5 evidence passes,
each with its own CSV row, digest, and a FRESH independent review
confirmation. 3 seats only after their own generator run, gates, and review.
Removal is per-shape via the gate table.

## Verification gates per stage

- cmake --preset release && cmake --build --preset release --target format
  && cmake --build --preset release
- ctest --preset release (full), focused ctest per stage during development
- ctest --preset asan on touched surfaces (neural model, codec, resolver hook)
- cmake --build --preset release --target benchmark-multistreet
- npm run check && npm run proto:check
- node bin/check-rfcs.mjs && node bin/check-docs.mjs

## Rollback

Remove the gate entry, the mounted artifact, and (until other consumers
exist) the bounded entry/driver; the resolver returns to today's
continuation-null behavior. No wire/persistence migration; per-shape
rollback is independent.
