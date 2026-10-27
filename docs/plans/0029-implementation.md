# RFC 0029 Implementation Plan

Status: Current

> **Provider-retirement note (2026-10-27, [RFC 0040](../rfcs/0040-retire-river-club-agent-api.md)).** Every reference in this plan to the River Club platform or Agent API v2.1, `apps/river-club-agent`, `platforms/river-club`, River bin launchers, `sessions/` journal replay, and provider token configuration is **Historical**: that integration was retired and deleted. Such references are retained only as dated evidence and must not be executed as current instructions. Engine-side scope, gates, and evidence remain Current.


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

Status: implemented (2026-10-22). Verified by `ctest` and the 34-site grep
recorded below; stage 2 has not started.

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
   `(GameState, const PracticeHand&, const HumanAgent&, SplitMix64&,
   PracticeObserver*)`; `play_hand` becomes a wrapper. The plan's original
   signature `(GameState, HumanAgent&, observers)` is not sufficient: the
   dispatch also reads the hole cards, the runout, the human seat, the hand id,
   and one bot policy per seat, none of which the `GameState` carries — so those
   moved into a `PracticeHand` argument rather than being re-derived inside the
   function from state the caller cannot supply. This is part of the change, not
   a refactor option — without it AC2 is unimplementable.

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

### Implementation evidence

**The 34-site grep, exactly as the plan predicted:**

| File | Oracle entry-point call expressions |
| --- | --- |
| `engine/tests/test_stage6_simulator.cpp` | 14 |
| `engine/tests/test_stage6_br_estimator.cpp` | 17 |
| `engine/tests/test_postflop_frontier.cpp` | 1 |
| `engine/tests/test_postflop_templates.cpp` | 1 |
| `engine/src/stage6/br_estimator.cpp` | 1 |
| **Total** | **34** |

Counted by matching each of the four names to its balanced parenthesised
argument list and checking the list contains an evaluator, so a comment that
merely mentions a name is not counted.

A further 42 expressions pass an evaluator too — the `mc_*`,
`estimate_deviation_gains`, and `learn_exact_frozen_best_response` entry points,
which gained the same parameter because they thread it into the three walks or
into the oracle. Over the same six files with the same balanced-call rule and
allowing any evaluator argument (the controls in `test_frontier_settlement_zero_sum`
pass named evaluators rather than `NoFrontier`), the total is 76 and none is
missing one. That is beyond the plan's "four names" enumeration, not a deviation
from it: those functions had no way to reach a frontier leaf without it, and one
of them was actively wrong (see the fifth-entry-point note below).

**`-Werror=switch` found three live defects, not preventive-only.** The plan
predicted no existing findings. In fact `br_estimator.cpp`'s `LearnWalk`,
`LegWalk`, and `PairWalk` each switched on `Phase` with no `Frontier` arm, which
is precisely the silent fall-through the flag exists to catch. All three now have
the arm. The flag stays where the plan put it — `bigshark_stage6_eval` and
`bigshark_practice` — and every phase switch in that scope carries no `default:`.

**`PracticeTable` needed a second assertion the flag cannot provide.**
`-Werror=switch` proves the `Frontier` arm exists; it cannot prove the arm
behaves. `run_practice_hand` is the seam that makes behaviour testable, and
`test_phase_dispatch_reaches_frontier` drives a *constructed* flop-terminal state
through it. Mutation-checked: replacing the throw with a fold settlement fails
exactly three assertions (`a constructed flop-terminal state throws instead of
spinning`, `the frontier throw names the frontier leaf`, `a mid-deal constructed
state also reaches the frontier throw`) and leaves the rest green.

**A fifth public entry point had the same defect as the walks.** The plan
enumerated four oracle entry points plus the three MC ones. A fifth,
`learn_exact_frozen_best_response`, hardcoded `bs::gto::NoFrontier{}` inside its
own body while calling `exact_best_response_utility` — so a flop-terminal caller
would compile cleanly and then throw at the first frontier leaf, precisely the
runtime failure the required-reference design exists to prevent. It now takes the
evaluator too, and its 10 test call sites pass `NoFrontier`. This is the same
class of bug as the missing zero-sum check: an internal default that quietly
contradicts the signature contract.

**The three walks AND both oracle settlements hold the frontier value to the
zero-sum check.** Table row 3 of this stage's contract says "evaluator value,
then the existing zero-sum check", and AC4 separately requires the check "in the
oracle and in the estimator". The first implementation did neither correctly:
`WalkBase::settle` returned the evaluator's vector before reaching the check, and
neither `exact_oracle::recurse` nor `exact_oracle::br_group` had any check at
all, even though `exact_oracle.hpp` documents the returned vector as zero-sum.
All three are fixed. `test_frontier_settlement_zero_sum` drives a real
three-seat preflop-rooted flop-terminal fixture through all three settlements
with paired controls:

- a zero-sum evaluator, which must be accepted — with a positive-control
  assertion per walk that it was actually consulted (and, for the estimator,
  that the leaf carried three board cards), so a walk that never reaches the
  frontier branch cannot pass vacuously;
- one biased by a single chip, which must abort with an error naming zero-sum.

`br_group` checks zero-sum **per deal, before applying `weight * reach`**: the
pool weights each deal differently, so a check on the pooled total would let
compensating errors at two deals cancel and pass.

The negative control caught this test's own first fixture, which split the pot as
`pot/n` per seat and therefore summed to `pot`, not zero: the check rejected it
and the positive control failed. That is the check doing its job on a real
mistake, and the fixture is now zero-sum by construction.

**`HandSimulator` needed the same construction.** `test_simulator_flop_terminal_throws`
builds the preflop-rooted flop-terminal `GameDef` from
`test_game_definition.cpp:case_flop_terminal_frontier`, runs it through a real
`HandSimulator`, and requires `stage6_sim_error` naming the frontier. Mutation
checked: changing the thrown message fails exactly the naming assertion.

**Sanitizer evidence.** `asan` was run on every target this stage touches.
`stage6_simulator` and `stage6_practice` pass clean under ASan+UBSan, including
the new frontier tests. `stage6_br_estimator` completed under ASan+UBSan with
`STAGE6 BR ESTIMATOR TESTS PASSED` and no sanitizer diagnostic — a 48-minute run
against 6 minutes in release, so it was run separately rather than inside
`ctest`. One pre-existing interaction is recorded rather than hidden:
`test_stage6_practice` aborts inside ASan's `annotate_contiguous_container` check
when `EngineProcess::start()` copies a `GetCapabilitiesResponse`
(`engine/src/practice/engine_process.cpp:174`) — a vendored-protobuf
`RepeatedField` copy in code this stage does not touch. With
`detect_container_overflow=0` the suite passes, which locates the abort in that
pre-existing interaction rather than in the new dispatch seam.

**A pre-existing gap was not papered over.** `HandSimulator::run` validates seat
and hole counts but never reads `def.terminal` or `def.board_size`; the frontier
throw is reached only because the state machine produces the phase. That is
correct for a river-terminal def and sufficient for the AC — the phase is the
single source of truth — but it means a malformed flop-terminal def with an
inconsistent board size would report the generic frontier error, not a specific
one. Recorded rather than fixed: the AC asks for the named throw, and a
defensive pre-check would be a second, untested path.

### Rollback

A single revert of the signature change. No persisted format.

## Stage 2 — Adapters and the resident measurement driver

Status: implemented (2026-10-22). The figure below is the stage's deliverable;
see "Implementation evidence" for the three defects the work found.

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

### Implementation evidence

**The figure, and what it took to get one.** The documented command on the
pinned fixture produces:

```text
NashConv       : 2.711476 chips/hand (summed over 2 live seats)
exploitability : 1.355738 chips/hand = 0.677869 bb/hand
served/misses  : 1056 / 0
uncontained    : 0 action nodes
action abstract: rfc0007-pot-fractions
evaluator      : equity-frontier/all-in-at-flop
```

AC4's four clauses are all satisfied and all measured rather than asserted: one
figure, zero misses, a measured uncontained count of 0, and both identities. The
fixture is `stack = 10`, `contribution = 2`, `big_blind = 2`, two seats, the
pinned 4-combo range, one flop class.

**Four things went wrong before the number arrived, and each changed the
design.**

1. **A `FlopDeal` leaf is not a chance node, and treating it as one produced a
   plausible wrong number.** The first working driver walked the abstract tree
   but routed `NodeKind::FlopDeal` into the chance arm, which enumerated
   `public_runout_cards` and found no matching children — so every branch was
   skipped and the strategy value collapsed to the fold terminals alone. The
   figure it printed was 0.488865, which looks entirely reasonable. A
   `FlopDeal` leaf collapses the whole three-card runout into ONE frontier leaf
   and carries the completed flop in its payload; for a flop-rooted fixture it
   is settled directly from the state it was reached at. With that fixed the
   figure is 2.711476. The zero-sum assertion on the strategy profile is what
   makes this class of error visible at all: a mis-settled terminal breaks
   conservation, and the driver now refuses to print a profile whose sum is
   non-zero.

2. **`exact_oracle_utility` cannot measure this fixture, and the reason is the
   point of the RFC.** The first driver wired the resident adapter into the
   exact oracle and got `off-tree-amount`. The oracle recurses the raw L1 game,
   which contains action sequences the 4-combo library never trained; the
   resident path answers only nodes in its own abstraction. A miss there is
   correct behaviour, not a bug — but it means a figure produced that way would
   be measuring the fallback. The driver now walks the **abstract tree**, asking
   the served policy at each of its nodes. That is also what makes the
   uncontained-node census meaningful: it counts nodes in the same tree the walk
   visits.

3. **The fixture range must be the trained range, not the library's premium
   profile.** Using `declared_library_range()` (40 combos) against a 4-combo
   artifact produced `untrained-combo` on the first decision. The driver now
   derives its deal enumeration from the same 4-combo range the artifact was
   built with, filtered for board blockers.

4. **The best response must pool by information set.** A per-deal maximum is the
   omniscient bound, not a best response; the walk memoizes one action per
   (node, own two cards) so every deal reaching that information set uses the
   same choice. This mirrors `exact_best_response_utility`'s contract rather
   than re-deriving it.

**The uncontained count is measured, and it moves.** Rebuilding the same fixture
at `stack = 11` — one chip past the RFC's documented boundary — raises the count
from 0 to 2 on the same census. A count that could not move would be
indistinguishable from a hardcoded zero, so this is the check that the number
means anything. (The RFC's own figure of 1 at stack 11 comes from its
reviewer's enumeration; this census counts nodes in the built tree and includes
the deeper lines the taller stack opens, so the two are not expected to agree on
the count, only on the direction.)

**AC3 is measured against the source, not a recording.**
`test_stage6_measured_policy` compares `NSeatBehaviorPolicy` to a direct
`NSeatPolicy::lookup` on **1651 information sets** of the trainer fixture, and
requires the adapter to refuse on all **37199** nodes the source does not cover.
Mutation-checked: replacing the adapter's throw with a uniform fallback fails
the refusal assertion on every miss.

**The shared formula is pinned separately.** `test_nash_conv_units` restates the
three normalisations independently and covers the two degenerate inputs. Its
first draft asserted that a negative big blind throws; `Chips` is `std::uint64_t`,
so that assertion was vacuous and is removed rather than left as false comfort.

**A pre-existing failure is not this stage's.** `bigshark-subgame-nash-conv`
reports `1 fixture(s) failed` at its largest configuration (`nseat trainer
exceeded the byte cap`). Verified pre-existing by stashing every stage-2 change
and re-running: identical failure. Stage 2 changed only that harness's
normalisation call, which cannot influence a trainer byte cap.

## Stage 3 — Reference document

Status: implemented (2026-10-22).

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
node bin/check-rfcs.mjs
node bin/check-docs.mjs
```

ASan is required for stage 1 (the walks gain a reference parameter that must not
outlive its evaluator) and stage 2 (per-thread scratch).
