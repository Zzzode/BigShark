# RFC 0042 Implementation Plan — Equity-Strength Postflop Card Bucketing

Status: Current (COMPLETE — Stage 3 returned KEEP 2026-10-30; engine
measurement code deleted; RFC status Implemented)
RFC: [0042](../rfcs/0042-equity-strength-bucketing.md) (Accepted 2026-10-30,
five-round independent review; approval agent af45086e55735fda6; Stage 3
Keep decision independently verified by the same agent)

Scope reminder: this RFC is MEASUREMENT-ONLY. Stage 2 adds the declared
kind, its exact-EHS machinery, in-memory trainer/policy seams, and the
benchmark ablation arm. No build/pack/checkpoint/header/codec/serve
site changes; no artifact is ever written. On Keep/Inconclusive all
Stage 2 code is deleted.

## Stage map

### Stage 1 — RFC and approval (COMPLETE)

RFC 0042 Accepted after five review rounds. Key review-driven
decisions recorded in the Decision section: builder override removed
(eight-site stamp inventory handed to a follow-on flip RFC); three-arg
`card_bucket` retained and refusing the new kind; exact enumeration
counts 990 / 1,035x44 / 1,081x990 with 1,176 flop fan-out / ~1.259B
terms; memoized E_h; mechanical Keep/Inconclusive predicate.

### Stage 2 — Declared function, seams, ctests, freeze (IN PROGRESS)

Implementation:

1. Abstraction layer (`engine/include/bs/abstraction.hpp`,
   `engine/src/abstraction/abstraction.cpp`):
   - `CardBucketKind::EquityStrengthV1 = 4` with its own
     `card_abstraction_id`;
   - pinned conditional exact enumeration (opponent pairs ascending,
     conditional runouts ascending, single nested accumulation),
     independent oracle conventions;
   - XorShift64* Monte Carlo EHS over the same disjoint space
     (code+ctest only; never selected in Stage 3);
   - `EquityStrengthGrid` (primary 32/64/64; descriptive 16/32
     turn/river variants), band table `169 + (L-2)*160 + street`,
     L = 2..10, max id 1,608;
   - three-arg `card_bucket` retained, refuses the new kind;
     four-arg entry and five-arg grid entry;
   - per-parameter-set process memo with stats/clear.
2. Trainer seam (`nseat_trainer.hpp/.cpp`):
   - `NSeatTrainerLimits::card_kind` + `equity_grid`, default
     production kind;
   - SweepContext kind/grid; live-seat count from
     `state.live_players()` at all three sites (`walk_action`,
     `walk_average`, `NSeatPolicy::lookup`);
   - `NSeatPolicy` carries its sealing kind/grid; `from_parts`
     extended with defaults; stage6 lookup caller unchanged.
3. Benchmark arm (`engine/benchmarks/subgame_nash_conv.cpp`):
   - process-active `Rfc0042Scheme`; all five walker kind sites
     parameterized;
   - `--rfc0042-ablation <lo> <hi> <wall_s> <S> [seed]` (three
     schemes), `--rfc0042-grid ...` (descriptive points),
     `--rfc0042-flop-probe [c1 c2 c3]`;
   - width-agnostic collision tracker replacing the 32-bit mask.
4. ctests (`engine/tests/test_abstraction.cpp`):
   - `equity_strength_ehs`: independent oracle exact-double equality,
     term counts 990 / 45,540 / 1,070,190, flop fan-out 1,176,
     lock/all-chop/order-invariance fixtures, malformed inputs, MC
     determinism/convergence, zero-draw rejection;
   - `equity_strength_bucket`: band table and max id 1,608, 3-arg
     refusal, other kinds identical on both entries, L-window only
     shifts ids, preflop 169 ids, river fan-out bin agreement, memo
     hits/separation/clear, digest distinctness, descriptive-grid
     windows and invalid-grid rejection.

Remaining Stage 2 evidence for the freeze addendum:

- [x] reference host named and exact per-fixture-board flop wall
      threshold predeclared (`--rfc0042-flop-probe`: 183.911 s vs the
      predeclared 600 s ceiling; evidence CSV committed);
- [x] equal-width vs equal-mass occupancy evaluated on the frozen
      reference deal set (full non-blocked holdings: 1.129/2.273 < 5x
      on 0/2 boards; equal-width retained; evidence CSV committed);
- [x] pilot: 5 paired seeds per shape at 10,000 ms (seed 777042);
      worst projected S=32 half-width 0.0096 BB on 3p-turn < 0.01;
      S=32 frozen, confirm base seed 888042;
- [x] freeze addendum appended to RFC 0042 (Section 1a);
- [x] mechanical gate evaluator `tools/rfc/rfc0042-gates.ts` +
      `bin/rfc0042-gates.mjs` (pilot/gate modes, BCa 10k, paired-t).

### Stage 3 — Paired ablation and mechanical gate decision (RUNNING)

### Stage 3 — Paired ablation and mechanical gate decision (COMPLETE: KEEP)

Ran 2026-10-30 (three parallel benchmark processes, OMP 14 each, base
seed 888042, S=32, 10,000 ms); 128 raw rows per scheme, zero declines:
`docs/plans/ablation/rfc0042-stage3-{tiers,tiersdraw,equity}.csv`.
`bin/rfc0042-gates.mjs gate` mechanically failed gates 1-4 for both
candidates: equity wins gate 1 decisively (2p-river d=0.2056 BB,
BCa LB 0.2040) and improves exact gaps on 3/4 shapes but fails the
per-shape river convergence gate (bucket sum 0.0986/0.3610 > 0.05)
and the turn byte gate (2.18x/2.29x > 2.0x); tiers-draw fails gate 1
(no river improvement), 3p-turn NI tightly, and 3p-river convergence.
The Keep-vs-Inconclusive predicate resolves to KEEP (the hinge
failures are structural seed means, not interval width); the
independent agent reproduced every statistic with its own Python BCa
implementation and approved the KEEP classification.

### Stage 4 — Post-decision disposition (COMPLETE)

Per Section 4 / AC5 and the no-legacy rule, all engine measurement
code was deleted: the enum value, all equity EHS/MC/memo machinery,
the 4/5-argument card_bucket entries and the 3-arg refusal case, the
NSeatTrainerLimits/NSeatPolicy fields and lookup threading, every
benchmark scheme/arm/CLI (`--rfc0042-*`), the width-agnostic identity
tracker reverted to the original 32-bit mask, and the two ctests.
Production stays at the nine-tier category scheme; the protected
build/resident/artifact/codec/serve paths were never modified.
Retained: this RFC with the Stage 3 amendment, the evidence CSVs, and
the evidence-analysis evaluator `tools/rfc/rfc0042-gates.ts` (+7 Node
tests and bin launcher; zero engine linkage) so the decision stays
reproducible — adjudicated and approved by the independent reviewer.

## Verification (Keep termination)

- `format`, full release build, full `ctest --preset release`,
  asan build + touched-path ctests, `npm run check` (27 Node tests),
  `npm run proto:check`, both doc/RFC checkers;
- benchmark-multistreet is part of the release ctest label run.

## Carried-forward finding (not a gate, not authorized work)

Equity-strength bucketing yields large exact-game gains but is not
convergible inside the 10-second online envelope on the shapes that
need it most (3p river exact == bucket at 0.361). It is a candidate
for the OFFLINE W4 blueprint/library track (no per-request wall),
which requires its own RFC and the eight-site flip process — not an
authorization to change production.

## Evidence locations

- Stage 3: `docs/plans/ablation/rfc0042-stage3-{tiers,tiersdraw,equity}.csv`;
- pilot: `rfc0042-pilot-{tiers,tiersdraw,equity}.csv`;
- Stage 2 probes: `rfc0042-flop-wall-probe.csv`, `rfc0042-occupancy.csv`;
- mechanical evaluator: `tools/rfc/rfc0042-gates.ts` (frozen historical
  tooling, retained for decision reproducibility).
