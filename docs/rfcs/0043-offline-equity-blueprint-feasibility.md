---
rfc: "0043"
subject: "Offline Equity-Strength Blueprint Feasibility (W4 Measurement)"
status: "Implemented"
authors: "Claude (zzzode agent session 2026-10-31)"
created: "2026-10-31"
updated: "2026-10-31"
supersedes: ""
superseded-by: ""
owners: "Engine abstraction, engine solver, engine benchmarks (measurement only); flop_library/resident/artifact/codec/serve ownership activated only by a follow-on flip RFC"
---

# RFC 0043: Offline Equity-Strength Blueprint Feasibility (W4 Measurement)

## Summary

RFC 0042 proved that exact-equity card bucketing removes most of the
exact-game loss (2p-river 0.335 -> 0.129 BB, 3p-river 0.716 -> 0.361)
but returned KEEP for the **online 10-second resolver**, because the
finer partition does not converge inside its own abstraction within
the wall. This RFC asks the follow-up question RFC 0042 deferred: does
the same frozen abstraction converge when trained OFFLINE with no
per-request wall, and what would a full blueprint campaign cost in
iterations, wall time, and disk? It is again deliberately
MEASUREMENT-ONLY: it **re-implements** the equity function from the
frozen textual specification (RFC 0042 Section 1/1a and
`docs/plans/0042-implementation.md` — that code was never committed
and does not exist in any git object, so this is a second
implementation, not a restore), adds an in-memory training seam and a
benchmark convergence driver, changes no artifact format, builder
stamp, packer, checkpoint, codec, mount, serve path, production
default, or mounted artifact, and leaves the engine byte-identical at
nine tiers for every shipped path. A go decision authorizes a
SEPARATE artifact-format/flip RFC; a no-go ends the line and the new
measurement code is deleted.

## Motivation

- RFC 0042's failures were convergence-within-abstraction (river
  bucket sums 0.099/0.361 > 0.05 at 113k-378k iterations) and row
  store size under a 10 s cap. Neither predicate is defined for an
  offline build, where iteration count is a campaign choice and disk
  is the binding resource.
- The post-0041 plan routes 4+ live seats and early-street play to
  OFFLINE blueprints; before designing new artifact formats we need
  evidence that the candidate abstraction actually converges under
  offline budgets and that a campaign fits the build host's finite
  resources (56 cores; ~483 GB free disk at RFC revision time,
  recorded as capacity evidence while the 200 GB budget is fixed).
- The flop-terminal library already shipped (six-way full-range,
  1,755 classes, 1,000 iterations/class, 188 MB at nine tiers), so the
  cost baseline and build machinery exist; the unknown is the
  finer-abstraction delta per live-seat band and street root.

## Goals

1. Decide, on predeclared mechanically evaluated gates, whether
   EquityStrengthV1 converges within its own abstraction under
   iteration-based offline training on the RIVER roots at 2/3 seats
   (the RFC 0042 gated shapes) and 4 seats (the first W4 band edge).
   Turn roots and flop-terminal classes are cost/benefit observations
   only and are explicitly not decision roots (Section 2a).
2. Record the iteration count, wall, in-memory row store, and sealed
   policy bytes at each convergence rung, and project from the named
   formulas (Section 3) the river-blueprint campaign cost against the
   host's measured CPU, RSS, and disk budgets.
3. Produce a per-band mechanical decision (river-Go for each passing
   band in {2p,3p} plus 4p iff its gate passes / no-go /
   measurement-incomplete); on any river-Go, a follow-on RFC owns the
   eight build/stamp sites (carried verbatim from RFC 0042 Section 2),
   the river-root artifact kind, the serve-path equity table/cache,
   full-range confirmation, and one coordinated flag day scoped to the
   Go bands. Turn/flop artifacts and 5/6-seat bands need their own later
   measurements.

## Non-Goals

- No artifact, builder stamp, BSFLIB header, checkpoint, packer,
  codec, mount, resident, or serve-path change.
- No production training default change; no mounted artifact is built
  or mounted.
- No preflop abstraction change (preflop keeps Preflop169).
- No design of turn/river library compression (16,432 turn classes;
  river concrete boards) — this RFC measures convergence on existing
  bounded-root shapes and a small flop-class sample only; the
  artifact topology is a follow-on design question gated on Go.
- No learning/neural component.
- No claim that an offline-converged policy is directly servable;
  real-time continuation, residual tree shape, and latency are
  follow-on concerns.

## Current State and Evidence

- RFC 0042 Stage 3 (S=32, 10 s): equity exact/bucket sums and
  iteration counts are in
  `docs/plans/ablation/rfc0042-stage3-{tiers,tiersdraw,equity}.csv`.
- The equity enumeration, band table, memo, oracle ctests, trainer
  seam, and benchmark arm existed only in the **uncommitted working
  tree** during RFC 0042 and were deleted on its KEEP; an audit of
  every reachable and unreachable git object finds none of them
  (verified 2026-10-31). They are therefore re-implemented here from
  the frozen TEXTUAL specification — RFC 0042 Section 1 (formula and
  pinned order), Section 1a's frozen DECISIONS (K=32/64/64,
  conditional exact, ascending order, equal-width floor bins,
  L=2..10 bands), and `docs/plans/0042-implementation.md`. The
  surviving text carries only fragments of the serialization
  (`enumeration=conditional-exact`); the full name/version/parameters
  byte string (`equity-strength` / v1 /
  `bucket=ehs-vs-uniform-1opp;k_flop=32;k_turn=64;k_river=64;`
  `enumeration=conditional-exact;order=opp-then-run-ascending;`
  `bin=equal-width-floor;ids=169+band(L)+street;L=2..10;freeze=2026-10-30`)
  is a RECONSTRUCTED serialization of those frozen decisions, newly
  authored byte-wise in this RFC. Because the FNV-1a digest is
  deterministic over that triple, the re-implemented
  `card_abstraction_id` digest is recomputed from the reconstructed
  string and pinned in the pre-run Stage-2 freeze addendum; no "same
  digest as before" claim is made. The independent oracle is newly
  written for this RFC; its exact-double equality asserts internal
  consistency inside one test binary (the RFC 0042 fixture
  expectations were deleted with that tree and cannot be compared).
- Board class universes (independently enumerated with the engine's
  minimum-key 24-suit-permutation rule, cross-checked against the
  pinned flop/turn ctests): flop 1,755 classes / 22,100 concrete
  boards; turn 16,432 / 270,725; **river 134,459 classes / 2,598,960
  concrete boards**. The river count is new to this RFC and is pinned
  in Stage 2 by a new `enumerate_canonical_river_classes()` ctest
  (sibling of the flop/turn enumerators); it is the natural class
  upper bound for any river-root campaign (Section 3).
- Build baseline: `artifacts/flop-library-6way-fullrange` — 1,755
  classes, full 1,326-combo range, 1,000 iterations/class, 188,432,384
  packed bytes (183,856,428 summed section bytes; overhead ratio
  1.0249), nine-tier bucket id 2.
- The flop builder trains flop-terminal trees; every action node is
  on the flop and the turn/river continuation is the
  `EquityFrontierEvaluator`. The RFC 0042 quality wins were measured
  on turn/river-rooted bounded trees, so this RFC measures the street
  roots separately rather than infer from flop libraries.

## Proposed Design

### 1. Re-implemented declared function (spec-frozen)

`CardBucketKind::EquityStrengthV1`, the exact conditional enumeration
(990 / 1,035x44 / 1,081x990 terms; ascending pinned opponent-then-run
order; single nested accumulation; win+0.5 chop), K_flop=32/K_turn=64/
K_river=64, the L=2..10 band table (max id 1,608), the three-arg
refusal/four-arg entry split, and the per-parameter-set memo are
re-implemented to RFC 0042's frozen textual specification. Nothing in
the function is tuned for this RFC; only the TRAINING BUDGET dimension
changes. The `card_abstraction_id` digest is recomputed from the
quoted parameter triple and recorded in the pre-run freeze addendum;
the ctests are NEW: an independently hand-written oracle asserting
exact-double identity against the production enumerator INSIDE one
test binary (same-binary 0-ULP; both the production TU and the test
TU compile with the same preset flags, and no cross-binary identity is
claimed), term-count and fan-out assertions, malformed-input and
L-window coverage — no comparison against the deleted RFC 0042
fixtures is claimed.

### 2. Measurement driver: iteration-based convergence curves

A benchmark arm (same driver binary family as RFC 0042, new command)
trains each cell at a frozen ladder of ITERATION caps with no serve
envelope. A predeclared per-cell SAFETY wall caps only runaway cells;
its value, the host name, and a `df` snapshot are pinned in the
pre-run freeze addendum (planned constant 3,600 s, reference host
n37-100-184). A cell that reaches the safety wall records the last
completed rung and the rung is marked TRUNCATED; the wall never serves
as a convergence target.

Per (scheme, street root, seats, iteration cap) row fields (frozen
CSV header in the addendum): exact-hole summed BR gap and per-seat
max; river bucket-constrained summed gap (-1 where chance nodes
exist); completed iterations; information sets; in-memory and sealed
bytes; wall seconds. The deciding statistic for every aggregate is the
**mean over the five seeds**, paired against the nine-tier control
trained at the same cap with the same seeds.

Frozen iteration ladders:

- river/turn rooted cells: `{100k, 300k, 1M, 3M, 10M}`;
- flop-terminal cells: `{300k, 1M, 3M}`.

Shapes, ranges, and schemes:

- **Convergence ladders** (river 2c3d7hJs9c and turn 2c3d7hJs roots at
  2, 3, 4 seats) train and evaluate NashConv on the same W1 enumerable
  representative ranges the M1 harness uses (the ten-hand
  representative range for 2..4 seats; exact NashConv enumeration is
  feasible there), nine-tier control vs equity primary grid, S=5
  paired seeds per cell (convergence is a mean trend; not the RFC 0042
  confirm-seed protocol).
- **Full-range cost runs**: once the convergence ladder gives r* per
  band, an additional full-range (all 1,326 combos, board-filtered)
  training is run at that band's r* rung, S=2 paired seeds, producing
  the campaign projection inputs `B_river(L)` (sealed bytes/class),
  seconds/iteration, and measured peak RSS_L. These are the ONLY
  inputs to AC6/AC7; the fixture-ladder rows never feed cost
  projection.
- **Flop-terminal cost cells**: four iso classes — rainbow
  (class of the concrete As Kd 7c), paired (class of Ks Kd 7c),
  monotone (class of As Ks Qs), and two-tone unpaired (class of As
  Kh 7s — unpaired two-tone boards cover 46.6% of concrete flops,
  10,296/22,100, the largest single unpaired texture; the inclusive
  two-tone total including paired boards is 55.1%). The classes are
  pinned by their CANONICAL representatives in the Stage-2 freeze
  addendum (the minimum-key board under the engine's 24-suit
  permutation rule) — at seats 2 and 6, full 1,326-combo range, at the
  flop ladder's largest rung (3M), nine-tier and equity, S=2,
  cost/scale descriptors only.
- The full-range cost runs record bytes, completed iterations, wall
  seconds, and measured peak RSS ONLY: no NashConv exact evaluation is
  run on full ranges (a 4p full-range exact BR enumeration is
  prohibitively expensive), so their gap columns are recorded as -1.
  Convergence/benefit predicates come solely from the fixture-ladder
  rows; cost projections solely from these cost runs.
- If a full-range cost run is truncated by the safety wall before r*
  completes, `B_river(L)` is undefined and AC6/AC7 for that band
  evaluate to MEASUREMENT-INCOMPLETE (truth table), never estimated
  from a lower rung.
- Turn descriptive cells use the same fixture range/ladders as the
  river convergence cells for comparability; they feed no predicate.

### 2a. What this experiment can and cannot decide

The deciding gates cover RIVER convergence and campaign cost only:

- **RIVER roots (2p/3p/4p):** within-abstraction convergence is
  measurable (chance-free, bucket BR defined) and is gated.
- **TURN roots:** the turn tree has chance nodes, so the
  bucket-constrained BR that defines "converged within abstraction"
  is not defined there (same limitation as RFC 0042). Turn rows are
  measured and reported as exact-gap paired deltas and cost only; a
  turn root CANNOT be a Go root on this evidence.
- **FLOP-terminal roots:** continuation values come from the exact
  frontier, so flop-library quality does not test card-bucket
  convergence at all. The four flop classes measure per-class
  cost/scale (and bracket 2..6 seats) only; they CANNOT be Go roots
  on this evidence.
- **5/6-seat bands:** only the flop 6-seat endpoint is cost-sampled;
  5p/6p river convergence is NOT measured, so no band above 4 seats
  can be Go. The shipped six-way nine-tier line stays in production
  regardless of outcome.

Consequently the decision is expressed as a per-band union with three
labels: **river-Go** for each independently passing band among {2p,
3p} (a single-band Go is legal; both passing is the 2p+3p Go), plus
4p iff its separate gate passes, authorizing a follow-on flip RFC
scoped to exactly the Go bands; **no-go** for a band; or
**measurement-incomplete** for a band, raised only under the narrow
predeclared half-width/under-run rules below (never for a structural
failure).

### 3. Pre-run freeze and campaign cost projection

Before the first cell runs, a Stage-2 freeze addendum pins, all with
Node tests as project-native regression:

1. the CSV headers (river/turn and flop files), field units, and the
   mean-over-5-seeds aggregation with paired seed alignment;
2. the evaluator extension (`tools/rfc/rfc0042-gates.ts`, a
   `convergence` mode) computing every predicate below;
3. the four flop classes pinned as their CANONICAL representatives
   under the engine's 24-suit permutation rule (exact tuples, relabel
   vectors, and the correction of the Accepted draft's mis-stated
   representatives are recorded in the Stage-2 freeze addendum),
   plus the verified 134,459-river-class enumeration;
4. the safety wall constant (3,600 s), host name, and a `df` snapshot
   as capacity EVIDENCE ONLY (483 GB free at RFC revision time); the
   200 GB disk budget and 7-day wall budget are FIXED constants and are
   never recomputed from the snapshot. Also the run-parallelism plan
   (independent per-cell processes, OMP width chosen so concurrent
   cells stay under the host; planned: three seat-band processes x 14
   OMP threads, matching RFC 0042's clean-wall setup);
5. every projection formula and its inputs:
   - per-class packed bytes `B_river(L)` = the measured full-range
     sealed bytes/class for river band L at that band's r* rung,
     multiplied by the shipped-library BSFLIB overhead ratio 1.0249
     (`library_bin_bytes / sum(section_bytes)`, measured on
     `artifacts/flop-library-6way-fullrange`; the ratio is assumed
     stable across class count and is flagged as such for the
     follow-on);
   - river campaign disk for one root profile and band L
     `= B_river(L) x 134,459 x overhead`. The 134,459 is the number of
     suit-isomorphic classes of the FULL five-card river board under
     the engine's canonicalization rule (2,598,960 concrete boards),
     NOT the 1,755 flop classes — the flop partition applied to river
     boards merges their classes 76.6:1 and is the compression
     topology the follow-on may later choose, which this RFC must not
     presuppose. A lower bound at 1,755 and natural upper bound at
     134,459 are both reported, but the AC6 gate is required to pass
     under the 134,459 projection (the honest worst case for an
     uncompressed river-root artifact);
   - per-class wall from the measured full-range seconds/iteration at
     r*; build wall `= 134,459 x per-class seconds / W(L)`, where the
     parallel width `W(L) = floor(0.75 x RAM_bytes / measured
     peak RSS_L)`, capped at 56; `peak RSS_L` is the measured full-range
     cost-run peak for band L (the existing 2 GiB/worker estimate is
     nine-tier-specific and is not reused);
   - turn (16,432-class) and any coarser river-compression artifact are
     explicitly NOT projected; choosing either is the follow-on's
     design, gated on a river-Go here.
   - no projection uses a rung, width, or class count not named here.

## Dependency Rules

- Abstraction owns the re-implemented equity function; the in-memory
  trainer seam and the benchmark driver are the only consumers. The
  three protected-entry invariants from RFC 0042 (retained 3-arg
  entry refuses the new kind; no default L) stand.
- `flop_library`, resident, artifacts, codec, resolver, seat policy,
  continuation range/root value, and the packed library are untouched.
- The benchmark is evidence tooling; nothing it produces is mounted.
- The cost projection reuses measured builder constants (75% RSS
  gate, section overhead) but invokes no turn/river builder that does
  not exist; that construction is the follow-on's work.

## Design Principles

- **Measure before the flip.** RFC 0042 settled the online envelope;
  this RFC settles offline feasibility with the same discipline
  (predeclared gates, raw CSV, mechanical evaluation) before any
  protected build/serve module changes.
- **Reuse the frozen specification, change only the budget.** The
  abstraction, parameter triple, enumeration order, and band table are
  re-implemented to the RFC 0042 text; re-tuning them after seeing
  convergence data would invalidate the measurement, and the digest is
  pinned in the pre-run addendum rather than assumed.
- **Smallest local closure.** In-memory training plus a benchmark
  driver suffice to answer convergence and cost; artifact formats and
  mount logic stay out of scope.
- **Finite resources declared first.** Disk (200 GB), wall (7 days),
  and per-cell safety wall (3,600 s) are predeclared from the host's
  measured capacity, not fitted to results.
- **Aggressive replacement only with evidence.** No feature flags, no
  dual artifact kind in the mounted path; a river-Go means one future
  coordinated flip, not parallel upkeep.

## Alternatives Considered

1. **Skip feasibility and write the flip RFC directly.** Rejected:
   the one open question (does 64-bin MCCFR converge offline at
   campaign-feasible cost) gates every one of the eight protected
   write sites; threading them first would be speculative shared-module
   change.
2. **Measure with the existing nine-tier or TiersDraw libraries only.**
   Rejected: both are the controls; the abstraction whose feasibility
   is unknown is equity.
3. **Build a full 134,459-class river campaign to "just see".**
   Rejected:
   weeks of CPU and unknown disk before any convergence evidence; the
   fixture cells plus the named projection formulas bound the decision
   at a fraction of the cost (R2 records the extrapolation risk
   explicitly and restricts the projection to river roots).
4. **Wait for a learned value model (RFC 0041 direction).** Rejected
   for this question: equity bucketing is classical, deterministic,
   and already proven beneficial; mixing in learning would confound
   the convergence measurement.
5. **Assume flop libraries are the beneficiary or gate on them.**
   Rejected: flop-terminal trees use the exact frontier for
   turn/river, so flop cells are cost-only descriptors and the street
   roots are measured separately (Section 2a, R3).
6. **Restore from git history.** Impossible: the RFC 0042 code was
   never committed and is absent from every reachable and unreachable
   git object (audited 2026-10-31); this RFC honestly re-implements
   from the frozen text and re-pins the digest.

## Compatibility and Migration

None. Production, artifacts, and mounted libraries stay at nine tiers;
the three-arg `card_bucket` and all protected callers are unchanged.
On no-go the newly written measurement code is deleted (its second
deletion) and production never diverged.

## Security and Operational Impact

Measurement code only; no credentials, network, game-I/O, or mounted
artifact changes. CPU cost is bounded by the predeclared campaign
plan (three seat-band processes at 14 OMP threads, the builder-style
RSS gate); disk writes are CSV evidence only.

## Risks

- R1: equity converges at 2-3 seats but not 4 (infosets grow faster
  than visits). The 4-seat trajectory is a first-class gate; its
  outcome is in the truth table (river-Go restricted to 2p+3p) rather
  than a discovered cost.
- R2: the campaign projection extrapolates from fixture cells to the
  full 134,459-class uncompressed river universe and full ranges;
  texture variance is bounded by the four flop classes (including the
  majority two-tone texture) plus the W1 boards, the section-overhead
  ratio 1.0249 is measured on the shipped library, and the follow-on
  confirms on the full set before any flip. Any COARSER river
  compression (down toward the 1,755 flop-class lower bound) is
  excluded from this projection and is the follow-on's explicit design
  choice after a river-Go.
- R3: flop-terminal gains cannot be tested here (exact frontier); the
  flop cells are cost-only, and no flop-Go is claimable.
- R4: a second implementation could drift from the frozen textual
  spec; the new independent oracle plus the addendum-pinned digest
  and parameter triple guard it, with no appeal to deleted fixtures.
- R5: truncation at the 3,600 s wall is plausible for the largest 4p
  cells; the truth table defines the truncated-curve outcomes
  explicitly instead of improvising after the run.

## Verification Plan

- New oracle/band/memo ctests conform to the RFC 0042 textual spec
  (term counts, fan-out, same-binary 0-ULP, L windows, malformed
  inputs) and the addendum-pinned digest;
- release + asan builds, full ctest, npm/proto, both doc/RFC
  checkers; default trainer builds stay bit-identical (digest test);
- the convergence driver's outputs are committed raw and evaluated by
  the addendum-frozen mechanical evaluator (the retained
  `tools/rfc/rfc0042-gates.ts` extended with a `convergence` mode,
  plus its Node tests); the pre-run addendum and evaluator land before
  any measurement cell runs.

## Rollout Plan

1. Re-implement the equity machinery and seam per the frozen text
   (measurement only); recompute and pin the digest.
2. **Pre-run freeze (Stage 2):** write the freeze addendum (CSV
   headers, mean-over-5 aggregation, verified flop class tuples,
   safety wall, host/df snapshot, parallelism plan, every projection
   formula and its inputs) and extend the mechanical evaluator with
   the convergence/truth-table predicates and Node tests. No cell
   runs until this is recorded.
3. Run the frozen iteration-ladder cells under the campaign plan;
   commit the raw CSVs.
4. Mechanically evaluate the convergence, truth-table, and cost gates;
   record river-Go / no-go / measurement-incomplete in this RFC after
   independent review.
5. On river-Go: a separate RFC designs the river artifact/flip
   campaign (turn/flop/5-6 seats not included). On no-go: delete the
   new measurement code; keep evidence and this RFC. On
   measurement-incomplete: the new code is likewise deleted per AC8,
   and any re-measurement is a THIRD implementation under a new freeze
   (higher safety wall); the code is not left dormant between campaigns.

## Rollback Plan

Delete the newly written enum/function/seam/driver/ctests (their
second deletion); the tree returns to its nine-tier state. Evidence
CSVs and this RFC remain.

## Open Questions

- At which iteration cap (if any) does 4-seat river equity bucket sum
  reach 0.05, and is the per-class wall campaign-feasible?
- Does the finer abstraction reduce or increase full-range row counts
  net at convergence (fewer policy-distinguishable nodes vs more
  buckets)?
- What is the per-class cost ratio of a hypothetical equity flop
  library at 6 seats (measured descriptively; not a Go input)?

## Acceptance Criteria

All predicates operate on the five-seed MEANS of the frozen CSV and
are evaluated by the addendum-frozen `convergence` mode before any
decision discussion. `bucket_sum(L, r)` is the mean river
bucket-constrained summed gap for band L at rung r; `exact(L, r)` the
mean exact summed gap; `tiers_exact(L, r)` the same-cap nine-tier
control. Rungs in ascending cap order; a TRUNCATED rung is the last
completed rung and ends the curve.

- AC1 (spec conformance): the re-implementation matches the RFC 0042
  frozen textual specification (enumeration terms 990/45,540/
  1,070,190, ascending order, K 32/64/64, band max id 1,608, retained
  3-arg entry refusing the kind, four-arg/L validation, memo
  separation); the recomputed `card_abstraction_id` digest over the
  reconstructed parameter string is pinned in the pre-run addendum
  and asserted in a ctest; the new independent oracle gives
  same-binary exact-double/bin agreement; a new
  `enumerate_canonical_river_classes()` enumerator plus ctest pins
  134,459 classes (sibling of the flop/turn enumerators). No protected
  production/build/serve file is modified.
- AC2 (raw evidence): the driver emits the frozen-header rows, all
  committed raw, for: (a) the nine-tier and equity convergence ladders
  over river/turn seats {2,3,4} on the fixture range, and (b) every
  full-range cost run (river per-band r*, four flop classes at seats
  {2,6}), with their gap columns recorded -1 (cost-only). Every
  completed or truncated rung is present; truncation is marked, never
  silently dropped.
- AC3 (2p/3p river convergence): for EACH band L in {2,3} define
  `r*_L` mechanically as the SMALLEST completed rung at or before 10M
  (with at least three completed rungs at or below it) such that
  `bucket_sum(L,r*_L) <= 0.05` BB and the three latest completed rungs
  at/below it satisfy the flattening predicate
  `|g(r-2)-g(r)| / max(g(r-2),epsilon) <= 0.20` on the bucket curve
  (a numerical plateau band, not strict monotone decrease; epsilon is
  the evaluator's divisor guard). Choosing the smallest qualifying
  rung is predeclared so the cost runs cannot be placed after seeing
  later rungs' bytes/seconds; if no rung qualifies the band does not
  converge. r*_2 and r*_3 are band-specific and may differ. Every
  benefit predicate (AC4/AC4b/AC5) and every cost run references this
  single evaluator-computed choice.
- AC4 (2p same-cap paired benefit): at r*_2 the paired improvement vs
  the same-cap nine-tier control `d2 = tiers_exact(2,r*_2) -
  exact(2,r*_2)` has mean >= (2/3) x 0.206 = 0.137 BB and five-seed
  range strictly positive (`min d2 > 0`). Apples-to-apples offline
  comparison; the RFC 0042 10 s control is not reused.
- AC4b (3p same-cap paired benefit): at r*_3, `d3 =
  tiers_exact(3,r*_3) - exact(3,r*_3)` has mean strictly positive and
  five-seed range strictly positive (`min d3 > 0`). No fixed magnitude
  threshold at 3p; the band is admitted only when equity does not
  worsen 3p exact quality at its converged rung.
- AC5 (4-seat river gate): the L=4 curve is reported at every
  completed rung (at least three completed rungs required for a
  structural verdict). PASS for the band iff `r*_4`, defined by the
  SAME smallest-qualifying-rung rule as AC3 applied to L=4, satisfies
  the convergence predicate AND the paired benefit `d4 =
  tiers_exact(4,r*_4) - exact(4,r*_4)` has five-seed range strictly
  positive. If a plateau above 0.05 is already visible on at
  least three completed rungs at/below 10M, the band is a STRUCTURAL
  FAIL even if the top rung was truncated (no cheap rerun). Only when
  fewer than three rungs completed, or the curve is still descending at
  the top completed rung with no plateau, is the band
  MEASUREMENT-INCOMPLETE (truth table).
- AC6 (campaign disk): with the Section 3 formula and the converged
  full-range bytes, the projected river campaign
  `sum over Go bands L of B_river(L) x 134,459 x 1.0249` is <= the
  FIXED 200 GB budget. If any Go band's cost run was truncated so
  B_river(L) is undefined, AC6 is MEASUREMENT-INCOMPLETE for that
  band.
- AC7 (campaign wall): the projected river build wall
  `134,459 x per-class full-range seconds at r*_L / W(L)` at the
  measured per-band peak RSS is <= the FIXED 7-day budget for the
  smallest Go set (the L=2 band, one river root profile); truncated
  cost runs make AC7 measurement-incomplete for that band.
- AC8 (mechanical decision): the truth table below is evaluated
  per band from the frozen CSVs; L=2 and L=3 are independent verdicts
  (both required for a 2p+3p Go; one band's fail does not block the
  other). The outcome and evidence are recorded after independent
  review; on any outcome other than river-Go the new code is deleted
  per the no-legacy rule.

Decision truth table (evaluated per band; cost is three-valued per
band — PASS = projected within both fixed budgets, FAIL = defined
projection exceeds 200 GB and/or 7 days for the candidate Go set,
INCOMPLETE = the full-range cost run was truncated so B_river(L) is
undefined):

| River band | convergence + paired benefit | cost | Outcome for that band |
|---|---|---|---|
| L=2 or L=3 | pass (AC3 + AC4 or AC4b) | PASS | river-Go for that band |
| L=2 or L=3 | pass | FAIL | no-go (cost infeasible at 134,459 classes; evidence retained) |
| L=2 or L=3 | pass | INCOMPLETE (cost run truncated) | measurement-incomplete; band does not flip |
| L=2 or L=3 | structural fail (plateau above 0.05, or paired-benefit range <=0, on >=3 completed rungs) | any | no-go for that band |
| L=2 or L=3 | no plateau yet and the 10M rung COMPLETED (not truncated) with g(10M)>0.05 still descending | any | structural no-go (catch-all: AC3's "no rung qualifies -> does not converge"; a completed non-converged top rung is never incomplete) |
| L=2 or L=3 | no plateau yet and the top completed rung was truncated, with >=3 completed rungs | any | measurement-incomplete ONLY IF the five-seed mean half-width of `bucket_sum` at the top completed rung (paired-t t_4/sqrt(5) x SD across the five seeds) exceeds the frozen 0.01 BB; otherwise structural no-go |
| L=2 or L=3 | fewer than 3 completed rungs total | any | measurement-incomplete (campaign under-run; re-freeze a higher safety wall), regardless of half-width |
| L=4 | convergence pass with positive benefit (AC5) | PASS | include 4p band |
| L=4 | convergence pass | FAIL | 4p excluded on cost; does not block 2p/3p |
| L=4 | convergence pass | INCOMPLETE | 4p does not flip; does not block 2p/3p |
| L=4 | structural fail (>=3-rung plateau above 0.05) | any | 4p excluded, no rerun |
| L=4 | incomplete per AC5 (<3 rungs or still descending at top) | any | 4p excluded pending a later freeze; 2p/3p may still Go |

Cost is evaluated for the candidate Go SET, not per band in
isolation: AC6 sums `B_river(L) x 134,459 x 1.0249` over every band
proposed for Go and AC7 takes the max projected wall over that set;
a FAIL is a cost no-go for the WHOLE candidate Go set, and no band may
be silently dropped post hoc to flip a subset (if a follow-on ever
wants to prioritize bands under a budget, that priority order must
itself be predeclared in its own freeze; this RFC defines none).

The overall outcome is the union of per-band results; a single-band
Go (e.g. 2p only) is a legal union outcome — bands are independent
verdicts and a 3p structural fail or incomplete does not block a 2p
Go, and vice versa. A measurement-incomplete band is resolved before
any flip. Turn roots and flop classes never appear in the outcome
columns; they are cost/benefit descriptors for the follow-on's
backlog.

## Stage-2 Pre-Run Freeze Addendum (2026-10-31)

This addendum is frozen BEFORE any campaign cell. All constants,
headers, formulas, and evaluator behavior below are pinned; the first
campaign cell runs after this addendum and the evaluator land. Driver
smoke runs during Stage 1 used 30/60 s safety walls and S=1 into throwaway
`/tmp` files only; no row quoted below exists from those runs, and they
feed no predicate.

### A. Recomputed digest and oracle

`card_abstraction_id(EquityStrengthV1)` recomputed over the
reconstructed parameter serialization (205-byte canonical string
`equity-strength<NUL>1<NUL>bucket=ehs-vs-uniform-1opp;k_flop=32;k_turn=64;k_river=64;enumeration=conditional-exact;order=opp-then-run-ascending;bin=equal-width-floor;ids=169+band(L)+street;L=2..10;freeze=2026-10-30`):

- **digest = `0x274f82fe89b66a1e`** (production FNV-1a; golden-literal
  assertion in `test_abstraction.cpp`, verified against the compiled
  production function).

The new independent oracle ctests pass in release and ASan:
term counts 990 / 45,540 / 1,070,190, fan-out 1,081, same-binary
exact-double/bin identity, L-window coverage, memo separation, and
`enumerate_canonical_river_classes() == 134,459`.

### B. Frozen CSV contract

One header for every campaign file (ladder, cost, and flop rows; the
`mode` column distinguishes them):

```text
mode,scheme,seats,street,seed,target_iters,safety_wall_s,completed,infosets,sealed_bytes,joint_deals,max_gap_bb,sum_gap_bb,bucket_sum_gap_bb,wall_observed_s,peak_rss_kb,truncated,note
```

Field semantics and units:

- `mode`: `ladder` (fixture convergence cells) | `cost` (full-range
  river/turn cost cells) | `flop` (flop-terminal cost cells).
- `scheme`: `tiers` (nine-tier control) | `equity` (EquityStrengthV1).
- `seats`, `street` (`river` | `turn` | `flop`), `seed` (paired across
  schemes by exact value), `target_iters` (the rung cap),
  `safety_wall_s` (always 3600 in the campaign).
- `completed`: last completed iteration count; `truncated=1` iff
  `completed < target_iters`.
- `infosets`: trainer information sets; `sealed_bytes`:
  `nseat_policy_accounted_bytes` of the trained policy;
  `joint_deals`: NashConv enumeration count (0 when gaps are not
  evaluated); `max_gap_bb`: largest live-seat exact BR gap in BB
  (big blind = 2 chips); `sum_gap_bb`: live-seat summed exact gap;
  `bucket_sum_gap_bb`: river bucket-constrained summed gap;
  `wall_observed_s`: training wall seconds (3 decimals);
  `peak_rss_kb`: per-cell resident-set peak in KiB; `note`: flop class
  key on flop rows, empty otherwise.
- `sum_gap_bb`, `max_gap_bb`, and `bucket_sum_gap_bb` are emitted 0.0 /
  recorded as `-1` whenever the corresponding evaluation does not apply
  (cost/flop cells; bucket gap wherever chance nodes exist — every turn
  and flop row). `-1` is a sentinel, never a measurement.

Peak RSS methodology: VmHWM/ru_maxrss are process-lifetime maxima and
cannot be attributed in a multi-cell process. The driver samples
`/proc/self/statm` resident pages every 100 ms on a dedicated timer
thread for the duration of one cell; `peak_rss_kb` is that cell's
maximum. This is conservative relative to a single end-of-run read.

Resource caps (offline measurement, not the serve-sized defaults):
trainer `max_bytes = 24 GiB` per process and
`max_information_sets = 64,000,000`; tree `TreeLimits` stay default
(the public action tree is small under cap-1 actions). A resource-cap
cross is not a truncated rung: training throws, the row carries a
`train:` note with zero completions, and the evaluator rejects the
campaign rather than estimating (a re-freeze decision, per Section 2).

Committed raw files (one header per shard invocation; names and the
wave/shard mapping are in Section G):
`rfc0043-ladder-seats{2,3,4}-shard{0..9}.csv`,
`rfc0043-cost-seats{2,3,4}-shard{0,1}.csv`,
`rfc0043-flop-seats{2,6}-shard{0,1}.csv` under
`docs/plans/ablation/`.

### C. Aggregation and evaluator (`convergence` mode)

Command: `node bin/rfc0042-gates.mjs convergence <csv...>`; it loads
any mix of frozen campaign files, splits by `mode`, and emits one JSON
report (per-band convergence/cost/verdict, candidate Go set disk, plus
descriptive turn ladder, turn full-range cost, and flop summaries —
none of which gate a verdict). Implementation:
`tools/rfc/rfc0042-gates.ts` (`RFC0043_*` exports); regression coverage
is 28 new project-native Node tests in
`tools/rfc/rfc0042-gates.test.ts` (35 total).

Frozen mechanics:

1. Aggregation key is `(scheme, seats, street, target_iters)`; means are
   over exactly the five ladder seeds (S=5) or two cost seeds (S=2).
   Every PRESENT ladder rung must carry exactly five equity and five
   tiers rows with identical seed sets, and the seed set must be
   identical across rungs; the evaluator throws on any count/set
   mismatch (a missing shard is an evidence error, never a binding
   n=4 verdict). A truncated tiers control row at a rung also throws.
2. Curve order is the frozen rung list
   `{100000, 300000, 1000000, 3000000, 10000000}`. A rung missing while a
   later rung is present is a gap and throws (evidence integrity); a
   missing rung with no later rungs ends the curve (campaign under-run).
3. The first rung where ANY equity seed is truncated is the curve's last
   rung; later rungs are excluded. A truncated rung can never be r*.
4. r* per AC3: smallest rung index `i >= 2` with five untruncated
   equity seeds, `bucket_mean <= 0.05`, and
   `|g(i-2) - g(i)| / max(g(i-2), 1e-9) <= 0.20`. The 20% comparison is
   literal floating-point `<=`; no extra boundary tolerance exists
   (epsilon guards only the divisor).
5. Paired benefit at r*: per-seed `d = tiers.sum - equity.sum`;
   AC4 `mean(d) >= 0.137 && min(d) > 0`; AC4b `mean(d) > 0 && min(d) > 0`;
   AC5 `min(d) > 0`.
6. Truncated-top half-width uses the five equity bucket values:
   `2.776 / sqrt(5) x sample SD` (t critical, df=4).
7. Classification follows the AC8 truth table exactly in this order:
   under 3 rungs -> incomplete; r* with failed benefit -> structural
   no-go; no r* with a truncated top whose THREE latest rungs are all
   above 0.05 within the 20% band -> structural no-go (AC5's "plateau
   above 0.05 on at least three rungs"); truncated top otherwise ->
   incomplete iff half-width > 0.01 else structural no-go; at a
   COMPLETED, non-truncated top the 10M rung is the catch-all —
   g(10M) > 0.05 is the literal structural no-go, while g(10M) <= 0.05
   without any qualifying three-rung plateau is structural no-go as
   well (AC3 unsatisfied) with a distinct reason string. When the top
   completed rung is a smaller rung and later rungs are simply absent
   (process never reached them, no truncation flag), the band is
   incomplete (campaign under-run), since the catch-all cannot fire
   below 10M.
8. Evidence integrity is validated row by row before any predicate and
   throws on: zero completions (a failed build/train cell), a safety
   wall other than the frozen 3,600 s, a truncated flag disagreeing
   with `completed < target`, non-empty notes on ladder/cost rows,
   ladder rows without exact/bucket gap enumeration, gap values other
   than `-1` on cost/flop rows, foreign ladder/cost rungs or seat
   counts, and flop notes outside the four frozen canonical keys (a
   failed flop cell keeps the class key with a sanitized build/train
   reason appended and is still rejected on zero completions). The CLI
   additionally runs a whole-set guard requiring all three river ladder
   bands and the complete flop evidence (both seat groups x four
   classes x two schemes x two seeds); a forgotten file fails the run
   instead of silently degrading a band.
9. Cost-state contagion per Section D: any truncated candidate-band
   cost run makes EVERY candidate band cost-INCOMPLETE (set disk
   projection undefined); a FAIL requires a defined projection that
   exceeds the 200 GB set budget or a band's own width/wall limit.

### D. Campaign-cost projection (AC6/AC7 frozen interpretation)

Inputs come ONLY from full-range equity `cost` rows at r* (S=2); a
truncated equity row, or a truncated nine-tier paired-control row at
the same rung, makes that band's cost state `INCOMPLETE` (the control
is cost context too, even though only equity bytes feed the
projection). Means over the two seeds:

- `B_raw(L)` = mean `sealed_bytes`; per-class packed
  `B(L) = B_raw(L) x 1.0249`.
- AC6 (shared): candidate-Go-set disk
  `sum_L B_raw(L) x 134,459 x 1.0249 <= 200,000,000,000` bytes
  (200 GB decimal; `df -B1` convention). Disk is one shared budget.
  When EVERY candidate band's cost is defined, a set-level overrun
  fails the whole candidate set (the AC8 rows "cost FAIL -> no-go", and
  no band may be silently dropped post hoc). When ANY candidate band's
  cost run is TRUNCATED, the set projection is UNDEFINED: every
  candidate band evaluates to INCOMPLETE, including bands whose own
  cost run completed (a defined projection that exceeds a budget is the
  only FAIL condition; an undefined one is never FAIL — an irreversible
  no-go cannot be minted from another band's truncation).
- AC7 (per profile): `seconds(L)` = mean `wall_observed_s`;
  `RSS(L)` = mean `peak_rss_kb x 1024`;
  `W(L) = min(56, floor(0.75 x 117,940,875,264 / RSS(L)))`;
  `wall(L) = 134,459 x seconds(L) / W(L)`. `W(L) = 0` is a FAIL for
  that band. Residual tension with the Accepted AC7 wording (which says
  "the max projected wall over the candidate set"): for any multi-band
  Go set the two wordings would differ if one band's profile exceeded
  7 days. This addendum freezes the per-band-profile reading, matching
  the truth table's explicit "4p convergence pass / cost FAIL /
  does not block 2p/3p" rows and the legal single-band-union outcome;
  the shared DISK budget still fails the whole set as above. The
  follow-on flip RFC inherits whichever bands actually pass.
- Flop/turn rows never enter any projection.

### E. Pinned flop classes

The four classes are pinned by the minimum-key CANONICAL board under
the engine's own 24-suit permutation canonicalizer
(`bs::abstraction::canonicalize()`), using the ENGINE suit order
`s=0, h=1, d=2, c=3` (`engine/include/bs/eval.hpp`, `cardId`; card id
= rank*4+suit). Verified empirically against the production canonicalizer
linked from the release build, independently reproduced by an S4
enumeration in the engine suit order, and consistent with the pinned
relabel expectations in `test_abstraction.cpp`:

| class | concrete member cited in Section 2 | canonical rep (frozen) | relabel |
|---|---|---|---|
| unpaired rainbow A-high | As Kd 7c | **7s Kh Ad** | 2,3,1,0 |
| paired | Ks Kd 7c | **7s Kh Kd** | 1,3,2,0 |
| monotone | As Ks Qs | **Qs Ks As** | 0,1,2,3 |
| unpaired two-tone | As Kh 7s | **7s Kh As** | 0,1,2,3 |

Record of the verification error this corrects: an earlier draft of
THIS addendum claimed the representatives were 7c Kd Ah / 7c Kd Kh /
Qc Kc Ac / 7c Kd Ac and asserted the Accepted draft was wrong on all
four. That "correction" was itself false. Its throwaway verifier
defined a private card parser with the INVERTED suit order
(c=0,d=1,h=2,s=3), and the supposedly independent S4 cross-check used
the same inverted convention, so the two checks agreed with each other
but not with the engine. (Hand-derivation also rejects the inverted
result: a suit permutation preserves ranks, and on a RAINBOW board the
three suits are distinct, so the lexicographically smallest assignment
maps the sorted ranks 7,K,A to suit codes 0,1,2 = spades, hearts,
diamonds deterministically — 7s Kh Ad, with the A card Ad, never Ah;
Ah would require hearts to be suit code 0, i.e. the inverted order.)
The Accepted draft's first two tuples were correct canonical
representatives, and its last two citations were the correct canonical
SETS written in non-ascending order (canonical tuples are sorted by
card id: Qs Ks As, 7s Kh As).

Impact on evidence: the driver's card parser is the engine `cardId`,
so the boards it trained during smoke runs were always members of the
intended iso classes (correct ranks and suit texture); cost is
invariant under suit relabeling, so no predicate or projection was
ever polluted. Only the draft's labels and "canonical" claims were
wrong. The driver table (`kRfc0043FlopClasses`) and the evaluator
whitelist now pin the true canonical boards/keys listed above; a
failed flop cell keeps its class key with the sanitized build/train
reason appended.

The river universe stays pinned at **134,459** suit-isomorphic classes
(`enumerate_canonical_river_classes()`, ctest-pinned; 2,598,960 concrete
boards); the flop 1,755 and turn 16,432 counts remain descriptive
bounds only.

### F. Host, wall, and parallelism snapshot (evidence)

- Host: `n37-100-184`, Linux 5.15.152.bsk.4-amd64, 56 logical CPUs.
- RAM: `MemTotal = 115,176,636 kB = 117,940,875,264` bytes (the AC7
  RAM constant; evidence only).
- Capacity snapshot 2026-10-31 (UTC 19:14): `/dev/vdb` mounted at
  `/home/zhangdi.zode`, 2,163,366,428,672 bytes total,
  499,452,289,024 bytes available (76% used). EVIDENCE ONLY; the fixed
  200 GB disk and 7-day wall budgets are never recomputed from it.
- Per-cell safety wall: **3,600 s**; it bounds runaway cells only.
- Parallelism: `train_nseat` is single-threaded (it calls the trainer
  with worker count 1), but the NashConv evaluation in the ladder cells
  IS OpenMP-parallel — three pragmas in
  `engine/benchmarks/subgame_nash_conv.cpp` (joint-deal value walk, the
  two pooled best-response walks). The build links libgomp, so without
  a pin every cell process would spawn up to 56 threads during gap
  evaluation; the 30-process ladder wave would oversubscribe roughly
  1,680:56 and, worse, steal CPU from other processes' TRAINING threads
  while the wall clock (which is also the 3,600 s safety-wall clock)
  keeps running — contention could manufacture truncation/incomplete
  verdicts. Every frozen campaign command is therefore run with
  **`OMP_NUM_THREADS=1`**: process-per-shard is the sole fan-out. Each
  RFC 0043 command takes optional `[shard_index shard_count]`
  arguments and enumerates its cells in ONE frozen order (ladder:
  street -> rung -> seed -> scheme; cost: street -> seed -> scheme;
  flop: class -> seed -> scheme), running only cells with
  `global_index % shard_count == shard_index`; every shard file carries
  the same frozen header and the evaluator consumes them as a set.
  Frozen wave plan:
  1. Ladder: 10 shards per band x 3 bands = **30 concurrent processes**
     (`OMP_NUM_THREADS=1`, 30 of 56 cores; fixture residency is tens of
     MB), every shard runs one (seed, scheme) pair across both streets
     and all five rungs (10 cells).
  2. Cost: only bands with a mechanical r*, in three SEQUENTIAL waves
     L=2 -> L=3 -> L=4; within a wave **2 shards** (the frozen
     street -> seed -> scheme enumeration partitions by scheme: shard 0
     is the four tiers cells, shard 1 the four equity cells, both
     streets x both seeds sequential per shard; gap evaluation is off,
     so no OMP region is entered regardless), so at most two full-range
     processes coexist (2 x 24 GiB trainer cap = 48 GiB, safe within
     0.75 x 115 GB).
  3. Flop: two SEQUENTIAL waves by seats (2 then 6, the 6-seat wave
     kept apart for its unknown residency); within a wave **2 shards**
     over the 16 cells (8 cells sequential per process; no gap
     evaluation).

### G. Frozen campaign invocations and seeds

Ranges: ladder uses the ten-hand representative W1 fixture
(river board 2c 3d 7h Js 9c, turn 2c 3d 7h Js); cost/flop use the full
1,326-combo range board-filtered per seat. Base seeds (the five/two
seeds are `base+s`, identical for tiers and equity within a band):

| phase | band/seats | base seed | shards | per-shard command |
|---|---|---|---|---|
| ladder | L=2 | 4320 | 10 | `OMP_NUM_THREADS=1 ... --rfc0043-ladder 2 3600 5 4320 <i> 10` |
| ladder | L=3 | 4330 | 10 | `OMP_NUM_THREADS=1 ... --rfc0043-ladder 3 3600 5 4330 <i> 10` |
| ladder | L=4 | 4340 | 10 | `OMP_NUM_THREADS=1 ... --rfc0043-ladder 4 3600 5 4340 <i> 10` |
| cost | L=2 | 4420 | 2 | `OMP_NUM_THREADS=1 ... --rfc0043-cost 2 3600 2 4420 <r*_2> <i> 2` |
| cost | L=3 | 4430 | 2 | `OMP_NUM_THREADS=1 ... --rfc0043-cost 3 3600 2 4430 <r*_3> <i> 2` |
| cost | L=4 | 4440 | 2 | `OMP_NUM_THREADS=1 ... --rfc0043-cost 4 3600 2 4440 <r*_4> <i> 2` |
| flop | 2 | 4520 | 2 | `OMP_NUM_THREADS=1 ... --rfc0043-flop 2 3600 2 4520 <i> 2` |
| flop | 6 | 4560 | 2 | `OMP_NUM_THREADS=1 ... --rfc0043-flop 6 3600 2 4560 <i> 2` |

(`...` is the benchmark path
`build/release/engine/bigshark-subgame-nash-conv`.)

Raw shard output files (header per file, committed as a set):
`docs/plans/ablation/rfc0043-ladder-seats{N}-shard{i}.csv`,
`rfc0043-cost-seats{N}-shard{i}.csv`,
`rfc0043-flop-seats{N}-shard{i}.csv`.

Cost commands run ONLY for bands with a mechanically selected r*, and
the `<r*>` argument is the evaluator-printed rung, never a re-chosen
value. The frozen intermediate read between the ladder and cost waves
(ladder files alone, before cost/flop evidence exists) is:

```text
node bin/rfc0042-gates.mjs rstar docs/plans/ablation/rfc0043-ladder-seats*-shard*.csv
```

It validates the ladder set (all three bands, river+turn five-seed
mirrors) and prints one CSV row per band:
`band,rstar_iters,rstar_index,rungs,river_bucket_mean_at_rstar,benefit_mean,benefit_min,note`
(`note = "no qualifying rung"` when the band has no r*). This is the
auditable source of the `<r*>` arguments; the operator writes no ad-hoc
script. The full `convergence` command remains the only verdict path
and requires the complete campaign set (ladder + cost + flop). Waves: all 30 ladder shards concurrently; cost shards per band
in sequential L=2 -> L=3 -> L=4 waves; flop seats 2 then seats 6. No
campaign cell runs before this addendum; no projection uses a rung,
width, seed, or class count not pinned here.

## Decision

Author agent: Claude (zzzode agent session, 2026-10-31)
Approved by: independent approval agent a77541adae094e220
Decision date: 2026-10-31
Review outcome: Approved
Reviewed scope: RFC 0043 across four review rounds; RFC 0042 Section
1/1a/2/3 and Stage 3 amendment; post-0041 plan; all cited CSVs;
`tools/rfc/rfc0042-gates.ts`; current engine source; flop manifest and
builder; full reachable/unreachable git object audit; independently
re-derived board class universes (flop 1,755 / turn 16,432 / river
134,459) and shipped-library arithmetic (1.0249 overhead, 76.6:1
merge).
Review summary: round 1 rejected the false "restored from git history"
provenance (the RFC 0042 implementation was never committed) and gate
mechanical gaps; round 2 established river-only decision scope; round
3 corrected the river class universe from 1,755 to the independently
enumerated 134,459 and required full-range cost runs as the sole AC6/7
input; round 4 pinned the smallest-qualifying-rung rule and completed
the per-band three-valued cost truth table. The proposal is a
measurement-only second implementation of a spec-frozen function with
predeclared mechanical gates, a fixed 200 GB/7-day campaign budget
evaluated on the honest uncompressed 134,459-class upper bound, and
mandatory deletion on any non-Go outcome. Accepted residual risks:
the 1.0249 overhead ratio assumption across the class-count scale
(flagged for the follow-on), plausible 4p safety-wall truncation
(pre-classified), fixture-convergence plus S=2 cost extrapolation
(full confirmation deferred to any flip), and same-binary-only 0-ULP
oracle equality. Five non-blocking hardening items were absorbed into
the round-4 text and are required in the pre-run Stage-2 freeze
addendum (catch-all structural-no-go classification; one
smallest-rung rule for L in {2,3,4}; set-level cost-no-go semantics;
measurement-incomplete deletion/re-implementation disposition;
46.6% unpaired two-tone label and five-seed mean half-width naming).
Implementation may begin at Stage 1 and Stage 2; no measurement cell
runs before the freeze addendum.

## Stage 3 Outcome: no-go (structural), recorded 2026-10-31

The frozen campaign ran on `n37-100-184` after the Stage-2 freeze and
two independent review rounds (freeze approval agent
`ad8498345d9e9ba69`). The second review round also audited and
approved the campaign launch; after the data arrived the SAME agent
ruled independently on the boundary case below, re-aggregating all
300 ladder rows itself rather than trusting the coordinator's summary.

Raw evidence retained:
`docs/plans/ablation/rfc0043-ladder-seats{2,3,4}-shard{0..9}.csv`
(300 ladder rows) and
`docs/plans/ablation/rfc0043-flop-seats2-shard{0,1}.csv` (16 flop
seats=2 rows). Flop seats=6 was not completed: the ruling agent
confirmed flop evidence has zero decision weight and directed the wave
be stopped to save CPU; no verdict uses it.

Measured facts:

- Every RIVER cell in all three bands completed all five rungs
  untruncated; the only five truncated rows are descriptive 4p
  turn-equity 10M cells (9.36M-9.87M at the 3,600 s wall).
- Equity river bucket-summed gap means at 10M were 0.0164 / 0.0272 /
  0.0249 BB for L=2/3/4, all below the fixed 0.05 BB bar, with five-seed
  paired-t half-widths 0.00014 / 0.00023 / 0.00094 BB (10-70x smaller
  than the 0.01 incomplete cutoff — the continued decline is not noise).
- The frozen AC3 three-rung relative plateau
  `|g(r-2)-g(r)|/max(g(r-2),1e-9) <= 0.20` was never satisfied at any
  rung of any band; two-rung relative declines were 0.71-0.84 across
  the whole ladder, a near-constant ~0.52 per-rung retention (~T^-0.55
  RM+ decay).
- Control that decided the interpretation: the nine-tier incumbent
  fails the IDENTICAL plateau predicate at every rung of every band
  (10M bucket 0.0064 / 0.0109 / 0.0168; declines 0.73-0.84). The frozen
  certification is unreachable for both grids on this solver within the
  frozen budget; it certifies RM+ decay rather than abstraction
  non-convergence.
- Paired exact-gap benefits at 10M were positive: L=2 +0.261 BB
  (all seeds 0.260-0.263; AC4's 0.137 floor would pass), L=3 +0.792 BB
  (0.787-0.795), L=4 +0.0115 BB with all five seeds strictly positive
  (0.0087-0.0130; negative below 1M). With no r* these benefits are
  moot for the gates but answer the separate question "is equity
  better than tiers" decisively.
- AC6/AC7 (the actual W4 campaign-cost question at 134,459 classes)
  were NOT MEASURED: the frozen protocol runs cost cells only for bands
  with a mechanical r*, and no band had one.

Verdict per band: **structural no-go for L=2, L=3, and L=4.** Basis:
AC3 makes r* a conjunction of bucket <= 0.05 AND the plateau with no
exception ("if no rung qualifies the band does not converge"); AC8 row
5 states a completed non-converged top rung is never incomplete; and
the data-blind Stage-2 addendum C.7 extension plus the pre-campaign
regression test pin the exact observed state (completed 10M,
g(10M) <= 0.05, no qualifying plateau) as structural no-go. The ruling
agent rejected the "measurement-incomplete" reading: the frozen text
exhausts incomplete into narrow half-width/under-run cases, and
relabeling a complete, highly consistent ladder post hoc would be the
post-hoc goalpost move the freeze exists to forbid. A third campaign
under a NEW RFC with a reachable, predeclared certification (e.g. an
absolute-BB slope window calibrated first on tiers, and/or an extended
ladder) remains procedurally open; the no-go label forbids only reusing
this campaign, its nonexistent r*, or its predicate.

Documented residual risks carried into the result:

1. Instrument horizon: the T^-0.55 RM+ retention applies to both grids;
   a 20% relative two-rung plateau may be unreachable for ANY
   abstraction at realistic budgets. Strong motivation for a new
   absolute-slope instrument, not evidence that equity converges.
2. The 134,459-class disk/wall feasibility (AC6/AC7) stays OPEN; only
   fixture-scale sealed bytes exist.
3. Text tension: AC5's loose "still descending at the top ->
   incomplete" sentence (written expecting 4p truncation) vs the later,
   more specific data-blind C.7 classification; resolved by hierarchy
   toward structural this time, and any future freeze should state both
   cases in one place.
4. 4p benefit crosses zero only at 1M-3M and is ~20x smaller than 2p;
   future measurements should predeclare a 4p benefit-magnitude
   expectation.

## Stage 4 Cleanup (second deletion, completed)

Per the Rollout Plan and the ruling agent's mandated scope, all
measurement code was deleted without feature flags; the production tree
returns to its nine-tier state:

- `EquityStrengthV1` (exact/MC enumerator, memo, grid, digest case,
  three-arg refusal), `canonicalize_river`, and
  `enumerate_canonical_river_classes` from
  `engine/{include/bs/abstraction.hpp,src/abstraction/abstraction.cpp}`;
- the `card_kind` / `equity_grid` trainer seams from
  `nseat_trainer.{hpp,cpp}`;
- every RFC 0043 block and the four bucket-hook sites in
  `engine/benchmarks/subgame_nash_conv.cpp` (restored to the committed
  nine-tier calls; the three pre-existing NashConv OpenMP pragmas were
  never part of this RFC and remain);
- the oracle / term-count / fan-out / memo / golden digest
  (`0x274f82fe89b66a1e`) / 134,459-enumerator cases in
  `engine/tests/test_abstraction.cpp`;
- the RFC 0043 `convergence` / `rstar` modes, validators, and tests in
  `tools/rfc/rfc0042-gates.ts(.test.ts)`. The retained RFC 0042
  pilot/gate modes and their 7 Node tests remain (RFC 0042 KEEP
  retention).
- The working-tree RFC 0041 measurement drivers present in the same
  benchmark file (RFC 0041 closed Stage-1 failure on 2026-10-29 with
  "no code shipped") were removed in the same restore; they were
  already-due-for-deletion measurement code and their evidence CSVs are
  unaffected.

Retained: this RFC, the raw CSV evidence cited above, and the RFC/README
indexes. No production build, artifact, pack, checkpoint, codec, or
serve path was changed at any point; no mount digest is affected.
