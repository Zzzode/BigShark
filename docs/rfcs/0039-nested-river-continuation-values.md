---
rfc: "0039"
subject: "Nested River Continuation Values for Multiway Turn Strategy"
status: "Accepted"
authors: "Claude (zzzode agent session 2026-10-27)"
created: "2026-10-27"
updated: "2026-10-27"
owners: "Engine resolver, engine tree, engine solver, engine host, engine benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0039: Nested River Continuation Values for Multiway Turn Strategy

## Summary

The RFC 0036 Phase B gates measured the current depth-limited turn candidate
and failed decisively: the river-boundary leaf model (rollout equity at a
complete board, which assumes no future betting) carries a summed model delta
of 0.402 BB heads-up and 6.161 BB at three seats against a 0.03 BB summed
gate, and the bounded on-demand solve for five/six seats converges only to
5.7-15.7 BB in-model NashConv against a 0.15 BB summed gate. This RFC adds
the missing river continuation value: a nested, range-conditioned river
subgame solved through the existing `NSeatTrainer`, warm-refined within one
turn solve via RFC 0012 initial state, and evaluated behind newly built,
default-deny serve/decline gates. RFC 0036 was only Accepted: its bounded
turn entry and host routing are not implemented, so building them in
default-deny form and wiring them only after measured gates pass is part of
this RFC. A persisted CV library and any learned estimator each require
their own later RFC with pre-declared triggers; no live routing changes
until the frozen quality gates pass.

## Motivation

Six-max GTO is complete through the flop (shipped 2..6-seat 100 BB preflop
profiles and the full-range BSFLIB03 six-way flop library) and has no
defensible turn/river strategy for multiway play. RFC 0036 bounded the
on-demand resolver design (`cap-1`, streets(1)) and measured it rather than
extrapolating:

- **M2 (leaf-model error), 2026-10-26, FAIL.** Full cap-1 turn-to-river
  reference (`DepthLimit::none()`) versus the streets(1) candidate at the
  same cap isolates the leaf functional form. Three-seed (101/202/303),
  100K-iteration model deltas with 95% t intervals: 0.402 BB summed at two
  seats (CI [0.183, 0.622]) and 6.161 BB at three seats (CI [4.793,
  7.529]) against the 0.03 BB summed gate. Evidence:
  `docs/plans/ablation/rfc0036-m2.csv`, `docs/plans/0036-implementation.md`
  (2026-10-26 entry).
- **M1 (bounded in-model NashConv), 2026-10-25, no-go at 5/6.** At the most
  iterations the 1,600 ms envelope permits, measured summed NashConv means
  over three seeds are 7.14 BB (five seats, river cap-1, 3,740 iterations),
  9.25 BB (six seats, river, 1,510), 5.73 BB (five seats, turn streets(1),
  2,690), and 15.73 BB (six seats, turn, 970), against the 0.15 BB summed
  gate — a 38-105x miss. No iteration count both fits and passes.

Both measurements identify the same missing object: the value of arriving
at the river with conditioned ranges under real river betting. Rollout
equity answers "who wins at showdown if nobody bets"; the missing object
answers "what is the equilibrium value of this river spot". This RFC
supplies it by solving the river subgame nested inside the turn solve — the
continuous re-solving alternative RFC 0036 named first when its gates
triggered.

## Goals

- G1: A range-conditioned continuation-value seam at the depth-limit river
  boundary, with per-seat per-combo marginal EV outputs (chip units inside
  the engine and cache matrix; BB only for reporting and thresholds, with
  1 BB = big blind = 2 chips in the RFC 0004 numeric profile) replacing the
  per-deal scalar assumption at that boundary.
- G2: A nested river value model using the existing `NSeatTrainer` on a
  river-rooted, River-terminal, one-street subgame that inherits the outer
  tree's exact action abstraction and the leaf's conditioned ranges.
- G3: Warm refinement of nested river solves across outer iterations within
  one turn solve through the RFC 0012 `NSeatInitialState`, keyed by leaf
  identity. Process-global or persisted reuse is out of scope until a
  measured trigger fires (see Stage 3).
- G4: Implement the missing RFC 0036 bounded-turn resolver entry and host
  routing in default-deny form, then re-measure on the frozen gates: M2
  leaf-model error and M1 in-model NashConv, 2..6 seats, multi-seed CIs,
  using the existing `bigshark-subgame-nash-conv` harnesses.
- G5: Enable routing only for shapes whose measured quality and latency
  gates pass; every other shape keeps today's `turn_root_unserved` /
  operational-fallback behavior.
- G6: Record pre-declared, evidence-based trigger conditions under which a
  persisted CV library or a learned estimator would be proposed, each under
  its own new RFC.

## Non-Goals

- No neural or learned value estimator in this RFC (see Alternatives).
- No new card or action abstraction; menus and caps are the outer tree's
  own.
- No process-global or persisted CV cache, and no new artifact format in
  Stages 1-2 (Stage 3 trigger only, separate RFC).
- No change to preflop profiles, flop libraries, or flop-rooted resolving.
- No change to the River Club wire protocol, the TypeScript adapter, live
  operation, or credentials.
- No multiplayer safety theorem. Multiplayer subgames have no two-player
  zero-sum safety guarantee; quality is established empirically by the
  frozen NashConv gates, as every prior multiway RFC does.
- No live data collection; all evidence is offline.

## Current State and Evidence

Verified against source on 2026-10-27:

- `engine/include/bs/resolver.hpp` defines `ResolveLimits` with an RFC 0024
  `tree::DepthLimit`. The tree builder creates `TerminalFrontier` leaves at
  street boundaries (`NodeKind::TerminalFrontier` in
  `engine/include/bs/abstract_tree.hpp`; leaf construction in
  `engine/src/tree/abstract_tree.cpp`).
- **RFC 0036 is Accepted, not Implemented.** In production there is no
  bounded-turn resolver entry, no serve/decline gate-constant table, and no
  `resolve_river_uniform`/`resolve_turn_uniform` symbol (grep: zero hits).
  A turn-rooted request reaches `engine/src/protocol/envelope.cpp:180`,
  which labels it `turn_root_unserved` and uses the operational fallback.
  The only RFC 0036 production landing is the Stage-0 build/train split in
  `engine/include/bs/subgame_resolver.hpp`; turn-rooted
  streets(1) trees exist solely in the M1/M2 benchmark
  (`engine/benchmarks/subgame_nash_conv.cpp`).
- In production `DepthLimit::streets(1)` is used only by `resolve_flop`
  (`engine/src/resolver/subgame_resolver.cpp`, flop-rooted board size 3).
  `SubgameResolver::resolve` builds a turn-rooted subgame with
  `DepthLimit::none()` and walks to the river for river decisions; the
  builder rejects `walk_to_river` with `max_streets < 2`.
- The frontier seam is `bs::gto::FrontierEvaluator`
  (`engine/include/bs/frontier.hpp`): `evaluate(board, hands, ledger)`
  returns one scalar chip utility per seat **for one joint deal**, with no
  range argument. Five `FrontierEvaluator` implementations exist:
  `NoFrontier`, the heads-up-only `DeclaredFrontierTable`,
  `EquityFrontierEvaluator` (the side-pot-safe rollout used by the
  streets(1) candidate M2 rejected), `PostflopFrontierEvaluator`
  (offline-declared flop-boundary table), and
  `OnTheFlyPostflopEvaluator` (`engine/include/bs/postflop_frontier.hpp:228`)
  which on a miss rebuilds a tree from an offline pre-trained policy, takes
  root values, and caches — but stays a per-deal scalar at the flop
  boundary consuming published policy, not an in-situ range-conditioned
  river matrix. `NSeatDeclaredFrontierTable`
  (`postflop_frontier.hpp:90`) is a data-table type, not an evaluator.
- The exact terminal continuation walk
  (`engine/src/resolver/continuation.cpp`) enumerates public-card
  continuations and settles fold/showdown per deal; it throws if an Action
  phase remains, which is the river-betting structure the nested model must
  replace at the boundary.
- Precedent this RFC must reuse, not duplicate: `extract_root_value`
  (`engine/include/bs/root_value_extractor.hpp`, RFC 0013) is the exact
  policy-walk kernel for per-traverser root value extraction; the boundary
  range export lives in `engine/include/bs/continuation_range.hpp`.
- `solver::NSeatTrainer` solves one-street river-rooted River-terminal
  subgames from arbitrary per-seat conditioned ranges; the M1/M2 harness
  has a `river_rooted=true` construction branch. It does **not** share the
  M2 reference construction — that arm is a full turn-to-river
  `DepthLimit::none()` tree whose river nodes are filled with the reference
  policy (`subgame_nash_conv.cpp:1555-1580`). Whether a converged nested
  model reproduces that reference within solver noise is therefore a
  testable proposition (AC2), not an existing fact.
- Warm state is RFC 0012's `NSeatInitialState`
  (`engine/include/bs/nseat_trainer.hpp:237-242`, passed to `train_nseat`),
  an in-memory per-node warm start. RFC 0028 sealed checkpoints persist
  offline training state and are not the mechanism for within-solve reuse.
- RFC 0037 exposes two distinct selectors (`engine/src/abstraction/
  abstraction.cpp:557-580`): `production_raise_cap` (heads-up standard =
  cap 0) and `production_flop_resolve_cap` (heads-up standard flop serving
  = cap 1). The M2 tree is fixed `ActionAbstraction::identity(1)` at both
  two and three seats. Re-classifying the shape at a river leaf could pick
  cap 0 heads-up and silently disagree with the cap-1 outer tree.
- Chance structure: the abstract tree emits 48 board-only river branches;
  per deal the legal river set is `48 - 2 * live_seats` (44/42/36 at
  2/3/6 seats) after blocking every live hand.

## Design Principles

- Measurement first: every routing decision follows the frozen
  `sixmax-gto-thresholds` numbers; the worst acceptable outcome at every
  shape is continued decline to the existing labeled fallback.
- Default deny: the bounded entry and host routing land unreachable /
  decline-by-default; a shape is wired only with its committed evidence
  row.
- One solver: the nested river is solved by the C++ `NSeatTrainer`. No
  parallel strategy implementation, no second language on the decision
  path.
- Exact identity, fail closed: nested input is reused only under a complete
  public identity; any mismatch is a fresh solve or a decline — never an
  approximate key match.
- Private independence: nested inputs are public; no hero hole card,
  timing, or opponent-specific information conditions the solve or its
  warm key.
- Local closure: the change belongs to resolver/tree/solver/host/benchmark
  modules, the same ownership RFC 0036 declared.

## Proposed Design

### Data flow

```mermaid
flowchart TD
    A[Turn decision request] --> B[Bounded turn entry, default-deny gates]
    B --> C[Streets(1) turn solve]
    C --> D[River TerminalFrontier leaf per river branch]
    D --> E[Exact conditioned leaf ranges]
    E -->|warm hit| F[Solve-scoped NSeatInitialState warm map]
    E -->|miss| G[Build river-rooted one-street subgame, outer abstraction]
    G --> H[NSeatTrainer cap-fixed CFR with conditioned ranges]
    H --> F
    F --> I[Per-seat per-combo range-conditioned marginal values]
    I --> C
    C --> J{Gate evaluation: M2/M1/latency}
    J -->|all pass for this shape| K[Serve nested-backed candidate]
    J -->|otherwise| L[Decline: turn_root_unserved / operational fallback]
```

### 1. Range-conditioned continuation-value seam

A new interface in the resolver module. `FrontierEvaluator` is deliberately
not widened: its per-deal scalar contract remains valid for equity leaves
and declared/postflop tables, and all five existing implementations speak
that contract.

```text
ContinuationValueModel
  identity() -> model/abstraction identity string
  value(leaf: RiverBoundaryLeaf,
        ranges: per-seat combo conditioned weights at the leaf,
        budget: ContinuationBudget)
       -> ContinuationValues | status{MISS, DECLINE, INVALID}

ContinuationValues
  per-seat per-combo marginal EV at the river root (chips; converted to BB
  only in reports and gate checks)
  inner_completed_iterations, inner convergence diagnostic, warm flag
```

`RiverBoundaryLeaf` is the full public identity: ordered five-card board,
live seats and folded flags, per-seat stacks and street contributions, pot
layers, the outer action-abstraction digest (RFC 0030), and the
terminal/utility identifiers.

**Semantics (M3).** The output for seat p and combo c is the range-
conditioned marginal

```text
V_p(c) = E_{opponent deals ~ conditioned leaf ranges}[ U_p(c, .) under the
          inner subgame average strategy ]
```

It is an expectation over opponent deals drawn from the conditioned leaf
ranges, never the value of one fixed joint deal. A fixed-deal
`extract_root_value` scalar would condition on opponents' exact holdings,
which is private-information leakage and changes the game; it is not a
substitute. The marginal computation reuses the RFC 0013 `extract_root_value`
walk kernel over the solved river tree and sums over the opponent deal
support: exhaustive enumeration while the deal count is within the bounded
fixture/menu size used by the existing NashConv harnesses, and the same
sampled-deal estimator with declared sample counts otherwise (sampling
error reported, never hidden). Zero-mass combos receive no value and are
absent from the matrix; a leaf whose every combo has zero mass is INVALID
and the outer candidate is discarded.

The outer sampled-MCCFR traversal samples one joint deal and one river
card per sweep, so conditioned ranges are **not** estimated from an
empirical observation window: at each outer sweep the trainer computes the
leaf's exact counterfactual reach weights from the current outer strategy
along the traversed trunk (the existing `continuation_range` export
machinery), and the nested solve for that leaf refines from its prior warm
state. The outer chance structure is unchanged: one river-boundary leaf
exists per board-only river branch, each with its own blocked-card ranges;
per-deal legal river support (48 − 2·live seats) and outer chance weighting
feed the update exactly as today.

### 2. Nested river value model

`NestedRiverValueModel` constructs, for each leaf miss:

1. a river-rooted (`board_size = 5`, `preflop = false`,
   `terminal = River`) `GameDef` with the leaf's pot layers, stacks, and
   live seats;
2. the action menu by reconstructing **the outer tree's own
   `ActionAbstraction` id** on the river GameDef — no shape
   re-classification at the leaf. A digest assertion must confirm the
   nested menu equals the outer tree's RFC 0030 digest; inequality is a
   DECLINE. This removes the heads-up cap-0/cap-1 selector ambiguity by
   construction: the M2 geometry is cap 1 everywhere and the nested model
   inherits it;
3. initial ranges equal the exact conditioned leaf ranges;
4. solves with `NSeatTrainer` under the nested budget, passing the prior
   `NSeatInitialState` when the leaf was visited in an earlier outer sweep,
   and extracts root marginal values via the RFC 0013 walk kernel.

The nested tree contains exactly one betting street: the smallest subgame
that still contains the betting M2 proved missing.

### 3. Budgets and the convergence predicate (M3)

`ContinuationBudget` is derived from one shared outer envelope:

- inner plus outer wall time share the request-derived turn budget; the
  model receives a per-refine wall/node cap such that a configured maximum
  number of leaf refinements per outer sweep cannot exceed the envelope;
- the iteration projection uses the RFC 0036 Stage-0 measured
  node/action/seat per-iteration cost coefficients for these geometries
  (the calibration behind the M1 envelope table), **not**
  `iteration_cap_for_budget`, whose deals-based formula structurally
  projects to zero iterations for full-range multiway games;
- all inputs are public (budget, leaf counts, menu, seats), so the same
  nested budget derives for every hero combo.

The nested solve is accepted only when an absolute inner best-response
gap measurement passes: the inner BR gap measured by the pooled walker on
the nested tree (exhaustive enumeration on bounded fixtures; the RFC 0036
sampled BR estimator with the upper CI bound on full-range shapes) must be
below a pre-registered tolerance (Stage 1 fixture value 0.01 BB summed).
A refinement-stall observation — improvement over the last refinement
window below a declared floor after a minimum iteration count — may only
**early-stop further refinement once an absolute gap measurement has
already passed**; it is never on its own an acceptance condition. For
4/5/6 shapes there is no full reference and the M1 in-model walker shares
the nested model, so it structurally cannot see leaf-form error; the
inner absolute gap is therefore the only leaf-quality evidence, and a
shape whose nested leaves have never produced a passing absolute gap is
DECLINED (never published) regardless of stall. This preserves the
fail-closed semantics RFC 0036 applied to wall-partial runs.

### 4. Warm reuse scope (M4)

Stages 1-2 keep one solve-scoped map: leaf identity →
`NSeatInitialState` plus a convergence tag, alive for one bounded turn
solve and released with it. Consecutive outer sweeps refine the same
leaves, which is where reuse actually occurs; conditioned ranges drift
between sweeps, and the warm state is a refinement start, never a reason
to skip the convergence predicate.

Warm-state reuse across drifting ranges does NOT inherit RFC 0012's
bitwise-identical warm-start guarantee, which holds only when ranges are
unchanged. It is a heuristic starting point: before reuse, the nested
solver must validate the initial state's revision/batch and the exact row
shape (menu identity, node indexing, action count) against the current
river subgame; any mismatch forces a cold start. The warm-versus-cold
comparison in the verification plan demonstrates the refinement benefit
under drift; the absolute gap gate protects quality either way.

A process-global or persisted cache is not built: leaf identities embed
conditioned ranges that change every sweep, and the 48 river branches plus
the per-request turn ranges give no demonstrated byte-identical reuse
across requests. Promotion to a process cache or an immutable packed CV
library requires measured hit-rate and latency evidence and is Stage 3
under a separate RFC. Nested solve and warm hit/miss/refine counts go only
through existing diagnostic strings and local logs — **no new wire
fields** in Stages 1-2.

### 5. Bounded entry, host routing, and measurement (G4/G5)

- Implement the RFC 0036 bounded-turn resolver entry and its
  `apps/engine-host/main.cpp` wiring behind a default-deny gate structure
  that initially declines every 3+ seat turn/river request (preserving
  today's `turn_root_unserved` label) and exercises the nested path only
  from the benchmark/measurement drivers.
- **M2 rerun**: extend `bigshark-subgame-nash-conv --rfc0036-m2` with a
  nested leaf arm. Sanity identity: with the inner solve driven to the
  convergence predicate, nested turn values reproduce the full-solve
  reference within a pinned tolerance on the existing 2/3-seat M2 fixtures
  (10-hand sub-ranges; tolerance 0.005 BB per combo marginal value and the
  frozen 0.01/0.03 BB M2 thresholds). Pass rule unchanged: per-seat gap
  below 0.01 BB and summed below 0.03 BB, upper 95% CI bound inside, three
  seeds (101/202/303).
- **M1 rerun**: `--rfc0036-m1-envelope` on nested-backed bounded trees at
  4/5/6 seats against `epsilon_seat = 0.05` and `epsilon_sum = 0.15 BB`
  with the existing family-wise CI procedure, at the outer iterations the
  1,600 ms envelope permits.
- **Latency gate**: receipt-to-action wall time on the host path, measured
  through the envelope handler's existing `solve_time_us` stamping in an
  offline host replay (same mechanism RFC 0023 used); no live traffic.
- Routing a shape is a gate-table change shipped together with its
  committed CSV row under `docs/plans/ablation/`.

### 6. Trigger-gated later stages

- **Stage 3 — separate RFC**: persisted/process CV cache or packed CV
  library. Trigger requires measured nested miss frequency, warm-refine
  cost, and latency distributions showing quality passes but serving
  misses the envelope; the RFC owns the format and artifact contract. No
  scope amendment promotes it under this RFC.
- **Stage 4 — separate RFC**: learned CV estimator, only if an exact
  table is measured infeasible, including training-data provenance, C++
  inference contract, RFC 0031-style abstraction loss measurement, and
  gates. The vendored Fullhouse network in an external client is evidence
  that small postflop networks exist, not an input to our engine.

## Dependency Rules

- New code lives under `engine/src/resolver/`, `engine/src/tree/`
  (boundary reach aggregation hooks), `engine/src/gto/` (trainer/
  extractor reuse), `apps/engine-host/` (default-deny routing only), and
  `engine/benchmarks/`.
- Resolver→solver/tree/poker/artifact identity directions are unchanged;
  nothing new depends upward on the service protocol or TypeScript layers.
- `FrontierEvaluator` keeps its contract; the nested seam is separate. No
  wire-protocol or persisted-format change in Stages 1-2.
- No dependency on any external model, network service, or non-C++ runtime.

## Compatibility and Migration

- Stage 1 is additive measurement code. Stage 2's default-deny host wiring
  preserves current observable behavior (`turn_root_unserved` /
  operational fallback, identical receipts) until a shape passes.
- Enabling a passing shape is a gate-table change shipped with measured
  evidence, the path RFC 0036 task 11 anticipated — but the bounded entry
  itself is new code, not a constants-only edit.
- No persisted data migration; Stage 3 formats arrive additive under a new
  RFC.

## Security and Operational Impact

- Offline only; no credentials, network calls, or live data.
- Nested CPU/memory use stays inside the bounded resolve: explicit nested
  budgets, resource exhaustion mapped to existing `Ineligible`/decline
  outcomes, and local diagnostic counts of nested solves and warm
  hit/miss/refine for reviewability.
- No hidden information: leaf ranges derive from public actions and
  policies, keys are public, no opponent profiling is introduced, and the
  marginal-value definition forbids conditioning on exact opponent
  holdings.
- Fairness: values remain symmetric policy expectations, never
  opponent-adaptive deviations.

## Alternatives Considered

1. **Learned river value/policy network (the external Fullhouse client
   approach).** Requires a data-generation campaign, a non-C++ training
   stack, a C++ inference contract, and measured abstraction error before
   any gate can run. The exact nested model needs none of it and is the
   natural data source if a network is later justified. Rejected;
   trigger-gated as Stage 4.
2. **More iterations or a larger envelope.** Ruled out by M2: the error is
   the leaf functional form, not convergence; heads-up deep fails at
   0.4 BB versus 0.03, and M2 compared equal cap-1 arms, so a cap change is
   equally irrelevant.
3. **RFC 0011 Phase 3 virtual deals (`TurnDeal`/`RiverDeal`,
   `walk_virtual_deal`).** They accelerate offline chance traversal; they
   do not supply a betting continuation value. They remain compatible as
   future Stage-3 table-production acceleration.
4. **Widening `FrontierEvaluator` with ranges.** All five existing
   implementers and their callers speak the per-deal scalar contract;
   changing it would blur deal-level equity versus range-conditioned
   equilibrium value. The one concrete caller needing the new contract
   (river boundary of a bounded turn solve) justifies a separate seam.
5. **Reusing `OnTheFlyPostflopEvaluator` directly.** It rebuilds from a
   pre-trained published policy and returns per-deal scalars at the flop
   boundary; the nested model solves in place from conditioned leaf ranges
   and returns marginals at the river boundary. Reusing its cache shape
   without its contract would hide that difference.

## Risks

- **Five/six-seat live feasibility is, on current numbers, expected to
  fail.** Stage 0 measured ~35,760 frontier leaves for six-seat turn
  streets(1) while the envelope affords ~970 outer iterations, and each
  inner river solve is a ~4,680-infoset tree at roughly the measured
  millisecond-scale per inner iteration. Continued decline after Stage 2 is
  a plausible and acceptable outcome; the offline quality evidence still
  decides Stage 3's trigger honestly.
- **Inner/outer convergence coupling.** Insufficient inner refinement
  biases the outer strategy. Mitigations: the objective convergence
  predicate, a fixture-level inner convergence-series test (recorded gap
  versus inner iterations), candidate discard on failure, and the AC2
  sanity identity.
- **Sampled-marginal error under sampled opponent deals.** Mitigations:
  exhaustive enumeration on bounded fixtures, declared sample counts and
  reported error elsewhere, and gating on the upper CI bound.
- **Multi-equilibrium ambiguity at three seats.** Different equilibria can
  yield different value vectors; deterministic public seeds and multi-seed
  spread reporting follow the project's multiway convention. If the
  0.01 BB sanity band proves unreachable even with exact solutions, Stage 1
  returns to RFC review rather than loosening the gate.
- **Cache-key incompleteness.** Keys reuse the RFC 0030 abstraction digest
  plus full ledger identity; tests mutate every component (board card, pot
  layer, stack, menu, range weight) and must miss.
- **Scope growth.** Stages 3/4 are separate RFCs; there is no self-amending
  scope clause and no feature flag selecting old/new leaf models.

## Verification Plan

- Native unit tests:
  - nested marginals on the small 2-seat river game equal a direct full
    solve of the same game within the pinned 0.005 BB-per-combo tolerance;
  - 3-seat side-pot/all-in leaves settle exactly (RFC 0010/0018 baselines);
  - outer/nested abstraction digest mismatch declines;
  - empty or zero-mass leaf ranges and resource exhaustion decline without
    publishing a candidate;
  - private independence: identical public states with different hero holes
    produce identical leaf keys, ranges, and values;
  - warm refinement converges from `NSeatInitialState` where a cold start
    with the same total budget is measurably worse.
- Measurement: nested-arm M2 (2/3 seats, three seeds, frozen thresholds)
  and M1 (4/5/6 seats) with CSV evidence; host-replay latency distribution
  on the existing envelope timing path.
- Full Engine Build Contract: release and asan presets, ctest,
  benchmark-multistreet, `npm run check`, `npm run proto:check`,
  `node bin/replay.mjs`, `check-rfcs`, `check-docs`.

## Rollout Plan

1. **Stage 1 — seam, nested model, warm map; 2/3 seats.** Add the
   interface, nested model, exact conditioned-range export, and the M2
   nested arm, reachable from the benchmark driver only. Stop condition:
   M2 passes at 2/3 seats (with the sanity identity) or a documented
   design-level failure returns this RFC to review.
2. **Stage 2 — bounded entry, 4/5/6 seats, host wiring.** Land the RFC
   0036 bounded-turn entry default-deny, run M1 and host-replay latency,
   then enable exactly the shapes whose evidence passes every gate. Stop
   condition: one committed evidence row per shape; no shape enabled
   without one.
3. **Stage 3 (separate RFC, trigger-gated)** process/persisted CV reuse;
   **Stage 4 (separate RFC, trigger-gated)** learned estimator.

## Rollback Plan

Stages 1-2 are additive behind default-deny gates; rollback is a revert
with no artifact, protocol, or data effect. Any shape enabled in Stage 2
is disabled by restoring the default-deny gate table; the operational
fallback path is never removed in this RFC.

## Open Questions

- None for Stages 1-2. Stage 3 storage and Stage 4 estimator design open
  only when their measured triggers fire.

## Acceptance Criteria

- AC1: A range-conditioned continuation-value seam exists separately from
  `FrontierEvaluator`, with the marginal-value semantics, identity, and
  MISS/DECLINE/INVALID statuses specified; the equity leaf is not used at
  the river boundary of the nested turn path.
- AC2: Nested river solves use `NSeatTrainer` with the outer tree's
  abstraction (digest-asserted) and exact conditioned leaf ranges; on the
  pinned 2/3-seat M2 fixtures (10-hand sub-ranges, BB units), converged
  nested marginal values match a direct full solve within 0.005 BB per
  combo, and M2 passes the frozen 0.01/0.03 BB thresholds over seeds
  101/202/303 with CI bounds inside.
- AC3: M1 at 4/5/6 seats is re-measured via `--rfc0036-m1-envelope` and
  recorded; no shape is routed unless its summed NashConv upper CI bound
  is within 0.15 BB AND every enabled shape has a committed evidence row
  of a passing nested-leaf absolute inner BR gap (exact pooled walker on
  enumerable fixtures; sampled BR estimator with upper CI bound for full
  ranges). Stall-plus-minimum-iterations without such a passing absolute
  measurement never enables a shape.
- AC4: Warm reuse is solve-scoped and identity-exact; key-component
  mutation tests miss; a warm state whose revision/batch or row shape
  (menu, node indexing, action count) mismatches cold-starts; nested
  budget exhaustion and absolute-gap failure decline without publishing a
  candidate.
- AC5: The bounded-turn entry and host routing land default-deny and
  preserve today's labels/receipts; routing changes are gate-table edits
  shipped with per-shape CSV evidence and host-replay latency rows; every
  other shape's observable behavior is unchanged, with no new wire fields.
- AC6: Full Engine Build Contract passes in release and ASan on touched
  surfaces; raw rows commit under `docs/plans/ablation/`.
- AC7: Documentation truth updated (`docs/design/gto-engine.md`, RFC 0036
  status/evidence, delivery plan) in the same change as the code.

## Decision

Author agent: Claude (zzzode agent session 2026-10-27)
Approved by: rfc0039-approval-1 (independent approval agent, auto_model/alwaysday1_max)
Decision date: 2026-10-27
Review outcome: Approved
Reviewed scope: Full RFC 0039 text across three review rounds, verified against engine source (resolver.hpp/subgame_resolver.cpp, frontier.hpp, postflop_frontier.hpp, root_value_extractor.hpp, continuation_range.hpp, abstract_tree.hpp, nseat_trainer.hpp NSeatInitialState, abstraction.cpp cap selectors, envelope.cpp turn_root_unserved, main.cpp host wiring, subgame_nash_conv.cpp M1/M2 harnesses) and the frozen M1/M2 CSV evidence.
Review summary: Three rounds. Round 1 Changes Requested (2 blockers, 5 major, 7 minor); round 2 Changes Requested (1 blocking gate-integrity finding); round 3 Approved after the absolute inner-BR-gap acceptance rule and all other findings were resolved.

Round 1 returned Changes Requested (2 blockers, 5 major, 7 minor):
fabricated default-deny gate/current-state claims (RFC 0036 is Accepted
only; bounded entry, gate table, and resolve_*_uniform are absent; turn
requests hit `turn_root_unserved` at `envelope.cpp:180`), false seam
claims (production streets(1) use; M2 reference construction), omitted
precedents (`OnTheFlyPostflopEvaluator`, `extract_root_value`,
`continuation_range`), heads-up cap-0/cap-1 selector ambiguity,
underspecified marginal/budget/convergence contracts, a speculative
process/persisted cache, a self-amending Stage 3, and telemetry/wire
tension. Round 2 returned Changes Requested for one blocking major: a
stall-based inner-solve acceptance branch could publish 4/5/6 shapes with
no absolute leaf-quality evidence because the M1 walker shares the leaf
model by construction. Round 3 verified the resolutions: acceptance
solely on an absolute inner BR gap (pooled walker; sampled BR estimator
plus CI for full ranges; stall only early-stops refinement after a passed
gap; never-passed means DECLINE, never PUBLISH), outer abstraction digest
pinned at nested tree build, the V_p(c) range-conditioned marginal with
its private-information argument, exact per-sweep trunk counterfactual
reaches, a solve-scoped `NSeatInitialState` warm map with the drift
caveat and cold-start validation, the default-deny bounded entry and host
wiring scoped as new code with engine host in owners, Stages 3 and 4 each
a separate RFC with pre-declared triggers, zero wire/persistence changes
in Stages 1-2, chips/BB units pinned, and AC2-AC4 objective and
evidence-backed. `check-rfcs` (39) and `check-docs` (436) were green at
approval.

Approved scope: the separate `ContinuationValueModel` seam, the nested
`NSeatTrainer` river model, solve-scoped warm refinement, the
default-deny bounded-turn entry plus host wiring, and the nested M2/M1
measurement arms (Stages 1-2). The approval does not cover persistence or
protocol changes (Stage 3 needs its own RFC; Stage 4 a separate RFC after
that), live play, credentials, or spending. Accepted residual risks:
5/6-seat serving is expected to keep declining on the measured arithmetic
(~35,760 frontier leaves versus ~970 outer iterations versus ~4,680-infoset
inner trees) — continued decline is an acceptable Stage 2 outcome;
empirical sampled-marginal error; multi-equilibrium value ambiguity at
three seats, where Stage 1 returns to review rather than loosening the
0.01 BB band. The full Engine Build Contract (release and asan presets,
ctest, benchmark-multistreet, npm checks, proto check, replay) is required
before this RFC may become Implemented.

**Factual errata (2026-10-27, [RFC 0040](0040-retire-river-club-agent-api.md)):**
the `replay` step named both in this RFC's Verification Plan and in the
approved build contract was removed by RFC 0040's retirement of the River
Club Agent API v2.1 integration and the journal replay tool. Stage-2
latency evidence is the C++ envelope `solve_time_us` timing harness; the
remaining build-contract gates are unchanged.
