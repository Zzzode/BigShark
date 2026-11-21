---
rfc: "0042"
subject: "Equity-Strength Postflop Card Bucketing (W5 Abstraction Refinement)"
status: "Implemented"
authors: "Claude (zzzode agent session 2026-10-30)"
created: "2026-10-30"
updated: "2026-10-30"
owners: "Engine abstraction (new declared CardBucketKind + pinned digest + live-seat card_bucket API), engine solver (measurement-only card-kind seam in NSeatTrainer/NSeatPolicy), engine benchmarks (bucket/exact BR ablation arm). No build/pack path, serve path, production default, or mounted-artifact change in this RFC — those belong to a follow-on flip RFC on Adopt"
supersedes: ""
superseded-by: ""
---

# RFC 0042: Equity-Strength Postflop Card Bucketing (W5 Abstraction Refinement)

## Summary

The postflop policy is keyed by nine made-hand categories
(`CategoryTiersV1`, reached in production through `Preflop169`'s
postflop fallback). The 2026-10-29 10-second recalibration
(`docs/plans/ablation/rfc0042-m1-10s-recalibration.csv`) measured that at
2 live seats the river policy is converged WITHIN its abstraction
(bucket-constrained BR sum 0.032 BB) while the exact-hole BR sum is
0.335 BB: roughly 0.30 BB of unavoidable loss is the card abstraction,
not solver noise or the time budget. RFC 0031 already drew this
distinction and built a 36-value intermediate (`TiersDrawV1`), but its
Stage 3 adoption gate was never passed and production stayed at nine
tiers.

This RFC proposes and gates a deterministic board-relative **equity-
strength bucket family**, `EquityStrengthV1`: each holding's exact
win/tie probability versus the uniform live-opponent range, computed by
exact conditional enumeration of card-disjoint opponent holdings and
runouts on river and turn and, by default, exact enumeration on the
flop (a frozen fixed-seed Monte Carlo stream is a conditional fallback
only if the measured exact-table build wall exceeds a predeclared
budget), quantized into K equal-width bins per street. The bucket
function is declared and frozen before any gated run and fits `uint16`.

This RFC is deliberately MEASUREMENT-ONLY: it adds the declared
function, a trainer measurement seam (an in-memory kind selector used
by the ablation benchmark only), and the ablation arm itself, then
runs a predeclared paired interval ablation against `CategoryTiersV1`
and `TiersDrawV1` on the shapes the solver converges. No outcome flips
production, touches the build/pack or serve path, or rebuilds mounted
artifacts here — in particular NO library-builder kind override is
added, because a partial write-path change could stamp a new-id
artifact whose header still claims the compiled-default kind (Section
2 records the full stamp-site inventory for the follow-on). On Adopt a
separate follow-on flip RFC must specify the serve-path equity table,
the full replacement artifact campaign, and one coordinated flag day
(mmap loading fail-closes on kind mismatch, so no overlap is
possible). On Keep/Inconclusive the seam, enum, and ablation arm are
deleted and production stays at nine tiers — the RFC 0031
measure-before-change discipline.

## Motivation

- Nine categories merge every one-pair hand with every other one-pair
  hand regardless of kickers/board texture. On the river made strength
  IS showdown value, so two holdings in one category can hold 0.05 and
  0.95 equity while the policy row forces one action distribution for
  both. The bucket floor is the dominant exact-game loss at every seat
  count measured (W1: 2-seat river exact 0.335 vs bucket 0.032;
  3-seat ~0.57 of 0.71 attributable to abstraction).
- Finer bucketing is also the only measured lever that can let 4-live
  shapes converge in the wall budget: information-set rows are shared
  across within-bucket combos, and a correct strength ordering removes
  the policy error the solver cannot iterate away.
- The previously explored neural continuation (RFC 0041) failed for an
  unrelated reason (deal-sampling label variance on flush boards); this
  proposal contains no learned component, no Python, and no online
  model — it is a classical declared card abstraction of the kind the
  engine already ships for preflop (`Preflop169`).

## Goals

1. Add one declared, deterministic, board-relative bucket kind
   `EquityStrengthV1` with a live-seat-aware API and a pinned parameter
   grid, plus a measurement-only runtime kind selector in the n-seat
   trainer used solely by the ablation benchmark (mirroring the
   existing stage6 `--card-kind` measurement precedent, not the build
   path).
2. Decide adoption against predeclared, mechanically evaluable gates
   using the existing exact-hole and bucket-constrained BR machinery on
   converged shapes (2 and 3 live seats, river and turn), with storage
   and wall costs recorded.
3. Keep the production scheme, build/pack path, serve path, and mounted
   artifacts byte-identical in this RFC regardless of the gate outcome.
4. Produce a gate decision a SEPARATE implementation RFC (the "follow-
   on flip RFC", not pre-approved here) consumes: on Adopt it must
   specify the complete build-side stamp/kind threading, the serve-path
   equity table/cache, the coordinated artifact flag day, and the
   production flip; on Keep/Inconclusive the measurement seam and the
   new enum are deleted per the no-legacy rule.

## Non-Goals

- No production kind flip, no build/pack/checkpoint/manifest/header
  kind change, no serve-path/resolver/resident/seat-policy bucket
  computation, no mounted-artifact change, no dual-kind reader in this
  RFC. The complete inventory of build and serve sites the flip needs
  is listed in Section 2; all belong to the follow-on flip RFC and
  require their own review.
- No library-builder `--card-kind` override and no new-kind BSFLIB
  section or sealed checkpoint is produced; Stage 3 evaluates
  in-memory policies only.
- No learned/neural embeddings or opponent-model-dependent buckets; the
  bucket is a pure function of (hole, board, live seat count) and uses
  uniform opponent ranges.
- No change to action abstraction, tree geometry, stake profile, or
  seat-count scope.
- No revival of RFC 0041 (unrelated failure); no online model.
- No claim about 4+-live-seat wall convergence; those shapes remain on
  offline-blueprint coverage per the post-0041 plan.
- The measurement driver DOES compute equity during training and
  evaluation. Any "no equity enumeration at serve time" guarantee is a
  property the follow-on RFC must design and gate, not asserted here.

## Current State and Evidence

- Production postflop bucketing is nine made-hand categories;
  `kNSeatCardKind = Preflop169` falls back to `CategoryTiersV1`
  postflop (`engine/include/bs/nseat_trainer.hpp:34-35`,
  `engine/src/abstraction/abstraction.cpp`). The kind is a compile-time
  constant at the trainer's three card_bucket call sites: `walk_action`
  (`nseat_trainer.cpp:497-498`), `walk_average`
  (`nseat_trainer.cpp:699-700`), and the public query method
  `NSeatPolicy::lookup` (`nseat_trainer.cpp:1173-1192`, bucket at
  `:1189`) — not a debug walk; the latter's only non-test caller is the
  stage6 measured policy (`engine/src/stage6/measured_policy.cpp:68`).
- `TiersDrawV1` (category x two-bit draw code, 36 dense values) is fully
  implemented in `card_bucket`/`abstraction.cpp` and unit-tested, but no
  SHIPPED binding selects it (`kNSeatCardKind` stays Preflop169). The
  measurement precedent exists in the stage6 measurement driver CLI
  `--card-kind {tiers|tiers-draw}`
  (`engine/benchmarks/stage6_measurement_driver.cpp:586`; the id->kind
  mapping it parses is `engine/src/stage6/candidate_policy.cpp:42-51`),
  but nothing mounted uses it. RFC 0031's Stage 3 rule returned KEEP for n=2 (negative
  improvement, overlapping intervals) and Inconclusive for n=3 at
  1,000 iterations.
- Identifiers are `uint16` in mmap libraries; the production postflop
  id window is 1..9 (`bucket_in_range` already branches for the
  unmounted TiersDrawV1 0..35 case, but its other stamp/read sites
  were never threaded); BSFLIB02/03 header byte 20 pins the kind and
  `open()` refuses mismatches against the compiled default (RFC 0031
  Current State). Any new scheme must fit `uint16` (<65,536) and
  travel with a new kind id and digest.
- W1 evidence (this date, `rfc0042-m1-10s-recalibration.csv`): at
  10,000 ms, 2-seat river 0.335 exact / 0.032 bucket (three seeds
  0.332-0.339 / 0.030-0.034); 3-seat river 0.710 / 0.139; turn bucket
  split is not measurable on chance nodes and the exact 2-seat turn sum
  is 0.306, near RFC 0039's 0.259-0.295 leaf floor (different fixture).
- Exact showdown evaluation is deterministic and available
  (`bs/eval.hpp`, the bitmask 5-7 card evaluator). Equity vs a uniform
  opponent range enumerates disjoint opponent holdings and conditional
  runouts; `bs/equity.hpp` provides a seeded Monte Carlo reference. The
  joint enumeration sizes are derived below (M1-corrected).
- The experimental `MultiStreetSolver` OCHS code
  (`engine/src/gto/multistreet_cfr.cpp:89-130`) is compiled into the
  engine library but is never called from `card_bucket` or any
  production binding; this RFC does not link it into the path.

## Design Principles

- **Declared, deterministic, cheap to verify.** The bucket function is
  a pure deterministic function any consumer can recompute; no
  training-data dependency, no RNG except an explicitly pinned
  fixed-seed stream in the conditional flop fallback.
- **Abstraction changes are digest-pinned.** New kind, new
  `AbstractionId`, new freeze timestamp; every artifact that stores
  rows carries the kind and refuses mismatches, exactly as today.
- **Measure before touching production.** The runtime kind selector is
  added as a measurement seam; `kNSeatCardKind` changes only after the
  predeclared gates pass.
- **One variable ablated.** Same action abstraction, same ranges, same
  fixture boards, same iteration wall across schemes. E_h itself is
  scheme-independent: the per-board per-holding equity table is
  computed once and memoized (freeze item 8), so gate 4 compares
  convergence, not repeated enumeration.

## Proposed Design

### 1. Bucket function `EquityStrengthV1`

For a holding `h`, board `b` (3/4/5 cards), and `L` live seats, the
enumeration is over the CONDITIONAL, card-disjoint joint space: first
the opponent holding `o` (two cards disjoint from `h U b`), then runout
cards disjoint from `h U b U o`. Every term is exact showdown value
under the bitmask evaluator (win + 0.5 chop):

```text
opp(b,h;street) = C(D,2) two-card holdings not blocking h U b,
                 where D = 52 - (|b| + 2):
                 river D=45 -> 990, turn D=46 -> 1,035, flop D=47 -> 1,081
run(o;street)   = river 1, turn 44 river cards, flop C(45,2)=990
                 two-card runouts disjoint from h U b U o
E_h = sum_{o, r} [P(h wins) + 0.5 P(chop)] / (|opp| * |run|)
```

Note the flop runout count is 990 conditional on the dealt opponent
holding, not C(47,2)=1,081 (that raw count includes impossible
dealings that share cards with `o`); enumerating `(o,r)` pairs over
the disjoint space gives the same result as enumerating runouts first
and non-conflicting opponents after. Enumeration ORDER is pinned so
two implementations agree bit-for-bit: cards use the engine's
canonical encoding `id = rank*4 + suit`, rank 0..12 (2..A),
suit 0..3 (s,h,d,c) (`eval.hpp:2`); opponent holdings iterate in
ascending lexicographic (c1,c2) order, runouts iterate afterward in
ascending order, and E_h accumulates in that single nested order with
no reordering or parallel reduction. The ctest oracle
(independently hand-written, frozen fixture set) must reproduce
IDENTICAL doubles (exact equality, no tolerance) and identical bin
assignments; any mismatch fails the test rather than being excused as
rounding. The oracle and the implementation are compiled in the same
test binary with floating-point contraction disabled for these
translation units, so 0-ULP equality under the pinned operation order
is the expected result; if a reference toolchain cannot honor that,
the compiler and flags are recorded in the Stage 2 addendum rather
than the assertion being loosened.

Each `(o, r)` term evaluates BOTH hands (hero and opponent) with the
7-card bitmask evaluator:

- River (5 board cards): 990 terms per holding (~2k evaluator
  invocations); exact.
- Turn (4 board cards): 1,035 opponents x 44 rivers = 45,540 terms
  per holding (~91k invocations); exact.
- Flop (3 board cards): 1,081 opponents x 990 conditional runouts =
  1,070,190 terms per holding (~2.14M invocations). After fixing a
  flop board, the number of hero holdings that do not block it is
  C(49,2) = 1,176 — NOT 1,081 (that is the opponent count for a fixed
  hero holding). The full per-board table is therefore
  1,176 x 1,070,190 ~= 1.259B terms (~2.52B evaluator invocations).
  No Stage 3 cell uses flop buckets (all four gated shapes are
  turn/river); Stage 2 builds the full table ONCE on one frozen
  fixture board solely to record the reference-host wall as
  feasibility evidence for the follow-on flip RFC. The declared
  default for any flop table is EXACT enumeration; a fixed-seed
  XorShift64 stream over the same disjoint `(o,r)` space is permitted
  only under the frozen build-budget threshold and error predicate
  (freeze items 5 and 7), and whether it ever fires is decided by the
  follow-on build, not this RFC.
- Multiway: E_h is the standard two-player EHS quantity against one
  uniform opponent range. In v1 the enumeration sees only h, b, and the
  undealt deck — folded cards are unseen by the bucket — so E_h is
  numerically INDEPENDENT of L; L solely selects the dense id window
  `base(street, L)`. Passing the wrong L therefore yields the correct
  bin in the wrong id window (and is caught by the wrong-L coverage
  ctest), never a different equity. Multi-way interaction is not
  modeled in the BUCKET (known simplification R4; the n-way CFR
  solution still carries it).

Dense id per street and live-seat band (the exact offset table is a
Stage 2 freeze artifact):

```text
bucket = base(street, L) + min(K_street - 1, floor(K_street * E_h))
```

`base` windows are tabulated for the unchanged preflop Preflop169 ids
(0..168) and for flop/turn/river at EVERY live-seat band L = 2..10,
even though Stage 3 exercises only turn/river at L = 2,3 (gated cells)
plus turn/river at L = 4..6 (descriptive projection rows), and flop
ids are oracle-tested only. Freezing the full L range costs nothing —
the maximum id is 169 + 9*(32+64+64) - 1 = 1,608 << 65,536 — and
prevents an id-window re-freeze at the follow-on campaign. The
complete table and its maximum-id ctest are frozen before Stage 3.

Primary grid point (the ONLY point eligible for the adoption decision
in this RFC): equal-WIDTH bins with
`K_flop = 32, K_turn = 64, K_river = 64`. Equal-width is primary
because the id reads as a probability band and no per-board quantile
table is carried. `K_flop = 32` is DECLARED/RESERVED for the follow-on
campaign only: no Stage 3 cell buckets a 3-card board (all four gated
shapes and the L=4..6 projection rows are turn/river-rooted, and the
turn frontier is value-based, not card-bucketed), so flop ids are
oracle-tested and wall-probed but never used in a Stage 3 row, and the
Stage 3 report must not present flop-variant rows as evidence.
Consequently the descriptive grid in Stage 3 spans the four DISTINCT
turn/river configurations (K_turn in {32,64}, K_river in {32,64}) at
K_flop=32; the nominal K_flop in {16,32} variants are the same rows
under a parameter that is inert there and are not run twice. These
descriptive rows cannot be adopted without a new freeze and a new
review (prevents post-hoc grid winner picking).
Switching the primary point to equal-mass bins is allowed at Stage 2
freeze only under a predeclared occupancy predicate measured on the
frozen reference deal set (below); after Stage 3 starts the function is
fixed.

Stage 2 freeze artifacts (committed before any Stage 3 row is produced
and recorded as an addendum in this RFC):

1. the exact `(opp,run)` formula above (this section) and its ctest
   fixture set;
2. the complete `base(street,L)` offset table for preflop 0..168 plus
   flop/turn/river at every L = 2..10, and the maximum-id ctest;
3. tie/chop treatment (already pinned by the evaluator: win+0.5 chop)
   and bin-boundary convention (`floor`, upper bin inclusive);
4. the equal-width occupancy predicate and its frozen reference deal
   set: switch to equal-mass only if, on that set, the most populated
   K/4-wide bin holds >5x the least populated bin's holdings for a
   majority of boards (the threshold and set are frozen now, not after
   seeing results);
5. the flop MC predicate IF exact enumeration misses the build budget:
   frozen stream seed/draw count, and the error requirement that on
   the frozen fixture boards/holdings the MC bucket assignment agrees
   with the exact assignment on >= 99.9% of holdings AND max
   |E_MC-E_exact| <= 0.1/K_street (draw count derived from a pilot
   power calculation committed in the addendum; no post-hoc tuning).
   The build-budget THRESHOLD that may trigger the fallback is itself
   frozen in the addendum: an exact per-fixture-board wall ceiling in
   seconds on the named reference host (measured first on the pilot
   power run), so the exact-vs-MC choice is made against a predeclared
   number, not observed results. Whether exact or MC is selected, the
   selection plus stream seed/draw count enter the kind's
   `card_abstraction_id` parameter string and digest — two builds
   differing there have different digests;
6. the ablation fixture scope (W1 boards and 10/12-hand enumerable
   ranges) and the explicit external-validity caveat that adoption is
   decided on those fixtures only, with full-range confirmation
   required in the follow-on flip RFC;
7. the flop full-table build wall (the one frozen fixture-board probe)
   measured on the reference host and compared against the item-5
   threshold;
8. the E_h memoization contract: equity for a `(board, holding)` pair
   is computed at most once per process per frozen parameter set and
   shared by every `card_bucket` visit (`walk_action`, `walk_average`,
   `lookup`) and by the bucket-BR evaluator; the cache is keyed on the
   FULL frozen parameter set (K grid plus exact-vs-MC selection,
   stream seed, and draw count), not merely `(board, holding)`, so
   distinct parameter sets never share entries; turn/river fixture
   holdings populate the cache lazily, and cache lifetime/scope (per
   benchmark process, cleared between scheme runs) is pinned. Gate 4's
   iteration ratio is measured ONLY under this memo, so repeated
   45.5k-term turn enumerations cannot pollute the 10 s wall.

Changing any frozen item invalidates the Stage 3 run.

### 1a. Stage 2 freeze addendum (2026-10-30)

Recorded before any Stage 3 row, on the named reference host
`n37-100-184` (Intel Xeon Platinum 8336C @ 2.30 GHz, 56 cores,
115 GB RAM; GCC 12.2.0, release preset `-O3`; single-threaded
enumeration). The mechanical statistics are computed by
`tools/rfc/rfc0042-gates.ts` (`bin/rfc0042-gates.mjs`): BCa percentile
bootstrap B=10,000 deterministic seed `0x0042004200420042`, paired-t
reported alongside; the script reads the frozen CSV header and is the
sole gate evaluator.

1. Enumeration/oracle: `equity_strength_ehs` in
   `engine/src/abstraction/abstraction.cpp`, ascending opponent pairs
   then ascending conditional runouts, single nested accumulation,
   one final division. The independent oracle ctest
   (`test_abstraction: equity_strength_ehs`) reproduces identical
   doubles and bins; observed 0-ULP in the same release/asan binaries.
   FP contraction is off for the equity enumeration TUs; no tolerance
   assertion exists.
2. Band table: preflop 0..168; postflop
   `169 + (L-2)*160 + {flop 0, turn K_flop, turn+K_turn}`, L=2..10,
   K=(32,64,64); maximum id 1,608 (ctest asserted).
3. Ties win+0.5 chop; bin `min(K-1, floor(K*E))`, upper bin inclusive
   through the clamp.
4. Occupancy predicate RESOLVED: equal-WIDTH retains. The reference
   set is the full non-blocked holdings of each W1 board (the bin
   geometry is a board-distribution property independent of the
   enumerated training range), not the 8-10 hand fixture range where
   the statistic is vacuous (empty quartiles at n=8/10). Evidence
   `docs/plans/ablation/rfc0042-occupancy.csv`: mega-bin max/min
   ratios 1.129 (river 2c3d7hJs9c, 1,081 holdings) and 2.273 (turn
   2c3d7hJs, 1,128 holdings); 0 of 2 boards exceed the frozen 5x
   threshold. No equal-mass switch is made.
5. Exact-vs-MC selection: EXACT enumeration is selected; the MC stream
   remains code+ctest only and no stream seed/draw count is
   activated. The digest parameter string therefore records
   `enumeration=conditional-exact`. The predeclared per-fixture-board
   wall ceiling for the FOLLOW-ON flop campaign is 600 s on this
   reference host, derived before the measurement from a campaign
   envelope of 24 h wall on 56 workers over the 1,755 suit-canonical
   flop classes (1,755 tables; 24 h*56/1,755 ~= 1,656 s budget, a
   conservative ~2.8x margin gives the 600 s ceiling; suit-isomorphic
   boards share one table).
6. Fixture scope: W1 boards (river 2c3d7hJs9c, turn 2c3d7hJs) and the
   ten-hand representative range filtered to non-blocked holdings;
   gated shapes are 2/3 seats only. Adoption remains fixture-scoped;
   full-range confirmation is deferred to the follow-on.
7. Flop exact-table wall: measured 183.911 s per board for 1,176 x
   1,070,190 = 1,258,543,440 terms
   (`docs/plans/ablation/rfc0042-flop-wall-probe.csv`; full-table mean
   E_h = 0.500000, the symmetry check). 183.911 s < the 600 s ceiling,
   so the exact build is admissible for the follow-on campaign
   (~1,755*184 s ~= 90 worker-hours, ~1.6 h wall on 56 workers).
8. Memo: one E_h per (parameter token, board, holding) per process;
   the token includes the grid and exact/MC selection; cleared between
   scheme runs; ctests assert hits and parameter-set separation.
9. Pilot power check (five paired seeds per shape, 10,000 ms wall,
   base seed 777042; evidence
   `docs/plans/ablation/rfc0042-pilot-{tiers,tiersdraw,equity}.csv`):
   expected S=32 paired-t half-widths via
   `rfc0042-gates pilot` — equity 2p-river 0.0009 (<0.05 target),
   2p-turn 0.0021, 3p-river 0.0052, 3p-turn 0.0096 (<0.01 target);
   tiers-draw 0.0001/0.0031/0.0001/0.0056. Every shape passes at
   S=32; the worst shape (3p-turn) sets S and passes. Frozen
   confirm seed count **S=32**, confirm base seed **888042**. No
   seed is added after Stage 3 results.
10. Pilot structural preview (NOT the Stage 3 decision): equity river
    bucket-constrained seed means are ~0.097 (2p) and ~0.376 (3p),
    tiers-draw 3p river ~0.140 — all above gate 3's 0.05; gate 3 is a
    structural seed-mean property that added seeds do not change. This
    is the R7-anticipated convergence finding; Stage 3 confirms it on
    the frozen S=32 sample and the mechanical predicate classifies the
    outcome (Keep, since gate 3 can never produce Inconclusive).

### 2. Enum, digest, API threading, and seams

- Add `CardBucketKind::EquityStrengthV1` with its own
  `card_abstraction_id` (declared parameter string + freeze date; the
  digest changes with any frozen parameter).
- API (TWO entry points; the existing one is retained verbatim so no
  protected caller is edited):
  1. the current `card_bucket(kind, hole, board)` is KEPT exactly as
     declared today, so every existing caller compiles unchanged —
     `resident_policy.cpp:469`, `subgame_resolver.cpp:767`,
     `seat_policy.cpp:126`, `continuation_range.cpp:157,290`,
     `root_value_extractor.cpp:118`, `mmap_library.cpp:450`, and the
     production stage6 callers (`mccfr_trainer.cpp:403`,
     `candidate_policy.cpp:435`), plus the other benchmark drivers;
     none of those files is modified by this RFC. This retained
     3-arg entry REFUSES `EquityStrengthV1` with `invalid_argument`:
     there is no call form that silently buckets a live-seat-aware
     kind without supplying L (a ctest asserts the throw and that
     every existing kind still returns its unchanged ids there);
  2. a NEW overload `card_bucket(kind, hole, board, live_seats)` is
     the only entry that accepts `EquityStrengthV1`; existing kinds
     accept and ignore `live_seats` so their ids are unchanged on
     either entry point. The measurement driver/trainer passes
     `def.player_count` minus folded seats; the ctest suite includes
     a wrong-L id-window coverage test (correct bin, wrong window).
  No default-argument convenience form is added (no silent L), and
  no file in the Section 2 serve/build inventories changes a
  `card_bucket` call in this RFC.
- Trainer seam (measurement only): `NSeatTrainerLimits::card_kind`
  defaults to `kNSeatCardKind` and the live-seat count is derived from
  game state at all THREE `card_bucket` sites: `walk_action`
  (`nseat_trainer.cpp:497-498`), `walk_average`
  (`nseat_trainer.cpp:699-700`), and the public query method
  `NSeatPolicy::lookup` (`nseat_trainer.cpp:1173-1192`, bucket at
  `:1189`). `lookup` receives the kind from the trained policy's own
  construction identity (it already holds game/action identity), so no
  signature change reaches its sole non-test caller
  (`engine/src/stage6/measured_policy.cpp:68`), which continues to get
  a default-kind policy. Default production training and stage6
  behavior are bit-identical; only the benchmark ablation constructs
  another kind.
- Benchmark ablation arm: the driver parameterizes EVERY currently
  hardcoded `kNSeatCardKind` site in
  `engine/benchmarks/subgame_nash_conv.cpp` (today `:441`, `:851`,
  `:1001`, `:1046`, `:3070`) with the arm's scheme, so policy
  construction, row lookup, exact-hole and bucket-constrained BR
  walkers all speak the trained scheme's id space. At the identity
  control (`:3062-3074`) the existing collision tracker is a 32-bit
  bitmask (`1u << b`), whose shift is undefined once new-kind ids
  exceed 31 (ids reach base+63, max 1,608 in the frozen table); the
  arm replaces that tracker with a width-agnostic `std::set`/dense
  boolean vector sized to the scheme's declared id window. The arm
  trains with `train_nseat` and evaluates the resulting in-memory
  policy (`rows()`/`nseat_policy_accounted_bytes`); it never calls
  `save_sealed_checkpoint` (whose sole production caller is the flop
  builder at `flop_library.cpp:529`) and never packs a BSFLIB section.
- Build/pack/stamp sites are EXPLICITLY OUT OF SCOPE, and the reason
  is concrete rather than stylistic: changing only the packer's
  re-bucketing call (`mmap_library.cpp:450`) while leaving the header
  kind byte (`mmap_library.cpp:831-832`, written unconditionally from
  `kNSeatCardKind`) would produce rows in the new id space under a
  header claiming the compiled-default kind; `open()`
  (`mmap_library.cpp:1417-1426`) compares that byte ONLY against the
  compiled constant and would MOUNT the mislabeled artifact, silently
  serving wrong rows. A safe override is therefore an all-or-nothing
  write-path change, and Stage 3 needs none of it. The complete
  inventory the follow-on flip RFC must thread atomically (build CLI
  flag plus per-site kind plumbing and a ctest asserting an engine
  compiled with the old default REFUSES to open a new-kind artifact):
  1. `engine/src/resident/mmap_library.cpp:450` — packer
     re-bucketing call and its range diagnostic;
  2. `engine/src/resident/mmap_library.cpp:284-298` —
     `bucket_in_range`, already kind-branched (TiersDrawV1 0..35);
     the new kind adds one branch/window, it is NOT the first
     parameterization;
  3. `engine/src/resident/mmap_library.cpp:831-832` — BSFLIB header
     byte 20 (the mislabeling hazard above);
  4. `engine/src/flop_library/flop_library.cpp:424-425` — warm-resume
     checkpoint kind comparison;
  5. `engine/src/flop_library/flop_library.cpp:525` — checkpoint
     `key_kind` stamp (the value the
     `sealed_checkpoint.cpp:150-164` `bucket_ceiling` writer switch
     validates; the switch needs the new case there as well);
  6. `engine/src/flop_library/flop_library.cpp:642` —
     `BuildIdentity.card_bucket_kind`;
  7. `engine/src/flop_library/flop_library.cpp:1430` —
     `LibraryManifest.card_bucket_kind`;
  8. the builder CLI that supplies the flag to all of the above.
- Serve-path consumers are likewise OUT OF SCOPE and listed for the
  follow-on flip RFC, which must thread `live_seats` and a memoized
  equity table to every one: `resident_policy.cpp:467-470`
  (find_bucket replay hot path), `subgame_resolver.cpp:764-768`,
  `nseat_policy_codec.cpp:246` (mounted-row decoder bucket window),
  `seat_policy.cpp:126`, `continuation_range.cpp:290`,
  `root_value_extractor.cpp:118`. Because mmap `open()` hard-refuses a
  header-kind mismatch (`mmap_library.cpp:1417-1426`), there is no
  overlap window: the follow-on RFC must build and verify the full
  replacement artifact set FIRST, then flip the compiled default,
  remount, and delete old-kind artifacts in one coordinated change.
- Stage 3 outputs are in-memory policies evaluated inside the
  benchmark; this RFC mounts, packs, or checkpoints no new-kind
  artifact.

### 3. Measurement and predeclared gates

Driver: extend `bigshark-subgame-nash-conv` with a card-kind ablation
arm that trains one shape at the 10,000 ms wall (same M1 harness as W1,
same fixture boards/ranges) for each scheme and reports, per seed:

- exact-hole summed BR gap and per-seat max (unchanged walker),
- bucket-constrained summed BR gap where the tree is chance-free
  (river),
- completed iterations, information sets, and sealed policy bytes.

Schemes: `CategoryTiersV1` (control), `TiersDrawV1` (built
intermediate), and `EquityStrengthV1` at the frozen PRIMARY grid point
(`K_flop=32, K_turn=64, K_river=64`). The other three distinct
turn/river grid points (Section 1; K_flop is inert on these shapes)
are descriptive rows only and never enter an adopt predicate. Shapes:
2-seat river, 2-seat turn, 3-seat river, 3-seat turn. The 4-6 seat
shapes are projection rows only (iteration-limited regardless of kind).

Statistical protocol (frozen before Stage 3, mechanically evaluable):

1. **Paired seeds.** Every (scheme, shape) cell uses the SAME seed list
   (identical joint-deal and chance RNG streams), so the per-seed
   differences `d_s = metric_tiers,s - metric_new,s` are paired; the
   adopt statistic is the mean paired difference with its 95% CI.
2. **Seed count.** `S = 32` confirm seeds per cell. This deliberately
   deviates from RFC 0031's 64: a Stage 2 pilot (5 paired seeds per
   shape) estimates the paired standard deviation and records whether
   S=32 gives an expected 95% half-width below half the predeclared
   target effect; if not, S is raised and the new S frozen in the
   addendum before Stage 3. The target effect is anchored to PRE-PILOT
   W1 evidence, never to the pilot's own point estimate (circularity
   guard): on the primary endpoint the W1 abstractable gap is
   0.335 - 0.032 = 0.303 BB, giving gate-1 threshold
   M = max(0.05, 0.303/3) = 0.101 BB, so the power target is a primary-
   endpoint half-width < M/2 ~= 0.05 BB; on each non-inferiority shape
   the target is half-width < delta/2 = 0.01 BB (margin
   delta = 0.02 BB), the worst shape setting S for all cells. No seeds
   are added after seeing Stage 3 results.
3. **Interval estimator.** Bias-corrected percentile bootstrap of the
   mean paired difference over the S paired rows (10,000 resamples,
   deterministic seed), with the paired-t interval reported alongside;
   gates use the bootstrap interval. All S raw rows are stored in the
   CSV so intervals are reproducible.
4. **Unit of analysis is the shape** (one paired mean per shape); no
   pooling across shapes inside the superiority gate.

Gates for adopting `EquityStrengthV1` (all required):

1. **Superiority on 2-seat river (primary endpoint):** the lower 95%
   bootstrap bound of the paired improvement
   `d = exact_sum_tiers - exact_sum_new` is strictly positive AND
   `mean(d) >= M`, where
   `M = max(0.05 BB, (1/3) * mean(exact_sum_tiers - bucket_sum_tiers))`.
   The 0.05 BB absolute floor is a deliberate deviation from RFC
   0031's 0.01 (the abstractable loss here is ~0.30 BB, so 0.01 would
   be an immaterial effect); the relative third is RFC 0031's rule.
2. **Non-inferiority on the other shapes** (2-seat turn, 3-seat river,
   3-seat turn): with predeclared margin `delta = 0.02 BB`, the UPPER
   95% bootstrap bound of `(exact_sum_new - exact_sum_tiers)` is
   `<= delta` on each shape — an equal-quality new scheme passes; this
   is not a superiority requirement.
3. **Convergence sanity at EACH river shape:** the new scheme's river
   bucket_sum <= 0.05 summed at the seed mean on BOTH the 2-seat and
   the 3-seat river — the finer partition must still converge inside
   10 s at every river shape, not on average across shapes. The
   threshold is deliberately stricter than the 3-seat control's OWN
   bucket_sum (0.139 in W1): pass requires the finer partition to reach
   the 2-seat-style near-convergence floor (W1 control 0.032), not
   merely to tie today's 3-seat within-abstraction gap.
4. **Cost:** mean sealed policy bytes <= 2.0x control and mean
   completed iterations >= 0.5x control at each shape (structural
   properties; seed spread reported).
5. **Beat the built intermediate:** gates 1-4 are evaluated for EACH
   candidate (`EquityStrengthV1` and `TiersDrawV1`) against the same
   `CategoryTiersV1` control and the same M. Complete truth table:
   - Equity passes 1-4, TiersDraw fails any of 1-4 -> adopt-
     `EquityStrengthV1`;
   - TiersDraw passes 1-4, Equity fails any -> adopt-`TiersDrawV1`
     (cheaper change, same follow-on process);
   - BOTH pass 1-4 -> `EquityStrengthV1` is chosen ONLY if its paired
     superiority over `TiersDrawV1` has a strictly positive 95% lower
     bound; the explicit ELSE — bound zero/negative or the interval
     touches zero — is adopt-`TiersDrawV1`, never `EquityStrengthV1`;
   - NEITHER passes 1-4 -> this rule cannot rescue either; the
     decision is Keep or Inconclusive per the predicate below.

Decision outcomes, recorded against the frozen header. Adopt
(`EquityStrengthV1` or `TiersDrawV1`) per the truth table. Otherwise
the Keep-vs-Inconclusive distinction is itself mechanical and
predeclared: an outcome is **Inconclusive iff it is not an Adopt AND
at least one gate on which the decision hinges failed only because its
observed interval half-width exceeded the frozen power target** — the
primary-endpoint target 0.05 BB or the non-inferiority target
0.01 BB on the shape whose upper bound exceeded `delta` (gates 3 and 4
are seed-mean structural properties, not interval predicates, and can
never produce Inconclusive); Inconclusive is resolved only by raising
S under a NEW freeze, never by moving a gate post-hoc. Every other
non-Adopt outcome — tight intervals that show no superiority, a
non-inferiority breach with half-width <= 0.01, or a gate 3/4
failure — is **Keep** (no material improvement / scheme unsuitable).
The CSV header, including raw per-seed rows and bootstrap parameters,
is frozen before the first Stage 3 run.

### 4. Post-gate process (no production change in this RFC)

This RFC ends with the gate decision; no outcome flips production here.

- **On Adopt**, a follow-on RFC is required and must specify (a) the
  serve-path E_h table/cache for every consumer listed in Section 2
  with its own replay-latency microbenchmark gate (the RFC 0031 serve-
  cost concern re-derived for equity computation), (b) build-side kind
  selection threading ALL EIGHT stamp/build sites inventoried in
  Section 2 atomically — including header byte 20 and the
  warm-resume/checkpoint/identity/manifest stamps — plus the
  reject-mount ctest (old-default engine must refuse a new-kind
  artifact), producing and digest-verifying the full replacement
  artifact set, (c) the coordinated flag day flipping the compiled
  default, remounting, and deleting old-kind artifacts in one change
  (mmap open() refuses kind mismatches, so overlap is impossible by
  construction), and (d) full-range confirmation beyond the enumerable
  fixtures.
- **On Keep/Inconclusive**, the new enum, trainer field, benchmark
  arm, and any freeze-only caches are deleted in the decision change
  (no-legacy rule); evidence CSVs and this RFC remain.

## Dependency Rules

- `bs::abstraction` owns the bucket function, its enum, and the declared
  identity/digest; no consumer re-derives an equity bucket. The
  retained 3-arg `card_bucket` stays the production entry point
  unchanged (and refuses the new kind); only the measurement seam in
  the trainer and benchmark uses the new 4-arg entry with an explicit
  live-seat argument.
- The n-seat trainer gains one measurement-only limit field carried
  through to `NSeatPolicy::lookup`; the benchmark gains the ablation
  arm. NOTHING in flop_library, resident, artifacts, or codec is
  modified: no build-side kind override exists in this RFC (the
  partial-override mislabeling hazard is documented in Section 2), and
  production defaults stay at the compile-time `kNSeatCardKind` /
  mounted kind until a follow-on flip RFC.
- Artifact and mmap code is untouched by this RFC. The new ids stay
  inside uint16 by construction (max 1,608 over the frozen table), but
  no artifact format, window validation, or header byte changes until
  the follow-on RFC threads all eight sites atomically.
- No wire/proto, resolver-API, serve-path, build/CLI, or stage6
  dependency is added (`lookup`'s existing stage6 caller keeps getting
  a default-kind policy); the compiled-but-unwired MultiStreetSolver
  OCHS code is not linked into `card_bucket`.
- The replacement artifact campaign and the serve-path changes depend
  on the gate decision and live in the follow-on RFC; neither is a
  prerequisite for the measurement stage.

## Compatibility and Migration

No wire/proto change. This RFC changes no production default, build or
pack path, sealed-checkpoint writer, resolver caller, or mounted
artifact, so measurement-stage compatibility risk is limited to the
new enum/API field, the trainer field, and the benchmark; no sealed
checkpoint or BSFLIB section is ever written under the new kind here.
Mounted artifacts do NOT load a different kind — mmap `open()`
hard-refuses kind mismatches and is untouched; there is deliberately
no overlap or dual reader, and any flip is the follow-on RFC's
coordinated flag day.

## Security and Operational Impact

Measurement/benchmark code only; no credentials, network, or game-I/O
changes. The equity enumeration is CPU-only. The one-board flop
full-table probe (~1.26B `(o,r)` terms, ~2.52B evaluator invocations
across the 1,176 non-blocked hero holdings) is measured once in Stage 2
and recorded; any conditional MC fallback is a frozen stream in the
declared function. Training-time enumeration is bounded by the
freeze-item-8 memo (each board/holding equity is computed once). There
is no per-request serve cost because the serve path is not touched.

## Alternatives Considered

1. **Keep nine tiers.** Zero work, but leaves the measured
   0.30 BB/hand ceiling in place on every converged shape; rejected as
   the default pending the gate outcome (the gate can still return
   Keep).
2. **Adopt TiersDrawV1 directly.** It is implemented, but it refines
   only with a two-bit draw flag and still merges all kicker strength
   within category — the dominant river error; measured as a gate
   participant rather than presumed.
3. **Precomputed EMD/OCH histograms with offline k-means clustering**
   (the Pluribus/supercomputer recipe). Stronger but needs a new
   offline clustering pipeline, cluster tables per board class, and a
   distribution artifact; it is the documented escalation if a simple
   deterministic EHS binning fails the gate (new RFC).
4. **Identity per-combo keying.** Exceeds uint16 and multiplies rows by
   up to 1,176 postflop combos (C(49,2) non-blocked hero holdings per
   board); infeasible and RFC 0031 records why.
5. **Learned embeddings / the RFC 0041 network.** Rejected there on
   evidence and irrelevant to the bucket question; a deterministic
   declared function is auditable and fits the existing artifact model.
6. **Potential-aware buckets only (draw flags).** That is
   `TiersDrawV1`; included as a measured arm, not presumed sufficient.

## Risks

- R1: equity bins may not align with DECISION relevance (similar equity,
  different preferred actions); the exact-BR superiority gate catches
  this (no measured exact-gap improvement => Keep).
- R2: flop MC-equity noise crossing bin boundaries; exact is the
  declared default, and no flop rows are run or reported in Stage 3
  (flop is reserved/oracle-tested only); the frozen stream, the
  assignment-agreement ctest (>=99.9%) and the 0.1/K error predicate
  bound it only when the follow-on build actually selects the MC
  fallback under its frozen threshold.
- R3: finer partitions reduce per-info-set sample counts and slow
  convergence inside 10 s; gates 3/4 measure it directly (gate 4 under
  the freeze-item-8 enumeration memo, so it reflects convergence only).
- R4: two-player EHS in n-way games may cap the 3-seat benefit;
  non-inferiority protects current quality; range/opponent-aware equity
  is the documented next step.
- R5: fixture external validity — gates run on the 10/12-hand
  enumerable fixture (30 river information sets); an Adopt decision is
  fixture-scoped, with full-range confirmation explicitly deferred to
  the follow-on flip RFC.
- R6: a gate pass does not mean production deployment; the serve-path
  design remains a separately reviewed RFC.
- R7: gate 3 likely binds — it demands the 3-seat river bucket_sum
  shrink from W1's 0.139 to <=0.05 (~2.8x) while a finer partition
  reduces per-info-set visits; a Keep outcome on that ground is the
  gate behaving honestly, and the Stage 3 report must record the
  per-shape bucket_sum means rather than averaging the failure away.
- R8: partial build-side threading could mount a mislabeled artifact
  (Section 2 header-byte hazard); this RFC eliminates the risk by not
  touching any write site, and the follow-on's reject-mount ctest is
  the permanent guard.

## Verification Plan

- New/updated ctests (release strict bands; ASan sanity): E_h
  determinism, symmetry, and the exact conditional `(o,r)` enumeration
  against an independent hand-written oracle on frozen fixtures (990 /
  1,035x44 / 1,081x990 term counts per holding, single ascending
  accumulation order, exact-double equality in the same build); flop
  full-table fan-out count 1,176 x 1,070,190 asserted structurally;
  tie handling; flop MC stream-vs-exact assignment agreement >=99.9%
  and max error <=0.1/K when the fallback predicate is exercised;
  id-window bounds/density for the frozen base table (all streets,
  L=2..10, max id 1,608); wrong-L window coverage; L-independence of
  E_h; retained-3-arg entry refusing `EquityStrengthV1` while all
  existing kinds keep identical ids on it; kind digest pinning
  including MC-selection parameters; memoization (second
  `(board,holding)` query performs no enumeration and cache
  separation across parameter sets); benchmark collision-tracker
  behavior at ids > 31; default-kind bit-identity: a
  default-constructed trainer and policy produce identical
  buckets/rows/digests to HEAD, and the stage6
  `measured_policy.cpp:68` lookup path is unchanged.
- Benchmark: the frozen primary-point ablation plus descriptive grid
  rows; evidence CSV with all S raw rows; gates (including the Keep/
  Inconclusive predicate) evaluated mechanically from the frozen
  header.
- Measurement stage: format, release + asan builds, focused + full
  release ctest, npm check, proto:check, both doc/RFC checkers. The full
  Engine Build Contract (incl. benchmark-multistreet) is required again
  at the follow-on production flip.

## Rollout Plan

1. This measurement RFC accepted.
2. Stage 2 freeze: implement `EquityStrengthV1` (enum, live-seat API,
   per-board E_h memo), the trainer measurement field (walkers plus
   `NSeatPolicy::lookup`), the benchmark arm parameterizing all five
   driver kind sites, and ctests; run the pilot power check and the
   one-board flop-table wall probe; freeze the full base offset table,
   occupancy predicate, build threshold, MC predicate/stream (if
   needed), memo contract, and S; append the freeze addendum.
3. Stage 3: run the S-seed paired ablation, commit the CSV with raw
   rows, evaluate the gates mechanically; record Adopt/Keep/Inconclusive
   through independent review.
4. Post-decision: on Adopt, author the follow-on flip RFC (all eight
   build/stamp sites with reject-mount ctest, serve cache, replacement
   artifact campaign, flag day); on Keep/Inconclusive, delete the
   enum/API field/trainer field/benchmark arm/freeze caches.

## Rollback Plan

Before the decision, deleting the new enum, API parameter, trainer
field, and benchmark arm restores HEAD with zero behavior change — no
artifact, header, checkpoint, or build file is modified by this RFC, so
there is nothing to unmount or regenerate. After an eventual follow-on
production flip, rollback is reverting the compiled default and
remounting prior-kind artifacts (digest/header pinning prevents
mixing); that plan belongs to the follow-on RFC.

## Open Questions

- Whether the frozen reference deal set trips the equal-mass
  occupancy predicate (the primary K grid 32/64/64 and equal-width
  bins are already declared; only this binning switch is open).
- The one-board Stage-2 probe decides only whether the frozen
  build-budget threshold admits exact flop tables for the FOLLOW-ON;
  exact remains the declared default and no Stage 3 cell uses flop
  buckets.
- Whether bucket_sum on turn trees can be made meaningful (currently
  chance nodes invalidate the bucket-BR walker); the turn gate uses the
  exact measure only.

## Acceptance Criteria

- AC1: `EquityStrengthV1` is a declared deterministic card-bucket kind
  reachable only through a new 4-arg `card_bucket` (the 3-arg entry is
  retained and refuses it), with a live-seat-aware signature, pinned
  id/digest (including MC-selection parameters), ids <= 1,608 over the
  frozen base table (all streets, L=2..10), and ctests including the
  exact conditional `(o,r)` oracle, the 1,176-holding flop fan-out,
  the flop-MC-vs-exact predicate when exercised, and the E_h memo.
- AC2: the n-seat trainer/policy accepts a measurement-only
  `card_kind` override at all three sites (walkers plus
  `NSeatPolicy::lookup`) via the new 4-arg `card_bucket`; the
  retained 3-arg entry is unchanged for every existing kind and
  throws on `EquityStrengthV1`; default-constructed trainers/policies
  and the existing stage6 lookup caller behave bit-identically to
  today (digest/bucket/row test); no build, pack, checkpoint, header,
  codec, serve-path, or production-default site is modified, and
  every current 3-arg caller listed in Section 2 compiles unchanged.
- AC3: the Stage 2 freeze addendum pins the enumeration formula and
  ascending-card-order summation, L-independence semantics, the full
  base offset table (preflop 0..168; flop/turn/river L=2..10), bin
  convention, occupancy predicate, the one-board flop wall probe and
  the predeclared build-budget wall threshold, the MC predicate/stream
  (if any), memo contract, fixture scope, and the W1-anchored
  pilot-derived seed count S, before any Stage 3 row is produced.
- AC4: the frozen ablation CSV carries all S raw paired rows for every
  scheme/shape, exact and (river) bucket gaps, bytes, and iterations,
  plus the deterministic bootstrap parameters.
- AC5: the decision follows the predeclared gates mechanically —
  superiority, per-shape non-inferiority, per-shape gate 3, gate 4
  under the memo, the four-cell gate-5 truth table, and the
  half-width-based Keep/Inconclusive predicate — and is independently
  reviewed; Keep/Inconclusive deletes the seam/enum/arm and leaves
  production unchanged.
- AC6: on Adopt, no production change happens in this RFC; a separate
  follow-on RFC owns all eight Section-2 build/stamp sites (including
  header byte 20 and the reject-mount ctest), the serve cache, full
  replacement artifact set, and the single coordinated flag day
  (digest/header fail-closed, no dual reader).
- AC7: no wire/proto/format change; code scope limited to abstraction
  (enum + live-seat API + memo), the n-seat trainer/policy
  measurement field, and the benchmark arm; every build, artifact,
  resident, codec, and serve-path site listed in Section 2 is
  untouched, and `git grep` over those paths shows no modification.
- AC8: measurement-stage verification (format, release + asan, focused
  and full ctest, npm, proto, both checkers) passes; the full Engine
  Build Contract applies again at the follow-on flip.

## Stage 3 decision amendment (2026-10-30) — outcome: KEEP

The frozen S=32 paired campaign ran on 2026-10-30 (base seed 888042,
10,000 ms wall, three parallel single-scheme processes, 128 raw rows
per scheme, zero declines):
`docs/plans/ablation/rfc0042-stage3-{tiers,tiersdraw,equity}.csv`.
Intervals are the mechanical output of
`bin/rfc0042-gates.mjs gate` (BCa 10k, deterministic seed).

Seed means per shape (exact sum / river bucket sum BB; iterations;
sealed bytes):

| shape | tiers (control) | tiers-draw | equity 32/64/64 |
|---|---|---|---|
| 2p river | 0.3346 / 0.0322; 607k; 6,740 | 0.3348 / 0.0324; 601k; 6,740 | **0.1290** / 0.0986; 378k; 12,692 |
| 2p turn | 0.3028; 485k; 6,740 | 0.2297; 459k; 8,724 | **0.1936**; 355k; 14,676 |
| 3p river | 0.7160 / 0.1365; 174k; 22,964 | 0.7162 / 0.1372; 173k; 22,964 | **0.3610** / 0.3610; 113k; 45,140 |
| 3p turn | 0.7937; 145k; 22,964 | 0.8762; 131k; 30,356 | 0.8084; 101k; 52,532 |

Gate results (evaluated mechanically):

- `EquityStrengthV1`: gate 1 PASS (2p-river paired d=0.2056 BB, 95%
  BCa lower bound 0.2040 > 0, mean >= M=0.101); gate 2 PASS on
  2p-turn (UB=-0.107) and 3p-river (UB=-0.346) but FAIL on 3p-turn
  (UB=+0.0262 > delta 0.02; half-width 0.0122); gate 3 FAIL on BOTH
  river shapes (bucket sum 0.0986 at 2p, 0.3610 at 3p, limit 0.05);
  gate 4 FAIL on BOTH turn shapes (sealed bytes 2.177x and 2.288x,
  limit 2.0x; iteration ratios 0.62-0.73 pass). Gates 1-4 do not all
  pass.
- `TiersDrawV1`: gate 1 FAIL (2p-river d=-0.0002, LB=-0.0004 — no
  river improvement at all); gate 2 FAIL on 3p-turn (UB=+0.0889,
  half-width 0.0061 < 0.01 — a tight, non-width failure); gate 3 FAIL
  at 3p river (0.1372 > 0.05); gate 4 passes. Gates 1-4 do not all
  pass.

Gate 5 truth table: NEITHER candidate passes gates 1-4, so neither
can be adopted and the rule cannot rescue either.

Keep-vs-Inconclusive predicate: the equity decision hinges on gate 3
and gate 4 failures, which are seed-mean STRUCTURAL properties (the
finer partition does not converge inside its own abstraction in 10 s,
and its turn row store exceeds 2.0x); additional confirm seeds cannot
change them, and the predicate declares structural gates incapable of
producing Inconclusive. TiersDraw's failures are likewise tight
(negative superiority bound; 3p-turn half-width 0.0061 under the
0.01 target). The outcome is therefore **KEEP**, not Inconclusive —
no S increase and no new freeze is warranted.

Interpretation (evidence, not a gate): equity-strength bucketing
produces large, statistically decisive EXACT-game improvements on
three of four shapes (+0.206 / +0.109 / +0.355 BB), proving the
card-abstraction direction is right, but it is not convergible inside
the 10-second online resolve budget on the shapes that need it most
(3p river exact == bucket at 0.361 = iteration-limited). That makes
it a candidate for the OFFLINE blueprint/library track of the
post-0041 plan (W4, no per-request wall), which requires its own RFC
and the eight-site flip process; it does not change this Keep.

Disposition per Section 4 / AC5 / the no-legacy rule: the new enum,
the 3/4/5-argument equity entries and all `equity_strength_*`
machinery, the `NSeatTrainerLimits`/`NSeatPolicy` fields, the
benchmark schemes/arms/CLI and the memo are deleted; production stays
at nine tiers. The evidence CSVs, this RFC, and the mechanical
evaluator (`tools/rfc/rfc0042-gates.ts`, an evidence-analysis tool
with no engine linkage, plus its Node tests) are retained so the
decision stays reproducible from the committed raw rows.

Completion record: the Keep deletion was executed on 2026-10-30 and
independently adjudicated by the approval agent (same session as the
five design rounds, agent af45086e55735fda6), which independently
reproduced all gate statistics with a separate Python BCa
implementation, verified pairing/provenance integrity (seed sets
888042..888073 aligned across files; `/tmp` outputs md5-identical to
the committed CSVs; three clean parallel processes), and approved
retaining the TypeScript evaluator. The deletion left
`nseat_trainer.hpp/.cpp` byte-identical to pre-RFC HEAD and removed
every equity symbol from the engine (the only surviving "RFC 0042"
engine comment is the retained W1 `--m1-wall-recalibrate`
bucket-column annotation, which depends on no deleted code).
Verification: `format`, full release build and full release ctest,
asan build and touched-path ctests, `npm run check` (27 Node tests),
`npm run proto:check`, and both doc/RFC checkers all green.

## Decision

Author agent: Claude (zzzode agent session 2026-10-30)
Approved by: independent approval agent af45086e55735fda6
Decision date: 2026-10-30
Review outcome: Approved
Reviewed scope: five independent-review rounds over the complete RFC,
with every cited source site verified against the implementation
(abstraction enum/API, the three trainer bucket sites including
NSeatPolicy::lookup, the eight Section-2 write/stamp sites, the eight
retained 3-arg callers, the five benchmark kind sites, mmap open()/
header byte 20, stage6 precedent), all enumeration arithmetic
independently recomputed, and both structural checkers run each round.
Review summary: rounds 1-2 closed the migration-scope contradictions,
serve-path inventory, statistical freeze, conditional enumeration
counts, and L threading; round 3 found the blocking partial-override
mislabeling hazard (header byte 20) which was resolved by removing the
builder override entirely and handing a complete eight-site inventory
to the follow-on flip RFC, plus the C(49,2)=1,176 / ~1.259B-term flop
table correction; round 4 closed gate truth-table completeness, the
mechanical Keep/Inconclusive predicate, E_h memoization, and call-site
accuracy; round 5 reconciled the card_bucket API (retained 3-arg entry
that refuses the new kind plus a new 4-arg overload), the inert
K_flop grid dimension, and the benchmark 32-bit shift UB. The
approver confirmed the proposal is internally consistent, the gates
are mechanically evaluable from the frozen CSV header, every
protected-path invariant is preserved by construction (no shared
module or established public contract changes), and Stage 2 may begin
as written. Remaining accepted risks: gate 3 may honestly return Keep
(3-seat river bucket_sum must fall ~2.8x to <=0.05); the NI
half-width target may require S well above 32 with no upper cap;
Adopt is fixture-scoped with full-range confirmation deferred to the
follow-on flip RFC; the 0-ULP oracle equality must be confirmed on the
first release+ASan run; the flop MC predicate activates only on the
follow-on's threshold miss.
