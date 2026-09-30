# RFC 0009 W2c-ii — State Layer Generalization (Plan, rev 2)

Status: Proposed, pending independent review. Rev 2 addresses the independent
review's REQUEST-CHANGES (two blocking, two non-blocking, one nit). Implements
RFC 0009 D4 (the resolver/resident generalization) on top of W2c-i (committed
883fc72), which generalized the resident *data layer* to consume v2
`SeatPolicy` artifacts via a two-seat flop-rooted projection. W2c-ii generalizes
the *state layer* so a 3..10-seat request reconstructs, loads, and serves
through the same engine.

Rev 2 changes from rev 1:

- **Blocking 1:** the gate is no longer "the existing suite passes unchanged"
  (it cannot — tests that construct `HeadsUpState` directly break). It is now a
  differential oracle plus golden-value preservation (see "The two-seat
  regression gate").
- **Blocking 2:** the plan now specifies the unified game view
  (`solver::UnifiedGame`) and the `Record`/load generalization, and places them
  in W2c-ii-a. `fixed_runout` is preserved so certified two-seat v1 fixed-runout
  artifacts do not regress.
- **Non-blocking 3:** the n>=4 cutoff no longer claims "infeasible." It
  distinguishes root existence (exact for every seat count at load) from the
  per-node belief marginal computation (exact for n=2,3; n>=4 deferred as a
  declared coverage limitation with a measured follow-up).
- **Non-blocking 4:** the guarantee-ladder surfaces D4 names are enumerated.
- **Nit 5:** W2c-ii-c depends on W2c-ii-b (not purely additive); the `history`
  passed to `BlueprintSource::row` mid-replay is the prefix span at the cursor.

## Goal

One engine serves 2..10 seats. After W2c-ii a validated 3..10-seat v1
`DecisionRequest` no longer fails at reconstruction: it reconstructs a unified
`GameState`, the resident set answers belief/hero-decision queries for the seat
count, and the terminal-only resolver certifies per-seat non-regression. The
two-seat path keeps its certified behavior exactly, pinned by a differential
oracle against the `HeadsUpState` it replaces.

## Architectural decision (load-bearing)

**Unify on `GameState` + a `PublicAction` history, for every seat count, and on
a single solver-domain `UnifiedGame` view.**

- `GameState` already reproduces `HeadsUpState` exactly at two seats via its
  `heads_up_profile_` flag (game_definition.hpp:238-241). A two-seat
  `GameState` replay is element-for-element the same state machine the heads-up
  path uses today.
- `GameState` is historyless; the action path is carried explicitly as
  `std::span<const PublicAction>` (game_definition.hpp:263-267). The mapper
  already replays the wire action history, so it produces the log alongside the
  state. The resident `fill_key` and the resolver prefix-reach read the provided
  log instead of a state-internal history.
- `PublicAction{street, seat, action}` is a faithful history replacement. The
  independent review verified every serving-path consumer
  (`fill_key` at resident_policy.cpp:117-122, `information_key` at
  heads_up_solver.cpp:575-585, the resolver prefix replay at
  counterfactual_reach.cpp:88-114) reads only street/actor/type/target_total; no
  `.paid` reader exists in `engine/`, `apps/`, or `platforms/`, and
  `make_information_key` (seat_policy.cpp:21) already takes the span.
- The host-services signatures (`V1HostServices::blueprintHeroDecision`,
  `resolvingDecision`) and the resolver `BlueprintSource` change from
  `HeadsUpState`/`HeadsUpGame` to `GameState` + history + hero seat, per D4.

### The unified game view (`solver::UnifiedGame`)

The resident `Record` currently stores a `solver::HeadsUpGame`, and v2 artifacts
load through `project_v2_game`, which throws for `player_count != 2`
(resident_policy.cpp:70-72, called at :240 and :282). Today an n-seat artifact
fails at *load* (`LoadFailed`), so W2c-ii-b's n-seat belief gate is unreachable.
The resolver also consumes `game.fixed_runout` (continuation.cpp:10-18,
certifier.cpp:31-40), `game.ranges`, and `game.sizes`; dropping `fixed_runout`
would regress certified two-seat v1 fixed-runout artifacts. The fix is one
solver-domain game view both layers share:

```cpp
// engine/include/bs/unified_game.hpp
namespace bs::solver {
struct UnifiedGame {
  poker::GameDef def{};                                                         // seat-generic root identity
  std::array<std::vector<WeightedHand>, poker::kMaxUnifiedSeats> ranges{};      // per-seat, seat-indexed
  abstraction::SizeSchedule sizes = abstraction::default_size_schedule();
  std::array<std::optional<int>, 2> fixed_runout{};                             // v1 only; empty for v2
};
}  // namespace bs::solver
```

- It lives in the solver domain so `bs::resolver` depends on it without a
  layering inversion; `bs::resident` implements `BlueprintSource` over it. The
  resolver already includes `heads_up_solver.hpp` (which defines `WeightedHand`,
  `SizeSchedule`, and the 2-seat `HeadsUpGame` this generalizes).
- **v1 load** converts `HeadsUpGame -> UnifiedGame` (a `to_unified_game` helper:
  `HeadsUpRoot -> GameDef` field copy — flop to `board[0..2]`/`board_size=3`,
  stacks/contributions/pot/big_blind/button, `preflop`/`blinds_posted` carried
  across — plus `ranges[0..1]`, `sizes`, `fixed_runout`).
- **v2 load** populates `UnifiedGame` from `SeatPolicy` (`game()` + `ranges()` +
  `sizes()`), with **no `player_count != 2` throw**: the W2c-i `project_v2_game`
  refusal is removed. `board_size != 3` (turn/river-rooted) stays refused —
  only flop-rooted artifacts serve, at every seat count (W2c-i decision 4).
- `Record` stores `UnifiedGame` (not `HeadsUpGame`). n-seat artifacts now LOAD:
  the record has a seat-generic identity, so W2c-ii-b is reachable.
- `BlueprintSource::game()` returns `const UnifiedGame&`. The resolver reads
  `def`/`ranges`/`sizes`/`fixed_runout` from it. `fixed_runout` is preserved
  field-for-field, so certified two-seat v1 fixed-runout artifacts do not
  regress.
- **Root identity stays the resident's six-field comparison, generalized to
  `GameDef`** (board[0..2], stacks[0..n-1], contributions[0..n-1], pot,
  big_blind, button). It does **not** adopt `poker::same_game_def`, which is
  stricter (it additionally compares `preflop`/`blinds_posted`/`ante`/`variant`/
  `terminal`) and would change two-seat root-matching — the documented
  divergence at resident_policy.cpp:42-54 deliberately keeps the looser
  resident identity. Migrating to `same_game_def` is a separate, explicitly
  justified decision and is out of scope for W2c-ii.
- `game_resident_bytes` and `ResidentIndex::estimate_bytes` generalize to
  `UnifiedGame`; a new declared accounting constant replaces the
  `HeadsUpGame`-specific `kGameCopyAccountingBytes` for the unified view (per
  the RFC 0007 layout-decoupling rule).

Rejected alternative: keep the resident set on `HeadsUpState`/`HeadsUpGame` and
have the host re-derive a `HeadsUpState` by replaying from a projected root for
two seats. This duplicates the replay, leaves two state machines and two game
views in the serving path, and does not satisfy D4's "unified state" signature
for the resolver `BlueprintSource`.

## The two-seat regression gate (rev 2)

The existing suite does **not** pass unchanged. Tests that construct
`HeadsUpState` directly and call `public_belief(HeadsUpState)`
(test_resident_policy.cpp:463-472,523-540) and tests that read
`reconstructed.root.flop/.stacks` (test_v1_resident_mapper.cpp:297-302) break
under the new signatures and are rewritten to the `GameState` + history API. The
gate is **golden-value preservation plus a differential oracle**:

1. **Mapper differential oracle.** `assertMatchesOracle`
   (test_v1_resident_mapper.cpp:285) already reconstructs a `HeadsUpState`
   independently (`oracleState`, line 203) and compares board/pot/stacks/
   street-committed/actor. Extend it to also reconstruct the new `GameState` +
   `PublicAction` history from the same request and assert the two agree on
   **phase, street, `legal()`, actor, board, pot, stacks, street-committed, and
   settle outcomes** (`settle_fold` and `settle_showdown` for a fixed pair of
   hands). This directly pins the "reproduces `HeadsUpState` exactly" claim,
   which currently has no differential test (test_game_definition.cpp:6-8
   excludes `heads_up.hpp`; test_solve_conformance.cpp:340-405 diffs only the
   constructor on two fixtures). After the refactor the resolver's own
   cross-check (counterfactual_reach.cpp:121-127) becomes GameState-vs-GameState,
   so this oracle is what differentially pins the two state machines.
2. **PublicAction log under test.** A new assertion checks the mapper's
   `PublicAction` log equals the expected `(street, seat, action)` sequence for
   each oracle fixture, so the history-carrying mechanism itself is verified —
   not just the state it produces.
3. **Resolver golden values.** test_resolver.cpp and test_v1_resolving.cpp keep
   their golden candidate/margin/status expectations but drive the resolver
   through the new `GameState` + history input.
4. **Resident golden values.** test_resident_policy.cpp's belief/hero-decision
   expectations (marginal sums, row probabilities, miss reasons) are preserved
   with the new `GameState` + history construction.

## Sub-stages (each is one reviewed commit set)

### W2c-ii-a — UnifiedGame + mapper + unified signatures + two-seat GameState refactor

- Introduce `solver::UnifiedGame` (above). `Record` stores it; v1/v2 load
  populate it (v2 no longer throws for n!=2; `board_size != 3` still refused).
  Generalize `game_resident_bytes`/`estimate_bytes` and the root comparison to
  the unified view.
- Generalize `v1_resident_mapper.cpp`: the `players_size() == 2` gate becomes a
  2..10 range check with the same fail-closed reasons; the two-seat seat-mapping
  generalizes to occupied-seat order for n seats; the matched-contribution check
  generalizes from `flopPot/2` to "all n contributions equal" (the side-pot
  gate already rejects uneven pots); the replay uses
  `GameState::after_action`/`after_card` and records a `PublicAction` per event.
- `ReconstructedPostflop` carries `GameState` + `std::vector<PublicAction>` +
  hero seat (replacing the `HeadsUpRoot`/`HeadsUpState` pair).
- `V1HostServices` signatures and the resolver `BlueprintSource` change to
  `GameState` + history + hero seat; `BlueprintSource::game()` returns
  `const UnifiedGame&`. The envelope caller (v1_envelope.cpp:121,162) and the
  host implementation (apps/engine-host/main.cpp) adapt.
- The resolver's `resolve` signature and prefix replay (counterfactual_reach.cpp)
  change from `HeadsUpState` + `BettingEvent` to `GameState` + `PublicAction`;
  the gadget itself (CFR, certification) is unchanged. The `history` passed to
  `BlueprintSource::row` mid-replay is the **prefix span at the cursor**, not the
  full node log.
- The resident set's two-seat query path refactors from `HeadsUpState` to
  `GameState` + history, reading from `UnifiedGame`. `resolve_root` uses the
  generalized six-field GameDef comparison; `replay_public_path`/`fill_key` read
  the `PublicAction` log; `runout_matches` reads `UnifiedGame.fixed_runout`.
- The load-time root existence check (`root_joint_mass`) generalizes to n seats
  so an n-seat artifact loads with the correct `RootStatus` (a correct
  existence/mass check; the efficient per-node belief is W2c-ii-b).
- 3..10-seat requests reconstruct and load successfully but miss at the resident
  query with a declared reason (the two-seat `ReachModel` cannot serve n>=3)
  until W2c-ii-b. This is the staged delivery: the wire already validates 3..10
  seats (v1_semantic_validator.cpp:458-479); only reconstruction, loading, and
  serving are added here.
- **Gate:** the differential oracle + golden-value preservation above; a new
  test reconstructs a 3-seat request, loads a 3-seat artifact, and asserts the
  resident miss reason.

### W2c-ii-b — n-seat belief existence + hero decision

- Generalize `ResidentScratch` from fixed `std::array<...,2>` to a
  seat-count-parameterized form (bounded by `kMaxUnifiedSeats`).
- `public_belief(GameState, history, hero_seat, ...)` computes the joint
  distribution over n card-disjoint hands conditioned on the observed path.
- **Belief existence vs. marginal computation (rev 2):** these are distinct, and
  the plan no longer calls n>=4 "infeasible."
  - **Root existence** (the load-time check, generalized in W2c-ii-a): exact for
    every seat count. For n<=3 the card-count inclusion-exclusion extends
    directly; for n=4 a meet-in-the-middle over seat-pair deals is exact
    (~1e7 ops); for n>=5 a general card-disjoint assignment check is still
    exact. This keeps `RootStatus::InvalidRange` correct for every artifact.
  - **Per-node belief marginal computation** (the `ReachModel` update along the
    observed path): exact for n=2 (existing) and n=3 (the novel
    inclusion-exclusion over card collisions). For n>=4 the exact per-node
    marginal is **deferred** — a declared coverage limitation, not an
    infeasibility claim. The lookup misses closed with a declared n-way reason.
    W2c-ii-b includes a measured feasibility spike for the n=4 per-node
    marginal; if it fits the per-query budget, a follow-up extends exact service
    to n=4. The two-seat path keeps its current exact check.
- `hero_decision(GameState, history, hero_seat, hero_cards, ...)` returns the
  actor seat's row at the node.
- **Gate:** n-seat belief/hero-decision tests for 2 and 3 seats (exact) and a
  4-seat declared-miss test; two-seat golden values preserved.

### W2c-ii-c — n-seat resolver gadget + per-seat certification

- The gadget's augmented game gains one `-x` infoset per non-hero seat (D4).
  The counterfactual weight of a joint deal is
  `prod over seats(range weight) * pi^prefix_hero`, with the hero-prefix product
  unchanged (resolver.hpp:28-37 generalizes from one responder to n-1). **This
  stage depends on W2c-ii-b** (the gadget's joint-deal enumeration and per-seat
  rows need the n-seat replay/belief machinery); it is not purely additive.
- `BlueprintSource::row` is per-seat: `row(GameState, history, seat, cards)`,
  where `history` is the prefix span at the cursor.
- **Certification at n >= 3 is a per-seat unilateral non-regression bound**: for
  every positive-mass infoset I of every non-hero seat,
  `BR_seat(I) <= b_seat(I) + 1e-9 * root_pot`, where `b_seat(I)` is the
  locked-blueprint continuation for that seat. This is NOT an equilibrium claim
  and NOT a two-player bound. `ResolveStatus` keeps its values; the semantic
  extension is documented at the enum and every consumer, **including the
  guarantee-ladder surfaces D4:541-543 names: `engine/src/policy/guarantee.cpp`
  and `v1_response_mapper.cpp:481`** (the mapping that renders a resolve status
  into a wire guarantee must say "unilateral non-regression" for multiway, never
  the two-player meaning). The wire level remains `certified_bound` only if the
  independent certifier recomputed every seat's bound and all passed; the
  response carries a diagnostic token identifying multiway certification so a
  journal reader can never take it for the two-player meaning.
- **Gate:** n-seat resolver tests (3-seat terminal-only gadget, per-seat
  certification pass/fail, diagnostic token); two-seat resolver golden values
  preserved.

## Sequencing and risk

- W2c-ii-a is the foundation and the highest-risk change (it touches the
  certified two-seat resident AND resolver paths). The two-seat semantic
  guarantee rests on `heads_up_profile_`; the differential oracle and golden
  values are the regression net. Because the `Record` and the two-seat query
  path both move to `UnifiedGame`/`GameState` in the same stage, the oracle must
  pass before the stage is accepted.
- W2c-ii-b and W2c-ii-c build on the unified signatures and the two-seat
  refactor. W2c-ii-c depends on W2c-ii-b (the n-seat gadget needs the n-seat
  belief/replay machinery).
- Each sub-stage is one reviewed commit set with an independent
  implementer!=reviewer gate, per the standing authorization. The full
  release/debug/asan matrix runs after each C++ change.

## Out of scope for W2c-ii

- Flop coverage (D5: suit canonicalization, preflop profiles, flop libraries)
  is W4/W5.
- The heuristic demotion (D6) is W3.
- Turn/river-rooted resident projection stays refused (W2c-i decision 4); only
  flop-rooted artifacts serve, at every seat count.
- Migrating the resident root identity from its six-field comparison to
  `poker::same_game_def` is a separate, explicitly justified decision (it would
  make two-seat root-matching stricter).
