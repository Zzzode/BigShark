---
rfc: "0036"
subject: "Six-Way Turn and River On-Demand Strategy"
status: "Accepted"
authors: "BigShark maintainers"
created: "2026-10-24"
updated: "2026-10-24"
owners: "engine resolver, engine solver, engine tree"
supersedes: ""
superseded-by: ""
---

# RFC 0036: Six-Way Turn and River On-Demand Strategy

## Summary

The genuine unserved spot is a **four- to six-seat, street-start turn**
decision with no blueprint: the no-blueprint host chain builds an uncapped
identity tree, which the T3 feasibility sweep proved 4-6 seat turn-rooted
games cannot construct at 25 BB or 100 BB, so the decision is labelled
`turn_root_unserved` and takes the operational fallback. The bounded
configuration that makes such trees affordable — `identity(max_raises=1)`
plus `DepthLimit::streets(1)` and the N-seat equity frontier — exists today
only on the flop path. This RFC adds the board-4 street-start entry and the
host routing, reusing the mechanism RFC 0024 already designed and RFC 0030
already capped; it is wiring plus honest labeling and measured gates, not a
new strategy core.

The river is a smaller change. `resolve_uniform` already accepts 2..10-seat
river nodes (`board_size == 5`) and serves them whenever the uncapped tree
builds and trains inside the deadline; RFC 0011 measured the four-seat
river tree as cheap (2,829 nodes, 1,000 iterations in 66 ms), and that
wide-menu path is preserved. This RFC adds an explicitly bounded cap-1
river variant only for seats 5 and 6, where the tree cost is unmeasured, so
river service has a guaranteed-buildable, deadline-projected form with its
own diagnostic label, and puts both new paths behind an explicit
serve/decline quality gate that the existing partial-publication path does
not have.

Turn value is approximate by construction: the depth-limited tree models the
turn betting street and an explicit 48-card river chance, but leaves river
Action nodes valued as immediate five-card showdown equity (all-in-at-river
value). The approximation is labelled on every receipt and is promoted only
if two separately measured errors — in-model convergence and the
river-betting model error — pass pre-declared numeric gates. No turn/river
library, no neural estimator, and no frontier-interface widening ships in
this RFC.

## Motivation

- The turn gap is concrete. With no blueprint, the host calls
  `resolve_uniform` (board 4 or 5, uncapped identity tree, no depth limit),
  then `resolve_flop` (which is ineligible off board size 3), and finally
  emits Unsupported → the `turn_root_unserved` fallback decision
  (`apps/engine-host/main.cpp:187-227`; `engine/src/protocol/envelope.cpp:171-188`).
  The T3 uncapped sweep showed 4-6 seat turn-rooted full trees exceed the
  builder byte/node caps at 25 BB and 100 BB
  (`docs/plans/t3-six-seat-feasibility.md`).
- The affordable configuration exists on the flop. `resolve_flop` builds
  `identity(max_raises_per_street=1)` with `DepthLimit::streets(1)` and the
  equity frontier (`engine/src/resolver/subgame_resolver.cpp:474-544`);
  RFC 0024 designed the identical turn-rooted cut and RFC 0030 pinned the
  cap into the action digest. The missing pieces are the board-4 entry, the
  host routing, cost gating, and labels.
- The machinery is N-seat. `EquityFrontierEvaluator` accepts 2..10 seats,
  3/4/5-card boards, and settles per side-pot layer
  (`engine/src/gto/equity_frontier.cpp:91-236`); the tree builder, the
  trainer walk, and the resolver all accept six seats.

## Goals

- **G1**: A five- or six-seat **river** decision is served on demand through
  a chance-free, river-rooted `identity(cap=1)` tree, offering the complete
  first-order menu (fold/check/call plus bet and raise), with at most one
  full re-raise per street, with exact fold/showdown settlement. Four-seat
  river decisions keep the existing uncapped path.
- **G2**: A four- to six-seat, **street-start turn** decision is served on
  demand from a turn-rooted `identity(cap=1)`, `streets(1)` tree whose river
  Action leaves are valued as five-card showdown equity by the existing
  N-seat frontier, with the approximation labelled on the receipt.
- **G3**: G1/G2 are routed only while measured to clear a serve/decline gate
  inside the live deadline. If the calibration measurement shows the gate
  cannot be met, the path stays unrouted and the board keeps
  `turn_root_unserved`; feasibility is a measured outcome, not an
  assumption.
- **G4**: Two errors are measured before promotion: (M1) convergence inside
  the depth-limited game and (M2) the model error of treating river play as
  all-in equity, measured against a full river-betting reference.

## Non-Goals

- No full turn/river class library (the terabyte-class option rejected by
  RFC 0032).
- No neural or learned value estimator; adopted only if the measured
  all-in-equity model error fails the gates.
- No widening of
  `FrontierEvaluator::evaluate(board, hands, TerminalPayload)`: cuts are
  taken only at closed-street boundaries where the ledger payload suffices.
- No service for **mid-turn facing-a-bet** nodes at 4+ seats. They keep the
  existing chain (blueprint `resolve`, then uncapped `resolve_uniform`, then
  fallback). Mid-street cuts are precisely where a richer payload would be
  needed; they belong to a follow-up.
- No change to two- and three-seat behavior, to the blueprint `resolve`
  path, or to any persisted format.
- No multiway equilibrium certification: at N ≥ 3 the receipt stays an
  approximate label, never a per-seat non-regression certificate.

## Current State and Evidence

- `resolve_uniform` roots at the live node (board 4 or 5, 2..10 seats) with
  an uncapped identity abstraction and no depth limit
  (`subgame_resolver.cpp:418-472`). `resolve_flop` is the only cap-plus-depth
  path and refuses boards other than size 3 (`:474-544`). The shared
  `solve_rooted_subgame` builds, trains, and extracts
  (`:634-743`) and rejects `streets(1)` for a river decision
  (`:644-647`).
- The tree builder already emits `TerminalFrontier` leaves at a closed
  street boundary (`engine/src/tree/abstract_tree.cpp:322-332`); the
  trainer values them through the injected evaluator
  (`engine/src/gto/nseat_trainer.cpp:401-412`); a five-card leaf is direct
  showdown equity with no runout enumeration
  (`equity_frontier.cpp:194-196`). The turn path still constructs the
  ordinary 48-outcome river Chance node, sampled externally by the trainer
  (`nseat_trainer.cpp:507-528`); what is absent is only RFC 0011 Phase 3's
  in-walk virtual dealing.
- RFC 0030's `raises_this_street` continuation exists for mid-street roots
  (`build_subgame_def_from_node`, `subgame_resolver.cpp:386-393`). A
  street-start root has every `street_committed == 0`, seeds
  `raises_this_street = 0`, and never exercises that continuation; this RFC
  only serves street-start turn roots.
- **Deadline semantics today publish partial policies.** On wall expiry the
  trainer breaks and returns a `WallClock` partial after one or more
  iterations; only zero completed iterations throws and maps to
  `SolveDeadline` (`nseat_trainer.cpp:1041-1066`). `ResourceLimit` is never
  produced by the trainer — info-set/byte limits throw directly
  (`nseat_trainer.cpp:261,293`) — and the resolver treats `WallClock`
  results as publishable whenever the extracted candidate is non-empty
  (`subgame_resolver.cpp:684-743`). The new paths do not inherit this
  behavior; see the serve/decline gate below.
- **The cheap-tree evidence uses a coarser menu than this RFC ships.** The
  T3-DL-CAP figures (six-seat turn tree 9,361 nodes / 9.1 MiB; feasibility
  across 2..6 seats at 10/25/100 BB) were measured with a declared-only
  menu: one half-pot bet and one half-pot raise
  (`engine/benchmarks/subgame_nash_conv.cpp:1157-1167`,
  `docs/plans/t3-six-seat-feasibility.md:13,57`). The identity menu this RFC
  serves seeds three bets and two raises per street before the cap
  (`engine/src/abstraction/abstraction.cpp:100-109`). The only identity
  cap-1 six-seat data point is the flop tree at roughly 466K nodes
  (`subgame_resolver.cpp:531-538`); an identity cap-1 turn tree has never
  been built or measured.
- **The quality evidence uses a toy range.** The T3 campaign used ten
  representative hands per seat (1,440 card-compatible six-seat joint
  deals); six seats reached the 60 s wall at roughly 60K iterations
  (~1,000 iterations/s) with 0.020 BB pooled-walker exploitability
  (`docs/plans/t3-six-seat-feasibility.md:349-385`). Production uniform
  priors are every board-unblocked combo — 1,128 combos per seat on a turn
  board and 1,081 on a river board
  (`subgame_resolver.cpp:404-417`) — so per-information-set visit density
  in the live window is far below the T3 fixture and is unmeasured.
- Live budget: 2,000 ms per decision envelope (p95 target 1,600 ms).
- The on/off switch already exists: `BIGSHARK_SUBGAME_UNIFORM` defaults ON
  (`apps/engine-host/main.cpp:121-124`). The new paths share that switch;
  no new flag is introduced.

## Design Principles

- **Reuse the N-seat frontier; build no storage.** Turn/river value comes
  from on-demand resolving with the existing equity evaluator.
- **Cut only at reconstructable boundaries.** A frontier cut is taken at a
  closed street (street closed, unmatched refunds applied), where the
  `TerminalPayload` ledger fully describes the root; no `GameState` crosses
  the evaluator seam.
- **Measure before routing.** A calibration spike with production menus and
  ranges gates host routing; a path that cannot clear the gate is not
  advertised.
- **Distinguish the approximations on the receipt.** A bounded river solve
  and an all-in-equity turn solve carry different diagnostic constants
  through the existing string slot; the wire gains no fields.
- **Do not burn a hopeless window.** Before training, the path projects
  affordable iterations from the built tree and declines without training
  when the projection cannot reach the minimum.

## Proposed Design

### Phase A — bounded river resolver for seats 5 and 6

Add `resolve_river_uniform`: a river-rooted (`board_size == 5`),
River-terminal, `identity(max_raises=1)` tree with no depth limit and no
chance node, trained from board-blocked uniform priors through `train_nseat`.
The injected `EquityFrontierEvaluator` is never invoked on this tree (there
are no `TerminalFrontier` leaves); it is passed only to keep the resolver's
construction uniform. Candidate extraction reuses the build/train seam
below with `walk_to_river = false`; the existing
`streets(1)`-on-a-river rejection is untouched because this path passes
`none()`. Seats 2-4 keep using the existing uncapped `resolve_uniform`
(see Host routing): RFC 0011 measured a four-seat uncapped river tree at
2,829 nodes / 1.4 MiB and 1,000 iterations in 66 ms, so the wide menu is
affordable there today and is not narrowed.

### Phase B — street-start turn resolver for seats 4 to 6

Add `resolve_turn_uniform`: accepted only when the node is street start
(predicate: every seat's `street_committed == 0`, matching the
`mid_street` reconstruction at `subgame_resolver.cpp:386-393`). It builds a
turn-rooted (`board_size == 4`), River-terminal, `identity(cap=1)` tree with
`DepthLimit::streets(1)`. The cut places `TerminalFrontier` leaves on river
Action nodes, which the equity evaluator settles as five-card showdown
equity; the ordinary 48-card river Chance node is still built and sampled.
Turn betting is therefore solved against an all-in river value: river
betting, bluff-catching, and bluff inducement are not modeled. A mid-turn
facing-a-bet node is ineligible for this entry point and keeps the existing
chain.

### Resolver seam: a behavior-preserving build/train split

The tree is currently constructed inside the shared
`solve_rooted_subgame` (`subgame_resolver.cpp:649-656`) and immediately
trained in the same function (`:667-684`); an entry point cannot observe
the real node count before training. This RFC adds an additive seam:
`solve_rooted_subgame` is split into a build step and a
train-and-extract step (equivalently, an overload accepting a prebuilt
tree), and the existing signature delegates to them. Behavior for the
three existing callers (`resolve`, `resolve_uniform`, `resolve_flop`) is
identical — same tree limits, same trainer call, same statuses, same
candidate extraction — and their tests pin that unchanged behavior. The
new entry points build first, run the gate, then train.

### Serve/decline gate

The new entry points do not inherit the current wall-partial publication
semantics. Each carries calibrated, per-seat constants, and the path
publishes only through all three checks:

1. **Pre-build screening.** Tree construction itself has no wall-clock
   limit (`TreeLimits` carries nodes/depth/bytes only). Before building,
   screen the projected node and byte totals against the configured caps
   and reserve a measured build-time share of the envelope. Stage 0
   records a measured node/byte table keyed by seat count and menu (cap-1
   tree depth proved stack-independent in T3, so a table upper bound with
   conservative margin is tighter than an analytic branching bound) and
   measures build time per node; T3 measured a 686K-node tree at 519 ms.
   A spot whose projection breaches either bound declines without
   building, so tree construction cannot by itself exhaust the window.
2. **Post-build, pre-train time projection.** After the tree is built
   (build failures map to `Ineligible`), project the iterations
   affordable in the remaining wall budget from a NODE/ACTION/SEAT-driven
   per-iteration cost, calibrated in stage 0 against actual builds (the
   T3 datapoint is the same shape: a 9,361-node six-seat tree trained at
   roughly 1,000 iterations/s, about one millisecond per iteration). This
   is deliberately not the gadget model in
   `resolver::iteration_cap_for_budget`: one `train_nseat` iteration is
   one traverser sweep over a SINGLE sampled joint deal
   (`nseat_trainer.cpp:1020-1029`), so its wall cost follows the tree
   walk and is independent of the total joint-deal population — plugging
   the roughly 10^18 compatible six-seat full-range deals into the
   gadget's `deals × (actions+1) × seats` formula would structurally
   project zero iterations. Only the helper's framework is borrowed:
   derive inputs from public quantities, use a conservative coefficient,
   and clamp to the configured cap.
3. **Quality floor.** The joint-deal and information-set populations
   enter here, not into the time projection: expected visits per
   information set are approximately
   `completed_iterations × seat_count / information_sets`. The
   pre-train information-set count is unknown, so the floor uses the
   conservative upper bound action-node count × bucket count (the N-seat
   key space is `{bucket, node}`); an underestimate biases toward
   decline, matching fail-closed. The post-train check re-validates
   visits against the actual `train_result.information_sets`. A
   projection below the calibrated minimum visits — or below
   `min_completed_iterations(seats)` — declines without training.
4. **Post-train minimum.** If the trainer returns fewer than
   `min_completed_iterations` (wall reached, readable from
   `SubgameResolveResult::completed_iterations`), discard the partial and
   return `SolveDeadline`; this check needs no helper change. A non-empty
   candidate is necessary but not sufficient.

The gate constants are committed during stage 0 with the measurements
that calibrate them; the new entries default to decline when no
calibrated constant exists for the seat count.

### Host routing and precedence

Under the existing `BIGSHARK_SUBGAME_UNIFORM` switch (default ON):

- Board size 5, seats 5 or 6: `resolve_river_uniform` first; on
  non-Solved, fall straight through to the `turn_root_unserved`/
  operational fallback. The uncapped `resolve_uniform` is not retried:
  its tree is a superset of the bounded one and cannot be cheaper.
- Board size 5, seats 2-4: the existing uncapped `resolve_uniform` chain
  is unchanged (the four-seat river tree is measured cheap, so its wide
  menu is preserved).
- Board size 4, street start, seats 4-6: `resolve_turn_uniform` first; on
  non-Solved, fall through to the fallback without an uncapped retry (the
  T3 uncapped sweep proved those trees fail at the builder cap at 25 BB
  and 100 BB).
- Board size 4, street start, seats 2/3: the existing uncapped
  `resolve_uniform` chain is unchanged.
- Board size 4, mid-street, any seat count: the existing chain
  (`resolve_uniform` then fallback) is unchanged.
- Board size 3: unchanged (`resolve_flop`).

This keeps every menu width stable for a given spot: a routed board always
uses the same abstraction, and the bounded paths never shadow the 2/3-seat
uncapped menu.

### Diagnostic labels

Two constants are added next to `kSubgameResolveToken`
(`engine/include/bs/resolver.hpp:108`) and attached through the existing
`BlueprintRow::diagnostic` string slot; no protobuf change:

- `subgame-resolve:river-bounded` — phase A, chance-free cap-1 river tree,
  exact settlement;
- `subgame-resolve:turn-allin-equity` — phase B, turn betting solved, river
  Action leaves valued as five-card showdown equity.

The existing `subgame-resolve:approximate` stays attached to the uncapped
uniform and flop paths. Receipts are therefore machine-distinguishable by
strength of model, satisfying the mislabeling control without a wire
change.

### Phase C — measured, not built

Real river continuation value at a turn cut needs either RFC 0011 Phase 3
virtual turn/river deals (`TurnDeal`/`RiverDeal` NodeKinds and
`walk_virtual_deal`) or a nested depth-limited re-solve with a
continuation-value cache. Stage 0/rollout measurement decides whether phase
C is required, using the numeric triggers in G4. If triggered, a separate
RFC extends this resolver seam, reusing the N-seat `JointDealDealer`, the
exact-oracle pooled walkers, and the stage-6 estimator.

## Dependency Rules

- The resolver owns the two new entry points and the behavior-preserving
  build/train split of `solve_rooted_subgame`; the tree, trainer, and
  frontier gain no new strategy surface beyond RFC 0024/0030.
- The host owns routing precedence and the fallback chain; the wire
  protocol gains no fields.
- No persisted format or artifact changes.

## Compatibility and Migration

- Additive entry points plus host routing; 2/3-seat, blueprint, mid-turn,
  flop, and four-seat river behavior is unchanged. The four-seat river
  keeps its existing uncapped wide menu deliberately: RFC 0011 measured
  that tree at 2,829 nodes and 1,000 iterations in 66 ms, so the bounded
  path is restricted to seats 5 and 6 rather than narrowing a spot that
  is already served cheaply.
- The build/train split is an internal refactor of
  `solve_rooted_subgame` with the existing signature delegating to it;
  the three existing callers observe identical statuses and candidates,
  pinned by their current tests plus a focused regression run.
- The two new diagnostic strings are additive values in an existing
  free-form field.
- One behavior change is disclosed for five- and six-seat river boards:
  today the uncapped path attempts them and may publish a wall-clock
  partial; after routing, the bounded path is tried first and the
  uncapped one is not retried, so if the bounded gate declines, the spot
  falls back instead of receiving a partial. This is expected to be rare
  (the bounded river tree is a strict subset of the uncapped one and the
  four-seat river tree is already cheap), and it is symmetric with the
  turn path's honest-fallback design.
- No data migration.

## Security and Operational Impact

On-demand CPU inside the existing deadline. Three checks cap the worst
case: a spot failing the node/byte or build-time pre-screen declines
without building; one that passes the build but projects below the
iteration or visits minimum declines without training; one that trains
below the floor is discarded. No new trust boundary.

## Alternatives Considered

1. **Full turn/river library.** Terabyte-class storage; rejected by RFC
   0032.
2. **Widen the frontier to carry GameState now.** Needed only for
   mid-street cuts with richer payloads; excluded by the street-start scope.
3. **Neural continuation value.** Deferred upstream; adopted only if the M2
   model error fails its gate.
4. **Keep publishing wall-partials on the new paths.** Rejected: full-range
   six-seat visits per information set are sparse, and an unmeasured
   partial policy is weaker than the honest labeled fallback.

## Risks

1. **The all-in-equity model error is large six-way**, especially in
   multi-way big pots where river betting changes values most. Accepted as
   a labelled approximation; M2 and the numeric trigger decide phase C.
2. **The six-seat turn tree cannot clear the gate inside 2,000 ms at the
   identity menu.** Then G2 is not routed; the pre-build screen and
   post-build projection make the decline cheap and the fallback remains.
   This is an honest measured outcome, not a regression (those boards are
   unserved today).
3. **Uniform same-distribution priors bias six-way strategy**, with no
   blueprint to correct them. The approximate label stays long-term;
   line-conditioned priors are separate work.
4. **Cap-1 is a substantial menu coarsening** (no second re-raise within a
   street). The `river-bounded`/`turn-allin-equity` labels expose it; at
   five/six river and four-six turn seats no menu is being replaced (the
   uncapped trees fail the builder cap for turn, and the five/six river
   tree is unmeasured), while four-seat river keeps its proven uncapped
   menu.
5. **M2 is a proxy at six seats.** The full river-betting game is
   solvable only for 2/3 seats, so a passing M2 does not certify the
   six-seat model error; phase C may still be required on live or sampled
   evidence regardless of the proxy result.

## Verification Plan

### Stage 0 — calibration spike (before any host routing)

Using production configuration — the identity action family with cap 1 and
the default size schedule, and full board-blocked uniform priors
(1,128/1,081 combos per seat):

- Build bounded river trees (seats 5/6; 2/3/4 as cross-checks) and
  street-start turn trees at 2..6 seats; record node counts, build time,
  bytes, and information-set counts, and calibrate both gate coefficients:
  build time per node and the node/action/seat-driven training cost per
  iteration. The 2/3-seat figures cross-check the existing uncapped paths;
  six-seat turn is the unknown.
- Under a 2,000 ms wall (p95 target 1,600 ms), record completed iterations
  and per-information-set visits, and fix each
  `min_completed_iterations(seats)` constant at the smallest count whose M1
  measurement (below) has its confidence bound inside the threshold.
- Measure M1 and M2 defined below; record go/no-go for routing.

### Metrics

- **M1, in-model convergence.** NashConv inside the depth-limited game via
  the pooled best-response walker on enumerable sub-ranges (the existing
  subgame harness), 2..6 seats. Full-range six-seat games are not
  enumerable (the Cartesian of 1,128-combo ranges is intractable); the
  sampled best-response estimator that exists in the stage-6 measurement
  driver and `test_stage6_br_estimator` is wired into the on-demand subgame
  harness as part of this RFC, with confidence intervals.
- **M2, river-betting model error.** Measured in 2/3-seat spots where the
  full game is solvable, under a fixed evaluation protocol: solve the full
  river-betting reference game; freeze the depth-limited policy's action
  distributions at the turn nodes; at river nodes, where the
  depth-limited tree stores no policy (they are frontier leaves), fill the
  reference policy; then compare turn-root values and turn action
  distributions between the composed policy and the full-tree reference.
  A second evaluation fills river nodes uniformly and is reported as a
  sensitivity bound on the fill choice. This measures the error M1 cannot
  see. The six-seat full game is not solvable, so six-seat M2 is not
  directly measurable; 2/3-seat M2 is used as a proxy, and passing it does
  not certify the six-seat model error.

Gates (numeric, pre-declared from `docs/plans/sixmax-gto-thresholds.md`):

- M1 may promote only while measured per-seat exploitability is below
  `epsilon_seat = 0.05 BB/hand` and the sum is below `epsilon_sum = 0.15
  BB/hand`, with the document's one-sided confidence treatment.
- Phase C is triggered when M2 exceeds 0.01 BB/hand at any measured seat —
  the scale the thresholds document identifies as 20% of `epsilon_seat`
  and within measurement noise — or 0.03 BB/hand summed.

### Test coverage

- New capped river fixtures for 5 and 6 seats and turn fixtures for 4, 5,
  and 6 seats, all-live and with 2-4 folded seats including a
  dead-money/side-pot layer; frontier settlement at four- and five-card
  cuts explicitly asserts the seat utility sum is zero (the trainer's
  existing zero-sum checks cover only fold/showdown paths,
  `nseat_trainer.cpp:419-424,442-447`).
- The cap suppresses the second re-raise; a street-start root seeds
  `raises_this_street == 0`; a mid-turn node is ineligible for the turn
  entry.
- A spot failing the pre-build node/byte screen declines without building;
  a built spot whose projected iterations or visits fall below the floor
  declines without training; a wall-returned partial below the minimum is
  discarded; a result at or above all gates is published with the matching
  diagnostic string; receipts for the two paths are distinguishable.
- The build/train split leaves the three existing callers behaviorally
  identical: their current resolver fixtures pass unchanged.
- Host precedence: bounded-first for board 5 at seats 5/6 and board 4
  street start at seats 4-6; the four-seat river and the 2/3-seat chains
  are unchanged; `BIGSHARK_SUBGAME_UNIFORM=0` disables the new paths.
- Full Engine Build Contract and an ASan run. Seat counts above six remain
  accepted by the N-seat machinery but are explicitly unmeasured.

## Rollout Plan

1. Stage 0 calibration spike and M1/M2 tooling (including wiring the
   sampled estimator into the subgame harness), plus the behavior-
   preserving build/train split; commit the gate constants and the
   go/no-go evidence. No host routing changes.
2. Phase A river entry, host board-5 routing for seats 5/6, labels, tests.
3. Phase B turn entry, host board-4 street-start routing for seats 4-6,
   zero-sum controls, labels, tests.
4. Promotion decision against the M1/M2 gates; a failed gate leaves the
   path declining to fallback and opens the phase C RFC.

## Rollback Plan

Each phase is additive behind the existing environment switch. Reverting
the routing restores `turn_root_unserved`; setting
`BIGSHARK_SUBGAME_UNIFORM=0` disables the paths operationally without a
revert.

## Open Questions

1. The calibrated `min_completed_iterations(seats)` values and the stage 0
   node/time figures — produced by the spike, not guessed.
2. Whether a no-blueprint mid-turn facing-a-bet node needs the richer
   GameState frontier in practice; tracked separately, excluded here.
3. Whether M2 passes at 2/3 seats and whether the sampled six-seat M1 is
   trustworthy at the live visit density; the numeric gates make this
   decidable.

## Acceptance Criteria

- **AC1**: `resolve_river_uniform` serves a 5/6-seat river decision from
  a chance-free cap-1 tree with exact settlement; a folded/side-pot case
  settles zero-sum. The four-seat river still resolves through the
  existing uncapped path.
- **AC2**: `resolve_turn_uniform` serves only street-start turn nodes
  (every `street_committed == 0`), builds the cap-1 `streets(1)` tree with
  48-outcome river chances and equity-valued river Action leaves, and
  rejects mid-turn nodes as ineligible.
- **AC3**: Both paths implement the pre-build screen, the post-build
  node-driven time projection with the visits floor, and the post-train
  minimum; below-gate cases return `SolveDeadline` (screen case: without
  building; projection case: without training) and never publish a
  partial; calibrated gate constants are committed with stage-0 evidence.
- **AC4**: Host precedence matches this RFC (bounded river at board 5 for
  seats 5/6; bounded turn at board 4 street start for seats 4-6; four-seat
  river and all 2/3-seat chains unchanged), the fallback label is
  preserved on decline, and `BIGSHARK_SUBGAME_UNIFORM=0` disables the
  paths.
- **AC5**: Receipts carry `subgame-resolve:river-bounded` versus
  `subgame-resolve:turn-allin-equity`, distinguishable from
  `subgame-resolve:approximate`, with no protobuf change.
- **AC6**: Stage 0 records production-menu/full-range node counts, build
  times, the calibrated per-node build and per-iteration coefficients,
  iterations and visits inside 2,000 ms for the routed seat counts; M1 is
  measured with the pooled (enumerable) and sampled (full-range)
  estimators and M2 under the defined freeze-turn/fill-river protocol at
  2/3 seats, with the uniform-fill sensitivity bound; the numeric gates
  are evaluated and recorded, and the 2/3-seat M2 is explicitly recorded
  as a six-seat proxy. Routing is enabled only where the gates pass.
- **AC7**: The Engine Build Contract and ASan pass; 2/3-seat, four-seat
  river, blueprint, mid-turn, and flop paths are behaviorally unchanged,
  including through the build/train split.

## Review history

- Round 1 — independent approval agent `rfc0036-approval-1`,
  2026-10-24 — Changes Requested, findings F1–F6. Dispositions in this
  revision — F1: current wall-partial publication semantics documented
  from source, new paths get a two-stage serve/decline gate in the new
  entry points with shared-helper semantics untouched; F2: T3 evidence
  corrected (declared-only half-pot menu, ten hands per seat, ~60K
  iterations/60 s, identity cap-1 turn tree unmeasured), stage 0
  calibration precedes routing, pre-train cost projection added, switch
  default documented; F3: G4 split into M1 in-model convergence and M2
  river-betting model error with numeric gates, sampled-estimator wiring
  made explicit work; F4: river re-scoped as an existing path plus a
  bounded variant, turn limited to street start, phase B described as
  RFC 0024 wiring, menu wording corrected, river chance language
  corrected; F5: two new diagnostic constants defined in the existing
  string slot with distinguishable receipts; F6: street-start predicate,
  zero-sum controls, folded/side-pot fixtures, 2..6-seat scope,
  production menu/range definitions, corrected citations, and
  measurable AC1–AC7 added.
- Round 2 — independent approval agent `rfc0036-approval-2`,
  2026-10-24 — Changes Requested, narrow findings N1–N5. Dispositions in
  this revision — N1: the build/train split of `solve_rooted_subgame` is
  now an explicit additive seam with the existing signature delegating
  and the three current callers pinned behaviorally identical; the
  post-train minimum (which reads `completed_iterations`) is distinguished
  from the split; N2: the pre-train projection uses a node/action/seat-
  driven per-iteration cost calibrated in stage 0, not the gadget's
  joint-deal-enumerating `iteration_cap_for_budget` model, and the joint-
  deal/information-set populations enter the visits quality floor;
  N3: the bounded river path is restricted to seats 5/6 and the
  four-seat river keeps its measured-cheap uncapped menu (RFC 0011:
  2,829 nodes / 66 ms), with the tradeoff recorded in Compatibility;
  N4: M2 protocol defined (freeze turn rows, fill river nodes with the
  full-tree reference policy, uniform fill as sensitivity bound) and the
  2/3-seat result recorded as an uncertifiable six-seat proxy; N5:
  routing table explicitly covers board-4 street-start seats 2/3, the
  river frontier injection is noted as interface-only, and a pre-build
  node/byte/build-time screen prevents construction from consuming the
  window.

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0036-approval-3 (independent approval agent, auto_model/alwaysday1_max)
Decision date: 2026-10-24
Review outcome: Approved
Reviewed scope: Round-3 full text (Proposed) with source verification of the
  N1–N5 dispositions across subgame_resolver.cpp, nseat_trainer.cpp,
  abstract_tree, abstraction.cpp, host main.cpp, RFC 0011's river
  measurement, T3, and the thresholds plan; structural checks (36 RFCs,
  416 docs) green.
Review summary: Approved on the third round. Round 1 returned Changes
  Requested (F1–F6): deadline semantics were described backwards, the
  feasibility evidence used the wrong abstraction (declared-only half-pot
  menu versus the shipped identity menu), rollout order burned the full
  window before measurement, and G4 measured convergence to an all-in-river
  game rather than model error. Round 2 returned Changes Requested with
  two blockers (N1: a pre-train projection contradicted leaving the shared
  helper un-split; N2: the projection reused a joint-deal-enumerating cost
  model that structurally projects zero iterations for full-range six-seat
  games) plus N3–N5. All were resolved: an explicit behavior-preserving
  build/train seam; a node/action/seat-driven calibrated time projection
  with a separate visits quality floor; bounded river restricted to seats
  5/6 preserving the measured-cheap four-seat uncapped menu; a defined
  freeze-turn/fill-river M2 protocol with the 2/3-seat result recorded as
  a six-seat proxy; a three-stage pre-build/pre-train/post-train gate; and
  complete six-case host precedence. Three non-blocking implementation
  suggestions were applied in the status-flip edit (measured node/byte
  screen table, action-nodes × buckets visits upper bound with
  post-train revalidation, and an explicit five/six-seat river
  partial-to-fallback behavior-change disclosure). Approval covers the
  two resolver entries, the helper split, host routing, and the two
  diagnostic strings only; no proto, persisted-format, or artifact change;
  enabling routing is gated on stage-0 evidence. It authorizes no
  spending, live play, or training.
```
