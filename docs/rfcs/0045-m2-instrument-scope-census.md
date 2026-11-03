---
rfc: "0045"
subject: "M2 Heads-Up Full-Street Blueprint — Instrument Resolution and Blueprint-Scope Census"
status: "Implemented"
authors: "Claude (zzzode agent session)"
created: "2026-11-01"
updated: "2026-11-04"
owners: "engine abstraction/solver/flop_library benchmarks; tools/rfc evaluator"
supersedes: ""
superseded-by: ""
---

# RFC 0045: M2 Heads-Up Full-Street Blueprint — Instrument Resolution and Blueprint-Scope Census

## Summary

The M2 milestone (2-seat / 100 BB full-street offline GTO served from
mounted artifacts) is blocked by two questions the RFC 0044 campaign
exposed but did not resolve. (1) The best measured HU card abstraction
(EquityStrengthV1 32/64/64) converges inside its own abstraction
(bucket gap 0.0027 BB at 300M) yet retains a W1 exact-game gap of
0.0459 BB — above the 0.03 BB exact threshold that the M2 milestone
delegated to the convergence certification RFC, so the incumbent
instrument does not qualify; ~94% of the remaining gap is
card-abstraction merge loss on the chance-free river, which makes a
finer river card space the obvious lever, but whether it reaches 0.03
on FULL card textures and FULL ranges is unmeasured. (2) The only
served postflop artifact is a SIX-seat single-root FLOP-terminal
library whose turn/river are an equity-frontier approximation, and
the resident path refuses board_size 4/5; the HU reachable flop-root
set trained to the River terminal under the frozen heads-up
uncapped action abstraction, and its storage/training cost, are
unknown — and the frozen HU training cap (0) diverges from the
serving cap (1). This RFC is MEASUREMENT/SCOPING ONLY: it
re-introduces the minimum measurement machinery, runs a gated
campaign over BOTH the range and the board-texture axes, enumerates
the reachable roots with the full root identity (including button),
and hands a mechanical instrument verdict plus a costed build
envelope to the production follow-on RFC(s). It changes no
production build/pack/serve path and ships no mounted strategy.

## Motivation

RFC 0044 (closed 2026-11-01, no-grid outcome) established on the W1
HU fixture, five independent seeds per rung:

- EquityStrengthV1 (K_flop=32, K_turn=64, K_river=64) CERTIFIES inside
  its abstraction at 100M iterations (bucket-summed gap 0.004856 BB,
  half-width 0.000043; 0.002717 BB at 300M).
- Its exact-hole summed gap is 0.048416 BB at 100M and 0.045944 BB at
  300M, above the HU EPS_EXACT = 0.03 BB threshold the M2 milestone
  acceptance item 2 references ("at or below the BB threshold frozen
  by the convergence certification RFC",
  `docs/plans/gto-10seat-milestones.md`). The milestone text defines
  no separate 0.05 BB bar; 0.05 was a historical framing in RFC 0044
  and is NOT the M2 acceptance value. W1@300M (0.0459) therefore
  fails the frozen threshold, exactly as RFC 0044's QUAL FAIL / "the
  M2 asset campaign does not consume it" disposition states.
- The exact curve flattens (0.05548 at 10M -> 0.05194 at 30M ->
  0.04842 at 100M -> 0.04594 at 300M) while the within-abstraction
  curve reaches 0.002717; 1 - 0.002717/0.045944 = 94.1% of the
  residual is card-abstraction merge loss, not solver
  non-convergence.
- Every seed is strictly better than the same-seed nine-tier
  incumbent (~0.314 BB): the EHS family removes ~85% of the incumbent
  exact gap, so the direction is right, but 64 river bins leave the
  residual above the M2 threshold.

Two facts make the RFC 0044 result insufficient to build against.
First, W1 is ONE board texture and a ten-hand range; RFC 0044 itself
records (sections 3 and 6) that its W1 numbers cannot be extrapolated
to the full range and that full-range exploitability is re-measured
at the M2 gate. Card merge loss is strongly texture-dependent (RFC
0041: 15.3% of turn boards were not labelable by the learned card
model; RFC 0042 froze four canonical flop representatives with very
different occupancy). Second, no HU River-terminal blueprint scope
has ever been costed: the existing flop builder is Flop-terminal,
and heads-up training is frozen uncapped while board-3 serving uses
a one-raise cap. Building multi-GB River-terminal libraries on an
instrument that may fail full textures, under an action abstraction
whose tree cost is unmeasured, is the expensive failure mode this
RFC prevents.

## Goals

- G1. Re-introduce measurement-only EHS card bucketing with
  configurable river and turn bin counts, reachable only through a
  benchmark arm and the n-seat trainer measurement seam; the
  production default card kind is unchanged and no build/pack/serve
  path can reach the new kind.
- G2. Mechanically select the SMALLEST tested EHS grid whose exact
  summed gap is at most 0.03 BB at a mechanically selected rung, on
  EVERY frozen board-texture stratum over the FULL 1,326-combo range,
  with the within-abstraction gap at or below 0.005 BB and a
  sampling-independent per-training-seed half-width at or below
  0.001 BB.
- G3. Enumerate the distinct reachable HU 100 BB cap-0 flop roots
  using the FULL root identity (pot, big blind, button, per-seat
  stacks and contributions, folded mask, line tag), including the
  all-in-no-flop lines, and measure the River-terminal tree size and
  per-class rows/seconds for the frozen cap-0 training abstraction
  with cap-1 serving reported as the explicit divergence comparator.
- G4. Report a costed full-M2 envelope (roots x 1,755 iso classes,
  core-hours, wall on n37-100-184, artifact bytes, per-stratum
  maxima) and one explicit instrument verdict for the production RFC;
  the go/no-go to actually build remains a separately authorized
  decision.
- G5. Preserve reproducibility: frozen evaluator, committed raw CSVs,
  independent evidence review; measurement code deleted on either
  terminal outcome.

## Non-Goals

- No mounted blueprint, flop builder output/BSFLIB packer, resident
  lookup, mmap mount, or practice-table change.
- No resident board_size 4/5 acceptance flip (the eight-site change
  is the follow-on production RFC).
- No 3..10-seat work; HU only.
- No potential-aware/implicit, OCHS, learned, or distributional card
  abstraction; only the strictly finer uniform-width EHS grid. If the
  finest tested grid fails G2, that finding is the output.
- No change to any threshold: 0.005 within-abstraction, 0.001
  half-width, and the M2-delegated 0.03 exact threshold are all
  inherited frozen; the M2 milestone text is not modified.

## Current State and Evidence

- **Served artifacts are preflop plus one six-seat flop-terminal
  library.** `engine/src/flop_library/flop_library.cpp` builds
  `TerminalDepth::Flop` games with an `EquityFrontierEvaluator`
  (lines 96, 278, 393-394, 498): turn and river are an equity
  approximation at frontier leaves, not an exact strategy. The
  published `artifacts/flop-library-6way-fullrange` is one limped
  root, 1,755 iso classes / 22,100 concrete flops; the per-class
  `stored_rows` summed over its manifest are 1,931,640,876 and
  `section_bytes` sum to 183,856,428; `library.bin` is 188,432,384
  bytes. (These are the correct figures; an earlier draft wrongly
  cited the seven-seat preflop profile's 12,976,236 rows /
  3,696,590,848 bytes.)
- **The resident path serves board 0/3 only.**
  `engine/src/resident/resident_policy.cpp:125` throws on
  `board_size` other than 0 or 3; guards at lines 821 and 836 require
  board_size 3. The index and mmap key decoders already accept 4/5
  (`resident_index.cpp:50`, `mmap_library.cpp:264`); the refusal is
  in the policy/build layer.
- **Root identity includes the button.** `same_root`
  (`resident_policy.cpp:54-58`) compares player count, pot, big
  blind, AND button in addition to per-seat stacks/contributions/
  folded. The BSFLIB03 header carries one button byte
  (`mmap_library.hpp:23`) and the flop builder hardcodes
  `def.button = 0` (`flop_library.cpp:82,273`), while the practice
  table rotates the button every hand (`practice_table.cpp`). A root
  census that omits the button undercounts production roots.
- **The frozen heads-up training abstraction is uncapped (cap 0) at
  every depth; serving diverges to cap 1.** `kTrainingCap`
  (`abstraction.cpp:537-545`, RFC 0037) is {0,0} for 2 players and
  {1,1} for 3+, and `production_flop_resolve_cap`
  (`abstraction.cpp:565-575`) is the single documented
  serving/training divergence: two-live standard board-3 serving
  stays at one raise because the cap-0 flop tree exceeded the 1 GiB
  builder byte cap. There is no heads-up production flop library and
  therefore no HU postflop cap "already in production"; the cap-0 /
  cap-1 choice is an unresolved M2 design fork this census measures.
- **River-terminal training exists generically.** `train_nseat`
  trains chance-resolved River-terminal trees and streets(1) turn
  trees with a frontier evaluator, with BSWARM03 warm start; RFC
  0044 exercised both and deleted only the EHS card kind and the
  benchmark arms, not this capability.
- **RFC 0044 HU W1 numbers (summed BB over two live seats).** exact
  0.2240/0.1398/0.0911/0.0675/0.0555/0.0519/0.0484/0.0459 across
  100k..300M; bucket 0.004856 at 100M and 0.002717 at 300M; tiers
  incumbent ~0.314; evidence byte-pinned in
  `docs/plans/ablation/rfc0044-stage3-verdict.json` and independently
  re-derived in the RFC 0044 evidence review.
- **Full-range deal universe (each live seat is dealt a TWO-CARD
  hand — `collect_joint_deals` recurses over `std::array<int,2>` per
  seat and rejects card collisions,
  `subgame_nash_conv.cpp:879-913`; the W1 CSV joint_deals=38 on the
  river / 70 on the turn corroborates the two-card model). With the
  uniform 1,326-combo range:**

  - fixed chance-free RIVER board (5 public cards):
    C(47,2) x C(45,2) = **1,070,190** ordered disjoint hand pairs, no
    remaining chance — exactly the EHS flop term count scaled by the
    990 opponent-river-holdings universe;
  - fixed TURN board (4 public cards): C(48,2) x C(46,2) =
    **1,167,480** hand pairs; under streets(1) the unresolved river is
    taken by the frontier equity evaluator so there is no runout
    factor, while a fully chance-resolved turn multiplies by the 44
    river cards: **51,369,120**;
  - FLOP-rooted River-terminal (3 public cards): C(49,2) x C(47,2) x
    C(45,2) = **1,258,543,440** hand-pair/runout combinations.

  C(45,2) is 990 (1,081 is C(47,2)); the earlier draft undercounted
  all three universes by treating each seat's holding as a single
  card. The full-range river walk (~1.07M deals) is therefore NOT
  second-scale; its actual wall is measured once in the Stage-1 smoke
  and pinned in the freeze addendum before the ladder fan-out, and the
  addendum fixes which rungs pay for full-range exact walks.
- **The EHS machinery existed and was deleted under RFC 0044 AC6**;
  this RFC re-introduces it and extends the grid, carrying over the
  0-ULP pinned-order oracle, the term counts (990 / 45,540 /
  1,070,190), and the full-range mean-E_h = 0.5 symmetry invariants
  (the turn/flop per-runout hero scoring and its permanent symmetry
  tests are restored verbatim).

## Design Principles

- Measure before building; grid and roots are selected mechanically,
  never tuned after blueprint costs are sunk.
- Measurement-only; production invariants (default card kind,
  flop-only served artifacts, board 0/3 resident gate) stay intact
  until a separately reviewed production RFC.
- Worst-texture, full-range qualification: a grid mounts only if it
  clears the exact threshold on EVERY frozen board stratum over the
  full 1,326-combo range, not on a fixture mean.
- Smallest grid that clears; the threshold is inherited from the M2
  delegation and is never re-derived here.
- One pinned evaluator and CSV contract; independent training seeds
  with independent sampled-deal batches; no post-hoc grid winner
  picking.
- Every cell is independently resumable and reversible (BSWARM03,
  process shards, OMP_NUM_THREADS=1).

## Proposed Design

### 1. Measurement card grid (re-introduced + extended)

Re-introduce `CardBucketKind::EquityStrengthV2` using a FRESH enum
ordinal (5; the deleted RFC 0044 value 4 and the new value 5 are both
permanently retired and never reused), with the RFC 0044 conditional
exact EHS enumerator: win+0.5-chop, pinned ascending
opponent/runout order, one final division, per-runout seven-card hero
scoring on turn/flop, and live-seat dense id windows
`169 + (L-2)*(K_flop+K_turn+K_river) + street_offset + bin`.

Tested points, all predeclared in the stage freeze before any cell
above the smoke rung runs:

- primary axis K_river in {64, 128, 256} with K_turn=64, K_flop=32;
- a single conditional secondary arm K_turn=128 (paired with
  K_river=256) whose cells, seeds, and rungs are declared in the SAME
  freeze, run only if K_river=256/K_turn=64 fails G2;
- K_flop fixed at 32: flop roots are Flop-terminal in this
  measurement and do not enter the exact gate.

The maximum window 32+128+256 = 416 pins the dense id ceiling
`169 + 9*416 = 3,913` over L=2..10. The three-arg `card_bucket`
throws on the new kind; it is reachable only through the measurement
live-seat overloads, the trainer seam, and the benchmark arm. Every
exhaustive kind switch must fail closed or no-op for the new
ordinal, including at minimum `card_abstraction_id`
(`abstraction.cpp`), `bucket_in_range` (`mmap_library.cpp`), and the
checkpoint codec; the stage6 offline source-policy edge guard and
production build/pack/serve paths do not gain a case. The measurement
TU links only into the measurement benchmark (CMake scoping as in
RFC 0044; `-ffp-contract=off` on the TU and its oracle ctest).

### 2. Board-texture strata and full-range roots (G2)

The gate covers BOTH axes, not just the range:

- **W1 anchor**: river `2c3d7hJs9c` and turn `2c3d7hJs` with the W1
  ten-hand range (regression to RFC 0044) AND with the full
  1,326-combo range.
- **Frozen board-texture strata (full 1,326 range on each)**: the
  four canonical flop classes, named with the exact frozen RFC 0043/
  0044 spellings — `7sKhAd` unpaired rainbow A-high, `7sKhKd` paired,
  `QsKsAs` monotone, `7sKhAs` unpaired two-tone (the earlier
  `AsKd7c`/`KsKd7c`/`AsKsQs`/`AsKh7s` spellings are the same
  suit-isomorphism classes, independently re-verified by the round-2
  reviewer) — each completed by the frozen W1 turn/river `Js` /
  `Js9c`, plus one raised-root stratum chosen by the deterministic
  rule below. Exact gaps are evaluated on the chance-free river
  completion; turn completions stay descriptive.
- **Raised-root stratum (anti-peek selection).** Because the
  instrument freeze is reviewed before the census runs, the raised
  root cannot be chosen after seeing census results. The freeze pins
  the deterministic function: among identity-complete raised-root
  tuples in the G3 output (ordered by the census key), take the one
  with the smallest pot, and use the lexicographically smallest
  non-blocking river card completing its board; if G3 finds no raised
  root, the stratum is recorded as absent rather than substituted.
- The selection gate is the WORST observed full-range exact gap over
  all strata, not a mean; one stratum above 0.03 fails the grid.

### 3. Exact vs sampled NashConv and the river/turn split

The 0.03 exact GATE is anchored on the chance-free RIVER only, exactly
as in RFC 0044: the river tree has no chance nodes so the
bucket-constrained best response and the exact-hole gap are both
defined, while a turn tree has chance nodes for which no bucket BR is
defined. Turn cells therefore keep the RFC 0044 descriptive role:
streets(1) with the equity frontier at unresolved leaves, exact over
the 1,167,480 hand pairs at the frontier boundary, and they do NOT
qualify or fail a grid.

- The chance-free full-range RIVER walk (1,070,190 disjoint hand
  pairs) is EXACT; sampling is prohibited for the gating metric. To
  bound cost, full-range exact river walks run only at the candidate
  gate rung and at 300M (the two points the decision uses), not at
  every ladder rung; the Stage-1 smoke measures one such walk and the
  addendum pins its wall and a per-process time budget.
- A fully chance-resolved TURN (51,369,120) and every FLOP-rooted
  River-terminal cell (up to 1,258,543,440) may use the sampled
  estimator; they never gate the grid. The frontier-turn (1,167,480)
  stays exact at its leaves. The sampled estimator's structural
  contract is fixed here; the freeze addendum pins only the deal
  count and time budget:
  - frame: uniform over ordered disjoint TWO-CARD-HAND pair
    assignments conditioned on the fixed board; every sampled deal
    enumerates the opponent's two-card holding and every runout
    exactly inside the deal;
  - the sampled BR is an exact best response over the sampled set;
  - aggregation weights and the per-deal normalizer (summed over the
    two live seats / big blind) are identical to the exact walker;
  - each TRAINING seed uses an INDEPENDENT sampled-deal batch, so
    sampling error is an independent per-seat error that enters the
    half-width rather than a shared bias that would not;
  - the fixed RNG, deal-count, and stratum seeds are frozen;
  - exact-vs-sampled agreement is an evidence gate: on W1 (exact) and
    on at least one full fixed board the sampled estimate at the
    frozen deal count must lie within a declared tolerance of the
    exact value before sampled cells are reported.

### 4. Instrument campaign (G2)

Schemes: the nine-tier incumbent control and each tested EHS grid.
Rungs: reduced ladder {1M, 10M, 100M, 300M} with BSWARM03 resume and
the RFC 0044 21,600 s per-segment guard; the RFC 0044 tail-fit rules
(prior-window prediction, tail residual/non-increasing) are retained.
With only four rungs the earliest rung that satisfies the rule-4
prior-window requirement (fit on i-3..i-1) is index 3 = 300M, so the
gate rung is effectively 300M; if 300M fails the prior-window or any
noise/distance rule, the result is directly no-grid for that ladder
(the campaign may extend the ladder in a later freeze, it may not
weaken the rules). The gate rung requires, on every texture stratum,
bucket gap <= 0.005 and half-width <= 0.001; exact qualification is
then evaluated there. Full-range exact river walks run at the gate
rung and 300M.

Five independent training seeds on W1 (half-width multiplier
t_0.975,4 = 2.7764); THREE independent seeds on the expensive
full-range cells, each with its own independent sampled-deal batch,
using t_0.975,2 = 4.303 for the half-width — the multiplier is part
of the frozen contract and the 0.001 bar applies with the correct
degrees of freedom for each cell type.

Selection: smallest K_river whose gate rung has worst-stratum
full-range exact gap <= 0.03 and, against the nine-tier incumbent,
every seed strictly better at the same seed (the RFC 0044 paired
superiority sub-rule is retained). If K_river=256 fails, run the
predeclared K_turn=128 arm; if that also fails, record
`instrument-inadequate-uniform-ehs` with the achieved per-stratum
floors and stop — no larger grid or other abstraction is added.

### 5. Reachable-root census (G3)

A census benchmark mode builds the HU 100 BB cap-0 preflop abstract
tree, walks every terminal sequence that enters the flop, and emits
distinct root tuples carrying the FULL identity used by
`same_root`/`RootDirectoryEntry`: player count, big blind, pot,
button, per-seat stacks, per-seat contributions, folded mask, live
count, and the line tag (Limped vs the other `RootLineTag` values).
Each distinct tuple is reported with the number of preflop action
sequences mapping to it; button is enumerated explicitly (a
future single-button mirroring claim must be proven and pinned by
the production RFC, not assumed here). The census also enumerates
the all-in-no-flop lines (no postflop decision; covered by the
preflop artifact's exact-equity-all-in-flop frontier) and states
their disposition separately so milestone acceptance item 1's
"every reachable shape" is closed.

For a stratified sample of MULTIPLE tuples per pot-stratum (at least
three where the census contains that many; the strata and tuple-count
rule are fixed in the addendum) the census builds the River-terminal
tree under BOTH abstractions and reports nodes/infosets/bytes:

- frozen HU training cap 0 (the primary production-training object);
- serving cap 1 (the comparator), explicitly labeled as the unresolved
  RFC 0037 fork the production RFC must adjudicate;

and for one flop class per sampled root reports sealed rows and
seconds-to-target measured by running to the target (not a
short-window extrapolation), so the cap-0 feasibility against the
tree/byte caps is an observation, not an assumption.

### 6. Full-M2 envelope and verdict (G4)

From G2 (grid; per-stratum per-class infosets/rows/seconds/bytes) and
G3 (identity-complete root counts, all-in lines split out), report
total cells `roots x 1,755 classes`, per-stratum MAXIMUM and the
range of per-class seconds/rows/bytes, total core-hours, projected
wall using the available process fan-out on n37-100-184, and
artifact bytes under the current row encoding, separately for cap 0
and cap 1. G4 emits exactly one instrument verdict
(`uniform-ehs-viable` with the selected grid, or
`needs-richer-abstraction` / `needs-continuation-lineage` with
measured floors) plus the envelope numbers. The build go/no-go
(spending, format choice) is a separate authorization this RFC does
not grant.

### 7. Evaluator and evidence

A new frozen TS evaluator `tools/rfc/rfc0045-gates.ts` (RFC 0044's
evaluator stays unchanged) reads the G2 CSV, recomputes the
independent-seed means/half-widths with per-seed deal-batch
independence, applies the worst-stratum 0.005/0.001/0.03 predicate
and the mechanical rung selector, and emits the JSON verdict of
record; it also ingests the G3 census output and renders the G4
envelope. Raw CSVs and the census commit under
`docs/plans/ablation/`; the measurement engine code is deleted after
the independent evidence review, evaluator/evidence/census retained.

## Dependency Rules

- Measurement code may depend on `bigshark_abstraction`,
  `bigshark_solver`, and existing NashConv walkers; it never links a
  serve path, the engine host, pack/publish, or a resident mount.
- The production default card kind and resident board 0/3 gate are
  untouched; no non-measurement caller constructs the new kind.
- No measurement output reaches serving code.

## Compatibility and Migration

No persisted production format change. The measurement checkpoint
uses the fresh card-kind ordinal 5 with an additive codec case and a
ceiling matching the 3,913-id maximum; older readers fail closed on
the unknown byte. Stage-4 deletion removes the ordinal, seams, arms,
codec case, and ctests; the evaluator remains.

## Security and Operational Impact

Offline single-host compute only; no network, credentials, platform,
or protocol. Process shards with OMP_NUM_THREADS=1 and BSWARM03
mode-isolated checkpoint names, summed sidecar walls, fail-closed
preflight. Full-range exact and flop-rooted sampled cells are larger
than W1 cells; per-process memory and the host envelope are measured
before the grid fan-out is frozen.

## Alternatives Considered

1. **Build the HU blueprint now at 64 river bins.** Rejected: W1
   already fails the delegated 0.03 exact threshold (0.0459), 94% of
   the residual is river merge loss, and the failure gets more likely
   on full textures; discovering it after building River-terminal
   libraries is the expensive outcome.
2. **Real-time endgame solving instead of a blueprint.** Excluded by
   the milestone itself (every in-scope shape served from a mounted
   artifact, zero real-time-approximation receipts); a later product
   may revisit it.
3. **Jump to a richer learned/potential/OCHS abstraction.** Larger
   and unjustified until the cheap monotone lever (more chance-free
   river bins across textures) is measured.
4. **Enumerate and build every root before measuring the
   instrument.** Couples the irreversible build cost to a question a
   few dozen cells answer.
5. **Collapse the two buttons by assumption.** Rejected: `same_root`
   and the BSFLIB03 header include button; mirror equivalence must be
   proven in the production mount RFC, not silently assumed by a
   census that feeds a root count.

## Risks

- K_river=256 (and K_turn=128) may not reach 0.03 on some texture
  because scalar EHS cannot separate equal-strength hands with
  different blockers; this is the `needs-richer-abstraction` result
  and is reported, not patched by lowering the bar.
- The exact frontier-turn walk covers ~1,167,480 hand pairs and the
  chance-resolved turn ~51,369,120 (sampling-permitted, never
  gating); their real cost and flop-rooted sampled variance may be
  higher than estimated, so the exact-vs-sampled gate and per-seed
  independent batches keep the noise honest, and any half-width
  relaxation needs an independent amendment.
- Cap-0 HU River-terminal trees may be too large for the builder;
  the dual-abstraction census makes the fork visible before design.
- The identity-complete root count may be much larger than the single
  limped-root assumption; G4 surfaces it as a reviewed scope
  decision.
- Finer grids grow rows/bytes; the per-stratum maxima make the
  tradeoff explicit.

## Open Questions

- Do the two HU buttons map to one mirror-image root for serving
  (proven in the production RFC), or are both trained?
- Is the frozen sampled-deal count sufficient for 0.001 half-widths
  on flop-rooted cells, or are those roots disqualified/expensive?
- The K_river vs information-set growth curve before the exact gap
  crosses 0.03.

## Acceptance Criteria

- AC1: measurement EHS enumerator with configurable K_river/K_turn,
  0-ULP pinned-order oracle, term counts, 3,913-id band table, memo,
  full-range mean-0.5 symmetry ctests on all three streets, frozen
  parameter digests; the production default kind digest is unchanged
  and every exhaustive kind switch fails closed on ordinal 5.
- AC2: instrument CSV and estimator contract frozen in a pre-run
  addendum (texture strata incl. the deterministic raised-root
  selection function, grids incl. the conditional K_turn=128 arm,
  rungs, the t multipliers 2.7764/4.303, independent deal batches,
  exact/sample boundary, the measured full-range river walk wall and
  time budget, resume) BEFORE any full-range cell above the 1M smoke
  rung runs; the addendum is independently reviewed; any half-width
  threshold change needs a new approval. The G3 structural census (root
  tuples, no strategy quality data) runs first to supply the
  raised-root domain; the addendum review is sequenced after it but
  before any G2 full-range/training cell.
- AC3: the G2 campaign returns the smallest tested grid meeting, at
  the chance-free RIVER gate rung (effectively 300M under the four-
  rung ladder's prior-window rule), bucket <= 0.005 and half-width
  <= 0.001 with the correct t multiplier, worst-stratum full-range
  exact <= 0.03, with per-seed paired superiority over tiers, OR a
  recorded `instrument-inadequate-uniform-ehs` floor per stratum;
  turns stay descriptive; the exact-vs-sampled agreement gate passes
  before sampled cells are reported; the verdict is mechanical and
  independently evidence-reviewed.
- AC4: the G3 census returns the identity-complete ordered root
  table (incl. button, BB, line tag), per-tuple sequence counts, the
  all-in-no-flop line disposition, the frozen pot-strata with at
  least three tuples per stratum where available, and cap-0/cap-1
  River-terminal node/row/run-to-target-seconds samples.
- AC5: G4 delivers the costed per-stratum envelope, both caps, and a
  single explicit instrument verdict; build authorization is out of
  scope and stated as such.
- AC6: on either terminal outcome all measurement engine code is
  deleted; the evaluator, CSVs, census, and the RFC are retained.

## Rollback Plan

Delete the measurement kind, seams, driver/census arms, codec case,
and ctests; retain the evaluator, evidence, census, and RFC. No
production artifact references the measurement kind.

## Verification Plan

Release + asan ctest, `npm run check`, proto check, both doc/RFC
checkers; independent review of the freeze addendum and of the G2/G3/
G4 evidence before any production RFC consumes a number.

## Rollout Plan

1. Independent approval of this RFC.
2. Re-implement/extend measurement EHS, evaluator, oracle/symmetry
   ctests; W1 regression against RFC 0044.
3. G3 structural root census (no quality data; fast) plus one
   measured full-range river exact walk for the smoke wall.
4. Pre-run freeze addendum pinning the G3-derived raised-root
   stratum, the measured walk wall/time, grids, rungs, seeds, t
   multipliers, deal batches; independent review.
5. G2 instrument campaign across texture strata; G4 envelope.
6. Independent evidence review; record the instrument verdict.
7. Cleanup; hand verdict and census to the blueprint/mount
   production RFC.

## Decision

Author agent: Claude (zzzode agent session, 2026-11-01/02)
Approved by: independent approval agent a7a6f1fb17506c447
Decision date: 2026-11-02
Review outcome: Approved
Reviewed scope: three independent review rounds over the complete RFC,
the cited engine sources (abstraction cap table, resident same_root and
board gate, BSFLIB03 header, joint-deal enumeration), the RFC 0044
verdict/CSV constants, and both structural checkers. Round 3
independently recomputed the four two-card-hand deal universes
(1,070,190 / 1,167,480 / 51,369,120 / 1,258,543,440), verified the
chance-free-river-only exact gate, the river/turn role split, the
2.7764 (S=5) / 4.303 (S=3) t multipliers, the four-rung/300M
eligibility consequence, the deterministic raised-root selector, the
multi-tuple census, and the measurement-only fail-closed boundary
(fresh ordinal 5, card_abstraction_id / bucket_in_range / codec
switches fail closed, no production build/pack/serve reach).
Review summary: round 1 returned Changes Requested with findings B1-B6
(mis-cited six-seat library evidence; 0.05/0.03 threshold framing; HU
cap-0-training vs cap-1-serving fork; missing board-texture axis;
under-specified estimator and wrong deal universe; button omitted from
root identity), all resolved in round 2; round 2 found B5-residual
(the universe still treated each holding as a single card) and added
freeze refinements, all resolved in round 3; round 3 verdict Approved
with two approval-conditioned factual errata (a stale "~99k
combinations" turn-cost line and this record), both fixed when
recording the decision. Authorization covers only the Stage-1
measurement-machinery rebuild, one measured full-range river exact
walk for the smoke wall, and the G3 structural census; no full-range
cell above the 1M smoke rung runs until the pre-run addendum is
independently reviewed; the approval grants no production
format/build/spending decision. The reviewer's non-blocking addendum
notes (the full 5-card raised-root board algorithm, gate-rung==300M
under the four-rung ladder, EHS factor wording, smoke gap numbers not
entering G2 selection) are inputs to the addendum review.

## Implementation Outcome (2026-11-04)

Stage status: Stages 1-4 complete; Stage 5 (measurement code removal) in
progress. Full narrative, both independent review records and both
addendum approvals are in `docs/plans/0045-implementation.md` and
`docs/plans/0045-addendum-stage3-freeze.md` /
`docs/plans/0045-addendum-stage4-rerun.md`.

A first G2 campaign was independently invalidated (a measurement-driver
resume defect double-counted cumulative iterations; all 300M cells were
short). The driver was fixed with a native bitwise-identity regression,
the exact-walk re-run rule was made mechanical and fail-closed, and the
walk band was corrected to key by (scheme, betting root). The second,
368/368-cell campaign was independently reviewed **VALID** on
2026-11-04, and its terminal instrument verdict is:

**`instrument-inadequate-uniform-ehs`**

At the valid 300M gate rung (n=3, t=4.303; all half-widths <=0.001 BB),
uniform EHS banding passes the exact-hole QUAL bar (<=0.03 BB) at
k_river 128/256 (worst exact 0.02241 / 0.01895) but fails the
within-abstraction CERT bar (<=0.005 BB) at every frozen grid: worst
bucket 0.01240 / 0.01531 / 0.01594 for k_river 64/128/256,
non-decreasing as k_river grows (95% lower bounds >=2.4x over the bar).
The tiers incumbent inverts the two gates (exact 0.393 fail, bucket
0.0036 pass). EHS is strictly better than tiers per seed (54/54). The
conditional (32,128,256) arm is river-inert and was not run. W1 bucket
0.002768 reproduces clean RFC 0044. G3 established that the full cap-0
HU 100 BB River-terminal tree is structurally infeasible (>100M nodes);
the G4 envelope is therefore cap-1/depth-limited only.

Implication for M2: a uniform EHS postflop banding is not sufficient for
full-street HU; the separately authorized production blueprint/mount RFC
must use a richer abstraction (depth-limited/nested continuation value
or non-uniform bucketing) over the bounded cap-1 object, with the
G3/G4 census sizing its cost. No production format, build, mount, or
spending decision is made by this RFC.
