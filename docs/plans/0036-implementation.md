# RFC 0036 Implementation Plan — Six-Way Turn and River On-Demand Strategy

Status: Current
Created: 2026-10-24
Owner: Engine resolver, engine solver, engine tree, engine host
RFC: [0036](../rfcs/0036-six-way-turn-river.md) (Accepted 2026-10-24)

## Intended outcome

Six-way bounded on-demand turn and river decisions are served only when
measured calibration evidence shows they can be served inside the live
deadline at a calibrated minimum quality; otherwise the labeled fallback
stays. No turn/river library is built.

## Owning module

`engine/resolver` (two entries, the `solve_rooted_subgame` split, the
serve/decline gate constants) and `apps/engine-host` (routing precedence
and the two diagnostic strings). The wire protocol, persisted formats, and
artifact layout do not change.

## Stages and gates

### Stage 0 — calibration spike (gates everything; no behavior routed)

Build-only spike; host routing stays unchanged and no path is advertised.

1. DONE (2026-10-24): behavior-preserving split of
   `solve_rooted_subgame` into `build_rooted_subgame_tree` and
   `train_and_extract_rooted_subgame` (both private on `SubgameResolver`),
   with the existing signature delegating. The split moves no logic: the
   street constraint and tree try/catch stay in the build step; the
   trainer call, termination check, river walk, and candidate extraction
   stay in the train step. Gate: release ctest `resolver`,
   `subgame_resolver`, `solve_conformance`, `conformance` all pass
   (51.7 s total, subgame_resolver 49.7 s) — `resolve`/
   `resolve_uniform`/`resolve_flop` behavior pinned identical (AC7).
2. MEASURED (2026-10-25, `bigshark-rfc0036-calibration`, 1,000 iterations,
   build time the median of five builds, production identity cap-1 menu and
   full-range uniform priors, seed 42). Raw log is section G1/G2 below;
   fixture root is the production limped root (100 BB, 2 chips per seat):

   | shape | seats | nodes | action | chance | frontier leaves | depth | bytes | build ms | infosets | ms/iter |
   | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
   | river cap-1 | 2 | 27 | 10 | 0 | 0 | 4 | 18.9 KiB | 0.023 | 50 | 0.028 |
   | river cap-1 | 3 | 88 | 39 | 0 | 0 | 6 | 45.5 KiB | 0.048 | 195 | 0.058 |
   | river cap-1 | 4 | 305 | 144 | 0 | 0 | 8 | 179.9 KiB | 0.135 | 720 | 0.184 |
   | river cap-1 | 5 | 781 | 380 | 0 | 0 | 10 | 363.5 KiB | 0.306 | 1,900 | 0.428 |
   | river cap-1 | 6 | 1,897 | 936 | 0 | 0 | 12 | 734.1 KiB | 0.855 | 4,680 | 1.061 |
   | turn cap-1 streets(1) | 2 | 459 | 10 | 9 | 336 | 5 | 293.4 KiB | 0.095 | 40 | 0.029 |
   | turn cap-1 streets(1) | 3 | 1,864 | 39 | 37 | 1,344 | 7 | 1,173.0 KiB | 0.483 | 156 | 0.082 |
   | turn cap-1 streets(1) | 4 | 7,073 | 144 | 141 | 5,424 | 9 | 4,686.4 KiB | 1.944 | 576 | 0.242 |
   | turn cap-1 streets(1) | 5 | 18,829 | 380 | 376 | 14,448 | 11 | 18,639.5 KiB | 6.944 | 1,520 | 0.593 |
   | turn cap-1 streets(1) | 6 | 46,585 | 936 | 931 | 35,760 | 13 | 37,375.6 KiB | 22.146 | 3,744 | 1.628 |

   Every shape builds well inside the 1 GiB builder cap; the earlier T3
   byte-cap fear does not reproduce for the production identity cap-1 menu.
   Cross-checks: the DECLARED-COARSE 10 BB four-seat river tree is exactly
   2,829 nodes, reproducing RFC 0011's pinned point; uncapped identity trees
   at 100 BB exceed the 1 GiB cap from three seats up (two seats: 33,693
   nodes); cap-1 node counts are stack-independent (25 BB = 100 BB =
   1,897 nodes at six-seat river). Build-time coefficient is ~0.45-0.47
   microsecond per node on this host.
3. Envelope projection against the 1,600 ms p95 budget with the measured
   per-iteration cost (iterations = (1600 - build_ms) / ms_per_iteration;
   visits/infoset = iterations x seats / information_sets):

   | shape | seats | affordable iters | visits/infoset |
   | --- | --- | --- | --- |
   | river cap-1 | 4 | ~8,690 | 48.3 |
   | river cap-1 | 5 | ~3,740 | 9.8 |
   | river cap-1 | 6 | ~1,510 | 1.93 |
   | turn cap-1 s1 | 4 | ~6,600 | 45.8 |
   | turn cap-1 s1 | 5 | ~2,690 | 8.85 |
   | turn cap-1 s1 | 6 | ~970 | 1.55 |

   The node/time gate therefore clears trivially for every seat count, but
   the information-set visit density at six seats is at most ~2 per
   information set inside the envelope; five seats reach ~9-10. Routing for
   seats 5/6 therefore hinges entirely on the M1 quality measurement at
   these iteration counts (a default-to-decline constant is fixed only from
   M1 CIs), never on the node table alone. The per-build build-time and
   node/action/seat-driven per-iteration coefficients above calibrate the
   pre-build screen and the post-build projection.

5. M1 GO/NO-GO MEASURED (2026-10-25) — **no routing for seats 5 and 6.**
   `bigshark-subgame-nash-conv --rfc0036-m1-envelope` trains each shape at
   its projected affordable iteration count and computes the in-model
   pooled-walker NashConv over three seeds (enumerable sub-range; six-seat
   rows use a 12-hand river-compatible range because the ten-hand T3 range
   admits only five disjoint river holdings):

   | shape | seats | iterations | sum NashConv BB (3 seeds) | mean BB |
   | --- | --- | --- | --- | --- |
   | river cap-1 | 5 | 3,740 | 6.04 / 8.44 / 6.95 | 7.14 |
   | river cap-1 | 6 | 1,510 | 9.20 / 9.25 / 9.31 | 9.25 |
   | turn cap-1 s1 | 5 | 2,690 | 5.05 / 5.90 / 6.24 | 5.73 |
   | turn cap-1 s1 | 6 | 970 | 17.82 / 15.94 / 13.44 | 15.73 |

   The gate is per-seat gap < 0.05 BB and sum < 0.15 BB. Every routed shape
   misses the sum gate by 38x-105x at the MOST iterations the 1,600 ms
   envelope permits. No `min_completed_iterations(seats)` exists that both
   fits the envelope and passes M1, so the serve/decline gate defaults to
   decline and the host routes neither the bounded river nor the bounded
   turn path for seats 5/6; the labeled fallback remains (the RFC's stated
   worst-acceptable outcome; those boards are unserved today). The 100-iter
   convergence series (`--rfc0036-m1 100`, 11-47 BB gaps across seats 2-6)
   shows the gap is iteration-limited and still two orders of magnitude off
   at the envelope ceiling. M2/Phase C is not reached: it only re-frames a
   turn cut whose bounded policy already fails the in-model floor. Two/three
   seats and the four-seat uncapped river chain are unchanged by this
   decision. The build/train seam landed in step 1 stands, so a later
   host-level budget change needs only re-running this calibration.
4. Full-release node ctest with both fuzz link configurations.
5. `M1` (in-model NashConv) tooling: wire the sampled stage-6 BR estimator
   (`stage6_br_estimator`) into an on-demand subgame harness with pooled
   walkers over enumerable sub-ranges (2..6 seats); full-range six-seat uses
   sampled estimation with 95% confidence intervals.

Gate: commit the table and coefficients, record go/no-go for routing per
(seat, street). Any shape whose bounded path is not measured to clear the
gate stays declined; the worst acceptable outcome is no routing.

### Phase A — bounded river resolver (seats 5/6)

1. `resolve_river_uniform` using `identity(cap=1)` with `none()`; chance-free
   tree; exact five-card showdown settlement.
2. `conformance_fixtures`-style legal river fixtures; 4/5/6-seat all-live
   and 2-4 folded, zero-sum frontier controls at the cuts.
3. Cap-suppression witnesses; receipts distinguish
   `subgame-resolve:river-bounded`.
4. Host: board size 5 seats 5/6 bounded-first, no uncapped retry, four-seat
   uncapped chain unchanged (with the disclosed 5/6-seat
   partial-to-fallback behavior change).

### Phase B — street-start turn resolver (seats 4-6)

1. `resolve_turn_uniform` with `identity(cap=1)` streets(1); 48-outcome
   chance nodes sampled externally; equity frontier at Action leaves.
2. Street-start predicate; rejects mid-turn; mid-turn keeps the existing
   chain.
3. Folded/side-pot/dead-money zero-sum fixtures; zero-sum settlement
   controls.
4. Host board-4 street-start seats 4-6 bounded-first; board-4 mid-turn and
   board-5 seats 2/3 unchanged.

### Stage M — model-error measurement

1. Define M2: freeze full river-betting reference policy, fill river leaves
   with the reference, compare turn root values/action distributions
   against the all-in-equity composition, with uniform-fill sensitivity
   bounds; revalidate visits against actual information-set counts.
2. M1 at two iteration counts on 2..6 seats; pass decision on the frozen
   numeric gates (epsilon_seat 0.05, epsilon_sum 0.15, M2 trigger 0.01,
   95% CI, M2 proxy does not certify six seats).
3. Phase C (virtual deals) is deferred until M2 triggers; this plan does
   not implement it.

## Main implementation stages

- Resolver entries and the helper split live behind the new gate constants;
  default `min_completed_iterations(seats)` is `decline` until calibrated.
- The pre-build screen uses the stage-0 measured table; the pre-train floor
  uses action-nodes × buckets as the information-set upper bound;
  post-train checks revalidate against the measured result.
- Host routing is enabled per (seat, street) only after the stage-0
  evidence for that pair is recorded.

## Verification

- Full Engine Build Contract and ASan for every stage; the build/train
  split is covered by the three existing callers' fixtures.
- New river/turn fixtures: capped river and turn trees chance-free/full-menu,
  zero-sum settlement, folded/side-pot cases, street-start predicate.
- Receipts: `river-bounded` versus `turn-allin-equity` distinguishable
  without proto changes.
- AC1–AC7 of RFC 0036 map to: split regression (AC7), gate constants and
  calibrated constants (AC3), river phase (AC1), turn phase (AC2), host
  precedence (AC4), receipts (AC5), M1/M2 measurement (AC6).

## Rollback

Per-phase source revert; routing is the only behavioral switch and the
fallback label remains valid throughout. No artifact, persisted format, or
wire change to undo. No stage trains or publishes an artifact.

## Open items deferred

- Phase C virtual deals (separate RFC on M2 trigger).
- Mid-turn facing-a-bet nodes (kept on the existing chain).
- Line-conditioned (non-uniform) priors; uniform-same-distribution is the
  only accepted prior.

## 2026-10-25 — M2 execution design (task 10 of the six-max work list)

Status: Stage 0 measured and no-go for seats 5/6 (M1; see the 2026-10-25
entry above). M1 cannot price the frontier model error by construction:
the evaluator and the walker share the all-in-equity leaf model, so the
error the real river-betting continuation introduces is invisible to it.
M2 is the defined measurement (RFC 0036, "Metrics") and has not been
executed; no tooling for it exists (`bigshark-rfc0036-calibration` is
build/train cost only; `--rfc0036-m1*` modes stop at in-model NashConv).
This section turns Stage M item 1 into buildable work. It implements an
Accepted RFC; no new RFC is required to measure. Phase C is still gated on
the trigger.

### Harness

New mode `bigshark-subgame-nash-conv --rfc0036-m2 <iterations> <seed>`
(alongside the existing M1 modes, which it reuses):

1. **Reference game.** For seats 2 and 3 at 100 BB, build the *full*
   turn-to-river reference tree with the identity menu (cap 0 at two seats;
   at three seats the richest cap whose reference tree clears the
   1 GiB builder/trainer caps — cap-0 feasibility at three seats is the same
   open cell RFC 0037's ablation measures, and an infeasible reference is
   recorded, not substituted silently). Train to `<iterations>` with the
   same representative ranges the M1 harness uses; this yields reference
   turn-root values `V*`, turn action distributions `p*`, and the river
   policy the reference stores at every river node.
2. **Depth-limited candidate.** Build and train the production turn shape
   (`identity(cap=1)`, `DepthLimit::streets(1)`, `EquityFrontierEvaluator`)
   via the existing stage-0 split
   (`build_rooted_subgame_tree` / `train_and_extract_rooted_subgame`,
   `engine/src/resolver/subgame_resolver.cpp:634,663`) at the same
   iteration count and CRN-paired seeds.
3. **Freeze-fill composer.** Freeze the candidate's turn-node action
   distributions. At every river node the depth-limited tree treats as a
   frontier leaf, compose a continuation policy two ways:
   - **reference fill:** the full tree's policy at that river node;
   - **uniform fill:** uniform over the river legal menu (sensitivity
     bound on the fill choice, as the RFC's M2 protocol requires).
   Walk the composed policy with the pooled best-response walker
   (the M1 `measure_rfc0036_m1` machinery, extended to accept composed
   leaves) to obtain turn-root values `V_c`, action distributions `p_c`,
   and the BR gaps.
4. **Quantities (BB, BB=2 chips):** `|V* - V_c|` per live seat and summed;
   turn action-distribution total variation per information set
   (mean and max); the same two quantities for uniform fill; 95% intervals
   over at least three learn/confirm seed groups. CSV:
   `docs/plans/ablation/rfc0036-m2.csv`.

### Decision rule (pre-declared in RFC 0036, restated mechanically)

- M2 **passes** when both fills keep per-seat value error below 0.01 BB and
  summed below 0.03 BB at every measured seat, with the upper confidence
  bound inside the threshold.
- Otherwise Phase C **triggers**: a new RFC extends the resolver seam with
  either RFC 0011 Phase 3 virtual deals (`TurnDeal`/`RiverDeal`,
  `walk_virtual_deal`) or a nested depth-limited re-solve with a
  continuation-value cache, as RFC 0036 Phase C already scopes.
- Six seats are not measured here (reference game unsolvable); the result
  is recorded as a 2/3-seat proxy and never certifies six-seat model
  error. Sampled-estimator six-seat evidence is a Phase C design input,
  not an M2 substitute.

### Sequencing

Built after the RFC 0037 ablation harness lands, because both extend
`subgame_nash_conv.cpp`'s mode parser and share the cap-parameterized
training entry; one build/test cycle covers both. Estimated cost is
bounded by the 2/3-seat reference solves: T3 measured 100K-iteration
two-seat solves at ~1 s and three-seat cap-1 solves at ~3 s; the cap-0
three-seat reference is the only potentially long cell (wall-bounded like
the T3 campaign, never silently truncated).

## 2026-10-26 — M2 MEASURED: FAIL at 3 seats (Phase C triggers)

Harness implemented: `bigshark-subgame-nash-conv --rfc0036-m2 <seats>
<iterations> <seed>`. The reference is the FULL turn->river cap-1
production-menu tree and the candidate is the cap-1 streets(1)
equity-frontier shape (same cap in both, so the measurement isolates the
river-betting model error, orthogonal to RFC 0037's raise cap). Frozen
candidate turn rows are projected onto reference turn nodes; reference
RIVER nodes are filled with the reference equilibrium policy (reference
arm) or uniformly (sensitivity arm); beyond-cap turn nodes get the pinned
passive fill. Raw rows: `docs/plans/ablation/rfc0036-m2.csv`.

Preliminary single-seed (42) 100K figures, BB:

| seats | reference NashConv | reference fill | uniform fill | model delta (ref fill - ref) |
| --- | --- | --- | --- | --- |
| 2 | 2.72 | 3.05 | 27.45 | ~0.33 |
| 3 | 7.36 | 13.27 | 42.14 | ~5.91 |

The uniform arm is far worse than the reference arm at both seat counts,
confirming the measurement is not trivially satisfied by the fill. The
M2 gate (0.01 BB per-seat / 0.03 BB summed) is FAILED at three seats by
a wide margin and is marginal even heads-up: the all-in-equity frontier
is not an adequate model of the real river betting continuation in
multiway play. This independently confirms RFC 0036's M1 no-go from the
model side (M1 only priced envelope convergence; M2 prices the leaf
model) and **triggers Phase C**: turning 5/6-seat turn/river on-demand
back on requires a value-continuation model (nested depth-limited re-
solve with continuation-value cache) or a neural estimator, via a new
RFC extending the resolver seam — not a budget or iteration change.

FINAL multi-seed result (100K decisive count, seeds 101/202/303, 95% t
intervals df=2): the model delta (reference-fill total minus reference
self-NashConv) is 0.402 BB at two seats (CI [0.183, 0.622]) and 6.161 BB
at three seats (CI [4.793, 7.529]). Both FAIL the summed 0.03 BB gate by
one to two orders of magnitude; no seat count passes. The uniform-fill
arm is ~28 BB (2p) and ~42 BB (3p), confirming the reference fill is
informative rather than vacuous. **M2 verdict: FAIL at every measured
seat; Phase C is required for ANY turn/river on-demand routing,
including heads-up deep.** The blocked work is a real river
continuation-value model (nested depth-limited resolve with a cached CV,
or a learned net — RFC 0036's Phase C alternatives), scoped as a new
RFC; it is not addressable by more iterations, a larger envelope, or a
cap change. Task 11 (re-enable host routing) is therefore blocked on
that research RFC rather than on the RFC 0036 gate configuration.
Caveat: T3 ten/twelve-hand fixture and the cap-1 river menu; full-range
and uncapped numbers belong to the later qualification campaign, but the
margin (6 BB vs 0.03 BB) is not plausibly closed by range width alone.
