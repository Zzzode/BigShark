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

The stage-6 evaluation stack cannot measure the resident serving path, for two
independent reasons. Its walks do not model `Phase::Frontier`, so a
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
(`engine/src/resident/resident_policy.cpp:1024-1142`). The measurement stack
consumes `stage6::BehaviorPolicy` keyed by `InfosetKey` (own cards plus framed
public history tokens, `engine/include/bs/stage6/infoset_key.hpp:26-52`). The
only `NSeatPolicy` → `BehaviorPolicy` bridge is test-private
(`NSeatBehaviorAdapter`, `engine/tests/test_postflop_frontier.cpp:253`).

Two obstacles are worth naming because they shape the design. `find_bucket` is
**not** the serving entry point. `hero_decision`
(`engine/src/resident/resident_policy.cpp:1024-1142`) canonicalizes the query
flop and the hero cards (`:1039-1044`, `:1066-1071`), resolves the root, calls
`find_row_words` → `find_bucket`, and then post-filters zero-reach hero combos
(`:1106-1110`). A direct `find_bucket` call on concrete cards skips the
canonicalization and the post-filter. (`class_id_for_board` is *not* part of
that path; its only caller is `engine/tests/test_flop_library.cpp:159`.) And
`solver::InformationKey` (`std::vector<std::uint64_t>`,
`engine/include/bs/heads_up_solver.hpp:83,96`) and stage-6 `InfosetKey` use
different layouts with `+1` offsets and per-street count framing; they are not
interchangeable.

## Goals

- **G1**: Every walker over a tree that can contain frontier leaves either
  models them through a supplied evaluator or throws naming the phase. The
  simulator terminates instead of spinning; the practice table gains the same
  exhaustive switch, and although it cannot currently be handed a flop-terminal
  definition, a future one cannot spin.
- **G2**: A measurement adapter reads the **resident serving path**, not a
  lookup that happens to be beneath it. The demonstrated number is on a named
  abstracted game, not on the shipped six-seat library; see Non-Goals.
- **G3**: The three exploitability normalizations in the tree are documented and
  labelled, and the n-seat BB/seat/hand one has a **single implementation that
  both n-seat harnesses link**, not two copies.
- **G4**: A documented command reproduces a NashConv figure for a named,
  abstracted game, with its miss count.

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
- **There are three exploitability normalizations live in the tree, not two.**
  A prior draft said two and would have written a false catalogue into
  `docs/reference/exploitability.md`:
  1. **BB per seat per hand** — `engine/benchmarks/subgame_nash_conv.cpp:552-553`
     computes `nash_conv / live_seats / big_blind`.
  2. **Pot-relative** — `engine/src/gto/heads_up_solver.cpp:754` computes
     `nash_conv / root.pot`, and `engine/src/gto/multistreet_cfr.cpp:610`
     computes `(brI - eqI) / pot + (brO - eqO) / pot`, which is algebraically
     the same rule (and is the one the two-player solver reports).
  3. **Summed chips per hand across seats** — `BrEstimatorResult` carries
     `gain[traverser][seed]` in chips/hand and
     `nashconv_replicates`, "Y_s = sum_i g_{i,s}"
     (`engine/include/bs/stage6/br_estimator.hpp:188-189`), and
     `stage6_measurement_driver.cpp:33-34` publishes both as "chips/hand … the
     scalar per-replicate seat sum". `preflop_nash_conv.cpp:143-149` prints the
     summed figure *and* a per-seat BB figure derived from it
     (`total.mean / seats / bb`).

  The third is not a rounding of the first: it is a different scalar, and it is
  what the stage-6 harness the RFC is about actually reports. The RFC therefore
  names all three and states which one it standardizes
  (`sum / (live_seats * big_blind)`) and which ones it leaves alone (the
  pot-relative rules belong to the two-player solver's own convention, and the
  estimator's summed chips figure is its public output).
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
  card — `52 - 3 - 2N` times per line, because board and every seat's hole
  cards are excluded (`engine/src/stage6/exact_oracle.cpp:104-113`): 45 at two
  seats, 37 at six. The value is equivalent by linearity because the
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
  canonicalization and the zero-reach post-filter are inside the measurement.
- **One number, one definition, and label the other.**
- **An estimate is labelled an estimate** in code, output, and docs.

## Proposed Design

### Walker enumeration and exhaustive switches

Add a `Frontier` case to each of the five walks. The phase switches carry **no
`default`**: every enumerator is listed explicitly, `Frontier` included, so
`-Wswitch` fires for a future sixth value. That is the repository's existing
precedent — `engine/src/policy/guarantee.cpp:29-38` switches exhaustively with
no `default` and falls through to a named result
(`engine/CMakeLists.txt:628-630`).

```cpp
case poker::Phase::Action:  ...
case poker::Phase::Deal:    ...
case poker::Phase::Folded:  ...
case poker::Phase::Showdown:...
case poker::Phase::Frontier:
  return settle_frontier(state, *frontier, log);
// no default: -Wswitch catches a new enumerator at compile time
```

A throwing `default` was the earlier draft. It cannot coexist with
`-Werror=switch`: with a `default` present, `-Wswitch` does not fire, so the
build-time guard becomes vacuous. Verified with a standalone `g++ -std=c++23
-Wall -Wextra -Werror=switch` probe — an enum switch with `default` produces no
diagnostic for an unhandled enumerator.

Each walker has a different settlement contract, and the RFC specifies them
rather than showing one snippet:

| Walker | Return shape | Frontier settlement |
| --- | --- | --- |
| `exact_oracle::recurse` | `std::array<double,10>` | evaluator value per seat, unweighted (the caller weights) |
| `exact_oracle::br_group` | `PooledValue{value, mass}` | evaluator value × `view.weight * view.reach`, and add `view.weight * view.reach` to `mass` |
| `LearnWalk`/`LegWalk`/`PairWalk` | `std::array<double,10>` (pair: on both legs) | evaluator value, then the existing zero-sum check at `br_estimator.cpp:333` applies |
| `HandSimulator` | none (it advances state) | **throws the named error**; it accepts a `GameDef`, so a flop-terminal definition reaches this today (`simulator.hpp:58-65`) |
| `PracticeTable` | none (it advances state) | **throws the named error** if ever reached. It cannot be reached today: `PracticeConfig` has no terminal-depth field (`practice_table.hpp:79-86`) and the file-local `make_def` never sets `terminal` (`practice_table.cpp:66-90`), so it stays at the `TerminalDepth::River` default. The switch still gains the case so a future flop-terminal configuration cannot spin. |

The `br_group` row is the one an implementer would get wrong, so it is spelled
out. Its sibling leaves in the same function weight by `weight * reach`:
`Folded` accumulates `out.mass += v->weight * v->reach`
(`engine/src/stage6/exact_oracle.cpp:329`) and `Showdown` uses
`const double w = v->weight * v->reach` (`:342-345`), with `BRView::weight`
documented as the unconditional joint-deal weight (`:146-150`). Using `reach`
alone would weight frontier leaves differently from fold and showdown leaves
inside the same pooled walk and produce a wrong NashConv — a plausible-looking
number with no error attached.

The evaluator is a **required reference** in the walk signatures, not a
nullable pointer. A tree that cannot contain frontier leaves passes an explicit
`NoFrontier` evaluator that throws if reached, so "this tree has no frontier" is
an assertion rather than a null check. This removes the contradiction in the
earlier draft, which called the dependency required and then invited `nullptr`.

**G1/AC2 become measurable** through two mechanisms, because the walks are in
anonymous namespaces and have no public seam:
(a) `-Werror=switch` on `bigshark_stage6_eval` and `bigshark_practice` — the
simulator compiles inside `bigshark_stage6_eval`
(`engine/CMakeLists.txt:703`), so there is no separate simulator target — so a
future phase addition fails the build; and
(b) a table-driven test that drives each **named public entry point** with one
state per phase and asserts either a finite result or the named throw.

### Adapter over the resident path

The seam is the served lookup, not `find_bucket`:

```cpp
// engine/include/bs/stage6/measured_policy.hpp (offline eval surface)
class ResidentBehaviorPolicy final : public stage6::BehaviorPolicy {
 public:
  ResidentBehaviorPolicy(ResidentPolicySet& resident,
                         const std::array<int, 2>& hero);
  // BehaviorPolicy: answer the stage-6 InfosetKey by converting it to the
  // resident call's inputs and running the full serving path.
};
```

The conversion is specified, not assumed. `distribution()` already receives the
live `GameState` (and `state.def()` is public,
`engine/include/bs/game_definition.hpp:226`), so the adapter does **not**
reconstruct a state. What it must synthesize is the
`std::span<const poker::PublicAction>` history from `ctx.hand_log`, which is a
per-street `HandLog` of `LoggedAction`
(`engine/include/bs/behavior_policy.hpp:57`). The constructor therefore takes the
resident set and the hero cards, not a query definition.

**Thread safety is part of the contract.** `hero_decision` mutates a
`ResidentScratch` of about 340 KiB
(`engine/include/bs/resident_policy.hpp:192`). The n-seat benchmark parallelizes
across **traversers** (`#pragma omp parallel for` over `p < seat_count`,
`engine/benchmarks/subgame_nash_conv.cpp:535-539`) with a separate across-deals
loop at `:466-467`; the new driver parallelizes across fixture deals, so both
patterns exist in the tree. The adapter owns one scratch per thread and a shared
instance would data-race.

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

The n-seat BB/seat/hand rule is implemented once, in the offline eval target.
Because the pooled walk is exact on its fixtures, the result is a single number
with **no interval**: an enumeration over the joint-deal support has no sampling
error. Only the Monte Carlo learned-deviation estimator produces intervals, and
it is not the path AC4 exercises.

```text
nash_conv_bb_per_hand = sum_seats(BR[seat] - profile[seat]) / live_seats / big_blind
```

The two pot-relative harnesses keep their formula — they price a subgame gadget
in pot units — and each gains a comment naming its definition. No existing
number is reinterpreted.

### The measurement driver

AC4 needs a binary that does not exist. No current target links
`bigshark_stage6_eval` (which owns the exact oracle and the adapter) with
`bigshark_resident`: `bigshark-subgame-nash-conv` links only `bigshark_solver`
and takes an `NSeatPolicy` rather than a `BehaviorPolicy`
(`engine/CMakeLists.txt:594-601`), and `bigshark-stage6-measurement-driver`
links eval and train but loads no library and never calls the exact oracle.
`exact_*_utility` is currently called only from `engine/tests/` and
`br_estimator.cpp:865`.

This RFC adds `bigshark-stage6-resident-nash-conv`, an offline binary linking
`bigshark_stage6_eval` + `bigshark_resident`, with the signature:

```text
bigshark-stage6-resident-nash-conv --library <dir> --fixture <name>
                                   [--players N] [--iterations M]
```

It builds the named fixture's `GameDef`, wraps the resident set in
`ResidentBehaviorPolicy`, runs the exact best-response and oracle-value entry
points, and prints the NashConv figure with the evaluator identity and the
adapter's miss count. It is introduced in Rollout stage 2 and must be added to
the offline guard's include closure, since the `nm` scan only covers linked
symbols.

**The oracle's public API must change, and that is a named, sized part of this
RFC.** No entry point takes a `FrontierEvaluator` today —
`grep FrontierEvaluator engine/include/bs/stage6/exact_oracle.hpp` returns
nothing — because each constructs its own `GameState` internally
(`engine/src/stage6/exact_oracle.cpp:386`, `:457`). The walks that would take an
evaluator are in an anonymous namespace (`:12`) with no seam. So the design is
not "the driver passes an evaluator"; it is:

1. Give `exact_deal_utility`, `exact_oracle_utility`,
   `exact_best_response_utility`, and
   `exact_omniscient_deviation_utility`
   (`engine/include/bs/stage6/exact_oracle.hpp:60-114`) a required
   `const FrontierEvaluator&` parameter, and thread it into the internal walks.
2. Migrate every caller. The set is 42 call expressions across
   `engine/tests/test_stage6_simulator.cpp` (16),
   `engine/tests/test_stage6_br_estimator.cpp` (19),
   `engine/tests/test_postflop_frontier.cpp` (3),
   `engine/tests/test_postflop_templates.cpp` (1),
   `engine/benchmarks/subgame_nash_conv.cpp` (2), and
   `engine/src/stage6/br_estimator.cpp:865` (1). Every one passes `NoFrontier`
   except the two `test_postflop_frontier.cpp` sites that mean to exercise a
   frontier and the new driver.

Making the parameter required rather than defaulted is deliberate: a defaulted
`NoFrontier` would let a new frontier-bearing caller silently throw at runtime
instead of failing to compile, which is the failure mode this RFC exists to
remove. AC1 asserts the count so the migration is checked, not estimated.

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
- Walk signatures gain a required evaluator parameter, and the four public
  oracle entry points do too. 42 call expressions migrate, every one passing
  `NoFrontier` except the frontier-exercising tests and the new driver. Callers
  over showdown-terminal trees behave exactly as before, and `Frontier` is
  unreachable in a showdown-terminal tree
  (`engine/src/poker/game_definition.cpp:739-742` gates it on
  `TerminalDepth::Flop`), so no existing number can move.
- The two pot-relative harnesses are unchanged in behavior and gain comments.
- No Legacy Code is satisfied explicitly: no flag selects an old and a new
  walk, no versioned path is retained, and `NoFrontier` is an assertion type
  that throws rather than a fallback. No persisted format changes.

## Security and Operational Impact

- Offline only; no credentials, no network, no live play.
- Cost: the exact pooled walk is exponential. A four-card frontier multiplies
  per-line evaluator calls by the available-card count (`52 - 3 - 2N`, so 45 at
  two seats and 37 at six), so a fixture that ran in seconds at a three-card
  leaf may take a minute.
- Observability: each run prints the fixture set, seat count, deal count,
  policy miss count, the evaluator identity, and the normalization used.

## Alternatives Considered

1. **Add frontier support only.** Leaves the library unmeasurable; the adapter is
   the point.
2. **Add the adapter over `find_bucket`.** Cheaper, but resolves the wrong bucket
   for any non-canonical flop and skips the canonicalization and zero-reach
   post-filter the served path performs. Rejected: it would measure something the engine does not run.
3. **Reimplement the resident lookup inside the harness.** Same objection, plus
   drift risk.
4. **Make the evaluator optional (`nullptr`).** Rejected: it repeats the pattern
   this RFC is fixing, where a missing dependency becomes a silent wrong answer.
5. **Unify the exploitability formula across all harnesses.** Rejected: the
   pot-relative figure measures a subgame gadget and converting it would
   silently reinterpret published numbers.

## Risks

1. **The MC estimator's known weakness would recur if it were used.** No AC
   exercises it — AC4's path is the exact pooled walk — but a future MC run
   should expect the null result the chart-reach campaign already produced
   (`docs/plans/0004-0006-implementation.md:4991-5046`). *Mitigation:* that is
   why this RFC does not build an MC path; each reported number names the method
   that produced it.
2. **Frontier handling changes existing small-game numbers.** *Mitigation:* a
   showdown-terminal fixture must reproduce its recorded value bitwise.
3. **The four-card frontier multiplies cost by `52 - 3 - 2N`.** *Mitigation:* state it, and
   let the tree-side `FlopDeal` leaf be the optimization if the cost bites.
4. **The evaluator differs from training's.** *Mitigation:* use the same
   constructed type, and require the zero-sum check at frontier settlement —
   carried into AC4, because a mitigation with no acceptance criterion is a
   hope.

## Verification Plan

- **Phase coverage.** A table-driven test drives each named public entry point
  with one state per `poker::Phase` and asserts a finite result or the named
  throw; `-Werror=switch` is enabled on the affected targets.
- **Simulator termination.** A **preflop-rooted** flop-terminal definition
  throws the named error instead of looping. The simulator does not currently
  reject that shape — it validates seat count and hole count only
  (`engine/src/stage6/simulator.cpp:58-61`) and never reads `def.terminal` or
  `def.board_size` — which is precisely why it spins today. This is a different
  fixture from Open Question 1's flop-rooted target. The practice table is
  covered by driving the extracted phase-dispatch function with a constructed
  `Frontier` state
  directly, since no configuration can reach one.
- **Regression.** Existing showdown-terminal fixtures reproduce their recorded
  values within the tolerance those tests already use.
- **Adapter equivalence.** `NSeatBehaviorPolicy` answers identically to a direct
  `NSeatPolicy::lookup` on every information set of a named fixture;
  `ResidentBehaviorPolicy`'s answers match a recorded `hero_decision` trace.
- **End-to-end.** The documented command runs on a named abstracted game and
  prints a NashConv figure with miss count, evaluator identity, and decision source.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

1. **Frontier and exhaustive switches.** Five walks plus the simulator and
   practice table. First run `-Werror=switch` over the target and record any
   pre-existing non-exhaustive switch it surfaces (for example in
   `translator.cpp` or `adapter.cpp`); those are in-scope fixes, not surprises.
   Gate: phase-coverage test and termination test pass; existing numbers
   unchanged.
2. **Adapter, driver, and fixture.** Both `BehaviorPolicy` implementations,
   their equivalence tests, the `bigshark-stage6-resident-nash-conv` binary, and
   the 4-combo fixture library built through the library API. Gate: equivalence
   tests pass, the offline guard covers the new header, and the driver runs on
   the fixture with zero policy misses.
3. **Definitions and reference doc.** Implement the single BB/hand rule, comment
   the pot-relative pair, write and register the reference document. Gate:
   `check-docs` covers the new file.
4. **Reproduction.** Run `bigshark-stage6-resident-nash-conv` on the named
   abstracted game and record the figure with its evaluator identity.

Stop conditions: any change to an existing showdown-terminal number halts the
rollout until explained — not because `Frontier` can appear in such a tree
(`engine/src/poker/game_definition.cpp:739-742` makes that impossible), but
because the added case sits in the shared settlement switch and the check
catches a mistake there.

## Rollback Plan

- Stage 1 is a **signature break** across every walk call site (the evaluator
  becomes a required reference), so it is not additive. Once callers migrate,
  stages 2-3 cannot be reverted independently: rollback is a single revert of
  the whole change.
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

- **AC1**: The five named walks handle `Phase::Frontier` through a required
  evaluator reference; `HandSimulator` and `PracticeTable` throw the named
  error instead; and **no** phase switch has a `default`, so `-Wswitch` remains
  live for a future enumerator.
- **AC2**: A table-driven test drives each named public entry point with one
  state per `poker::Phase`; a **preflop-rooted** flop-terminal definition makes
  `HandSimulator` throw the named error instead of looping.

  **`PracticeTable` cannot be tested through its current interface, and the RFC
  says so rather than pretending otherwise.** It exposes only `play_hand` and
  observers (`engine/include/bs/stage6/practice_table.hpp:88-131`), `Impl` is
  private, and `play_hand` builds its own `GameState` from the file-local
  `make_def` (`engine/src/stage6/practice_table.cpp:237,246`) before entering a
  private `while (true) switch` (`:252-253`). No test can hand that switch a
  constructed `Frontier` state.

  The phase dispatch is therefore **extracted into a testable free function**
  taking `(GameState, HumanAgent&, observers)` and returning the hand utility;
  `play_hand` becomes a thin wrapper that builds the state and calls it. The
  table-driven test drives the extracted function directly with a constructed
  `Frontier` state. That extraction is part of the change, not an optional
  refactor — without it the AC is unimplementable, and the `-Werror=switch`
  guard alone cannot prove the case behaves, only that it exists.

  `-Werror=switch` is enabled on `bigshark_stage6_eval` and `bigshark_practice`.
- **AC3**: `NSeatBehaviorPolicy` matches a direct `NSeatPolicy::lookup` on every
  information set of a named fixture; `ResidentBehaviorPolicy` matches a recorded
  `hero_decision` trace — not a `find_bucket` call — and the driver holds one
  `ResidentScratch` per worker thread. The last clause is a property of the new
  driver rather than of the adapter in isolation: the driver parallelizes across
  fixture deals, so the requirement is exercised there and a shared scratch
  would fail under the sanitizer build. An earlier draft stated it as an
  adapter-only property with no parallel caller, which nothing could falsify. The trace is produced from a **2-seat library
  built for this RFC**. Note that the builder's CLI cannot produce it: `--range`
  accepts only `premium` or `full`
  (`engine/benchmarks/flop_library_builder.cpp:122-133`), so the fixture library
  is built through the library API with an explicit 4-combo
  `BuildParams.range` (`engine/include/bs/flop_library.hpp:155-168`), via a
  small fixture driver committed with this RFC. `same_root` requires equal `player_count` and `pot`
  (`engine/src/resident/resident_policy.cpp:55`), so the shipped six-seat
  artifact cannot serve this fixture and is not used by any AC.
- **AC4**: `bigshark-stage6-resident-nash-conv` produces a single NashConv
  number (no interval: the pooled walk is exact on its fixture) with the
  evaluator's identity, for the 2-seat 4-combo fixture named in Open Question 1,
  **with zero policy misses**. The fixture's `GameDef` is the library's declared
  root verbatim, reconstructed from the manifest's **scalar** fields plus the
  library's construction rule: `LibraryManifest` carries `player_count`, `stack`,
  `contribution`, `big_blind`, and per-class `canonical_board`
  (`engine/include/bs/flop_library.hpp:50-88`), while `stacks`,
  `contributions`, `folded`, `button`, and `pot` are **not** stored — they are
  uniform and `button = 0` by construction (`engine/src/flop_library/flop_library.cpp:73-92`),
  with `pot = player_count * contribution`. The RFC says this instead of
  implying per-seat manifest fields that do not exist; an earlier draft did.
  `same_root` requires all of them
  (`engine/src/resident/resident_policy.cpp:53-87`), and a mismatch would make
  every lookup miss while still "producing a number" that measures the fallback.

  **The figure must be non-negative, and the driver must check why.** The
  deviation menu is `declared_behavior_menu`, built from `kPotFractions`
  (`{1/3, 1/2, 3/4, 1/1, 3/2}`, `engine/src/behavior/behavior_policy.cpp:18-24`)
  plus min and cap, while the resident rows are sealed under the library's own
  `ActionAbstraction` schedule (`{1/3, 3/4, 3/2}` bets, `{1/2, 1}` raises,
  `engine/src/abstraction/abstraction.cpp:81-90`). `exact_oracle.hpp:104-109`
  warns that BR−profile can go negative when the profile's support is not
  contained in the declared menu — an honest property of the quantity, and a
  meaningless number if it happens here. AC4 therefore requires the driver to
  assert menu containment for every resident row it serves and to fail loudly
  rather than print a negative figure, instead of leaving it to be read.

  **Frontier settlement must be zero-sum-checked.** `settle_fold`/`settle_showdown`
  carry a zero-sum test (`engine/src/stage6/br_estimator.cpp:313-335`); the new
  evaluator settlement has no equivalent, so an evaluator that breaks the
  zero-sum assumption stated under Assumptions would corrupt the figure with no
  error. AC4 requires the same check at the frontier settlement in the oracle
  and in the estimator.
- **AC5**: One BB/seat/hand formula exists in the n-seat code, in a shared
  location that **both** `bigshark-subgame-nash-conv` and the new stage-6
  resident driver link, and `engine/benchmarks/subgame_nash_conv.cpp:551-553`
  calls it instead of computing `nash_conv / live_seats / big_blind` itself.
  This is not satisfied by writing the formula in the new driver alone: the
  existing implementation is in `subgame_nash_conv.cpp`, that target links only
  `bigshark_solver` (`engine/CMakeLists.txt:594-599`), and the new driver would
  otherwise become the second copy — the exact duplication the AC forbids. The
  two pot-relative harnesses carry comments naming their different definition,
  `stage6_measurement_driver.cpp`'s summed-chips output keeps its own documented
  label, and `docs/reference/exploitability.md` states all three and is
  registered in `docs/README.md`.
- **AC6**: The full Engine Build Contract passes, ASan is clean, and
  `check-rfcs` / `check-docs` pass.

## Decision

Pending independent approval-agent review.
