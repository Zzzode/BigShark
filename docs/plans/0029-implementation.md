# RFC 0029 Implementation Plan

Status: Current

Execution state: RFC 0029 is Accepted (independent approval record in the RFC's
Decision section, 2026-10-21). This plan maps its stages to tasks, evidence, and
gates. The RFC cannot reach `Implemented` from repository evidence: its headline
deliverable is a measurement driver whose acceptance criterion requires a corpus
that does not exist (`sessions/` is empty and gitignored). It stops at
`Implementing` until that run happens under separate authorization.

## Ordering relative to RFC 0030

RFC 0030 changes the action-abstraction digest, which rewrites every shipped
artifact once. Stage 1 here is a **signature break** across 34 call expressions;
doing it first keeps that conflict surface small, because the two changes touch
disjoint code (`engine/src/stage6/`, `engine/benchmarks/` versus
`engine/src/abstraction/`). There is no artifact-format interaction: AC4
reconstructs the fixture root from manifest **scalars**, and RFC 0030 does not
change those.

## Stage 1 — Walk signatures and the oracle API

Status: not started.

This stage is a signature break, so it is not additive: once callers migrate, it
cannot be reverted independently of stages 2-3.

### Tasks

1. **`Phase::Frontier` in the five walks.** Each handles the phase through a
   required `const FrontierEvaluator&`, with these settlement contracts — they
   differ and the difference is where a wrong number would come from:

   | Walker | Return | Frontier settlement |
   | --- | --- | --- |
   | `exact_oracle::recurse` | `std::array<double,10>` | evaluator value per seat, unweighted (the caller weights) |
   | `exact_oracle::br_group` | `PooledValue{value, mass}` | value and mass both weighted by **`view.weight * view.reach`** |
   | `LearnWalk`/`LegWalk`/`PairWalk` | `std::array<double,10>` | evaluator value, then the existing zero-sum check |
   | `HandSimulator` | none | throws the named error |
   | `PracticeTable` | none | throws the named error |

   The `br_group` row is the one to get right: its sibling leaves in the same
   function weight by `weight * reach` (`exact_oracle.cpp:329`, `:342-345`), so
   `reach` alone would weight frontier leaves differently from fold and showdown
   leaves in one pooled walk.

2. **`HandSimulator` termination.** A preflop-rooted flop-terminal definition
   must throw instead of spinning. The simulator currently validates seat and
   hole counts only (`engine/src/stage6/simulator.cpp:58-61`) and never reads
   `def.terminal` or `def.board_size`.

3. **`PracticeTable` phase-dispatch extraction.** The private `while (true)`
   switch (`engine/src/stage6/practice_table.cpp:252-253`) cannot be handed a
   constructed state. Extract it into a testable free function taking
   `(GameState, HumanAgent&, observers)`; `play_hand` becomes a wrapper. This is
   part of the change, not a refactor option — without it AC2 is unimplementable.

4. **The four public oracle entry points** gain a required
   `const FrontierEvaluator&` and thread it into the internal walks
   (`engine/include/bs/stage6/exact_oracle.hpp:60-114`). Required, not defaulted:
   a default would let a new frontier-bearing caller throw at runtime instead of
   failing to compile.

5. **Migrate 34 call expressions**, every one passing `NoFrontier`:
   `test_stage6_simulator.cpp` (14), `test_stage6_br_estimator.cpp` (17),
   `test_postflop_frontier.cpp` (1), `test_postflop_templates.cpp` (1),
   `br_estimator.cpp:865` (1). The exception list is **empty** — no existing call
   site sets `TerminalDepth::Flop`, and `test_postflop_frontier.cpp`'s single call
   is River-terminal (`:288-291`). A grep over the four names is the
   verification, recorded with the commit.

6. **`-Werror=switch`** on `bigshark_stage6_eval` and `bigshark_practice`. This is
   preventive: `translator.cpp` (`:127, :189, :208`) and `adapter.cpp:75` are
   already exhaustive and default-free over their enums, and `adapter.cpp:26`'s
   `default:` is over a `std::size_t` where `-Wswitch` never applies. No existing
   findings are expected. Scope the no-`default` rule to the five walks plus the
   two throw-only walkers; `mccfr_trainer.cpp:495-519` and
   `nseat_trainer.cpp:598-611` already handle `Frontier` without a `default`.

### Gates

`ctest` green; `-Werror=switch` builds clean; the 34-site grep recorded; existing
showdown-terminal fixtures reproduce their recorded values within the tolerance
those tests already use.

### Rollback

A single revert of the signature change. No persisted format.

## Stage 2 — Adapters and the resident measurement driver

Status: not started.

### Tasks

1. **`NSeatBehaviorPolicy`** — an adapter over `NSeatPolicy` resolving
   `node_index` through `NSeatPolicy::lookup`, exactly as the trainer does.
2. **`ResidentBehaviorPolicy`** — an adapter over `ResidentPolicySet` that calls
   `hero_decision` rather than `find_bucket`, holding **one `ResidentScratch` per
   worker thread** (the driver parallelizes across fixture deals, so a shared
   instance would data-race and the sanitizer build would catch it).
3. **The fixture library.** Built through the library API with an explicit
   4-combo `BuildParams.range` (`engine/include/bs/flop_library.hpp:155-168`);
   the builder CLI cannot produce it, because `--range` accepts only
   `premium|full` (`engine/benchmarks/flop_library_builder.cpp:126-131`). Pin
   `stack = 10`, `contribution = 2`, `big_blind = 2`, and record the flop class
   chosen from the manifest's `canonical_board`, excluding any
   `undealable_boards` entry (`engine/src/flop_library/flop_library.cpp:293-318`).
4. **`bigshark-stage6-resident-nash-conv`** — an offline binary linking
   `bigshark_stage6_eval` + `bigshark_resident`. It builds the fixture's
   `GameDef` from the manifest scalars plus the library's construction rule,
   runs the exact best-response and oracle-value entry points, and prints the
   NashConv figure with the evaluator identity and the adapter's miss count. It
   must be added to the offline guard's include closure, since the `nm` scan only
   covers linked symbols.
5. **The shared BB/seat/hand formula** moves into **`bigshark_solver`**, because
   `bigshark-subgame-nash-conv` links only that target
   (`engine/CMakeLists.txt:594-599`) and a shared implementation both harnesses
   call cannot live in a benchmark. `subgame_nash_conv.cpp:551-553` then calls
   it instead of computing `nash_conv / live_seats / big_blind` itself.

### Gates

- AC3: both adapters match their recorded traces on every information set of the
  fixture.
- AC4: the documented command produces a single NashConv figure for the pinned
  fixture with zero policy misses, the evaluator identity, and a **measured
  uncontained-node count of 0**. The count is defined as: over every action node
  reachable from the fixture root under the abstract tree's own menus
  (`bs::tree::abstract_node_menu`, `engine/include/bs/abstract_tree.hpp:41`), the
  number whose menu contains an action absent from `declared_behavior_menu`
  (`engine/include/bs/behavior_policy.hpp:104`). Both are already public.
- AC4 also requires a **zero-sum check at frontier settlement**, mirroring
  `settle_fold`/`settle_showdown` (`engine/src/stage6/br_estimator.cpp:313-335`),
  and the driver **fails loudly rather than printing a negative figure**.

### Fixture containment note

The fixture sits **one chip** from the containment boundary: with
`big_blind = 2, contribution = 2` the uncontained count is 0 at stacks 4-10 and 1
at stack 11. The reference document records the invariant ("no re-raise node with
an aggressive action is legal at this depth") rather than the samples, and notes
that changing `big_blind`, `contribution`, or adding an ante re-opens the
divergence at unchanged `stack`. The root cause is that
`declared_behavior_menu` omits `actor_committed`
(`engine/src/behavior/behavior_policy.cpp:75-77`) while the tree adds it
(`engine/src/abstraction/abstraction.cpp:211,225`); fixing that is Open Question
2, deferred because it widens the estimator's published coarse-game NashConv.

### Rollback

The adapters and the driver are new files; the shared formula is one function.
Revert restores `subgame_nash_conv.cpp`'s inline arithmetic.

## Stage 3 — Reference document

Status: not started.

`docs/reference/exploitability.md` (new) records all three normalizations, which
harness computes what, the fixture conventions including its pinned parameters
and containment invariant, and the labeling rule. Registered in `docs/README.md`
in the same change.

### Gates

`check-docs` and `check-rfcs` pass; the document states the three definitions
without collapsing them.

## Stage 4 — Measurement run (cannot be discharged from the repository)

Status: blocked on live play under separate authorization.

`docs/plans/rfc0029-replay.md` records the figure for the pinned fixture with the
corpus SHA-256 and extraction command. Until that exists the RFC stays
`Implementing`.

## Cross-cutting

- **No Legacy Code.** `NoFrontier` is a required-reference assertion that throws,
  not a flag selecting an implementation and not an internal version. It must
  never acquire a value-returning fallback.
- **Documentation lands with its stage**: `docs/design/gto-engine.md` for the
  frontier settlement contracts, `docs/reference/exploitability.md` for the
  taxonomy.
- **The fixture library is the RFC's own deliverable**, not an input; the
  reviewer's probes established tree shape and deal enumeration independently but
  did not train rows.

## Verification commands per stage

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target benchmark-multistreet
npm run check
npm run proto:check
node bin/replay.mjs
node bin/check-rfcs.mjs
node bin/check-docs.mjs
```

ASan is required for stage 1 (the walks gain a reference parameter that must not
outlive its evaluator) and stage 2 (per-thread scratch).
