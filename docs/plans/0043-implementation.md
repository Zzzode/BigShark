# RFC 0043 Implementation Plan — Offline Equity-Strength Blueprint Feasibility

Status: Current (COMPLETE — Stage 3 returned structural no-go for all
three bands 2026-10-31; second deletion performed; evidence CSVs and
the RFC retained)
RFC: [0043](../rfcs/0043-offline-equity-blueprint-feasibility.md) (Accepted
2026-10-31, four-round independent review; approval agent
a77541adae094e220)

Scope reminder: this RFC is MEASUREMENT-ONLY. It re-implements the
EquityStrengthV1 declared function to RFC 0042's frozen text, adds
iteration-ladder convergence cells and full-range cost cells to the
subgame benchmark, and mechanically decides whether an uncompressed
134,459-class river blueprint campaign is feasible. No artifact, build,
pack, checkpoint, or serve site changes. On any outcome other than
river-Go the new code is deleted (its second deletion); evidence CSVs
and the RFC remain.

## Stage map

### Stage 1 — Re-implementation and measurement driver (COMPLETE)

1. Abstraction layer (`engine/include/bs/abstraction.hpp`,
   `engine/src/abstraction/abstraction.cpp`):
   - `CardBucketKind::EquityStrengthV1 = 4`, exact conditional
     enumeration (990 / 45,540 / 1,070,190 terms), K 32/64/64,
     L=2..10 band table (max id 1,608), four-arg entry with the
     three-arg entry refusing the kind, mutex-guarded per-set memo;
   - Monte Carlo EHS seam (test-only; corrected `pair_index` inversion,
     verified in release + ASan);
   - `CanonicalRiverBoard`, `canonicalize_river`, and
     `enumerate_canonical_river_classes() == 134,459`;
   - digest recomputed and golden-pinned: `0x274f82fe89b66a1e`.
2. Trainer seam (`nseat_trainer.hpp/.cpp`): `card_kind` /
   `equity_grid` on `NSeatTrainerLimits` (defaults = production kind),
   threaded through the three walk/lookup bucket sites and
   `from_parts`; production defaults unchanged.
3. Benchmark (`engine/benchmarks/subgame_nash_conv.cpp`):
   - RFC 0043 scheme global and `rfc0043_bucket` at the four NashConv
     bucket sites;
   - `--rfc0043-ladder`, `--rfc0043-cost`, `--rfc0043-flop` cells with
     one frozen 18-column CSV header, per-cell `/proc/self/statm` RSS
     sampling, 3,600 s safety wall, and the four canonical flop
     representatives;
   - smoke-validated: HU ladder (exact/bucket gaps, truncation, turn
     sentinels), HU cost (gap sentinels), flop tree builds.
4. Tests: 20 ctest cases in `test_abstraction.cpp` including the
   independent oracle and 134,459 enumeration; release + ASan pass.

### Stage 2 — Pre-run freeze (COMPLETE, independent-review round 2 fixes)

- Stage-2 freeze addendum in the RFC (digest, CSV contract, aggregation
  mechanics, AC6/AC7 interpretation, canonical flop representatives,
  host/disk/RAM snapshot, parallelism, commands, seeds).
- `convergence` mode in `tools/rfc/rfc0042-gates.ts` with 26 new Node
  tests (33 total): r* selection, plateau, paired benefits, half-width,
  truth table, cost projection, evidence integrity, whole-campaign set
  guard.
- Independent review (agent ad8498345d9e9ba69) returned Changes
  Requested with 1 BLOCKING + 3 MAJOR; all fixed:
  - B1: a truncated candidate-band cost run now makes every candidate
    band cost-INCOMPLETE (undefined set projection), never a cost FAIL;
  - M1: the addendum's flop canonical tuples were themselves wrong
    (private verifier used an inverted suit order c=0..s=3; the engine
    is s=0,h=1,d=2,c=3). Correct frozen tuples: 7s Kh Ad / 7s Kh Kd /
    Qs Ks As / 7s Kh As; driver table, evaluator whitelist, and the
    addendum now agree with production `canonicalize()`;
  - M2: ladder rungs enforce exactly five paired seeds per scheme with
    stable seed sets across rungs; a truncated tiers control throws;
  - M3: NashConv walks ARE OpenMP-parallel (three pragmas in the
    benchmark); every frozen command is pinned `OMP_NUM_THREADS=1`.
  - minor fixes: exact 10M catch-all reason strings, three-rung
    plateau-above requirement, 3,600 s wall validation, cost-rung
    whitelist, whole-set campaign guard, flop failure-note preservation,
    removal of the unsupported `-ffp-contract=off` claim, explicit null
    wall projection when W=0.

### Stage 3 — Frozen campaign (PENDING)

Only after Stage 2: run the frozen shard waves listed in the addendum's
command table on `n37-100-184`. The engine is single-threaded (no
OpenMP TU exists), so fan-out is process-per-shard via the
`[shard_index shard_count]` arguments:

1. ladder: 30 concurrent processes (10 shards x seats 2/3/4);
2. cost: per converged band, 2 shards, sequential waves L=2 -> 3 -> 4,
   rung = evaluator-printed r*;
3. flop: 2 shards per seats group, seats 2 then 6.

Commit the raw shard CSVs to `docs/plans/ablation/` and evaluate the
complete set with
`node bin/rfc0042-gates.mjs convergence <csv...>`. No cell may use a
rung, seed, width, or wall not pinned in the addendum.

### Stage 4 — Mechanical decision and review (PENDING)

Record the per-band river-Go / no-go / measurement-incomplete verdict
in the RFC after independent review of evidence + evaluator behavior.
On river-Go, a separate RFC designs the river artifact/flip campaign
(scoped exactly to Go bands). On no-go / incomplete, delete all new
code per the no-legacy rule and mark this RFC Implemented with the
evidence retained.

## Verification

- `cmake --build --preset release --target format`;
- release + asan `ctest` (new oracle / bucket / canonical-enumerator
  cases);
- benchmark build and smoke runs;
- `npm run check`, `npm run proto:check`,
  `node bin/check-rfcs.mjs`, `node bin/check-docs.mjs`.

### Stage 3 — Frozen campaign (COMPLETE, no-go)

30 ladder shards (2/3/4 seats, 5 paired seeds, five rungs) and the
flop seats=2 grid completed. Every river rung completed; the frozen
20% relative plateau never formed for either abstraction (incumbent
nine-tier fails identically), so no band has an r* and the AC6/AC7
cost gates were never measured. Mechanical verdict and independent
ruling (agent ad8498345d9e9ba69): structural no-go for L=2, L=3, L=4
under the frozen catch-all; not measurement-incomplete. Evidence
retained in `docs/plans/ablation/rfc0043-ladder-*.csv` and
`rfc0043-flop-seats2-*.csv`; flop seats=6 stopped per the ruling agent
(zero decision weight).

### Stage 4 — Ruling and second deletion (COMPLETE)

RFC 0043 marked Implemented with the no-go outcome and full evidence
tables. All measurement code deleted (abstraction EHS machinery and
river enumerator, trainer seams, benchmark drivers and bucket hooks,
ctest oracle cases, RFC 0043 evaluator modes/tests). The RFC 0042
pilot/gate evaluator and its 7 tests remain; production nine-tier
paths are byte-for-byte restored. A future convergence-certification
campaign needs a new RFC with a reachable predeclared instrument
(absolute-BB slope window calibrated on tiers, possibly an extended
ladder) and must re-measure the 134,459-class cost.
