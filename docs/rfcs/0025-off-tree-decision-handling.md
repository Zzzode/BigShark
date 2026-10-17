---
rfc: "0025"
subject: "Off-Tree Decision Handling"
status: "Proposed"
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
action line. The resolver is extended to support flop-rooted subgames with
rollout-to-showdown leaf valuation, and the prefix-reach computation is
extended to tolerate off-tree actions in the observed history by falling
back to uniform prior ranges. All off-tree resolutions are labeled
`approximate`; no certified bound is claimed.

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
  build a subgame rooted at the flop state, solve it with CFR, and serve
  the hero's policy at the root.
- Off-tree action line tolerance: when the observed action line contains
  an action not in the blueprint row's menu, fall back to uniform prior
  ranges for the affected seat and continue the resolve instead of failing
  with `CoverageMiss`.
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

Evidence: `engine/src/resolver/subgame_resolver.cpp` lines 365-375.

### Prefix-reach computation

`resolve` derives prior ranges by replaying the observed action line through
the blueprint. At each step, it looks up the blueprint row for the current
cursor and calls `action_probability(view, event.action)`. If the action is
not in the row's menu, `action_probability` returns `-1.0`, and the resolver
fails with `CoverageMiss`:

```cpp
const double probability = action_probability(*view, event.action);
if (probability < 0) {
  status = SubgameResolveStatus::CoverageMiss;
  return {};
}
```

Evidence: `engine/src/resolver/subgame_resolver.cpp` lines 188-192.

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
- **Bounded**: every resolve has a deadline (`solve_time_budget_ms`). A
  deadline miss falls through to the operational fallback.
- **Offline**: the resolver is offline (no wire, no credentials) and
  thread-affine.

## Proposed Design

### Flop-rooted subgame resolving

Extend `SubgameResolver` with a `resolve_flop` entry point that:

1. Accepts a flop-rooted action node (`board_size == 3`).
2. Builds a subgame def from the live node state (same as
   `build_subgame_def_from_node` but with flop board size).
3. Builds uniform prior ranges (same as `uniform_prior_ranges`).
4. Uses identity action abstraction (no blueprint sizing schedule).
5. Solves the subgame with CFR. The subgame includes the turn and river
   chance cards and all remaining action streets.
6. River leaves are valued by the existing `settle_showdown` settlement
   (exact equity, no new evaluator).
7. Serves the hero's policy at the root with `approximate` guarantee.

The subgame tree for a flop-rooted resolve is larger than a turn-rooted
resolve because it includes the turn chance (44 cards for 2 seats, 42 for
3, ...) and the full turn/river action tree. The depth-limited resolving
infrastructure (RFC 0024) bounds the tree size with `max_raises_per_street`.

### Off-tree action line tolerance

Extend the prefix-reach computation in `resolve` to tolerate off-tree
actions:

1. When `action_probability` returns `-1.0` (action not in blueprint menu),
   do not fail with `CoverageMiss`.
2. Instead, mark the affected seat's prefix reach as "off-tree" and continue
   the replay.
3. After the replay, for any seat marked "off-tree", replace its prior range
   with the uniform prior range (same as `uniform_prior_ranges` for that
   seat).
4. Seats not marked "off-tree" retain their blueprint-derived prior ranges.
5. The resolve proceeds with the mixed prior ranges (blueprint for on-tree
   seats, uniform for off-tree seats).

This is not a projection: the off-tree action is replayed at its actual
amount, and the game tree is built from the live state. The approximation is
that the off-tree seat's range is reset to uniform after the off-tree action,
which discards the blueprint's range information for that seat.

### Wiring

In `envelope.cpp` `SOLVER_MODE_AUTOMATIC`:

1. Blueprint miss → try `lookupResolving` (existing).
2. Resolver miss with `Ineligible` (flop node) → try `resolve_flop` (new).
3. Resolver miss with `CoverageMiss` (off-tree action line) → retry with
   off-tree tolerance (new).
4. All resolver misses → operational fallback (existing).

The `HostServices` interface is extended with:
- `resolvingFlopDecision(state, history, hero_cards, deadline_ms)`: flop-
  rooted resolve with uniform priors.
- The existing `resolvingDecision` is extended with an `off_tree_tolerant`
  flag, or a new `resolvingDecisionOffTree` method is added.

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
  turn-rooted subgame. The `solve_time_budget_ms` deadline bounds the solve
  time. A deadline miss falls through to the operational fallback.
- **Incident recovery**: if the flop-rooted resolve produces a bad policy,
  the operator can disable it with `BIGSHARK_SUBGAME_UNIFORM=0`, which
  restores the operational fallback for all resolver misses.

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

- **Flop subgame size**: the flop-rooted subgame includes the turn chance
  and the full turn/river action tree. With `max_raises_per_street=1` and
  6 seats, the tree is bounded (RFC 0024 measured 9,361 nodes for the
  largest 6-seat fixture), but the flop-rooted tree is larger than the
  turn-rooted tree. Mitigation: the `solve_time_budget_ms` deadline bounds
  the solve time; a deadline miss falls through to the operational fallback.
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
  - Verify that the off-tree seat's prior range is uniform and the on-tree
    seats' prior ranges are blueprint-derived.
  - Verify that the result is labeled `approximate` with the correct reason
    code.
- **Conformance tests**: extend `test_conformance.cpp` with off-tree
  decision request/response fixtures.
- **Benchmarks**: measure flop-rooted subgame solve time for 2..6 seats at
  10/25/100 BB with `max_raises_per_street=1`.
- **Engine Build Contract**: full release build, ctest, benchmark, npm
  check, proto check, replay.

## Rollout Plan

1. Implement `resolve_flop` and off-tree tolerance in the resolver.
2. Wire the new paths into `HostServices` and `envelope.cpp`.
3. Add native tests and conformance fixtures.
4. Run the Engine Build Contract.
5. Benchmark flop-rooted solve time.
6. Commit and document.

The feature is enabled by default. `BIGSHARK_SUBGAME_UNIFORM=0` disables
all resolver paths and restores the operational fallback.

## Rollback Plan

Set `BIGSHARK_SUBGAME_UNIFORM=0` to disable all resolver paths. This
restores the pre-RFC behavior: blueprint miss → operational fallback.

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
      within the solve deadline.
- [ ] Off-tree action line tolerance produces a policy (not `CoverageMiss`)
      when the observed line contains an off-tree action.
- [ ] All off-tree resolutions are labeled `approximate` with
      `SOLVER_SOURCE_RESOLVING` and the correct reason code.
- [ ] The existing on-tree resolver path is unchanged.
- [ ] The operational fallback is still served when the resolver cannot
      produce a policy (deadline, ineligible, invalid input).
- [ ] Native tests, conformance tests, and benchmarks pass.
- [ ] The Engine Build Contract passes.

## Decision

Pending independent approval-agent review.
