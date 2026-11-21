---
rfc: "0044"
subject: "Reachable Convergence Certification for Offline Equilibrium Artifacts (M2 Prerequisite)"
status: "Accepted"
authors: "Claude (zzzode agent session, 2026-10-31)"
created: "2026-10-31"
updated: "2026-10-31"
owners: "Engine solver, engine benchmarks, tools (evaluator), tests"
supersedes: ""
superseded-by: ""
---

# RFC 0044: Reachable Convergence Certification for Offline Equilibrium Artifacts (M2 Prerequisite)

- RFC type: Measurement-only (no production strategy, artifact, build,
  pack, checkpoint, codec, protocol, or serve change in this RFC).
- Owners: engine solver, engine benchmarks, tools (evaluator), tests.
- Mainline: this RFC unblocks Milestone M2 (heads-up 100 BB full GTO)
  in `docs/plans/gto-10seat-milestones.md`. It is not itself a
  milestone; it produces the certification instrument and the
  extended-ladder evidence the M2 asset campaign is gated on.
- Authors: Claude (zzzode agent session, 2026-10-31).
- Created: 2026-10-31.
- Status: Accepted (independent approval agent, 2026-10-31).

## Summary

RFC 0043 proved the frozen offline convergence test (a 20% relative
two-rung plateau) is unreachable under regret-matching's ~T^-0.55
power-law decay for any card grid, even though measured within-abstraction
gaps fell well under 0.05 BB. This RFC replaces it with a reachable,
data-blind certification based on absolute BB distance (0.005 BB), seed
noise control, and power-law tail consistency, extends the iteration
ladder to 300M with checkpoint resume, and adds an exact-game quality
gate (0.03 BB HU). Measurement-only; its output mechanically selects
the grid and rung for the later M2 asset-building campaign.

## Motivation

RFC 0043 measured offline iteration-ladder convergence without a serve
wall. All three bands reached within-abstraction river gaps BELOW the
fixed 0.05 BB bar at 10M iterations (0.0164 / 0.0272 / 0.0249 BB,
five independent-seed half-widths (t_0.975,4 = 2.7764) of 0.0001-0.0009 BB), with large positive
exact-gap benefits over the nine-tier control. No band could select an
r*, however, because the frozen certification was a 20% RELATIVE
two-rung plateau `|g(r-2)-g(r)|/g(r-2) <= 0.20`, and the regret-matching
decay is a near-exact power law (~T^-0.55...T^-0.68, per-rung
retention ~0.53) for BOTH the equity grid and the nine-tier
incumbent. The independent ruling was structural no-go: the
certification predicate was unreachable for any abstraction on this
solver within the ladder, not evidence against the equity grid. RFC
0043 was closed with its measurement code deleted (per the no-legacy
rule) and this explicit follow-up: a reachable, predeclared
certification calibrated first on the incumbent, plus an extended
ladder.

This RFC supplies that instrument. It deliberately does NOT select an
abstraction for M2, build any mounted artifact, or change the serve
path; it measures, on the W1 ten-hand fixture and full ranges, whether
a policy is practically at its abstract-game equilibrium and how much
compute that takes.

## Why the old predicate failed and what replaces it

The relative plateau asks the regret decay to have visibly flattened.
RM+ average-strategy exploitability does not flatten inside practical
budgets; it follows a smooth power-law tail. A relative-movement test
therefore certifies only asymptotic stagnation, which arrives far
later than "good enough" — if ever within a campaign. The quantity a
GTO product actually cares about is an ABSOLUTE distance: the measured
within-abstraction summed best-response gap `g(T)` in BB per hand.
For regret-matching average strategies this is the practical
exploitability bound: when it is e.g. 0.005 BB and falling
non-increasing on a power-law-consistent tail with sub-0.001 BB seed
noise, the remaining distance to the abstract-game equilibrium is
negligible versus the product quality bar (0.05 BB), regardless of
whether the curve is mathematically flat.

Evidence from the retained RFC 0043 CSVs
(`docs/plans/ablation/rfc0043-ladder-seats*.csv`) used to set the
numbers below:

- log-linear tail fits `g(T) = a*T^-b` on the last three frozen rungs
  (1M/3M/10M) give b = 0.54-0.68 and describe ALL five rungs with
  relative residual <= 0.24 (equity 2p residual 0.03). The tail regime
  is identifiable from three rungs.
- Extrapolated equity-grid gaps: 2p 0.0090 BB at 30M, 0.0047 at
  100M; 3p 0.0147 / 0.0076; 4p 0.0117 / 0.0052. A 0.005 BB residual
  is reachable around 100M iterations (2p/4p) and ~300M for 3p.
- Nine-tier incumbent extrapolations: 2p 0.0017 BB at 100M; its
  within-abstraction gap converges faster, but its exact gap (card
  abstraction loss) stays ~0.32 BB — within-abstraction convergence
  and true-game quality are separate gates, both retained here.

## Goals

1. A mechanical, data-blind CERTIFICATION predicate over an extended
   iteration ladder that (a) a genuinely converged policy satisfies at
   finite compute, (b) a still-substantially-improving policy fails,
   and (c) cannot be satisfied by seed noise.
2. Extended iteration rungs up to 300M with checkpoint-resume training
   (no serve wall; offline training wall is a resume trigger, never a
   convergence target).
3. Per-band two-grid measurements (nine-tier control and the exact-EHS
   grid re-implemented to the RFC 0042 frozen textual spec) producing
   BOTH within-abstraction gaps and exact-hole best-response gaps, so
   the M2 abstraction choice is mechanical.
4. Strictly measurement-only: a second/third re-implementation;
   non-adoption deletes all new code.

## Non-Goals

- Mounted turn/river/extended-flop artifacts, resident root
  generalization (`board_size` 4/5 acceptance), and the artifact format
  flip: separate RFC(s) after certification lands.
- Selecting the M2 abstraction: this RFC reports which grid passes
  which gate at which rung; the M2 campaign RFC makes the selection.
- Multi-seat certification: heads-up (M2) is the only band in scope;
  the instrument is designed to generalize to 3..10 but only the 2-seat
  campaign runs here.
- Non-100-BB depths.

## Current State and Evidence

Same as RFC 0043 Section 1 for the re-implemented machinery (exact-EHS
enumerator, MC seam, trainer card-kind seam, river canonical
enumerator, CSV driver) — all deleted in the RFC 0043 closure and
re-implemented here. The RFC 0042 frozen textual spec remains the
authority for the equity grid (K 32/64/64, L bands, digest case). The
RFC 0043 freeze addendum's CSV contract and 134,459 class universe
remain valid evidence inputs and are reused verbatim.

## Design Principles

1. Certify the product-relevant quantity (absolute BB exploitability),
   not an asymptotic-shape signature the solver never exhibits.
2. Data-blind: EPS_GAP, noise bar, tail-residual window, and ladder are
   frozen before any 30M+ cell runs and never retuned post hoc.
3. Two gates, not one: within-abstraction convergence (CERT) and
   exact-game quality (QUAL) both required, so a converged coarse grid
   cannot be mistaken for a mountable GTO strategy.
4. Reuse the validated RFC 0043 evidence contract (paired seeds,
   bucket/exact split, 134,459 universe, shard/wall methodology).
5. Measure-only: no artifact, mount, build, pack, or serve change;
   non-adoption deletes all new code.

## Proposed Design

### 1. Extended iteration ladder and run shapes

- River/turn ladder rungs (fixture, W1 ten-hand representative range):
  `{100k, 300k, 1M, 3M, 10M, 30M, 100M, 300M}` (cap 300M).
  Rungs 100k..10M reproduce the RFC 0043 ladder (re-trained, not
  reloaded — no committed code survived); 30M/100M/300M are the
  certification tail.
- Five independent seeds per grid (S=5; RFC 0043 base seeds
  4320..4324 for HU), identical across grids and rungs. Cross-grid
  comparisons at a rung pair observations by the same base seed;
  within-rung mean and half-width use independent-seed statistics.
- Shapes: HU only in this RFC; river root
  (`2c3d7hJs9c`, DepthLimit none) and descriptive turn root
  (`2c3d7hJs`, streets(1)). Full-range cost runs at the certified r*
  only (S=2) per RFC 0043's cost protocol, in case the M2 campaign
  RFC consumes them directly.
- Training wall: none as a convergence input. A generous per-process
  wall (21,600 s = 6 h) exists only as an infrastructure guard; a cell
  hitting it records truncated and resumes from its warm-start
  checkpoint — the BSWARM03 sealed checkpoint codec
  (`engine/include/bs/sealed_checkpoint.hpp`, magic BSWARM03 /
  version 3; older codecs are foreign) already supports exact resume:
  it persists raw regret/sum/visit rows with completed_iterations, and
  the PRNG stream derives from master seed + ABSOLUTE iteration index,
  so warm-starting to N is bitwise identical to training to N from
  zero (no averaged-strategy reset, no seed discontinuity). Campaign
  training uses the SERIAL `train_nseat` entry (the parallel entry has
  no warm-start seam) with process shards and OMP_NUM_THREADS=1 per
  the RFC 0043 freeze. The CSV wall_observed_s for a resumed cell is
  the SUM of all its segment wall times (checkpoint time excluded),
  pinned in the CSV contract so cost columns stay comparable.

### 2. The certification predicate (CERT)

Computed from the five-seed mean river bucket-summed gap series
`g[i]` (within-abstraction, BB; the same metric RFC 0043 used). At
each rung the five seeds are INDEPENDENT runs (base seeds fixed across
rungs and across grids, but a rung's five observations are not paired
among themselves): arithmetic mean over the five seeds, sample
standard deviation with the n-1 denominator, half-width
`hw[i] = t_0.975,4 * s_i / sqrt(5)` with `t = 2.7764`, evaluated in
full double precision and rounded only for display. Pairing is used
exactly once in this RFC: grid-vs-incumbent comparisons at the same
rung pair observations by identical base seed.

A rung index `i` (`i >= 3`, zero-based; the prior-tail rule needs
three observed rungs before it) CERTIFIES iff ALL hold:

1. **Absolute distance**: `g[i] <= EPS_GAP`, with `EPS_GAP = 0.005 BB`
   (one tenth of the 0.05 BB product quality bar).
2. **Noise control**: `hw[i] <= 0.001 BB` (five-independent-seed
   half-width).
3. **Tail regime consistency**: a log-linear fit
   `ln g = ln a - b*ln T` on the three rungs ENDING at i
   (`i-2, i-1, i`) has `b in [0, 1.5]`; the maximum relative residual
   measured against the OBSERVED gaps,
   `max_k |g_fit(k) - g[k]| / g[k]`, is `<= 0.20`; and the three
   observed gaps are non-increasing. Allowing `b` down to 0 is
   deliberate: a flat plateau BELOW EPS_GAP is a certified outcome.
   The RFC 0043 failure was demanding continued decay toward a
   RELATIVE plateau; here the absolute bar carries the quality claim
   and the fit only certifies "smooth tail, not a transient", so it
   must not reject a converged floor that is already good enough. The
   conjunction of residual and prior-tail checks rejects
   single-rung crashes; the 1.5 cap bounds extreme artifacts (it
   triggers only past ~81% one-rung drops).
4. **Prior-tail prediction consistency**: fit the same log-linear
   form on the three rungs IMMEDIATELY BEFORE i (`i-3, i-2, i-1`),
   using no observation at or after i, giving `(a0, b0)`; require
   `|g[i] - a0*T_i^-b0| / (a0*T_i^-b0) <= 0.20`. Review finding: a
   one-rung -45% dip still yields a smooth three-rung fit and passes
   a forward-projection rule, but is rejected here because the prior
   window did not predict it. Every window rung must be an observed,
   non-truncated rung (no imputation).
5. **Failure classification**: if no observed rung (including 300M)
   certifies, the grid does not mount. Fit an additive-floor
   diagnostic `g(T) = c + a*T^-b` with `c >= 0` over the frozen tail
   window defined in Section 6 (the last four observed rungs; the
   optimizer, margin band, and budget-reporting rules are pinned
   there — fitting the 100k/300k transients would risk labeling a
   compute ceiling as a floor) and classify: (a) fitted
   `c >= EPS_GAP` is a NON-ZERO-FLOOR structural finding for this
   grid at this campaign's reach; (b) `c < EPS_GAP` with the
   projected crossing rung beyond 300M is a COMPUTE-CEILING finding,
   reported with the fitted budget required to reach EPS_GAP (or with
   no finite budget inside the Section-6 near-floor band). Neither
   class is resolved by loosening constants after the run.

The smallest certifying rung is `r_cert`.

Predicate constants (EPS_GAP, the half-width bar, residual and
prediction tolerances, b bounds, ladder, seeds) are frozen in the
pre-run addendum; EPS_GAP and the half-width bar are NOT retuned
after seeing 30M+ data. The 0.20 fit tolerances come from RFC 0043
observed rung-to-rung scatter, which sat well inside 20% on smooth
tails; they are not re-estimated from this campaign's data.

### 3. The exact-game quality gate (QUAL)

Within-abstraction convergence is necessary but not sufficient: the
nine-tier incumbent converges to ~0.002 BB within its abstraction while
staying ~0.32 BB exploitable in the EXACT game (its card abstraction
loss). The M2 campaign may only mount a grid that also satisfies, at
`r_cert`:

- five-seed mean exact summed gap `e[i]` (exact-hole BR gap, BB,
  summed over the two live seats) `<= EPS_EXACT = 0.03 BB` HU. In
  per-seat terms this is 0.015 BB/hand/seat, i.e. 1.5 BB/100/seat of
  summed exploitability (the normalization RFC 0039 used).
- every seed's exact gap is strictly below the PAIRED incumbent
  control's exact gap at the same rung (observations paired by
  identical base seed; the new grid must strictly beat the incumbent
  it would replace).

Honest projection at freeze time: HU RFC 0043 measured e = 0.055 BB
at 10M, and re-fitting local log slopes on the retained ladder shows
the decay slowing (local exponent ~0.43 on the low rungs falling to
~0.16 at the top): projections are e(100M) in [0.033, 0.038] and
e(300M) in [0.026, 0.032] depending on the fit window. QUAL is
therefore a real gate that may not pass at any reachable rung. The
disposition is pre-declared: if a grid CERTifies but e(300M) >
EPS_EXACT, the verdict is converged-but-too-coarse, no grid mounts,
EPS_EXACT is not lowered, and AC5 closes with the fitted exact-gap
tail and the budget estimate for crossing 0.03; the follow-up M2
campaign RFC then chooses the next instrument (richer abstraction or
larger compute) explicitly, instead of inheriting a silently loosened
bar.

EPS_EXACT is frozen pre-run. A grid that CERTifies but fails QUAL is
usable evidence, not a mountable strategy.

### 4. Re-implemented machinery (measurement-only)

Per RFC 0043's accepted architecture, rebuilt with no legacy carryover:

- exact-EHS enumerator (990 / 45,540 / 1,070,190 terms), MC seam,
  mutex+map memo, EquityStrengthV1 kind + four-arg seam, digest case
  (recompute the digest from the frozen parameter string in the
  addendum);
- trainer `card_kind`/`equity_grid` in-memory seams (production
  defaults unchanged);
- benchmark CSV driver: extended rung set, checkpoint-resume support,
  per-cell RSS/wall/bytes, process shards;
- canonical river enumerator (134,459 pinned by ctest) as evidence
  universe;
- the canonical flop representatives stay as verified in RFC 0043
  (7sKhAd / 7sKhKd / QsKsAs / 7sKhAs; only relevant if full-range flop
  cost rows are produced).

### 5. Evaluator

A new TypeScript evaluator (`tools/rfc/rfc0044-gates.ts`, independent
of the RFC 0042 pilot/gate tool that remains) implements the frozen
CSV contract, independent-seed statistics with paired cross-grid
comparison, the tail fit, and CERT/QUAL
classification as exported pure functions plus a CLI
(`certification` mode). Project-native Node tests cover: a certifying
smooth-tail synthetic series; a one-rung dip below EPS_GAP that passes
its own three-rung tail fit but is rejected by rule 4 (prior-window
misprediction); a sub-bar plateau with fitted b = 0 that CERTIFIES
(guards against re-introducing a positive lower bound on b); a
single-rung crash rejected by rule 3; a noisy series rejected by rule
2; a converged-but-coarse grid (CERT pass, QUAL fail); rule 4 refusing
when a prior-window rung is truncated or missing; and the additive-floor
diagnostic labeling compute-ceiling vs non-zero-floor. Constants
freeze in the pre-run addendum.

### 6. Stage 2 pre-run freeze addendum (2026-10-31)

Recorded before any 30M+ cell is run, per the independent approval's two
mandated items. The addendum is itself independently reviewed; Stage 3
cells at 30M and above start only after that review.

**Host and toolchain.** Reference host `n37-100-184` (Intel Xeon
Platinum 8336C, 56 cores, 109 GB RAM); GCC 12.2.0, release preset
`-O3`; Node.js v22.23.2; single-threaded cells with
`OMP_NUM_THREADS=1`. Engine binary:
`build/release/engine/bigshark-subgame-nash-conv` built from the
working tree the addendum review approves (uncommitted on branch
`feat/rfc0027-0028-storage`; the eventual commit is the campaign's
prerequisite and must not alter these hashes).

**Frozen campaign inputs.**

- Ladder `{100k, 300k, 1M, 3M, 10M, 30M, 100M, 300M}`, cap 300M.
- Shapes: HU only; river root `2c3d7hJs9c` (TerminalDepth none) and
  descriptive turn root `2c3d7hJs` (DepthLimit streets(1),
  EquityFrontierEvaluator).
- Schemes: `tiers` (CategoryTiersV1, incumbent control) and `equity`
  (EquityStrengthV1 at the PRIMARY grid `K_flop=32, K_turn=64,
  K_river=64`; card-identity digest
  `d3d44763b062a2c1`). No descriptive grid variant is run in this
  campaign.
- Seeds: five independent base seeds 4320..4324 per scheme/rung;
  grid-vs-incumbent QUAL comparisons pair identical base seeds.
- Predicate constants: EPS_GAP 0.005 BB, half-width bar 0.001 BB with
  `t_0.975,4 = 2.7764`, n-1 sample SD, full double precision
  (rounding only for display), rule-3 b range [0, 1.5], rule-3/rule-4
  tolerance 0.20. EPS_EXACT 0.03 BB HU (1.5 BB/100/seat).
- Per-segment infrastructure wall 21,600 s; checkpoint resume is the
  only response to it.

**Cell topology and sharding.** Cell order is
street(river, turn) x scheme(tiers, equity) x rung(8) x seed(5) = 160
ladder cells; 40 independent process shards (4 cells each), launched
as:

```bash
export OMP_NUM_THREADS=1
BIN=build/release/engine/bigshark-subgame-nash-conv
CKPT=<campaign-checkpoint-dir>
for i in $(seq 0 39); do
  nohup "$BIN" --rfc0044-ladder 2 21600 5 4320 "$i" 40 \
      --rfc0044-ckpt "$CKPT" >"rfc0044-ladder-shard$i.csv" 2>"shard$i.log" &
done
```

Full-range COST cells run only after r* exists: S=2 (seeds 4320,
4321), both streets and both schemes, eight cells in eight shards,
`--rfc0044-cost 2 21600 2 4320 <shard> 8 <r_cert>
--rfc0044-ckpt <dir>`; gap columns stay -1, no NashConv walk.

**CSV contract (frozen).** Header:
`mode,scheme,seats,street,seed,target_iters,safety_wall_s,completed,
infosets,sealed_bytes,joint_deals,max_gap_bb,sum_gap_bb,
bucket_sum_gap_bb,wall_observed_s,peak_rss_kb,truncated,note`.
One row per segment end: truncated=1 receipt rows (gap columns -1,
metrics absent) and exactly one final truncated=0 row per cell.
`wall_observed_s` is the SUM of every training segment's wall for the
cell, excluding checkpoint serialization time; the sidecar file
`<checkpoint>.wall` carries the running sum across invocations and is
deleted on completion. Gaps print `%.6f`, wall `%.3f`. A cell that
cannot resume a malformed checkpoint is a campaign failure (the row
names the refused field); checkpoints are never hand-edited.

**Mode-isolated checkpoint names (review finding B1).** A checkpoint
filename is
`rfc0044-<mode>-s<seats>-<scheme>-<street>-K<KF><KT><KR>-seed<seed>-T<target>.
bswarm03` plus `.wall`, where `<mode>` is `ladder` (W1 fixture) or
`cost` (full 1,326-combo range). The mode token is mandatory because
the seven-item preflight cannot distinguish training ranges: ladder
and cost cells share root ledger, tree shape, root-table hash,
card/action digests, seeds, and (at r*) the rung, so without the token
a residual ladder checkpoint could silently warm-start a full-range
cost cell. Cost waves may share a root directory but never resolve a
`ladder`-named file, and vice versa; a leftover checkpoint after a
final row is a campaign hygiene failure regardless.

**Cross-wave numerical-drift note.** This campaign re-trains every
rung with the re-implemented RFC 0044 driver and walkers. Gap values
need not match the retained RFC 0043 CSVs at levels below the frozen
noise (the 1M resume smoke, 0.173831/0.329245/0.023365, is a different
build from the deleted RFC 0043 driver's 0.172391/0.325324/0.022639),
and no cross-wave continuity is assumed. AC3 runs the evaluator on
the retained files for the no-cheating check only; campaign evidence
is the fresh CSV set.

**Checkpoint protocol.** BSWARM03 sealed checkpoints only, written by
the serial `train_nseat` seam, using the mode-isolated names defined
above (the protocol applies identically to `ladder` and `cost`). A
resume preflight hard-refuses mismatch of algorithm revision,
tree-shape witness, root-table hash (single limped root, 200/2, every
seat live), card kind, card digest (`equity_strength_id(grid)` /
incumbent id), action-id digest, or completed-iteration counter.
Absolute-iteration PRNG seeding makes the resumed policy bitwise
identical to from-zero training at the target (verified: a 1M cell
run in 17 one-second segments matched the one-shot run exactly, gaps
0.173831 / 0.329245 / 0.023365).

**Item 1 (mandated): rule-5 additive-floor fit protocol.**

- Window: the LAST FOUR observed rungs (`windowRungs=4`, exported
  constant `RULE5_WINDOW_RUNGS` in the evaluator). The 100k/300k
  transients never enter the fit, so they cannot manufacture a
  positive floor. The window must hold at least two observed
  rungs; otherwise the verdict is campaign-incomplete (rerun missing
  cells), never a structural finding.
- Model `g(T) = c + a*T^-b`, c,a,b >= 0, on the five-independent-seed
  MEAN bucket gaps. Optimizer (evaluator `floorFit`): a 201-point
  deterministic grid over b in [0, 2] (step 0.01), at each b the
  optimal c,a solved exactly by OLS projection onto the interior /
  c=0 edge / a=0 edge, followed by 60 golden-section refinement
  iterations within the best grid cell.
- Classification (constant `NEAR_FLOOR_RATIO = 0.8`):
  c >= EPS_GAP -> non-zero-floor, crossing budget Infinity;
  c > 0.8*EPS_GAP but below the bar -> compute-ceiling, and NO finite
  crossing budget is quoted (the denominator EPS_GAP - c is too
  ill-conditioned; the budget reports Infinity with the fitted
  tail); c <= 0.8*EPS_GAP -> crossing
  `T* = (a/(EPS_GAP-c))^(1/b)`, class compute-ceiling if T* > 300M,
  else crossing-below-cap.
- Only non-truncated observed rungs enter any fit; no imputation.

**Item 2 (mandated): non-increasing clause tradeoff.** Rungs are
independent re-runs, so a fully flat real plateau can show tiny mean
inversions; the clause uses a 1e-12 relative tolerance and is
retained because observed RM+ rung drops (~10% per tripling at
b=0.1, and ~35-45% at measured b) dwarf the 0.0001-0.0009 BB
half-widths. A statistically flat-but-tiny-inversion rejection is
thereby effectively impossible; this rationale, not a post-hoc
loosening, is the basis.

**Evaluator identity.** `tools/rfc/rfc0044-gates.ts`
(sha256 `6fdcb184141d0f9a019a9925825229a6b9c1de471208a6579c79bc969af3498e`)
and launcher `bin/rfc0044-gates.mjs`
(sha256 `7b6398fca9405ef1f7650ac4e2dc61672b8ae023670a28b299be6f3c251749eb`)
at the approved tree; 14 Node tests. Campaign verdict command:
`node bin/rfc0044-gates.mjs certification <shards...csv> --json`
(default rule-5 window 4; `--rule5-window all` is a diagnostic only
and cannot produce a campaign verdict). The `--json` object is the
frozen verdict EVIDENCE: it carries every rung's exact gap
(`meanExact`) and the full per-seed paired QUAL comparisons, including
the nine-tier incumbent's CERT rung and any QUAL failure; the plain
text rendering is a console convenience and is not the evidence of
record. The evaluator reads only `ladder` rows; cost rows are ignored
by CERT.
AC3 on the retained RFC 0043 CSVs under this exact binary: all six
river series NOT_CERTIFIED at 10M, tail b tiers 0.570 / equity 0.541
at 2 seats, 100M projections 0.00172 / 0.00471; tail-four rule-5
classes: 2p equity crossing ~96M, 2p tiers ~17M, 3p equity
non-zero-floor (c~0.00540), 3p tiers crossing ~45M, 4p equity
non-zero-floor (c~0.00706), 4p tiers near-band compute-ceiling with
no finite budget (c~0.00493).

**Systematic (non-sampling) external-validity gap, recorded for the
M2 asset RFC.** CERT/QUAL gaps and the rule-5 classification are W1
fixture-system quantities, and full-range cost rows deliberately do
not measure gaps; neither a tight seed half-width nor a clean tail fit
bounds the W1-to-full-range SYSTEMATIC shift (review note N4; a
seed-bootstrap also showed the four-rung three-parameter floor fit has
weak discriminating power on the 3p/4p retained series, so the rule-5
class is HU evidence only and future-seat campaigns need a wider
window or more seeds). Full-range exploitability is independently
re-measured at the M2 milestone acceptance gate, not inferred here.

Nothing in this addendum is changed after the first 30M cell starts.

## Dependency Rules

- Measurement code only; engine production default kind stays
  nine-tier; serve/build/pack paths untouched.
- No network, credentials, or game-platform I/O; offline hosts only.
- Independent approval agent before Stage-1 implementation; pre-run
  freeze addendum before any 30M+ cell runs.

## Compatibility and Migration

None. Measurement-only RFC: no on-disk format, protocol, public
interface, or served strategy changes. The production default card kind
stays nine-tier; resident/build paths are not touched. A later RFC will
consume this RFC's certified grid+rung to design artifact changes.

## Security and Operational Impact

No game-network I/O, no credential reads, no game-platform endpoints.
Campaign runs are offline single-host compute on n37-100-184 using the
already-audited process-shard methodology (OMP_NUM_THREADS=1 pinning,
resident-RSS sampling, 6-hour per-process infrastructure guard with
warm-start resume). Extended rungs consume more CPU than RFC 0043
(estimated ~1 h/seed for HU at 100M); disk is checkpoint-only and does
not change any served artifact.

## Alternatives Considered

- **Keep the relative plateau with a looser fraction (e.g. 50%).**
  Rejected: still a shape signature on a T^-b curve; the threshold would
  be arbitrary and would pass only at much larger T without tying the
  decision to an absolute quality bar.
- **Certify on the exact-game gap alone.** Rejected: at high seats the
  exact gap enumeration is prohibitively expensive and mixes card
  abstraction loss with solver convergence; the two must be separable
  to diagnose failures. Both gates retained.
- **Fix a training budget and certify whatever gap results.**
  Rejected: turns the instrument into a budget announcement; the
  certified rung must be mechanically selected, as RFC 0043 required.
- **Fit the floor with an additive constant (asymptote c > 0).**
  Considered but not adopted for the primary predicate: RM+ averaged
  strategy gap tends to zero, and a free constant risks fitting the
  non-asymptotic transient as a positive floor. Absolute distance plus
  tail consistency (residual + non-increasing + prior-tail prediction)
  is sufficient and simpler; the additive fit `c + a*T^-b` is retained
  only as the rule-5 failure classifier (compute ceiling vs non-zero
  floor) and never promotes a rung to certified.

## Risks

1. EPS_GAP 0.005 BB may require more than 300M at HU for the equity
   grid; mitigated by the fitted-budget report (AC4) and resume; result
   is then a bounded compute finding, not an instrument failure.
2. Power-law regime could change above 10M (the 30M/100M/300M points
   may deviate); rules 3 and 4 detect exactly this — a regime break
   makes the prior window mispredict — and withhold certification
   rather than mis-certifying.
3. A one-rung measurement accident (sharded-cell loss, machine load)
   could fake a crossing; rule 4's prior-window prediction rejects a
   crossing the smooth tail did not predict, and only non-truncated
   observed rungs are admissible.
4. The exact gap may plateau ABOVE EPS_EXACT even while the bucket gap
   certifies: the retained HU ladder already shows its local exponent
   falling (~0.43 to ~0.16), with projections straddling 0.03 at 300M.
   This is not an instrument defect but a possible abstraction
   finding; the disposition is pre-declared in section 3 (no mount,
   no bar lowering, fitted-budget report, next instrument via a new
   RFC).
5. The re-implemented exact-EHS machinery repeats RFC 0043's
   same-binary-only 0-ULP limitation; accepted measurement constraint.
6. Five independent seeds with tight half-widths may miss rare
   deal-sampling variance; exact river enumeration at HU makes the
   metric itself seed-independent in shape (seeds only change the
   trained strategy being measured), and the M2 campaign adds
   full-range cost rows.
7. CERT/QUAL numbers are measured on the W1 ten-hand representative
   fixture, which cannot prove full-range system behavior; full-range
   cost rows in this RFC do not measure gaps. Full-range
   exploitability is independently re-measured at the M2 milestone
   acceptance gate, so a fixture-specific certification cannot mount
   an artifact on its own.

## Open Questions

- The exact EPS_EXACT value for 3..10 seats (scaled by live-player
  count) is deferred to each seat's certification campaign; only HU
  0.03 BB is frozen here.
- Whether turn certification can reuse the river instrument directly
  or needs its own bar (turn has chance nodes; bucket BR is undefined)
  is settled in the M2 artifact RFC, not here.

## Acceptance Criteria

- AC1 (spec + digest): ctests pin term counts, fan-out, memo and seam
  behavior, the 134,459 river count, and the golden digest; the
  default production digest is unchanged.
- AC2 (evidence): five independent seeds at every completed HU rung,
  aligned across grids by base seed (pairing applies only to
  grid-vs-incumbent comparisons), river gaps evaluated, turn rows
  descriptive; raw CSVs committed; truncation only via the resume
  path.
- AC3 (calibration check on existing evidence): the evaluator, run on
  the retained RFC 0043 CSVs, must classify all six series (two grids
  x three bands where available, HU mandatory) as NOT CERTIFIED at 10M
  with the fitted budgets reported, and its fit parameters must
  reproduce the hand-computed values in this RFC (b in [0.5,0.7] HU,
  100M projections within 10% of the quoted numbers). This is a
  no-cheating check before new compute.
- AC4 (extended ladder): for the HU equity grid, report the first rung
  satisfying CERT and the exact gap there; for the HU nine-tier grid,
  report its CERT rung and QUAL failure. If no grid certifies by 300M,
  report the rule-5 classification (non-zero floor vs compute ceiling)
  with the fitted crossing budget; the finding stands and constants are
  not retuned.
- AC5 (abstraction recommendation): a single mechanically selected
  grid for M2 (CERT+QUAL pass, smallest rung) or an explicit finding:
  either "no grid CERTIFIES by 300M" with the rule-5 class and budget
  estimate, or "CERT but QUAL fails at 300M" with the exact-gap tail;
  in both cases no grid mounts and EPS_GAP/EPS_EXACT are unchanged.
- AC6 (rollback): on any non-adoption outcome all new code is deleted;
  evidence and this RFC remain.

### 7. Stage 3 campaign outcome (2026-11-01)

The frozen 40-shard campaign ran to completion on n37-100-184:
160/160 cells, every row `completed == target`, zero truncations,
zero resume events, zero error notes, no leftover checkpoints.
Evidence: `docs/plans/abstraction/rfc0044-ladder-seats2-shard0..39.
csv` and the evaluator's frozen verdict
`docs/plans/abstraction/rfc0044-stage3-verdict.{json,txt}`. Total
training 62.4 core-hours; slowest cell equity turn/river 300M at
~9.3-9.4 ks (2.6 h), inside the 21,600 s guard; peak RSS 9 MB.
Low-rung rows are bitwise continuous with the retained RFC 0043
seed-4320 coordinates (the apparent drift noted in section 6 was a
seed-42 vs seed-4320 smoke comparison, not a wave difference).

Mechanical verdict (evaluator, independently evidence-reviewed):

- **EquityStrengthV1: CERTIFIED at r_cert = 100,000,000.** River
  five-seed bucket-summed gap 0.004856 BB (half-width 0.000043),
  tail b 0.525, residuals and the prior-window prediction inside
  tolerance; at 300M the bucket gap is 0.002717. The tiers incumbent
  CERTIFIES earlier, at r_cert = 30,000,000 (bucket gap 0.003565).
- **QUAL FAILS at r_cert (predeclared no-grid outcome).** Mean exact
  summed gap is 0.048416 BB at 100M against EPS_EXACT 0.03 BB; all
  five seeds are strictly below the same-seed tiers incumbent
  (0.3144/0.3136/0.3141/0.3136/0.3131; mean improvement 0.2657 BB,
  ~85% of the incumbent's exact gap), so the failure is the absolute
  exact bar alone — a structural seed-mean property, not an
  interval-width/power outcome (no Inconclusive is available).
- The exact gap flattens: 0.0555 (10M) -> 0.0519 (30M) -> 0.0484
  (100M) -> 0.0459 (300M); a further tenfold budget from 30M to 300M
  buys only 0.006 BB, while the within-abstraction gap at 300M is
  0.0027. The residual ~0.046 BB is therefore the 64-bin EHS card
  abstraction's own merge loss, not solver non-convergence; the
  crossing is far beyond the campaign ceiling.

Disposition per AC5: **no grid mounts; EPS_GAP and EPS_EXACT are
unchanged; no finer/alternative grid is selected after r_cert under
this RFC.** EquityStrengthV1 at (32,64,64) is confirmed the strongest
measured HU card abstraction (it removes ~85% of the incumbent's
exact gap and converges inside the absolute bucket bar by 100M) but
remains ~0.046 BB too coarse in the exact game for a 0.03 BB mount,
so the M2 asset campaign does not consume it. The next M2 instrument
— a river-card-space refinement attacking the 0.046 BB merge loss
and/or the continuation-value lineage — requires a new RFC and its
own frozen campaign; W1-to-full-range systematic validity remains
deferred to the M2 milestone acceptance gate. Stage 4 deletes the
measurement machinery (AC6); the evaluator, CSVs, and this RFC are
retained for decision reproducibility.

## Rollback Plan

Stage 4 (AC6): on the independently-approved no-grid outcome the
re-implemented enum value, seams, driver arms, and the equity-strength
translation unit and its ctests are deleted; the TS evaluator, all
campaign/evidence CSVs, the verdict artifacts, and this RFC are
retained for decision reproducibility.

## Verification Plan

Release + asan ctest, `npm run check`, proto check, both doc/RFC
checkers; campaign cells on n37-100-184 with the frozen shard/thread
pinning (OMP_NUM_THREADS=1 for process fan-out per RFC 0043's
finding); independent review of the freeze addendum and of the final
CERT/QUAL evidence before any M2 campaign RFC consumes it.

## Rollout Plan

1. Independent approval of this RFC.
2. Re-implementation + ctests/evaluator; AC3 retrospective check.
3. Pre-run freeze addendum (CSV header, constants, resume protocol,
   host snapshot).
4. HU extended-ladder campaign (100k..300M, two grids, five seeds;
   resume as needed).
5. Mechanical CERT/QUAL evaluation, independent evidence review,
   recorded recommendation for the M2 asset campaign.
6. On non-adoption: delete the code.

## Decision

Author agent: Claude (zzzode agent session, 2026-10-31)
Approved by: independent approval agent a5aab85824bfb3745
Decision date: 2026-10-31
Review outcome: Approved
Reviewed scope: two independent review rounds over the complete RFC.
Round 1 returned Changes Requested with blocking findings F1-F4
(rule 4 passed a one-rung -45% dip; single-sided QUAL optimism;
per-seat BB/100 normalization; wrong BSWARM01 codec/wall semantics)
plus statistical nits. Round 2 re-read the revised RFC and
independently re-aggregated all 30 retained RFC 0043 ladder CSVs
(300 rows) with the reviewer's own scripts, recomputing every cited
number: the prior-window rule rejects the -45% dip (deviation
0.447/0.458); the six retained series are all NOT CERTIFIED at 10M
under the new predicate (AC3 holds); HU tail fits, exact-gap local
exponents (0.429/0.356/0.272/0.163) and projections (e(100M)
0.033-0.038, e(300M) 0.026-0.032) match; b=0 sub-bar plateaus
certify; extreme speed-ups and slow-downs are both withheld rather
than mis-certified. The reviewer also verified BSWARM03/version 3
against sealed_checkpoint.hpp, the absolute-iteration resume
semantics, the nash_conv_units normalization, measurement-only
scope, and both structural checkers.
Review summary: F1-F4 confirmed resolved. The CERT conjunction
(absolute 0.005 BB, independent-seed half-width 0.001 BB, [0,1.5]
tail fit against observed gaps, prior-tail prediction within 20%,
smallest certified rung) and the QUAL gate (0.03 BB HU exact,
per-seed strict paired superiority) are mechanically evaluable from
the frozen CSV header and reachable in principle (first certifications
at 30M for tiers and 100M for equity on the clean-tail simulation).
Two items are mandated for the independently-reviewed pre-run freeze
addendum, before any 30M+ cell: (1) the rule-5 additive-floor fit
protocol — tail-only fit window, optimizer/initial-grid/tolerances,
tie-breaking when fitted c is near EPS_GAP, no finite-budget
extrapolation when c dominates; (2) documentation of the
non-increasing-clause tradeoff for independent re-runs. Three
non-blocking text revisions (AC2 independent-seed wording, the 1.5
cap crash wording, W1 fixture representativeness risk) are
incorporated in the accepted revision. Stage 1 (machinery
re-implementation, evaluator, AC3 retrospective) is unblocked; the
addendum gates Stage 3.
