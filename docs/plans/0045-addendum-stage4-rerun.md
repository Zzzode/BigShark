# RFC 0045 Stage-4 Re-run Addendum (APPROVED, incl. section 2.3b root correction)

- RFC: [0045 M2 Instrument Resolution and Blueprint-Scope Census](../rfcs/0045-m2-instrument-scope-census.md)
- Status: **APPROVED 2026-11-02** for sections 1-6 by a separately
  launched, non-author independent approval agent; run 2 ran under that
  contract. During walk-certification close-out a further
  measurement-arm defect was found and corrected in **section 2.3b**
  (walk pin keyed by scheme but not betting root; the timing arm built
  every board on a limped root). Section 2.3b was **independently
  APPROVED 2026-11-04** by a separate non-author agent (verbatim record
  below); the eight walkslow raised EHS cells are then re-walked with a
  single serial worker under the corrected raised band. Four non-blocking
  findings from the first review are closed (sections 2.4, 4, 5 and the
  aggregator fail-closed hold); the second review's informational notes
  F1 (shared host during review) and F2 (below) are recorded.
- Author: Claude (zzzode agent session, 2026-11-02).
- Approval agent: independent general-purpose agent launched in a
  separate process (did not author the RFC, Stage-3 addendum, evaluator,
  driver, or campaign). Decision date 2026-11-02.
- Supersedes for run 2, minimally: Stage-3 freeze addendum section 6's
  single walk pin (now scheme-keyed, section 2.3) and the section-4
  segment-wall value (21,600 s -> 1,800 s, section 4 note). Every other
  Stage-3 pin is unchanged.
- Decision record of record (verbatim):

> 2026-11-02 RFC 0045 Stage-4 re-run addendum APPROVED by independent
> approval agent (separately launched, non-author): verified the guarded
> cumulative-assignment resume fix (subgame_nash_conv.cpp with the
> target-checkpoint retrain path; all other warm-start callers assign),
> the native bitwise-identity segmented-resume regression
> (test_warm_start.cpp; release + ASan pass), the mechanical per-scheme
> walk re-run (4 attempts, 30/60/90 s backoff, walkretry receipts,
> bounded post-sweep passes) with walkslow fail-closed in rfc0045-gates.ts
> and rfc0045-g2-aggregate.mjs (both exercised), the scheme-keyed EHS
> 50.996 s / tiers 29.781 s +/-20% bands as a no-loosening correction
> (enumeration deterministic; fast and slow both rejected), the unchanged
> six-board/grid/ladder/seed/t/threshold pins and 368-cell space with the
> conditional (32,128,256) arm absent from the driver, and the ordinal-5
> measurement-only boundary; independently reproduced W=16 hot-state
> bursts (EHS max 52.82 s, tiers max 30.73 s) and a kill -9
> stale-lock/resume smoke with strictly linear cumulative receipts
> across restart; four non-blocking findings (fill section-5 TBDs before
> launch; document the 1800 s segment wall vs Stage-3 section 4;
> PID-reuse stale-lock delay only; aggregator exits 0 on incomplete
> campaigns so done=368/zero-walkslow is a mandatory pre-verdict hold);
> no production decision authorized.

## 1. Why a re-run is required

The Stage-4 independent evidence review returned CHANGES REQUESTED on
the 368-cell run-1 campaign. Record of record (verbatim):

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

Run-1 evidence is moved to
`docs/plans/ablation/rfc0045-run1-invalid/` with an audit README and does
not support any verdict.

## 2. Defects fixed and the one freeze correction

### 2.1 Resume accounting (root cause of invalid 300M cells)

`train_nseat` documents `iterations` as ADDITIONAL samples beyond
`initial_state->completed_iterations`, but
`NSeatTrainingResult::completed_iterations` is the CUMULATIVE absolute
count (`start_sample + completed`). The G2 ladder driver accumulated the
result with `done += trained.completed_iterations`, re-adding the prefix
on every warm-started segment; the inflated count was then persisted, so
the next segment's absolute PRNG start index jumped over untrained sample
windows. Every production caller (flop library builder, nested-river
driver) assigns the result; the defect was local to the measurement
driver.

Fix (measurement driver only, no shared-module change):

- guarded assignment `done = trained.completed_iterations`, throwing if a
  segment does not strictly advance the count;
- regression `test_segmented_resume_lands_on_target` in
  `engine/tests/test_warm_start.cpp`: fixed chunk boundaries AND a
  wall-interrupted multi-segment resume must both land on exactly the
  target count with a raw table bitwise-identical to an uninterrupted run.
  The reference runs use an effectively unbounded wall and assert
  termination=Complete, so the property holds under sanitizer builds too.

### 2.2 Mechanical walk re-run (the Stage-3 rule was not enforced)

The Stage-3 freeze required re-running a full walk outside
50.996 s +/-20%, but the driver only recorded the timing and never re-ran
anything. Run 2 enforces it mechanically: up to four walk attempts with
30/60/90 s backoff, `walkretry` receipts in the worker log, and a final
`note=walkslow` row only if every attempt fails. The frozen evaluator
refuses to load a `walkslow` row and the aggregator exits non-zero, so a
gate can never be computed from a cell that exhausted its retries.

### 2.3 Per-scheme walk pins (corrects an under-specified Stage-3 pin)

The 50.996 s Stage-1 pin was measured with the EHS 32/64/64 policy. The
walk enumerates the same 1,070,190-deal universe for every scheme, but
its wall includes the policy lookup at every reached node, and the table
size is scheme-structural: river cells saturate at ~50 infosets for
tiers vs ~240-850 for EHS (fixed by the grid by the 1M smoke rung,
independent of k_river and of the iteration count up to 300M). One band
around an EHS pin would reject a structurally valid idle tiers walk.

Idle re-measurement on n37-100-184 (`OMP_NUM_THREADS=1`, release,
2026-11-02), 1M smoke rung:

| scheme | infosets (W1 board) | idle full walk (s) | +/-20% band (s) |
|---|---|---|---|
| tiers | 50 | 29.781 | 23.825 - 35.737 |
| ehs2 32/64/64 | 500 | 50.838 | 40.670 - 61.006 |
| ehs2 32/64/256 | 810 | 51.179 | 40.943 - 61.415 |

The Stage-1 number (50.996) reproduces within 0.3%. kr64 and kr256 idle
walks differ by 0.7% despite a 62% larger table, and under run-1
contention the three EHS grids measured 72.1/72.4/73.0 s — enumeration
dominates and the EHS grids share one pin. Run 2 therefore keys the band
by scheme family: **EHS grids 50.996 s +/-20% (40.797-61.195 s), tiers
29.781 s +/-20% (23.825-35.737 s)**. The +/-20% rule itself, its purpose
(reject machine-load distortion of the deterministic enumeration), and
the re-run/fail-closed behavior are unchanged. The measurement-only
`--rfc0045-river-wall ... tiers` selector exists only to obtain the tiers
pin; it reaches no production path.

### 2.3b Root correction — the pin must be keyed by betting root, not scheme only

Run 2 exposed that scheme-only keying is still insufficient. The twelve
raised-stratum full-300M cells (`2c3d7hJs2s`, the single-raised-pot root)
would not certify: their single-concurrency, otherwise-idle walks timed
61.9-62.6 s for EHS, just above the limped-derived 61.195 s ceiling, and
repeating them serially reproduced the same time, which ruled out
machine load and lockstep contention.

Root cause was in the measurement ARM, not the campaign. The
`--rfc0045-river-wall` arm built every board through
`make_rfc0036_production_root(2, /*river_rooted=*/true)`, which is a
LIMPED HU root (contribution 2, pot 4, stacks 198, button 1). The board
string alone does not encode the betting root, so timing
`--rfc0045-river-wall 2c3d7hJs2s` built a limped-pot river tree (400 EHS
infosets at 1M), whereas the campaign cell for that stratum is an SRP
root (`rfc0045_campaign_def`: contribution 4, pot 8, stacks 196,
button 0; 480 EHS infosets at 1M). The raised-stratum walk was
therefore being compared against a pin measured on a structurally
different, cheaper tree. The five limped strata and the Stage-1 pin are
unaffected and remain correct (the limped 1M walk re-measured with the
corrected arm is 50.996 s exactly, infosets 500).

Fix (measurement arm only): `--rfc0045-river-wall` now builds the
campaign-identical GameDef directly and accepts an order-insensitive
`limped|raised` root token (default limped; historical invocations are
unchanged). Tree identity with the campaign cell is proven per
scheme/grid by the infoset count matching the campaign rows exactly.

Idle re-measurement on n37-100-184 (`OMP_NUM_THREADS=1`, release,
2026-11-04, host load ~1, 1M rung), CORRECT roots:

| betting root | scheme/grid | infosets | idle walk (s) |
|---|---|---|---|
| limped | tiers | 50 | 29.781 |
| limped | ehs 32/64/64 | 500 | 50.996 |
| raised (SRP) | tiers | 60 | 35.491 |
| raised (SRP) | ehs 32/64/64 | 480 | 61.671 |
| raised (SRP) | ehs 32/64/128 | 672 | 61.853 |
| raised (SRP) | ehs 32/64/256 | 732 | 62.071 |

The SRP river enumeration is structurally ~21% slower than the limped
one and, like the limped family, grid-independent (61.67/61.85/62.07
across a 52% table-size span), so the three raised EHS grids share one
pin. The frozen band is now keyed by **(scheme family, betting root)**:

| key | pinned idle walk (s) | +/-20% band (s) |
|---|---|---|
| limped, EHS grids | 50.996 | 40.797 - 61.195 |
| limped, tiers | 29.781 | 23.825 - 35.737 |
| raised, EHS grids | 61.9 | 49.520 - 74.280 |
| raised, tiers | 35.5 | 28.400 - 42.600 |

This widens only the raised-stratum acceptance range to match its real
structural cost; it cannot admit a genuinely load-distorted walk (the
raised ceiling still rejects anything >20% over the true SRP idle
time). No limped band, threshold, seed, grid, ladder, or gate statistic
changes. The driver selects the band from the cell's own stratum flag.
Only the eight walkslow raised EHS cells are prefix-resumed and
re-certified under the corrected raised band with a single serial worker;
the three raised tiers final rows (walks 35.4-35.6 s) already satisfy the
corrected raised tiers band [28.4, 42.6] and are retained as-is. Every
gap value is enumeration-deterministic and was never in question; only
the EHS raised timing stamps are refreshed.

Review note F2: the old wall arm differed from the campaign limped cell
not only in pot for the raised case but also in the button (arm used
button 1, campaign uses button 0). HU mirror symmetry makes the two
heads-up river trees cost-equivalent (both 500 EHS infosets; 50.910 s vs
50.996 s, 0.17%), so the limped band stayed valid; the corrected arm
nonetheless aligns every field including button 0.

Section 2.3b decision record (verbatim):

> 2026-11-04 RFC 0045 Stage-4 re-run addendum section 2.3b root
> correction APPROVED by independent approval agent (separately
> launched, non-author): the corrected --rfc0045-river-wall arm builds
> the campaign-identical GameDef field-by-field (2 players, button 0,
> BB 2, contribution 2 limped / 4 SRP, pot 4 / 8, stacks 198 / 196,
> board_size 5, preflop=false, River) with an order-insensitive
> limped|raised token; independent 1M single-thread runs reproduced
> campaign infoset counts exactly (raised ehs 64/128/256 = 480/672/732,
> raised tiers 60; limped ehs 500, tiers 50), while forcing the old
> limped root on 2c3d7hJs2s yielded 400 infosets at 51.3 s,
> independently proving the wrong-tree calibration defect; idle raised
> walks 62.2-63.1 s EHS and 35.401 s tiers confirm the 61.9 / 35.5 pins,
> a load-contended tiers walk at 44.599 s empirically breached the
> corrected 42.600 ceiling proving the unchanged +/-20% rule still
> rejects machine-load distortion, and repeated walks produced
> bitwise-identical gap values (0.215344 / 0.214197) from an RNG-free
> deterministic enumerator; the four final rows satisfy the corrected
> bands and the eight walkslow rows (360 .done of 368 rows) have
> verified near-target 278-292M BSWARM03 prefix checkpoints for a
> genuine single-serial re-walk with no note hand-edit, while
> rfc0045-gates.ts (16/16 tests) and rfc0045-g2-aggregate.mjs were both
> re-exercised to fail closed on note=walkslow (throw / exit 1); no
> threshold, seed, t multiplier, grid, ladder, board set, evaluator
> statistic, or production path changed — pins exist only in the
> measurement benchmark and EquityStrengthV2 (kind 5) is created solely
> there and is reject-closed by the mmap mount guard; release
> test_warm_start and check-docs pass; remaining step: the eight-cell
> serial re-walk and the done=368/zero-walkslow aggregation remain for
> the Stage-6 evidence review.

### 2.4 Worker-count certification

Run 1 ran 44 workers; its final wave put 64/72 full walks above the band
(observed 62.5-94.3 s for EHS). The host (Intel Xeon Platinum 8336C, 56 physical cores) shows thermal throttling under sustained all-core
load: a single walk is ~51 s regardless of momentary external load, but
a concurrent burst measured immediately after a preceding burst is
materially slower than the first. Certification therefore uses the HOT
steady state (back-to-back bursts, no cooldown), which is the state a
multi-hour campaign actually ends in.

Concurrent full-walk bursts measured 2026-11-02 (release,
`OMP_NUM_THREADS=1`; every process trains a fresh 1M policy so the
lookup table is scheme-saturated, then times the full walk):

| burst | concurrency | state | max (s) | mean (s) | out of band |
|---|---|---|---|---|---|
| EHS W=36 | 36 | warm | 61.88 | 58.11 | 1/36 |
| EHS W=32 | 32 | warm | 65.76 | 61.41 | 17/32 |
| EHS W=24 A | 24 | warm | 55.08 | 53.51 | 0/24 |
| EHS W=24 B | 24 | hot | 66.71 | 61.64 | 12/24 |
| EHS W=16 A | 16 | warm | 62.27 | ~54 | 1/16 (+1.8%) |
| EHS W=16 B | 16 | hot | 60.20 | ~53 | 0/16 |
| EHS W=16 C | 16 | hot | 53.18 | 52.12 | 0/16 |
| tiers W=18 | 18 | warm | 31.58 | 30.79 | 0/18 |

W=24/32/36 are rejected: in the hot steady state their walks cluster at
or above the 61.195 s ceiling. W=16 passes the EHS band in the hot
steady state with ~8 s margin on two independent bursts; the single warm
burst outlier (62.27 s, +1.8%) is the transient the in-cell re-run is
designed for (30/60/90 s backoff, then the post-sweep prefix-resume
passes). tiers passes its band at concurrency 18 (above the launch
count), so 16 workers certify both families.

**Certified launch: exactly 16 worker processes.** This deliberately
trades wall-clock (~186 core-h / 16 ~= 11.6 h plus walks) for an
unattended run whose hot-state final wave stays inside the frozen band.

## 3. What stays frozen (unchanged from Stage-3)

- six strata / six gate boards incl. raised-srp `2c3d7hJs2s`, mandatory
  coverage validation in the evaluator;
- grids (32,64,64), (32,64,128), (32,64,256) plus tiers control; the
  conditional (32,128,256) arm is NOT run (river-inert, review-confirmed);
- ladder {1M,10M,100M,300M}; gate eligibility only at 300M;
- seeds 4320..4324 (W1, n=5) and 4320..4322 (full, n=3);
  t multipliers 2.7764 / 4.303; independent per-seed deal batches;
- gate thresholds: worst-stratum full-range exact <=0.03 BB, bucket
  <=0.005 BB, half-width <=0.001 BB, per-seed paired superiority vs tiers;
- mechanical selection: smallest passing k_river, else predeclared
  k_turn=128 (only relevant for TURN cells, which this campaign does not
  contain), else `instrument-inadequate-uniform-ehs`;
- the frozen evaluator `tools/rfc/rfc0045-gates.ts` (16 node tests after
  the walkslow fail-closed addition) and the canonical aggregation
  `node bin/rfc0045-g2-aggregate.mjs`.

## 4. Run-2 execution contract

- fresh gitignored scratch directory `rfc45-run2/`; zero reuse of run-1
  rows or checkpoints;
- one fixed release binary (formatted, rebuilt after all fixes), the same
  binary for every worker;
- all 368 cells of the frozen cell space (80 W1 + 288 full);
- `OMP_NUM_THREADS=1`, work-stealing LPT driver, **in-cell segment wall
  1,800 s** (see the note below), per-cell mkdir locks with owner-PID
  stale recovery, one worker log each;
- exactly **16** worker processes (certified in section 2.4);
- campaigns stop only on completion or a logged per-cell error (an error
  cell is triaged and re-run, never silently skipped);
- single provenance: the run-1 leftovers from the aborted static shard
  are not present in the fresh directory.

Segment-wall note (resolves the Stage-3 section-4 value). Stage-3 froze
a 21,600 s per-segment safety wall (the RFC 0044 convention); run 1 in
fact drove segments at 1,800 s. Run 2 freezes **1,800 s**. This changes
no scientific pin: BSWARM03 resume continues at the absolute PRNG sample
index (verified bitwise-identical by the segmented-resume regression in
release and ASan), so a shorter wall only seals checkpoints more often
and bounds crash loss to one ~30-minute segment instead of six hours. It
is strictly more conservative operationally.

Operator hold before any verdict (finding N4): the aggregator now fails
closed — it writes the monitoring CSV but exits non-zero and prints no
verdict unless done=368/368 with zero walkslow rows (an interim
diagnostic verdict requires an explicit `--partial`). The mandatory
pre-verdict checks are exactly 368 `.done` markers, zero
`walkslow_left` in every worker log, and the second independent evidence
review.

## 5. Certification results (completed 2026-11-02)

- worker count W: **16** (table in section 2.4);
- certified EHS hot-steady-state full-walk bursts at 16 concurrency:
  max 60.20 s and 53.18 s (both inside the 40.797-61.195 s band);
- certified tiers burst at 18 concurrency: max 31.58 s (inside the
  23.825-35.737 s band);
- release: full ctest 79/79 passed; ASan: `test_warm_start` passes with
  zero sanitizer reports; `npm run check` 57/57 node tests plus
  typecheck/proto/source-policy; `node bin/check-docs.mjs` and
  `node bin/check-rfcs.mjs` pass;
- the independent approval agent additionally reproduced W=16 bursts and
  a kill -9 stale-lock/prefix-resume smoke with strictly linear
  cumulative receipts across the process restart.

## 6. Post-run

Aggregate with the frozen evaluator; a second independent evidence review
(same mandate as the first: recompute counts, statistics, gate; audit
resume receipts for linear cumulative counts; audit every full walk
against its scheme band; confirm no conditional arm) must approve before
any terminal instrument verdict is recorded. Measurement engine code is
then deleted per the no-legacy rule (Stage 5), evaluator/evidence/census
retained.
