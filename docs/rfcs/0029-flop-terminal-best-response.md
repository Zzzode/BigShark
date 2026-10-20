---
rfc: "0029"
subject: "Make the Stage-6 Walkers Model Frontier Leaves and Measure the Resident Path"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-20"
owners: "stage6 evaluation, engine resident, engine benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0029: Make the Stage-6 Walkers Model Frontier Leaves and Measure the Resident Path

## Summary

The stage-6 evaluation stack cannot measure the policy the engine ships, for
two independent reasons. Its walks do not model `Phase::Frontier`, so a
flop-terminal game either returns zeros, dereferences an empty optional, or
spins forever; and no adapter converts the resident serving path into the
`stage6::BehaviorPolicy` those walks consume. This RFC fixes both, settles the
definition of exploitability (there are two, not three), and adds a reference
document. It changes no strategy and publishes no strength claim.

## Motivation

The repository carries substantial measurement machinery — an exact pooled best
response, a Monte Carlo learned-deviation estimator, exact heads-up evaluation,
an independent pure-policy oracle in tests, a pooled n-seat walker. None of it
can measure the shipped six-max policy.

**Phase coverage is incomplete and fails unsafely.** The shipped flop library is
trained flop-terminal (`engine/src/flop_library/flop_library.cpp:86-90`), so its
leaves are frontier states. Five walks omit `Frontier` from their switch:

| Walk | File | Consequence at a frontier state |
| --- | --- | --- |
| `exact_oracle::recurse` | `engine/src/stage6/exact_oracle.cpp:42` | falls to `return value;` at `:143`, zeros |
| `exact_oracle::br_group` | `:193` | falls to `return out;` at `:350`, zero mass |
| `LearnWalk::run` | `engine/src/stage6/br_estimator.cpp:354` | reaches `*state.actor()` at `:367` — UB |
| `LegWalk::run` | `:493` | reaches `*state.actor()` at `:517` — UB |
| `PairWalk::run` | `:564` | reaches `*state.actor()` at `:593` — UB |

`GameState::actor()` returns `nullopt` for any non-`Action` phase
(`engine/src/poker/game_definition.cpp:434-438`), so the estimator's three walks
dereference an empty optional. The simulator and practice table are worse: both
switch inside `while (true)` with no `Frontier` case and no `default`
(`engine/src/stage6/simulator.cpp:89`,
`engine/src/stage6/practice_table.cpp:253`), so a flop-terminal definition
never terminates. `mccfr_trainer.cpp:514-519` already throws by name, so the
defect is per-walker, not uniform.

**The resident path has no adapter.** The trainer emits `NSeatPolicy` keyed by
`NSeatInformationKey {own_card_bucket, node_index}`
(`engine/include/bs/nseat_trainer.hpp:100,121,134`), where `node_index` is a
tree-layout index resolved by `NSeatPolicy::lookup(tree, state, seat, own)`
(`:152-167`). The deployed path is
`ResidentPolicySet::hero_decision` → `find_row_words` → `find_bucket`
(`engine/src/resident/resident_policy.cpp:1096-1210`). The measurement stack
consumes `stage6::BehaviorPolicy` keyed by `InfosetKey` (own cards plus framed
public history tokens, `engine/include/bs/stage6/infoset_key.hpp:26-52`). The
only `NSeatPolicy` → `BehaviorPolicy` bridge is test-private
(`NSeatBehaviorAdapter`, `engine/tests/test_postflop_frontier.cpp:253`).

Two obstacles are worth naming because they shape the design. `find_bucket` is
**not** the serving entry point: the resident path canonicalizes the hero cards,
maps the board through `class_id_for_board`, and builds a ledger that
`find_bucket` does not (`resident_policy.cpp:753-760,1096-1210`). And
`solver::InformationKey` (`std::vector<std::uint64_t>`,
`engine/include/bs/heads_up_solver.hpp:83,96`) and stage-6 `InfosetKey` use
different layouts with `+1` offsets and per-street count framing; they are not
interchangeable.

## Goals

- **G1**: Every walker over a tree that can contain frontier leaves either
  models them through a supplied evaluator or throws naming the phase. The
  simulator and practice table terminate instead of spinning.
- **G2**: A measurement adapter reads the **resident serving path**, not a
  lookup that happens to be beneath it.
- **G3**: The two exploitability definitions in the tree are documented and
  labelled, and the n-seat one is implemented once.
- **G4**: A documented command reproduces a NashConv figure for a named,
  abstracted game, with its interval and miss count.

## Non-Goals

- **No strategy change.** No bucketing, sizing, cap, or training change.
- **No strength claim.** G4 is a reproducibility smoke test on a named abstracted
  game. The shipped library was trained on the full 1,326-combo range, and an
  exact evaluation on a 2..4-combo fixture is a different game, not a weaker
  estimate of that library's exploitability.
- **No full-range six-seat exact evaluation.** The pooled walk is exponential in
  joint-deal support and cannot run there.
- **No exposure of the private row-decode surface** unless a separate
  boundary decision is taken; see Dependency Rules.
- **No turn/river or preflop-rooted measurement.**

## Current State and Evidence

### Facts (verified in this repository)

- **Phase has five values, including `Frontier`**
  (`engine/include/bs/heads_up.hpp:22`).
- **The five walks above omit it**, with the consequences tabulated in
  Motivation.
- **`subgame_nash_conv.cpp` already handles the frontier.** It walks
  `NodeKind::TerminalFrontier` at `:375` and `:625` and accepts a
  `FrontierEvaluator`. Its inability to measure the shipped library is the
  adapter, not frontier support — the earlier claim that every harness predates
  the flop-terminal choice was too broad.
- **There are two exploitability definitions, not three.**
  `engine/benchmarks/subgame_nash_conv.cpp:552-553` computes BB per seat per
  hand (`nash_conv / live_seats / big_blind`).
  `engine/src/gto/heads_up_solver.cpp:754` computes `nash_conv / root.pot`, and
  `engine/src/gto/multistreet_cfr.cpp:610` computes
  `(brI - eqI) / pot + (brO - eqO) / pot`, which is algebraically the same
  pot-relative rule. Two definitions: BB/seat/hand and pot-relative.
- **Two other policy bridges exist, but not for `NSeatPolicy`.**
  `CandidateBehaviorPolicy`
  (`engine/include/bs/stage6/candidate_policy.hpp:104`) bridges frozen postflop
  artifacts, and `EngineServedPolicy`
  (`engine/include/bs/practice/engine_served_policy.hpp:23`) bridges the live
  engine host. Neither consumes a trained `NSeatPolicy` or a packed library.
- **The offline/live boundary is enforced by linked symbols.** The targets are
  defined at `engine/CMakeLists.txt:691-747`; the enforcement is
  `engine/cmake/stage6_offline_guard.cmake`,
  `engine/cmake/stage6_train_edge_guard.cmake`, and the `nm` scan in
  `engine/cmake/stage6_link_negative.cmake` (test registered at
  `engine/CMakeLists.txt:1041-1052`). It scans **linked symbols** and a fixed
  seven-source include closure; a new header that nobody links is not covered by
  the symbol scan.
- **The builder constructs the same evaluator the measurement must use.**
  `gto::EquityFrontierEvaluator` is constructed at
  `engine/src/flop_library/flop_library.cpp:348`. It already handles 3-, 4-, and
  5-card boards (`engine/src/gto/equity_frontier.cpp:191-215`).
- **A flop-rooted frontier state is a four-card state.** `close_street`
  (`engine/src/poker/game_definition.cpp:591`) sets `Phase::Deal`, and
  `after_card` (`:735-746`) sets `Frontier` only after a card is dealt. So the
  oracle and estimator deal one turn card and call the evaluator once per turn
  card — 49 times per line. The value is equivalent by linearity because the
  4-card evaluator enumerates rivers, but the cost is real and the tree-side
  counterpart is `AbstractTree`'s `FlopDeal` leaf
  (`engine/src/tree/abstract_tree.cpp:310-316`).

### Assumptions (to be verified)

- The evaluator used at measurement time is the same object training used. A
  cached or codebook-rounded evaluator could break the zero-sum property the
  estimator's `settle` checks (`engine/src/stage6/br_estimator.cpp:333`).

## Design Principles

- **Fail closed, never spin.** A phase a walker does not model throws naming the
  phase and the walker. A zero-value return and an infinite loop are both
  unacceptable.
- **Measure the served path.** The adapter wraps `hero_decision`, so
  canonicalization and ledger construction are inside the measurement.
- **One number, one definition, and label the other.**
- **An estimate is labelled an estimate** in code, output, and docs.

## Proposed Design

### Walker enumeration and the throwing default

Add a `Frontier` case to each of the five walks, and a throwing `default` to
every phase switch, including the simulator and practice table:

```cpp
case poker::Phase::Frontier:
  return settle_frontier(state, *frontier, log);
default:
  throw std::logic_error("walk does not model phase " +
                         std::to_string(static_cast<int>(state.phase())));
```

Each walker has a different settlement contract, and the RFC specifies them
rather than showing one snippet:

| Walker | Return shape | Frontier settlement |
| --- | --- | --- |
| `exact_oracle::recurse` | `std::array<double,10>` | evaluator value per seat, unweighted (the caller weights) |
| `exact_oracle::br_group` | `PooledValue{value, mass}` | evaluator value × `view.reach`, and add `view.reach` to `mass` |
| `LearnWalk`/`LegWalk`/`PairWalk` | `std::array<double,10>` (pair: on both legs) | evaluator value, then the existing zero-sum check at `br_estimator.cpp:333` applies |
| `HandSimulator`, `PracticeTable` | none (they advance state) | treat as terminal: settle and exit the loop |

The evaluator is a **required reference** in the walk signatures, not a
nullable pointer. A tree that cannot contain frontier leaves passes an explicit
`NoFrontier` evaluator that throws if reached, so "this tree has no frontier" is
an assertion rather than a null check. This removes the contradiction in the
earlier draft, which called the dependency required and then invited `nullptr`.

**G1/AC2 become measurable** through two mechanisms, because the walks are in
anonymous namespaces and have no public seam:
(a) `-Werror=switch` on the stage-6 eval target and on the simulator and
practice targets, so a future phase addition fails the build; and
(b) a table-driven test that drives each **named public entry point** with one
state per phase and asserts either a finite result or the named throw.

### Adapter over the resident path

The seam is the served lookup, not `find_bucket`:

```cpp
// engine/include/bs/stage6/measured_policy.hpp (offline eval surface)
class ResidentBehaviorPolicy final : public stage6::BehaviorPolicy {
 public:
  ResidentBehaviorPolicy(ResidentPolicySet& resident,
                         const poker::GameDef& query_def,
                         const std::array<int, 2>& hero);
  // BehaviorPolicy: answer the stage-6 InfosetKey by converting it to the
  // resident call's inputs and running the full serving path.
};
```

The conversion is specified, not assumed. `InfosetKey` carries `own[2]` plus
framed public tokens; the adapter reconstructs the `GameState` the resident call
needs from the query definition plus the token stream, then calls
`hero_decision`. It does **not** call `find_bucket`.

For the trainer-side comparison, a second adapter takes an `NSeatPolicy`:

```cpp
class NSeatBehaviorPolicy final : public stage6::BehaviorPolicy {
 public:
  NSeatBehaviorPolicy(const solver::NSeatPolicy& policy,
                      const tree::AbstractTree& tree);
  // Resolves node_index through NSeatPolicy::lookup(tree, state, seat, own),
  // exactly as the trainer does.
};
```

The interface carries the tree because `node_index` is not derivable from the
cards. The earlier draft's `lookup(const solver::InformationKey&, ...)` could not
express that, and mixed the two incompatible key layouts.

`hero_decision`'s signature and the `CompactRowView` decode are already public
through `ResidentPolicySet`; if a decode detail is not, the RFC forbids widening
its visibility unilaterally — that is a cross-module boundary change and needs
its own decision (see Dependency Rules).

### Two definitions, labelled

The n-seat BB/seat/hand rule is implemented once, in the offline eval target:

```text
nash_conv_bb_per_hand = sum_seats(BR[seat] - profile[seat]) / live_seats / big_blind
```

The two pot-relative harnesses keep their formula — they price a subgame gadget
in pot units — and each gains a comment naming its definition. No existing
number is reinterpreted.

### Reference document

`docs/reference/exploitability.md` (new) records both definitions, which
harness computes what, the fixture conventions, and the labeling rule. It is
registered in `docs/README.md` in the same change.

## Dependency Rules

- The adapter lives in the offline eval surface and may depend on
  `bigshark_resident` and `bigshark_solver`.
- It must not be linked into `bigshark_service`, the engine host, or any live
  binary. The enforcement is a linked-symbol `nm` scan plus the fixed
  seven-source include closure; a header-only addition must be added to
  `stage6_offline_guard.cmake`'s shell list explicitly, and the RFC says so
  because the symbol scan alone would miss it.
- **No private-header exposure without a separate decision.** If the adapter
  needs a decode API that is not currently public, that is a cross-module
  boundary change and is proposed separately rather than smuggled in here.

## Compatibility and Migration

- No persisted format changes. No library, checkpoint, or artifact is rewritten.
- Walk signatures gain a required evaluator parameter. Callers over
  showdown-terminal trees pass `NoFrontier` and behave exactly as before.
- The two pot-relative harnesses are unchanged in behavior and gain comments.

## Security and Operational Impact

- Offline only; no credentials, no network, no live play.
- Cost: the exact pooled walk is exponential. A four-card frontier multiplies
  per-line evaluator calls by the turn-card count (49), so a fixture that ran in
  seconds at a three-card leaf may take minutes.
- Observability: each run prints the fixture set, seat count, deal count,
  policy miss count, the evaluator identity, and the normalization used.

## Alternatives Considered

1. **Add frontier support only.** Leaves the library unmeasurable; the adapter is
   the point.
2. **Add the adapter over `find_bucket`.** Cheaper, but resolves the wrong bucket
   for any non-canonical flop and skips the ledger construction the served path
   performs. Rejected: it would measure something the engine does not run.
3. **Reimplement the resident lookup inside the harness.** Same objection, plus
   drift risk.
4. **Make the evaluator optional (`nullptr`).** Rejected: it repeats the pattern
   this RFC is fixing, where a missing dependency becomes a silent wrong answer.
5. **Unify the exploitability formula across all harnesses.** Rejected: the
   pot-relative figure measures a subgame gadget and converting it would
   silently reinterpret published numbers.

## Risks

1. **The MC estimator's known weakness recurs.** The chart-reach campaign already
   produced a null result
   (`docs/plans/0004-0006-implementation.md:4991-5046`). *Mitigation:* report the
   interval and miss count; a wide interval is a result.
2. **Frontier handling changes existing small-game numbers.** *Mitigation:* a
   showdown-terminal fixture must reproduce its recorded value bitwise.
3. **The four-card frontier multiplies cost by 49.** *Mitigation:* state it, and
   let the tree-side `FlopDeal` leaf be the optimization if the cost bites.
4. **The evaluator differs from training's.** *Mitigation:* use the same
   constructed type, and require the zero-sum check at frontier settlement.

## Verification Plan

- **Phase coverage.** A table-driven test drives each named public entry point
  with one state per `poker::Phase` and asserts a finite result or the named
  throw; `-Werror=switch` is enabled on the affected targets.
- **Simulator termination.** A flop-terminal definition runs to completion
  instead of looping.
- **Regression.** Existing showdown-terminal fixtures reproduce their recorded
  values within the tolerance those tests already use.
- **Adapter equivalence.** `NSeatBehaviorPolicy` answers identically to a direct
  `NSeatPolicy::lookup` on every information set of a named fixture;
  `ResidentBehaviorPolicy`'s answers match a recorded `hero_decision` trace.
- **End-to-end.** The documented command runs on a named abstracted game and
  prints a NashConv figure with interval, miss count, and evaluator identity.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

1. **Frontier and the throwing default.** Five walks plus the simulator and
   practice table. Gate: phase-coverage test and termination test pass; existing
   numbers unchanged.
2. **Adapter.** Both `BehaviorPolicy` implementations and their equivalence
   tests. Gate: equivalence tests pass and the offline guard covers the new
   header.
3. **Definitions and reference doc.** Implement the single BB/hand rule, comment
   the pot-relative pair, write and register the reference document. Gate:
   `check-docs` covers the new file.
4. **Reproduction.** Run the documented command on the named abstracted game.

Stop conditions: any change to an existing showdown-terminal number halts the
rollout until explained.

## Rollback Plan

- Stages 1-3 are additive; reverting restores the previous behavior. The only
  behavioral changes are that silent failures become exceptions and the
  simulator terminates.
- No persisted data is touched.

## Open Questions

1. **Which abstracted game is the standing reproduction target?** Decided here
   rather than deferred: the target is a **2-seat flop-rooted flop-terminal
   game with a 4-combo-per-seat range**, the smallest game where the pooled walk
   enumerates and a frontier leaf is still reached. Its NashConv is a property of
   that abstracted game, not of the shipped library, and the reference document
   says so. A full-range six-seat figure is out of scope (Non-Goals).
2. **Pooled or sampled best response?** Decided: pooled, on fixtures where it
   enumerates. No sampling estimator is in scope, so no sampling error is
   introduced.

## Acceptance Criteria

- **AC1**: The five named walks plus the simulator and practice table handle
  `Phase::Frontier` through a required evaluator reference, and every phase
  switch has a throwing default.
- **AC2**: A table-driven test drives each named public entry point with one
  state per `poker::Phase`; a flop-terminal definition runs to completion in the
  simulator and practice table; `-Werror=switch` is enabled on the affected
  targets.
- **AC3**: `NSeatBehaviorPolicy` matches a direct `NSeatPolicy::lookup` on every
  information set of a named fixture, and `ResidentBehaviorPolicy` matches a
  recorded `hero_decision` trace — not a `find_bucket` call.
- **AC4**: The documented command produces a NashConv figure, with interval,
  miss count, and evaluator identity, for the 2-seat 4-combo fixture named in
  Open Question 1.
- **AC5**: One BB/seat/hand formula exists in the n-seat code; the two
  pot-relative harnesses carry comments naming their different definition, and
  `docs/reference/exploitability.md` states both and is registered in
  `docs/README.md`.
- **AC6**: The full Engine Build Contract passes, ASan is clean, and
  `check-rfcs` / `check-docs` pass.

## Decision

Pending independent approval-agent review.
