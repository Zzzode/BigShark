# RFC 0044 Implementation Plan — Reachable Convergence Certification

Status: COMPLETE (Stage 1-4 2026-11-01: Stage 3 HU campaign
finished 160/160 cells; mechanical verdict CERT@100M for equity but
QUAL FAIL (exact 0.0484 BB > 0.03; 300M 0.0459, card-abstraction
floor) = predeclared no-grid outcome; independent evidence review
Approved; Stage 4 measurement code deleted, evaluator+evidence retained)
RFC: [0044](../rfcs/0044-convergence-certification.md) (Accepted
2026-10-31, independent approval agent, two review rounds)

Scope reminder: MEASUREMENT-ONLY. Stage 1 re-implements the RFC 0042
EquityStrengthV1 machinery (deleted at RFC 0042/0043 closure) plus the
extended-ladder driver and the frozen CERT/QUAL evaluator. No
build/pack/resident/serve/protocol change; the BSWARM03 codec kind-byte
case is additive and older readers fail closed on the new byte.

## Stage 1 — RFC and approval (COMPLETE)

Accepted after two independent review rounds. Round 1 findings F1-F4
(forward-projection rule passed a one-rung dip; single-sided QUAL
optimism; BB/100 normalization; BSWARM01/wall semantics) all resolved
and independently recomputed; two addendum items and three text
revisions attached and incorporated.

## Stage 2 — Machinery, evaluator, freeze (IN REVIEW)

Engine (`engine/`, uncommitted on branch `feat/rfc0027-0028-storage`):

1. `src/abstraction/equity_strength.cpp` (new TU,
   `-ffp-contract=off`): exact conditional EHS enumeration
   (990 / 45,540 / 1,070,190 terms, pinned ascending order, one final
   division); independent-oracle ctests with exact-double equality;
   XorShift64* Monte-Carlo seam (determinism + convergence tests);
   EquityStrengthGrid (32/64/64 primary; the three other turn/river
   points descriptive; invalid grids refused); band table
   `169 + (L-2)*160 + offset`, L=2..10, max id 1,608; preflop falls
   through to Preflop169; process memo keyed on the full parameter
   token with stats/clear; canonical river class enumerator pinned at
   134,459 with a canonicality ctest.
2. `abstraction.hpp/.cpp`: `CardBucketKind::EquityStrengthV1 = 4`;
   the three-arg `card_bucket` throws on the new kind; four-arg and
   five-arg live-seat overloads; existing kinds ignore live seats and
   keep identical ids; `equity_strength_id` parameter digest
   (golden `d3d44763b062a2c1` pinned by ctest).
3. `nseat_trainer.hpp/.cpp`: `NSeatTrainerLimits::card_kind/
   equity_grid` measurement seams defaulting to the production kind;
   all three bucket sites (`walk_action`, `walk_average`,
   `NSeatPolicy::lookup`) derive live seats from the state;
   `NSeatPolicy` carries its sealing kind/grid; production training is
   bit-identical.
4. BSWARM03 codec (`src/artifacts/sealed_checkpoint.cpp`): kind byte 4
   decodes to EquityStrengthV1; exclusive bucket ceiling 1,609;
   round-trip ctest with over-ceiling load refusal.
5. Benchmark (`benchmarks/subgame_nash_conv.cpp`):
   `--rfc0044-ladder SEATS WALL S BASE SHARD SHARDS
   [--rfc0044-ckpt DIR] [--rfc0044-grid KF KT KR]` (160 cells,
   checkpoint/resume, per-segment RSS sampler, exact + bucket gap
   walks parameterized through a measurement-only scheme global) and
   `--rfc0044-cost ... RUNG` (S=2 full-range cost rows, no gap walk).
   All NashConv walker bucket sites are scheme-parameterized.
6. Tests: `equity_strength_ehs`, `equity_strength_bucket`,
   `canonical_river_classes` in test_abstraction; kind-4 codec
   round-trip in test_sealed_checkpoint; the equity-kind trainer seam
   in test_solve_nseat.
7. Evaluator: `tools/rfc/rfc0044-gates.ts` (+`bin/rfc0044-gates.mjs`)
   implements the frozen CERT rules 1-4, rule-5 floor classifier
   (tail-four window, near-floor margin 0.8), QUAL paired
   superiority, named-column CSV loading, and the `certification`
   CLI; 14 Node tests including the AC3 regression over the 30
   retained RFC 0043 ladder CSVs (all six river series NOT_CERTIFIED
   at 10M; tail fits and projections match the RFC values).

Smoke evidence: HU tiers/equity 100k RIVER cells reproduce the retained
RFC 0043 rows bit-for-bit (tiers 0.372838/0.094313, equity
0.221726/0.203208, seed 4320); 17 one-second-segment resume runs at 1M
are bitwise identical to the one-shot run (gaps 0.173831 / 0.329245 /
0.023365); cost rows verified.

Defect found and fixed during Stage 1 validation: the turn/flop EHS
branches compared the opponent's seven-card score against a hero score
precomputed from board+holding only (six/five cards), undervaluing the
hero (full-range turn mean 0.332 instead of 0.5). The hero is now
scored per completed runout in the exact enumerator and the MC seam;
the oracle was corrected and permanent full-range symmetry ctests were
added (mean E_h exactly 0.5 over all 1,176 flop / 1,128 turn / 1,081
river holdings) plus a turn MC convergence assertion. After the fix
the equity turn cell at seed 4320/100k reproduces the retained RFC 0043
row exactly (max 0.179690, sum 0.314655, 70 infosets); the golden
identity digest is unchanged.

Freeze addendum: RFC 0044 section 6 pins host/toolchain, constants,
cell topology and shard commands, CSV contract, BSWARM03 resume
protocol/preflight, the rule-5 fit protocol, the non-increasing
tradeoff, and evaluator/driver identity hashes. Independent approval
is required before the first 30M cell.

## Stage 3 — HU extended-ladder campaign (COMPLETE: NO GRID)

Ran 2026-11-01 on n37-100-184: 40 OMP=1 shards, 160/160 cells, zero
truncations/resumes/errors, 62.4 core-hours, max cell 2.65 h inside
the 6 h guard, no leftover checkpoints. Verdict artifacts
`docs/plans/abstraction/rfc0044-stage3-verdict.{json,txt}` (the JSON
is the frozen evidence of record). tiers CERT at 30M; equity CERT at
100M (bucket gap 0.004856, hw 0.000043) but QUAL FAILS: mean exact
gap 0.048416 BB at r_cert vs EPS_EXACT 0.03 (all five seeds strict
better than tiers yet every seed above 0.03; 300M exact 0.0459 vs
bucket 0.0027 -> card-abstraction floor, not non-convergence). The
predeclared AC5 no-grid disposition holds; constants unchanged; no
full-range cost wave was authorized or run. Independent evidence
review (separate agent, own Python re-aggregation) Approved with
zero blocking findings and byte-identical verdict reproduction.

## Stage 4 — Disposition (COMPLETE)

The M2 asset-building campaign does NOT consume EquityStrengthV1. The
next instrument must attack the ~0.046 BB exact-game card-abstraction
merge loss (a river card-space refinement and/or the continuation-
value lineage) under a NEW RFC with its own freeze; W1->full-range
exploitability stays deferred to the M2 milestone gate. Done 2026-11-01: deleted equity_strength.cpp, the EquityStrengthV1
enum/grid/EHS/memo/river-enumerator declarations and their
abstraction.cpp cases, the nseat trainer/policy seams, the BSWARM03
codec kind-4 branch, the benchmark --rfc0044 arms and scheme global
(walker bucket sites restored to the production kind), the CMake
TU/link/compile-option additions, and the engine ctests; retained
the TS evaluator (tools/rfc/rfc0044-gates.ts + launcher + 14 tests),
all campaign CSVs, the verdict artifacts, this plan, and the RFC.
Release build is green after deletion; the 8-seat preflop size gate
(separate D4 finding) is unrelated.

## Verification (Stage 1)

- `format`; full release build; `ctest --preset release` 79/79;
- ASan preset on touched paths (abstraction incl. the 134,459
  enumerator and MC runs, sealed_checkpoint, solve_nseat,
  warm_start) — all pass;
- `npm run check` (typecheck, build, source policy, proto, docs/RFC
  checkers, 14 rfc0044 + 14 rfc0042 Node tests).
