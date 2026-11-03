# RFC 0045 Implementation Plan — M2 Instrument Resolution and Blueprint-Scope Census

Status: Current (RFC Accepted 2026-11-02 after three independent review
rounds; Stage 1 complete 2026-11-02, Stage 2 in progress)
RFC: [0045](../rfcs/0045-m2-instrument-scope-census.md)

Scope: MEASUREMENT/SCOPING only. Production build/pack/serve, BSFLIB,
resident board 0/3 gate, and the practice table stay untouched. Fresh
measurement card ordinal 5 (values 4 and 5 both permanently retired).

## Stage 1 — Measurement machinery (gated by smoke, no full-range cells)

1. Re-introduce the EHS translation unit with CONFIGURABLE
   K_river/K_turn: conditional exact enumerator (990 / 45,540 /
   1,070,190), per-runout seven-card hero scoring (RFC 0044 fix
   carried verbatim), MC seam, process memo, band table with the
   3,913-id ceiling (169 + 9*(32+128+256)), 0-ULP pinned-order oracle,
   and the three full-range mean-0.5 symmetry ctests.
2. CardBucketKind::EquityStrengthV2 (ordinal 5); three-arg refusal;
   live-seat overloads; fail-closed cases at card_abstraction_id,
   mmap bucket_in_range, and the checkpoint codec; CMake scopes the TU
   to the measurement benchmark only; stage6 edge guard unchanged.
3. N-seat trainer measurement seams (card kind/grid in limits, the
   three bucket sites, policy-carried identity) as in RFC 0044.
4. Benchmark arms:
   - a full-range RIVER exact walk timing cell (W1 board + the four
     canonical boards) used ONLY to measure the ~1,070,190-deal wall
     for the addendum — its gap number does NOT enter G2 selection;
   - the G3 structural root census mode (see Stage 2).
5. New frozen evaluator tools/rfc/rfc0045-gates.ts with the
   worst-texture selector, the 2.7764/4.303 multipliers, independent
   deal-batch handling, and the census/envelope ingestion.

Stage 1 gate: ctests green (release+asan), npm check, W1 regression
matches RFC 0044, and the smoke wall measured.

Stage 1 outcome (2026-11-02, release build, `OMP_NUM_THREADS=1`):

- Configurable EHS TU (`equity_strength.cpp`, kind ordinal 5), n-seat
  trainer measurement seams (limits card_kind/equity_grid; policy
  carries the identity; all three bucket sites key through it), codec
  ordinal-5 case with exclusive ceiling 3,913, and mmap fail-closed
  landed. ctests: pinned 0-ULP oracle, full-range mean-0.5 symmetry
  (1,081 river / 1,128 turn / 1,176 flop holdings), grid bands,
  codec round-trip/rejection, trainer seam.
- W1 regression is bit-identical to the retained RFC 0044 evidence:
  exact `sum_gap_bb=0.221726`, bucket `0.203208`, 60 infosets, 38 joint
  deals at 100k/seed 4320 (`rfc0045-stage1-river-smoke.csv`).
- The one authorized full-range river exact walk (1,326-combo range,
  1,070,190 disjoint hand pairs, 1M smoke rung) measured
  **exact_walk_s = 50.996 s, single thread**; training 33.3 s;
  informational-only gaps exact 0.272248 / bucket 0.268479. This wall
  is the figure the Stage-3 addendum freezes for full-range walks
  (gate rung and 300M only).
- Frozen evaluator `tools/rfc/rfc0045-gates.ts` (+ `bin` launcher,
  node:test suite) implements the 2.7764/4.303 multipliers,
  worst-stratum selector, mechanical rung selector, census ingestion,
  and the G4 envelope.

## Stage 2 — G3 structural root census (authorized now)

Preliminary Stage-2 observations (2026-11-02; the freeze addendum
finalizes strata and the raised-root selector before G2):

- Identity-complete cap-0 HU 100 BB roots: **200 distinct tuples**,
  exactly two per pot stratum (one per explicit button), pots 4..400
  in 4-chip steps; 100 per button. Line distribution: 2 limped, 2 SRP,
  196 three-or-more-raise (the exact raise count is in the `raises`
  column; the diagnostic tag stays in the three-value RootLineTag
  vocabulary). Only **2 all-in-no-flop tuples** (pot 400), split out
  for the exact-equity-all-in-flop frontier. Because each pot carries
  just two tuples, the RFC's "at least three tuples per stratum" must
  be re-read as pot-INTERVAL strata in the addendum (one interval
  spans several consecutive pots).
- Worst-shape River-terminal tree (limped, pot 4, flop 2c3d7h):
  - cap 1 (serving comparator): **4,769,246 nodes / 3.03 GiB tree**,
    1,334,074 infosets at 2,000 iterations (1.33M sealed rows,
    2.28 GiB), 8.8 s; the cap-1 object itself is buildable but
    multi-GiB.
  - cap 0 (frozen HU training): materialization **exceeds 100 M
    nodes / 55 GiB** (node cap, not byte cap). The uncapped
    limped 100 BB River-terminal tree is structurally infeasible on
    this machine; the cap-0/cap-1 fork the production RFC must
    adjudicate is now an observation, not an assumption. A 200-root x
    1,755 cap-0 full-River blueprint is therefore not buildable as
    specified; depth-limited / nested-continuation objects are the
    production RFC's alternatives.
- SRP pot-8 tuple (root_id 1, button 0, flop 2c3d7h): cap 1 is
  6,292,172 nodes / 3.08 GiB / 1,742,930 rows / 2.98 GiB / 13.0 s at
  2,000 iterations (larger than the limped object); cap 0 likewise
  exceeds the 50M-node bound. Both buttons' limped trees have identical
  node counts (4,769,246) and within-1% row counts.
- Evidence: `rfc0045-g3-roots-preliminary.csv` (root enumeration),
  `rfc0045-g3-structural-preliminary.csv` (cap builds, in progress).

Build the HU 100 BB cap-0 preflop abstract tree; walk every terminal
sequence into the flop; emit identity-complete distinct root tuples
(player count, BB, pot, button, per-seat stacks/contributions, folded
mask, live count, RootLineTag) with per-tuple sequence counts; split
out all-in-no-flop lines (exact-equity-all-in-flop frontier, no
blueprint rows). For >=3 tuples per frozen pot-stratum build
River-terminal trees under BOTH cap 0 (primary) and cap 1
(comparator): nodes/infosets/bytes, and run-to-target per-class rows/
seconds for one flop class. Output feeds the addendum raised-root
domain and G4.

## Stage 3 — Pre-run freeze addendum (independent review gate)

## Stage 3 — Pre-run freeze addendum (APPROVED 2026-11-02)

`docs/plans/0045-addendum-stage3-freeze.md` was approved by a separate
independent approval agent after a CHANGES-REQUESTED round. The one
blocking finding (a default `g2` verdict could omit the raised-srp
board) is closed mechanically: the evaluator registers all six gate
boards in `GATE_BOARDS`, validates coverage before reading any CSV, and
rejects an explicit incomplete `--boards` set unless `--allow-partial`
is passed. `EquityStrengthGrid::is_supported()` was tightened to exactly
the four frozen grid points. Evaluator tests: 15/15. Full-range G2
cells above the 1M smoke rung are now authorized under the frozen
contract.

**Next (Stage 4):** run the G2 instrument campaign (W1 five seeds on
all rungs; full-range six strata at the gate rung 300M and tiers
paired controls), then an independent evidence review; no grid is
mounted from this RFC — G4 hands the verdict to a separately authorized
production blueprint/mount RFC.

Pins: texture strata incl. the deterministic raised-root selector
(smallest-pot raised tuple in census-key order + lexicographically
smallest non-blocking river completing the W1 flop+turn), grids
(K_river 64/128/256, conditional K_turn=128 predeclared), rungs
{1M,10M,100M,300M} (gate eligible only at 300M under rule 4), t
multipliers 2.7764/4.303, independent per-seed deal batches, the
measured river walk wall/time, which rungs pay exact walks (gate/300M
= same point under the four-rung ladder), resume protocol. No
full-range cell above 1M before approval.

## Stage 4 — G2 instrument campaign + G4 envelope (RUN 1 INVALIDATED 2026-11-02; fixed re-run in progress)

All **368/368 cells** completed on n37-100-184 with 44 work-stealing
workers (`OMP_NUM_THREADS=1`, zero worker exceptions, zero residual
checkpoints), ~202 core-hours. Evidence of record (RUN 1, SINCE
INVALIDATED — retained for audit, replaced by run 2) under
`docs/plans/ablation/rfc0045-run1-invalid/`: `rfc0045-g2-campaign.csv`
(368 rows), `rfc0045-g2-verdict.json`, `rfc0045-g2-full-summary.csv`,
`rfc0045-g2-w1-summary.csv`, worker logs, and an audit README.

### Independent evidence review — CHANGES REQUESTED (2026-11-02)

> 2026-11-02 Stage-4 G2 independent evidence review (separate non-author
> agent): CHANGES REQUESTED — 368-cell counts, six-board coverage, n=3
> t=4.303 / n=5 t=2.7764 statistics, per-seed superiority, evaluator tests
> 15/15, and evaluator verdict needs-conditional-kt128 independently
> reproduced on the recorded rows, but every 300M cell is invalid: resume
> double-counts cumulative iterations (subgame_nash_conv.cpp:3288 vs
> nseat_trainer.cpp:1114), so all 72 full + 20 W1 300M policies really saw
> ~128–256M updates over disjoint RNG-index windows (50 cells ~135M) and
> 64/72 exact walks breached the 50.996s±20% re-run rule; re-run all 300M
> cells (full campaign recommended) after the fix before recording
> instrument-inadequate-uniform-ehs; the conditional (32,128,256) arm is
> river-inert (river bins use k_river only, equity_strength.cpp:333-340;
> campaign has no turn cells) and is not to be run.

Independent corroboration performed by the author before acting: the
recorded `completed` column shows 528-595M on nominally-300M cells; the
trainer header documents `iterations` as ADDITIONAL samples while
`NSeatTrainingResult::completed_iterations` is the CUMULATIVE absolute
count; every production caller (flop_library, nested-river driver)
assigns the result, only the G2 driver accumulated it.

Root cause and fixes (measurement driver only; no shared/module change):

1. `rfc0045_run_campaign_cell`: `done += trained.completed_iterations`
   replaced by a guarded assignment (`done = trained.completed_iterations`,
   throwing on a non-advancing result). On warm start the result is total;
   `+=` re-added the prefix and the inflated checkpoint jumped the next
   segment's absolute PRNG start over untrained windows.
2. Regression `test_segmented_resume_lands_on_target` in
   `engine/tests/test_warm_start.cpp`: explicit chunk boundaries AND a
   5 ms-wall multi-segment loop must both land on exactly the target with a
   table bitwise-identical to the uninterrupted run (a `+=` accumulator
   overshoots and fails the table comparison).
3. The frozen addendum section-6 walk re-run rule is now mechanical: the
   driver re-runs a full-range walk outside 40.797-61.195 s up to four
   times with 30/60/90 s backoff (enumeration is deterministic; only the
   timing refreshes), records `walkretry` receipts, and emits
   `note=walkslow` if every attempt fails; the evaluator refuses to load a
   `walkslow` row and the aggregator exits non-zero (16/16 node tests).
4. A single fixed-binary, single-provenance re-run of all 368 cells
   executes in a fresh scratch dir; worker count is calibrated beforehand
   so final-wave concurrent walks fall inside the frozen band.

Run-1 walk data exposed a second freeze defect that the Stage-3 addendum
must amend BEFORE run 2: the single pinned 50.996 s wall is
scheme-specific, not universal. Idle re-measurement reproduces the EHS
pin (50.838 s, 1M, identical gaps), but under the 44-worker final wave
the run-1 full walks split by scheme — tiers mean 43.3 s (min 36.0,
max 64.4; 50 infosets), EHS mean 72.5 s (min 62.5, max 94.3; 500
infosets). One +/-20% band around the EHS-idle pin would reject a valid
idle tiers walk structurally below 40.797 s and endless-retry it. The
amendment therefore pins per-scheme idle walls (tiers measured
separately with the measurement-only `tiers` selector added to
`--rfc0045-river-wall`; EHS grids share the 50.996 s pin) and certifies
the chosen worker count with a concurrent-walk burst before the campaign;
the mechanical re-run rule and walkslow fail-closed behavior above are
unchanged except the band is scheme-keyed.

Idle pins measured 2026-11-02 (`OMP_NUM_THREADS=1`, release,
n37-100-184, 1M smoke rung; the Stage-1 EHS number reproduces to 0.3%):

| scheme | river infosets | idle full walk (s) | band +/-20% (s) |
|---|---|---|---|
| tiers | 50 | 29.781 | 23.825-35.737 |
| ehs2 32/64/64 | 500 | 50.838 | 40.670-61.006 |
| ehs2 32/64/256 | 810 | 51.179 | 40.943-61.415 |

Infosets are grid-determined and saturated by the 1M rung (identical at
300M in run 1 for every board/grid), and kr64 vs kr256 walks differ by
0.7% idle and ~1.3% under run-1 contention despite the 62% table-size
difference — the 1,070,190-deal enumeration dominates, so the three EHS
grids share the Stage-1 50.996 s pin and tiers gets the 29.781 s pin.
The Stage-4 re-run addendum (`0045-addendum-stage4-rerun.md`) freezes
this, the mechanical re-run, the fixed driver, and a worker-count
certification burst; it was **APPROVED 2026-11-02** by a separately
launched non-author agent (verbatim record in the addendum), which also
independently reproduced the W=16 hot-state burst and a kill -9
stale-lock/prefix-resume smoke. All four non-blocking findings are closed
pre-launch: section-5 certification filled; the 1,800 s segment wall vs
the Stage-3 21,600 s value documented (no science change — absolute PRNG
resume is bitwise-identical, shorter segments only bound crash loss);
PID-reuse stale-lock noted as delay-only; the aggregator now fails closed
on done!=368 / any walkslow (CSV still written for monitoring; `--partial`
is a diagnostic-only interim verdict). A walkslow revisit also carries
the prior attempt's training time into the final `train_s` column. Run 2
launches exactly 16 workers into fresh `rfc45-run2/`.

Regression verification: `test_segmented_resume_lands_on_target` passes
in both release and ASan with zero sanitizer reports (its uninterrupted
references use an effectively unbounded wall and assert Complete — the
first test draft compared a completed segmented run against a
wall-interrupted reference under ASan and "failed" for that test-only
reason; release masked it; no engine defect was involved).

The run-1 numerical table below is retained as a historical artifact only;
it does not support a verdict. The conditional arm is not run.

Worst-stratum full-range results at the 300M gate rung (RUN 1, INVALID):

| grid | worst exact (BB) | QUAL <=0.03 | worst bucket (BB) | CERT <=0.005 |
|---|---|---|---|---|
| tiers | ~0.394 | fail | ~0.0043 | pass |
| ehs2 32/64/64 | ~0.0348 | fail | ~0.0193 | fail |
| ehs2 32/64/128 | ~0.0274 | (passed on invalid rows) | ~0.0235 | fail |
| ehs2 32/64/256 | ~0.0260 | (passed on invalid rows) | ~0.0244 | fail |

Run-1 observation (NOT a verdict): EHS grids showed better exact gaps than
tiers per seed but bucket gaps 0.0107-0.0244 over the full range, with no
improvement from k_river 64->256. Whether a true 300M point passes the
0.005 CERT bar is decided only from run 2; a valid 300M point is strictly
lower-trained than the recorded interrupted policies, so the
"structural floor" reading is uncertified.

The conditional (32,128,256) arm cannot change the chance-free river
gate: a 5-card River-terminal tree keys its bands on k_river only
(`equity_strength.cpp` five-card branch), so (32,128,256) and
(32,64,256) are identical on the river; k_turn enters only
turn/flop-straddled trees, and the frozen campaign contains no turn
cells.

Worst-texture full-range chance-free river exact gate; five W1 seeds,
three full-range seeds; smallest K_river clearing worst-stratum exact
<= 0.03 + bucket <= 0.005 + hw <= 0.001 + paired superiority; else the
predeclared K_turn=128 arm; else
`instrument-inadequate-uniform-ehs`. G4 produces the bounded
roots x 1,755 envelope (core-hours, wall, bytes, cap0/cap1) and one
verdict. Independent evidence review.

Campaign launched 2026-11-02 (n37-100-184, release, `OMP_NUM_THREADS=1`):

```
bigshark-subgame-nash-conv --rfc0045-ladder <worker_id> rfc45-ckpt 1800
```

- **Work-stealing driver (revised after the first launch)**: an initial
  static `cell_index % shards` sharding concentrated every 300M cell on
  shards 23-47 (critical path ~12 h, 23 workers idle after the short
  cells). The driver now orders all 368 cells by descending cost and any
  number of identical workers atomically claim a cell by
  `mkdir rows/cell-N.lock`, skip `cell-N.done`, and release the lock on
  completion or a segment boundary. This is LPT-balanced: ~202 core-hours
  total, critical path ~4.6 h over 44 workers.
- Two driver bugs were caught and fixed during launch: (a) on this
  toolchain `std::filesystem::create_directory(p, ec)` returns **false**
  for an existing directory while leaving `ec` clear — the claim must
  gate on the boolean, not `ec` (testing ec double-claimed); (b) the
  BSWARM03 checkpoint tag omitted the scheme name, so tiers and the
  (32,64,64) EHS grid collided on one file — the tag now includes
  `tiers`/`ehs2`. A single bad cell is logged and skipped rather than
  aborting the worker.
- 368 cells: W1 anchor 4 schemes x 5 seeds (4320..4324) x 4 rungs = 80;
  full range 6 strata x 4 schemes x 3 seeds (4320..4322) x 4 rungs =
  288. The reduced ladder {1M,10M,100M,300M} runs for every grid; full
  exact river walks are paid only at 300M per the frozen budget; W1
  exact walks run every rung.
- BSWARM03 resume with an 1,800 s segment wall (checkpoint + re-invoke
  is internal to each claimed cell, bitwise identical via
  absolute-iteration PRNG seeding). Per-cell `rows/cell-N.{row,done,lock}`
  in `rfc45-ckpt/` (gitignored scratch); worker logs
  `rfc45-ckpt/worker-*.log`.
- Aggregate/verdict of record:
  `node bin/rfc0045-g2-aggregate.mjs rfc45-ckpt [--json]`, which
  concatenates the 368 per-cell rows to
  `docs/plans/ablation/rfc0045-g2-campaign.csv` and runs the frozen
  six-board gate. The evaluator's CLI is guarded so importing it from
  the aggregator does not parse the aggregator's argv; header-only /
  empty cell files and `truncated=1` segment receipts are skipped.
- The 68 short cells completed by the aborted static-shard launch were
  migrated to per-cell `.row`/`.done` markers and are reused (not rerun).

### Run 2 launch (2026-11-02, APPROVED addendum)

After the fixes and the independently approved Stage-4 re-run addendum,
run 2 launches into a fresh gitignored `rfc45-run2/` with **exactly 16
workers** and a 1,800 s in-cell segment wall:

```
bigshark-subgame-nash-conv --rfc0045-ladder w00..w15 rfc45-run2 1800
```

Single fixed release binary, single provenance, zero reuse of run-1
rows/checkpoints. Aggregate of record:
`node bin/rfc0045-g2-aggregate.mjs rfc45-run2 [--json]` (defaults to
rfc45-run2; fails closed unless done=368/368 with zero walkslow;
`--partial` is diagnostic-only). Run 2 output overwrites the canonical
`docs/plans/ablation/rfc0045-g2-campaign.csv` name with valid rows; the
run-1 file stays archived under `rfc0045-run1-invalid/`. A second
independent evidence review is mandatory before any terminal verdict.

### Run 2 result (COMPLETE + VALID 2026-11-04; terminal verdict instrument-inadequate-uniform-ehs)

Run 2 ran 2026-11-02 19:45 -> 2026-11-03 17:38 (~22 h, 16 workers) plus
a single-serial raised-cell re-walk. Final state: **368/368 cells
`note=final`, 368 `.done`, zero locks/checkpoints residue, zero worker
errors, every `completed` exactly equals its rung target, all 72 full
300M walks inside their frozen (scheme x betting-root) bands**.
Evidence of record: `rfc0045-g2-campaign.csv` (368 rows),
`rfc0045-g2-verdict.json`, `rfc0045-g2-full-summary.csv`,
`rfc0045-g2-w1-summary.csv`.

One further measurement defect was found during close-out and corrected
under an independently approved addendum section 2.3b (2026-11-04): the
walk-timing arm built every board on a limped root, so the raised SRP
stratum's structurally slower river walk was timed against the wrong
tree; the band is now keyed by (scheme, betting root). Of the twelve
raised full-300M cells: **eight** were genuinely re-walked serially
under the corrected binary from near-target prefix checkpoints (walks
62.5-65.5 s), **one** raised EHS row (cell-342, 61.084 s) had already
completed inside both the old and corrected bands during an earlier
retry wave and was retained, and **three** raised tiers rows (35.4-35.6
s) were retained as already inside the corrected raised tiers band.

The **second independent evidence review returned VALID 2026-11-04** and
confirmed the terminal verdict of record:

> 2026-11-04 RFC 0045 Stage-4 run-2 SECOND independent evidence review
> (separately launched non-author agent, distinct from the run-1
> reviewer and the section-2.3b approver): VALID — 368/368 cells
> (80 W1 + 288 full), 368 .done, single provenance, zero run-1 residue,
> committed CSV byte-identical to the deterministic per-cell
> concatenation; every completed equals its rung target exactly with
> strictly additive segment receipts across all 111 multi-segment cells
> and bitwise-identical gaps across attempts (guarded cumulative
> assignment; test_segmented_resume_lands_on_target release pass; run-1
> inflated 301.6-599.9M contrasts with run-2 exact 300M); all 72 full
> 300M walks inside the frozen (scheme x betting-root) bands, including
> the eight serial re-walked raised EHS cells (62.478-65.496 s), three
> retained raised tiers (35.400-35.627 s), and one additional
> pre-correction raised EHS row (cell-342, 61.084 s) verified inside
> the corrected band; root correction independently reproduced via the
> read-only arm (raised EHS 480 infosets/62.1 s, raised tiers 60/36.6 s,
> limped EHS 500/54.7 s, bitwise-identical 0.215344/0.214197 gaps);
> statistics independently recomputed with n=3 t=4.303 / n=5 t=2.7764
> and match the committed summaries and verdict JSON with zero diffs
> (tiers worst exact 0.393222 / bucket 0.003640; kr64 exact 0.031312
> QUAL fail / bucket 0.012399 CERT fail; kr128 0.022413 pass /
> 0.015311 fail; kr256 0.018945 pass / 0.015941 fail; max half-widths
> 0.000322/0.000569 <= 0.001; 54/54 per-seed EHS superiority; worst
> bucket non-decreasing 0.01240->0.01531->0.01594 with 95% lower
> bounds 2.4x above the 0.005 bar; W1 kr>=128 bucket 0.002768
> consistent with clean RFC 0044 0.002717); fail-closed chain verified
> (aggregator exit 1 with 367 .done, evaluator exit 2 on note=walkslow,
> node tests 16/16, npm check 57/57, test_warm_start plus kind-5 ctests
> release pass, proto:check exit 0); conditional (32,128,256) arm is
> river-inert (equity_strength.cpp bins 5-card trees on k_river only;
> EHS scalar is grid-independent; no turn cells exist) and was not run;
> no frozen or pre-authorized arm passes both QUAL and CERT; terminal
> verdict of record: instrument-inadequate-uniform-ehs.

Worst-stratum full-range results at the VALID 300M gate rung (n=3,
t=4.303):

| grid | worst exact (BB) | QUAL <=0.03 | worst bucket (BB) | CERT <=0.005 |
|---|---|---|---|---|
| tiers | 0.393222 | fail | 0.003640 | pass |
| ehs2 32/64/64 | 0.031312 | fail | 0.012399 | fail |
| ehs2 32/64/128 | 0.022413 | pass | 0.015311 | fail |
| ehs2 32/64/256 | 0.018945 | pass | 0.015941 | fail |

All half-widths <=0.001 BB (max exact 0.000322, max bucket 0.000569);
EHS is strictly better than tiers per seed on every paired cell
(54/54). W1 valid 300M returns bucket 0.002768 for k_river 128/256,
matching the clean RFC 0044 0.002717 (run-1's corrupted 0.0042 is
explained by the resume defect). Uniform EHS passes the exact-hole QUAL
bar at k_river >=128 but cannot reach the 0.005 BB within-abstraction
CERT bar on the full 1,326-combo range at any frozen grid — worst bucket
does not fall as k_river 64->256 (0.01240 -> 0.01531 -> 0.01594) and the
95% lower bounds stay >=2.4x over the bar. The conditional
(32,128,256) arm is river-inert and this campaign has no turn cells.
**Terminal verdict of record: `instrument-inadequate-uniform-ehs`.**
M2 cannot use a uniform EHS banding; the production path needs a richer
postflop abstraction (e.g. depth-limited/nested continuation or
non-uniform bucketing), handed with the G3/G4 census to a separately
authorized blueprint/mount RFC.

## Stage 5 — Cleanup and handoff

After run 2 and the second independent evidence review, delete all
MEASUREMENT ENGINE code per no-legacy (the frozen TypeScript evaluator,
the TS node tests, all evidence CSVs/census, the RFC, and both addenda
are RETAINED so the evidence stays independently re-verifiable):

- delete `engine/src/abstraction/equity_strength.cpp` and its
  `CMakeLists.txt` source entry + `-ffp-contract=off` property;
- remove `CardBucketKind::EquityStrengthV2` (ordinal 5),
  `EquityStrengthGrid`, `equity_strength_id`, the live-seat EHS overloads
  from `engine/include/bs/abstraction.hpp` and the three-arg
  `card_bucket`/`card_abstraction_id` branches in
  `engine/src/abstraction/abstraction.cpp`;
- remove the measurement seams in `nseat_trainer.{hpp,cpp}`
  (`card_kind`/`equity_grid` limits fields, policy-carried identity, the
  5-arg measurement `from_parts`, the sweep-context bucket routing) and
  restore the single production identity at the three bucket sites;
- sealed checkpoint codec: drop the kind-5 round-trip case but KEEP
  ordinal 5 in the tamper/unknown-byte rejection set (it joins 4/6 as a
  permanently retired gap; it must never decode after deletion);
- restore `mmap_library.cpp` `bucket_in_range` to its pre-measurement
  form (served libraries never validated kind 5);
- delete the three `--rfc0045-*` benchmark arms and their helpers from
  `subgame_nash_conv.cpp` (incl. the scheme selector added for the tiers
  pin) and revert benchmark-only CMake links if nothing else uses them;
- delete the kind-5/EHS tests added under Stage 1
  (`test_abstraction.cpp`, `test_sealed_checkpoint.cpp`,
  `test_solve_nseat.cpp`) while KEEPING
  `test_segmented_resume_lands_on_target` in `test_warm_start.cpp`
  (that regression pins a general trainer-contract property, not the
  measurement grid);
- run the full release+asan and npm/proto/docs/RFC verification after
  deletion.

Then hand the verdict + G3/G4 census to the separately authorized
production blueprint/mount RFC (cap0 infeasibility >100M nodes,
board_size 4/5 flip, artifact format, depth-limited/nested object).

### Stage 5 COMPLETE (2026-11-04)

Executed after the VALID run-2 review. All measurement ENGINE code is
deleted; the frozen TypeScript evaluator (16 node tests), bin launchers,
every evidence CSV/JSON (run 1 archived under
`rfc0045-run1-invalid/`), the G3 census, this plan, both addenda, and
RFC 0045 are retained. Net engine change vs the measurement baseline is
only two explanatory comments plus the retired-ordinal codec rejection:
`abstraction.cpp`, `nseat_trainer.{hpp,cpp}`, `mmap_library.cpp`,
`subgame_nash_conv.cpp`, `test_abstraction.cpp`, and
`test_solve_nseat.cpp` are byte-identical to their pre-measurement HEAD;
`equity_strength.cpp` is deleted; the only modified files are
`abstraction.hpp` (enum comment), `sealed_checkpoint.cpp` (raw bytes
4/5/6/255 now all hit the generic unknown-kind refusal),
`test_sealed_checkpoint.cpp` (kind-5 round-trip removed; the
`rejects_retired_or_unknown_kind` test now asserts bytes 4/5/6/255 are
rejected), and `CMakeLists.txt` (source entry removed). The codec round
ceiling special-case (3913) is gone; there is no `EquityStrengthV2` /
`equity_strength` / `EquityStrengthGrid` symbol anywhere in engine,
apps, or clients; ordinal 5 remains permanently non-decodable.
`test_segmented_resume_lands_on_target` is retained (general
warm-start-contract regression). Production build/pack/serve/practice
paths are untouched; no shared contract changed; nothing committed.

Verification (independent author rerun after the executor):
`cmake --build --preset release` clean; `ctest --preset release`
79/79 (exit 0, ~1661 s); the four affected tests pass under ASan with
zero sanitizer reports; `npm run check` 57/57; `npm run proto:check`
exit 0; `check-docs.mjs` (421 files) and `check-rfcs.mjs` (45 RFCs)
pass; code-symbol grep clean. RFC 0045 is Implemented. The terminal
verdict and G3/G4 census are the input to a separately authorized
production blueprint/mount RFC — no production decision is made here.

## Verification

Release + asan ctest, `npm run check`, proto check, both doc/RFC
checkers; independent review of the addendum and the G2/G3/G4
evidence. All green as of Stage 5 completion (2026-11-04): release
ctest 79/79, ASan affected tests clean, npm 57/57, proto/docs/RFC
checkers pass; G2 evidence independently reviewed VALID twice
(addendum approvals + run-2 evidence review, all records above).
