---
rfc: "0025"
subject: "Off-Tree Decision Handling"
status: "Accepted"
authors: "BigShark maintainers"
created: "2026-10-17"
updated: "2026-10-17"
owners: "engine resolver, engine solver, engine protocol"
supersedes: ""
superseded-by: ""
---

# RFC 0025: Off-Tree Decision Handling

## Summary

Extend the runtime decision pipeline so that legal actions outside the
blueprint action abstraction (off-tree amounts) receive a resolved policy
instead of an operational fallback, for every street and every observed
action line. The resolver is extended to support flop-rooted subgames using
depth-limited resolving with the existing RFC 0024 frontier evaluator, and
the prefix-reach computation is extended to tolerate off-tree actions in the
observed history by falling back to uniform prior ranges. All off-tree
resolutions are labeled `approximate`; no certified bound is claimed.

## Motivation

The six-max qualification proposal states:

> A normal unsupported state returning operational fallback is an unclosed
> feature gap, not a passing GTO decision.

The current AUTOMATIC decision path serves the operational fallback
(check/call/fold) in two off-tree scenarios:

1. **Flop off-tree**: the blueprint lookup misses with `OffTreeAmount`, and
   the subgame resolver only supports turn/river-rooted subgames
   (`board_size == 4 || board_size == 5` in `resolve_uniform`).
2. **Turn/river off-tree with an off-tree action line**: the resolver's
   prefix-reach computation walks the observed action line through the
   blueprint; when an action is not present in the blueprint row's menu,
   `action_probability` returns `-1.0` and the resolver fails with
   `CoverageMiss`.

Both scenarios produce a labeled operational fallback, which is safe but is
an unclosed feature gap under the qualification standard. This RFC proposes
a runtime path that produces a resolved policy for these states, honestly
labeled as approximate.

The qualification proposal also states:

> Never equate nearest-target projection with justified off-tree policy
> behavior.

This RFC does not project off-tree amounts onto the nearest blueprint node.
It builds the actual game tree from the live state (including the off-tree
amounts as legal actions) and solves it with CFR. The approximation is in
the prior ranges and leaf valuation, not in action projection.

## Goals

- Flop-rooted subgame resolving: when the blueprint misses on the flop,
  build a depth-limited subgame rooted at the flop state, solve it with CFR,
  and serve the hero's policy at the root. The depth limit uses the existing
  RFC 0024 `DepthLimit` and `EquityFrontierEvaluator` infrastructure.
- Off-tree action line tolerance: when the observed action line contains
  an action not in the blueprint row's menu, stop the blueprint replay at
  the first off-tree action and fall back to uniform prior ranges for all
  seats from that point onward, instead of failing with `CoverageMiss`.
- Honest labeling: all off-tree resolutions are labeled `approximate` with
  `SOLVER_SOURCE_RESOLVING` and reason code `subgame-resolve-offtree`.
- No action projection: the subgame is built from the live state with the
  actual legal action menu, including off-tree amounts.

## Non-Goals

- Training a wider action abstraction to reduce off-tree frequency. This is
  a training concern covered by T5; this RFC is runtime-only.
- Certified bounds for off-tree resolutions. The qualification proposal
  requires independently justified quality evidence; an approximate resolve
  is not a certified bound.
- Quality evaluation of off-tree decisions. The qualification campaign
  (T8) evaluates whether the approximate off-tree policy is good enough;
  this RFC only provides the runtime path.
- Changing the blueprint training algorithm, artifact format, or wire
  protocol.

## Current State and Evidence

### Blueprint lookup

`resident_policy.cpp` `hero_decision` looks up the blueprint row by
information key (board + prefix action span + hero cards + seat). When the
observed action amount does not match any action in the row's menu, the
lookup returns `BlueprintMiss::OffTreeAmount`. This is mapped to
`ERROR_CODE_OFF_TREE_AMOUNT` in the BLUEPRINT solver mode.

Evidence: `engine/src/resident/resident_policy.cpp` `locate_action` (line
779), `engine/src/protocol/envelope.cpp` `mapMissToError` (line 187).

### Subgame resolver

`subgame_resolver.cpp` has two entry points:

- `resolve(node, history, blueprint, limits)`: builds a turn-rooted
  River-terminal subtree, derives prior ranges from the blueprint by walking
  the observed flop action line, solves with CFR, and walks the solved tree
  to extract the hero's policy.
- `resolve_uniform(node, history, limits)`: blueprint-free mode that builds
  uniform prior ranges and solves the subgame directly from the live node
  state. Used when no blueprint source is available (controlled by
  `BIGSHARK_SUBGAME_UNIFORM`, enabled by default).

Both entry points reject flop-rooted nodes:

```cpp
if (board.size() != 4 && board.size() != 5) {
  result.status = SubgameResolveStatus::Ineligible;
  return result;
}
```

Evidence: `engine/src/resolver/subgame_resolver.cpp` lines 371-375
(`resolve_uniform`) and lines 435-439 (`resolve`).

### Prefix-reach computation

`resolve` derives prior ranges by replaying the observed action line through
the blueprint. At each step, it looks up the blueprint row for the current
cursor and calls `action_probability(view, event.action)`. There are two
distinct `CoverageMiss` failure paths:

1. **Missing blueprint row** (line 179-183): `blueprint.row(cursor, prefix,
   actor, cards)` returns no view. This triggers when the prefix contains a
   previous off-tree action — the blueprint has no rows for off-tree
   prefixes, so every subsequent lookup fails.

2. **Off-tree action** (line 188-192): the row exists but the observed action
   is not in the row's menu. `action_probability` returns `-1.0`.

```cpp
const auto view = blueprint.row(cursor, prefix, actor, cards);
if (!view) {
  status = SubgameResolveStatus::CoverageMiss;
  return {};
}
// ...
const double probability = action_probability(*view, event.action);
if (probability < 0) {
  status = SubgameResolveStatus::CoverageMiss;
  return {};
}
```

Path 1 is the prefix-contamination case: after the first off-tree action,
the information key includes the off-tree `target_total`, so all subsequent
row lookups fail. Path 2 is the direct off-tree action case.

Evidence: `engine/src/resolver/subgame_resolver.cpp` lines 179-183 (missing
row) and lines 188-192 (off-tree action).

### AUTOMATIC mode

After the fix committed in `e31a624`, the AUTOMATIC solver mode:
1. Tries the blueprint lookup.
2. On a blueprint miss, tries the resolver (`lookupResolving`).
3. On a resolver miss, serves the operational fallback
   (`mapOperationalFallbackResponse`).

The operational fallback is a deterministic check/call/fold with
`SOLVER_SOURCE_OPERATIONAL_FALLBACK` and guarantee level
`operational_fallback`.

Evidence: `engine/src/protocol/envelope.cpp` `SOLVER_MODE_AUTOMATIC` case.

### Turn/river on-tree off-tree amounts

When the observed action line is on-tree (every action matches a blueprint
node) but the hero's legal actions include off-tree amounts, the resolver
builds the subgame from the live state with the actual legal action menu
(identity action abstraction in `resolve_uniform`, or the blueprint's
abstraction in `resolve`). The CFR solver handles the off-tree amounts as
first-class actions in the game tree. This path already works correctly.

Evidence: `engine/src/resolver/subgame_resolver.cpp`
`build_subgame_def_from_node`, `abstraction::ActionAbstraction::identity()`.

## Design Principles

- **No action projection**: off-tree amounts are included as first-class
  actions in the game tree, never rounded or mapped to the nearest blueprint
  node.
- **Honest labeling**: off-tree resolutions are `approximate`, never
  `certified_bound` or `exact_solved`.
- **Additive**: the existing on-tree resolver path is unchanged. The new
  paths are only taken when the existing path would fail.
- **Bounded**: every resolve has a deadline (`solve_time_budget_ms`), a
  depth limit (`DepthLimit::streets(2)` for flop-rooted resolves), and a
  raise cap (`max_raises_per_street=1`). A deadline miss or tree-cap breach
  falls through to the operational fallback.
- **Offline**: the resolver is offline (no wire, no credentials) and
  thread-affine.

## Proposed Design

### Flop-rooted subgame resolving

Extend `SubgameResolver` with a `resolve_flop` entry point that:

1. Accepts a flop-rooted action node (`board_size == 3`).
2. Builds a subgame def from the live node state (same as
   `build_subgame_def_from_node` but with flop board size).
3. Builds uniform prior ranges (same as `uniform_prior_ranges`).
4. Uses identity action abstraction with `max_raises_per_street=1` to bound
   branching.
5. Uses depth-limited resolving with `DepthLimit::streets(2)`: the tree
   covers the flop and turn action streets, and the river boundary is valued
   by the existing RFC 0024 `EquityFrontierEvaluator`. At a 5-card river
   boundary the evaluator returns exact showdown equity (no more cards to
   deal); the approximation is in the depth limit (the river betting street
   is not solved), not in the evaluator. No new evaluator is introduced.
6. Solves the subgame with CFR.
7. Serves the hero's policy at the root with `approximate` guarantee.

The depth-limited machinery (`DepthLimit`, `FrontierEvaluator`,
`EquityFrontierEvaluator`, `max_raises_per_street`) is implemented and
unit-tested under RFC 0024 but is not currently wired into the engine-host.
The host constructs `ResolveLimits` with only `.time` and `.public_seed`;
`depth_limit` stays `DepthLimit::none()` and the `SubgameResolver`'s
frontier evaluator stays null. This RFC wires the existing machinery into
the host for the flop-rooted path:

- `apps/engine-host/main.cpp` constructs an `EquityFrontierEvaluator` and
  calls `subgame_resolver_.set_frontier_evaluator(&evaluator)` at startup.
- `resolve_flop` sets `limits.depth_limit = DepthLimit::streets(2)` and
  `action.set_max_raises_per_street(1)` before building the tree.
- The existing turn/river paths are unchanged: they continue to build full
  trees to showdown with no depth limit and no raise cap.

**Feasibility.** The T3 depth-limited + raise-capped scan
(`docs/plans/t3-six-seat-feasibility.md`, T3-DL-CAP results, 2026-10-16)
measured turn-rooted subgames with `DepthLimit::streets(1)`,
`max_raises_per_street=1`, and a coarse declared-only abstraction: the
largest 6-seat tree is 9,361 nodes / 9.1 MiB, independent of stack depth.
A flop-rooted resolve with `streets(2)` covers one additional action street
and the turn chance (49 outcomes — all non-board cards, independent of seat
count). The
raise cap bounds per-street branching; the depth limit bounds the number of
action streets. The tree is further bounded by `ResolveLimits` caps
(5M nodes, 1 GiB); a breach returns `Ineligible` and falls through to the
operational fallback. The identity abstraction has more bet sizes than the
T3 coarse fixture (3/1-pot-fraction bets + 2 raises + min-raise + all-in
versus 1/2-pot only), so the actual flop-rooted tree will be larger; the
Verification Plan benchmarks the actual sizes for 2..6 seats at 10/25/100
BB.

### Off-tree action line tolerance

Extend the prefix-reach computation in `resolve` to tolerate off-tree
actions:

1. Walk the observed action line through the blueprint as today.
2. At each step, look up the blueprint row. If the row lookup fails
   (`!view`, the prefix-contamination path at line 179) or the action is
   not in the row's menu (`action_probability < 0`, the off-tree path at
   line 188), **stop the blueprint replay** (stop consulting the blueprint
   for range derivation).
3. The GameState replay (cursor progression via `after_action`) continues
   to reach the turn root for subgame construction. Only the blueprint
   range derivation stops; the state reconstruction is unaffected.
4. Seats that were successfully replayed before the first off-tree action
   retain their blueprint-derived prior ranges.
5. All seats from the first off-tree action onward (including the off-tree
   seat itself) receive uniform prior ranges (same as
   `uniform_prior_ranges` for those seats).
6. The resolve proceeds with the mixed prior ranges (blueprint for
   pre-off-tree seats, uniform for post-off-tree seats).

This is not a projection: the off-tree action is replayed at its actual
amount, and the game tree is built from the live state. The approximation
is that the blueprint's range information is discarded for all seats after
the first off-tree action, which is the honest cost of not having blueprint
rows for off-tree prefixes.

The previous design (mark individual seats as off-tree and continue the
replay) is not implementable: after the first off-tree action, the
information key includes the off-tree `target_total`, so every subsequent
`blueprint.row` lookup fails with `!view` before `action_probability` is
ever called. Stopping the replay at the first off-tree action is the only
option without key normalization (which would be a form of projection,
explicitly rejected).

### Wiring

In `apps/engine-host/main.cpp` `ResidentHostServices`:

1. At startup, construct an `EquityFrontierEvaluator` and call
   `subgame_resolver_.set_frontier_evaluator(&evaluator)`.
2. In `resolvingDecision`, after the existing resolver paths return
   `Ineligible` (flop node), try `resolve_flop` (new). This is gated by
   `BIGSHARK_SUBGAME_UNIFORM` (same flag that gates `resolve_uniform`,
   since `resolve_flop` is a uniform-prior path).
3. The off-tree tolerance is built into `resolve` itself: when the
   prefix-reach computation encounters an off-tree action, it stops the
   replay and uses uniform priors for post-off-tree seats. This is gated by
   `BIGSHARK_SUBGAME_RESOLVER` (same flag that gates the blueprint-derived
   `resolve` path).

In `envelope.cpp` `SOLVER_MODE_AUTOMATIC`:

1. Blueprint miss → try `lookupResolving` (existing).
2. Resolver miss with `Ineligible` (flop node) → try `resolve_flop` (new,
   via `HostServices`).
3. Resolver miss with `CoverageMiss` → the off-tree tolerance inside
   `resolve` now produces a policy instead of failing, so this path is
   taken transparently inside `resolve`.
4. All resolver misses → operational fallback (existing).

The `HostServices` interface is extended with:
- `resolvingFlopDecision(state, history, hero_cards, deadline_ms)`: flop-
  rooted resolve with uniform priors, depth limit, and frontier evaluator.

### Reason codes and metadata

- Flop-rooted resolve: `reason_code = "subgame-resolve-flop"`,
  `source = SOLVER_SOURCE_RESOLVING`, `guarantee = approximate`.
- Off-tree-tolerant resolve: `reason_code = "subgame-resolve-offtree"`,
  `source = SOLVER_SOURCE_RESOLVING`, `guarantee = approximate`.
- Both set `cache_hit = false` and `artifact_sha256 = ""` (no artifact).

## Dependency Rules

- `engine/src/resolver/` may depend on `engine/src/poker/`,
  `engine/src/solver/`, `engine/src/abstraction/`, and
  `engine/src/settlement/`.
- `engine/src/resolver/` must not depend on `engine/src/protocol/`,
  `engine/src/resident/`, or `platforms/`.
- `engine/src/protocol/envelope.cpp` calls `HostServices` methods; it does
  not call the resolver directly.
- `apps/engine-host/main.cpp` implements `HostServices` and owns the
  `SubgameResolver` instance.

## Compatibility and Migration

This is an additive change. The existing on-tree resolver path is unchanged.
The new paths are only taken when the existing path would fail.

No wire protocol change is needed. The `SOLVER_SOURCE_RESOLVING` enum value
already exists. The reason codes are new string values in
`SolverMetadata.reason_code`.

No artifact format change is needed. The flop-rooted resolve uses uniform
priors and identity action abstraction; no blueprint is loaded.

No migration is needed. The change is internal to the engine.

## Security and Operational Impact

- **Credentials**: no change. The resolver is offline.
- **Fairness**: no change. The resolver uses only public information and
  the hero's hole cards.
- **Observability**: the reason codes (`subgame-resolve-flop`,
  `subgame-resolve-offtree`) allow operators to distinguish on-tree,
  flop-resolve, off-tree-resolve, and fallback decisions in logs.
- **Resource limits**: the flop-rooted subgame is larger than the
  turn-rooted subgame. The `DepthLimit::streets(2)` and
  `max_raises_per_street=1` bound the tree size; the `solve_time_budget_ms`
  deadline bounds the solve time. A tree-cap breach returns `Ineligible`;
  a deadline miss falls through to the operational fallback.
- **Incident recovery**: if the flop-rooted resolve produces a bad policy,
  the operator can disable it with `BIGSHARK_SUBGAME_UNIFORM=0`, which
  disables all uniform-prior resolver paths (`resolve_uniform` and
  `resolve_flop`). If the off-tree tolerance produces a bad policy, the
  operator can disable it with `BIGSHARK_SUBGAME_RESOLVER=0`, which
  disables the blueprint-derived `resolve` path (including off-tree
  tolerance). Both flags together restore the pre-RFC behavior for all
  new paths.

## Alternatives Considered

### Train a wider action abstraction

Train the blueprint with a wider action abstraction that covers more legal
amounts. This would reduce the frequency of off-tree decisions but would
not eliminate them (there are always legal amounts outside any finite
abstraction). This is a training concern covered by T5, not a runtime
solution. Rejected as the sole solution because it does not close the
runtime gap.

### Heuristic leaf valuation for flop subgames

Use a hand-strength heuristic (e.g., percentile vs. random) to value flop
terminal nodes instead of rolling out to showdown. This would be faster but
would introduce a heuristic into the GTO pipeline, which violates the
project's "GTO by default" principle. Rejected.

### Nearest-target projection

Project the off-tree amount onto the nearest blueprint node and use that
node's policy. This is explicitly prohibited by the qualification proposal
("Never equate nearest-target projection with justified off-tree policy
behavior"). Rejected.

### Serve the operational fallback

Keep the current behavior. This is safe but is an unclosed feature gap
under the qualification standard. Rejected as the final state, but retained
as the fallback when the resolver cannot produce a policy.

## Risks

- **Flop subgame size**: the flop-rooted subgame covers the flop and turn
  action streets (depth-limited at `streets(2)`) and the turn chance (49
  outcomes — all non-board cards, independent of seat count). The T3-DL-CAP scan
  (`docs/plans/t3-six-seat-feasibility.md`, 2026-10-16) measured turn-rooted
  subgames with `DepthLimit::streets(1)`, `max_raises_per_street=1`, and a
  coarse declared-only abstraction: the largest 6-seat tree is 9,361 nodes /
  9.1 MiB, independent of stack depth. The flop-rooted tree with `streets(2)`
  and the identity abstraction (more bet sizes) will be larger. The
  `ResolveLimits` caps (5M nodes, 1 GiB) bound the tree; a breach returns
  `Ineligible` and falls through to the operational fallback. The
  Verification Plan benchmarks the actual sizes.
- **Frontier evaluator approximation**: the `EquityFrontierEvaluator` values
  river-boundary nodes by exact equity (the "all-in" approximation, assuming
  no future betting). This discards the river action's strategic value. The
  result is labeled `approximate`; the qualification campaign (T8) evaluates
  whether the quality is sufficient.
- **Uniform prior quality**: the uniform prior for off-tree seats discards
  the blueprint's range information. The resulting policy may be weak
  against opponents who take off-tree actions with polarized ranges.
  Mitigation: the result is labeled `approximate`; the qualification
  campaign (T8) evaluates whether the quality is sufficient.
- **Convergence**: CFR on the flop-rooted subgame may not converge within
  the deadline for large trees. Mitigation: the resolver returns the best
  policy found so far on a deadline miss, labeled `approximate`.

## Verification Plan

- **Native tests**:
  - Flop-rooted resolve with a known-correct policy on a small fixture.
  - Off-tree action line tolerance: verify that the resolver does not fail
    with `CoverageMiss` when the observed line contains an off-tree action.
  - Verify that pre-off-tree seats retain blueprint-derived ranges and
    post-off-tree seats receive uniform ranges.
  - Verify that the result is labeled `approximate` with the correct reason
    code.
  - Verify that the depth-limited tree has `TerminalFrontier` leaves at the
    river boundary and that the `EquityFrontierEvaluator` is called.
- **Conformance tests**: extend `test_conformance.cpp` with off-tree
  decision request/response fixtures.
- **Benchmarks**: measure flop-rooted subgame tree size (nodes, bytes,
  information sets) and solve time for 2..6 seats at 10/25/100 BB with
  `DepthLimit::streets(2)` and `max_raises_per_street=1`. Confirm that all
  configurations fit within the `ResolveLimits` caps (5M nodes, 1 GiB).
- **Engine Build Contract**: full release build, ctest, benchmark, npm
  check, proto check, replay.

## Rollout Plan

1. Wire the `EquityFrontierEvaluator` and `DepthLimit` into the engine-host
   for the flop-rooted path.
2. Implement `resolve_flop` and off-tree tolerance in the resolver.
3. Wire the new paths into `HostServices` and `envelope.cpp`.
4. Add native tests and conformance fixtures.
5. Run the Engine Build Contract.
6. Benchmark flop-rooted tree size and solve time.
7. Commit and document.

The feature is enabled by default. `BIGSHARK_SUBGAME_UNIFORM=0` disables
all uniform-prior resolver paths (`resolve_uniform` and `resolve_flop`).
`BIGSHARK_SUBGAME_RESOLVER=0` disables the blueprint-derived `resolve` path
(including off-tree tolerance). The terminal-only resolver is not gated by
any flag (existing behavior).

## Rollback Plan

- `BIGSHARK_SUBGAME_UNIFORM=0` disables `resolve_uniform` and `resolve_flop`
  (all uniform-prior paths).
- `BIGSHARK_SUBGAME_RESOLVER=0` disables the blueprint-derived `resolve`
  path (including off-tree tolerance).
- Both flags together restore the pre-RFC behavior for all new paths:
  blueprint miss → terminal-only resolver → operational fallback.
- The terminal-only resolver is not gated by any flag and is unchanged by
  this RFC.

To revert the code change, revert the commit. No data migration is needed
because no artifact or wire format changes are made.

## Open Questions

- Should the flop-rooted resolve use the blueprint's prior ranges for
  on-tree seats (when a blueprint source is available) instead of uniform
  priors? This would improve quality but would require loading the blueprint
  for the flop root, which may not be available for all seat counts.
- Should the off-tree tolerance use a conditional prior (e.g., polarized
  range for a bet) instead of a uniform prior? This would improve quality
  but would introduce a heuristic into the range construction.

## Acceptance Criteria

- [ ] `resolve_flop` produces a policy for a flop-rooted off-tree decision
      within the solve deadline, for 2..6 seats at 10/25/100 BB stacks,
      with `DepthLimit::streets(2)` and `max_raises_per_street=1`.
- [ ] Off-tree action line tolerance produces a policy (not `CoverageMiss`)
      when the observed line contains an off-tree action. Pre-off-tree seats
      retain blueprint-derived ranges; post-off-tree seats receive uniform
      ranges.
- [ ] All off-tree resolutions are labeled `approximate` with
      `SOLVER_SOURCE_RESOLVING` and the correct reason code.
- [ ] The existing on-tree resolver path is unchanged (turn/river resolves
      continue to build full trees to showdown with no depth limit).
- [ ] The operational fallback is still served when the resolver cannot
      produce a policy (deadline, ineligible, invalid input).
- [ ] Native tests, conformance tests, and benchmarks pass.
- [ ] The Engine Build Contract passes.

## Decision

Author agent: main session (continued from compacted session)
Approved by: a375317ae9764f14e (independent approval agent)
Decision date: 2026-10-17
Review outcome: Approved
Reviewed scope: RFC 0025 full text (all 18 required sections), re-verified
against engine source (`subgame_resolver.cpp`, `envelope.cpp`,
`resident_policy.cpp`, `main.cpp`, `seat_policy.cpp`, `equity_frontier.cpp`,
`abstract_tree.cpp`, `abstraction.hpp`, `subgame_resolver.hpp`,
`resolver.hpp`, `equity_frontier.hpp`), plans (`t3-six-seat-feasibility.md`,
`sixmax-gto-qualification.md`), and depth-limited tests
(`test_depth_limited.cpp`, 50 assertions).
Review summary: Initial review returned Changes Requested with three
blocking findings: B1 (feasibility contradiction — full-rollout tree with
inapplicable T3-DL-CAP evidence), B2 (prefix contamination — off-tree
tolerance design unimplementable as written), B3 (rollback plan factually
inaccurate). All three were resolved in the revised proposal: B1 by
switching to depth-limited resolving with `DepthLimit::streets(2)` and the
existing RFC 0024 `EquityFrontierEvaluator`, B2 by stopping the blueprint
replay at the first off-tree action and using uniform priors for
post-off-tree seats, B3 by correcting the flag semantics and gating for
each new path. Four non-blocking findings (M1: misattributed 9,361-node
citation; M2: acceptance criterion lacking configuration specificity; m1:
imprecise evidence citation; m2: incomplete CoverageMiss evidence) were
also resolved. Three minor non-blocking findings (turn chance count 49 not
47, frontier evaluator language precision, GameState replay continuation
clarity) were addressed in the final revision. The hard constraint from
the qualification proposal (no nearest-target projection) is respected:
the tree is built from the live state with off-tree amounts as first-class
actions, and the approximation is honestly located in prior ranges and
leaf valuation. Residual risks: frontier evaluator approximation (river
betting street not solved), uniform prior quality for post-off-tree seats,
tree size for identity abstraction (benchmarked in Verification Plan),
CFR convergence within deadline. All residual risks are acknowledged in
the RFC and deferred to T8 qualification.
