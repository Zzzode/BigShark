# RFC 0004-0006 Implementation Plan

Status: Current

Execution state: RFCs 0004, 0006, and 0008 Implementing (0008 from
stage 1, committed d60ce13); Stages 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, and
the isolated Stage 12 ICM are complete with recorded
evidence. Stage 10 is PARTIALLY complete: the heads-up preflop RULES and their
independent native tests are implemented, but preflop training and
continuation-range export are NOT. The blocker is measured, not assumed: a
full traversal from a preflop root is unbounded (probe-level: >3M nodes and
>1M postflop information sets at a three-big-blind stack), and the preflop
street's own information-set count grows with range size and stack depth (30
at 6bb, 606 at 25bb, 8,010 at 100bb, 29,946 at 200bb for six combos per seat).
RFC 0007 is now ACCEPTED and unblocks the first, narrow slice: the
flop-terminal abstraction, artifact coexistence, and a measured convergence
result for a DECLARED SMALL preflop profile. It does NOT satisfy Stage 10's
criterion as written, which implies a realistic-size profile. Bounding the
preflop street at realistic range and stack depth needs a card or action
abstraction with a measured error bound; RFC 0004 defers that to a separate
design, and that follow-up RFC is the critical path item for Stage 10 and is
not yet scheduled. Stage 10 must not be marked delivered on the back of RFC
0007 alone, and any stage-4 training numbers must be published as
declared-profile coverage, never as Stage 10 completion.
Stage 13 (multiway) is PARTIALLY complete: the 3..6-player RULES and the
joint-deal sampler are implemented and independently tested, but training and
deviation evaluation are NOT, and a design gap was found while starting them.
Stage 14 (final audit) remains. RFC 0005 is implemented through the Stage 9
resolving scope; its general live subgame work remains explicitly deferred.
Live enablement of resolving still requires explicit authorization.

The Stage 13 gap, recorded rather than papered over:

- **Stage 13's criterion names an artefact that was never designed.** The
  required evidence is "compatible joint deals, seat permutations, unilateral
  deviations, honest nonconvergence and quality reporting". The first two exist.
  Unilateral-deviation evaluation does not, and it is not a one-parameter
  generalization of anything that does: `ExactEvaluation` is
  `std::array<double, 2>` (`engine/include/bs/heads_up_solver.hpp`), and the
  resolver, river, and certifier best-response machinery are all heads-up. A
  multiway evaluator is new code, not a widened call.
- **What is genuinely blocked, stated accurately.** NashConv generalizes to N
  players and is computable - a best response against a fixed profile is a
  single-agent maximization, linear in tree size, and `NashConv = 0` still
  characterizes a Nash equilibrium by definition. There is no definitional or
  computational blocker, and this plan does not claim one. What does not
  generalize is NashConv's role as a CONVERGENCE TARGET: CFR drives two-player
  zero-sum exploitability to zero, while at larger seat counts it converges to a
  coarse correlated equilibrium in general-sum games, and multiplayer Nash is
  neither unique nor payoff-interchangeable (RFC 0006 says as much).
- **What needs design (RFC-worthy).** RFC 0006's large-game requirement -
  "predeclared adversarial policies, held-out seeds, seat permutations, and
  confidence intervals" - is a sampling design with no existing analogue. The
  heads-up side pairs a full enumeration with a certified bound; neither covers
  a sampled multiway estimate. That estimator is the design work that does not
  yet exist.
- **Stage 13's training half is downstream of the abstraction.** Repeated full
  multiway traversal is a documented dead end, so multiway training needs an
  abstracted tree, which is RFC 0008 stages 3-4. RFC 0008's rollout order is
  therefore correct and Stage 13 is not next in line: only its no-training parts
  - a fold-only exact check, and the deviation evaluator once the estimator above
  is designed - could land before the abstraction.

Cross-reference: RFC 0008 stage 6 needs a measured larger-table deviation
estimate, so the estimator above is on that RFC's critical path as well. The two
should be designed once, not twice.

Active goal: finish all accepted RFC 0004-0006 scope. A checkpoint is not
goal completion; continue remaining stages until their acceptance evidence
exists. External authorization gates remain explicit.

RFCs 0004-0006 received independent formal approval on 2026-09-14.
This plan records implementation progress separately from future stages.
No further user RFC approval is required. Completion still requires the
evidence specified for each stage.

## Governing Contracts

- [RFC 0004](../rfcs/0004-heads-up-blueprint.md): betting rules, full/sampled
  traversal, exact information states, blueprint coverage, and preflop scope.
- [RFC 0005](../rfcs/0005-strategy-artifacts-and-resolving.md): artifact
  boundary, resident lookup, restricted resolving, and minor-1 integration.
- [RFC 0006](../rfcs/0006-extended-poker-utility.md): payout ledger, declared
  rake, bounded ICM, and experimental multiway evaluation.
- [Existing migration plan](0001-0002-implementation.md): Protobuf Stages 5-9.
- [Complete roadmap](gto-delivery-roadmap.md): external dependencies and work
  intentionally outside these initial designs.

## Stages and Evidence

Stages 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, and the isolated Stage 12 ICM are
complete; Stage 10 is PARTIAL (rules only); Stage 13 is in progress (rules and
joint-deal sampling landed, training and deviation evaluation remain); Stage 14
remains Pending. Owners name existing modules or the explicitly approved
artifact boundary, not separate services.

| Stage | Roadmap IDs | Owner | Implementation | Required completion evidence |
| --- | --- | --- | --- | --- |
| 0008-1 | RFC 0008 §L1 | `engine` poker | One `GameDef` / `GameState` for 2..10 seats, constructing two seats only, with one legal-transition implementation. Additive: no existing behavior changes and no adapter forwards to it yet | Independent equivalence oracle that does not include the heads-up header; every semantic mutation of the new code either shown red or proven equivalent by a reachability measurement recorded in the mutation config; `test_heads_up` figures unchanged |
| 1 | B1 | `engine` poker | Checked chip state, real actor order, commitments, legal targets, raise rights, refunds, and showdown | Table-driven legal transitions, overflow and all-in tests; independent exact chip conservation |
| 2 | B2 | `engine` solver | Public heads-up facade, exact information keys, joint chance, full traversal and independent BR oracle | Tiny-game reference agreement; full-range normalized policies; no hidden-information conditioning |
| 3 | B2/B3 | `engine` solver/benchmarks | External sampling, specified PRNG, regret/average updates, iteration rollback, resource caps | Enumerated expected updates, repeatability, interruption/resume, fixed/sampled quality thresholds and existing benchmark parity |
| 4 | B3 | `engine` benchmarks | Frozen board/range/depth matrix and capacity reporting | Separate exact/estimated metrics, node/memory/time/coverage reports; no unsupported full-game claims |
| 5 | S1 | `engine` artifact boundary | Authoritative SQL schema, transactional checkpoints, immutable export, digest and bounded reader | Crash/full-disk/corruption/version tests; split-run equality; macOS/Linux native dependency builds |
| 6 | S2 | `engine` solver/policy | Resident complete root subsets, public policy reach, covered lookup and explicit misses | No request-path SQL, exact artifact/range identity, correct blockers and provenance; lookup latency measurements |
| 7 | P5/P6/P7 | Protocol/service/host/client/River | Existing RFC 0002 minor-0 production migration | Framing/fuzz and bigint conformance; golden/replay parity; dry-run and authorized canary; exercised v0 rollback |
| 8 | P7a | Protocol/policy/River | Negotiated minor-1 full distributions, source/guarantee fields, opt-in blueprint | Old/new client matrix including small distributions; exact sampled-action membership and source-accurate fallback |
| 9 | S3/S4 | Solver/service/River | Offline gadget/certification, whole-range candidate selection, explicit eligible terminal-decision routing | Counterexample and independent BR gates; private-independent selection; same-snapshot provenance differences; failure invalidation tests |
| 10 | F1 | Poker/solver/policy | Heads-up preflop roots and policy-derived continuation ranges | Correct heads-up blind option/order; independently declared preflop coverage and quality evidence; six-max charts unchanged |
| 11 | U1 | Poker | Gross/refund/net ledger, all pot layers, eligibility, ties and remainder rules | Exhaustive small contribution grids, independent settlement, exact conservation, no double refunds |
| 12 | U2/U3 | Poker | Declared capped rake and exact bounded ICM | Rake rounding/conservation; analytic and enumerated ICM, prize units, eliminations, limits and unsupported rules |
| 13 | U4 | Solver/benchmarks | Experimental 3..6-player game/training and full-reference averages | Compatible joint deals, seat permutations, unilateral deviations, honest nonconvergence and quality reporting |
| 14 | D1 | Each stage owner | Current design/reference/build/operational docs and release evidence | Every advertised capability matches verified behavior; all acceptance criteria audited |

Stages 5, 6, 8, and 9 are authorized by accepted RFC 0005, subject to their
preceding evidence gates. Stage 7 is separately approved under RFC 0002
and can proceed independently of solver strategy changes. Stage 11
depends on Stage 1; Stages 12 and 13 depend on settlement and relevant trainer
work. Do not parallelize changes to the same poker or service-domain contract.

## RFC 0008 Stage 1 Evidence (2026-09-19)

RFC 0008 was accepted 2026-09-19 (`a927284`) after three review rounds by a
separately launched approval agent (the RFC's own Decision section records all
three, including the two it returned Changes Requested on). Its first rollout
stage is the unified game definition for two seats, preserving `HeadsUpState`
behavior exactly. That stage's plan wording says "behind adapters"; the adapters
are NOT part of this stage, and no existing type forwards to the new one. This
section is that stage's evidence. Scope is stage 1 only.

**What landed.** `engine/include/bs/game_definition.hpp`,
`engine/src/poker/game_definition.cpp`, and
`engine/src/poker/game_definition_settlement.cpp`, registered under
`bigshark_poker`, plus `engine/tests/test_game_definition.cpp` registered as
`game_definition`. Additive: no existing file changed behavior, and no
production path constructs a `GameState` yet.

**The oracle does not include the heads-up header, and that is the point.** It
drives `GameState` against a reference ledger written from the rules of no-limit
hold'em and from `GameDef`'s structure. Comparing the unified state to
`HeadsUpState` would only prove two implementations agree, never that either is
right; a reviewer can check the claim by reading the include list. This was not
theoretical: the first run of the oracle found a 2^64-1 unsigned underflow the
author had missed, precisely because it checked a different implementation rather
than a copy.

**Measured coverage.** 144 preflop roots / 1,026,792 nodes, 150 flop roots /
367,348 nodes, and 137,560 showdown terminals. The preflop sweep varies BOTH
seats independently, which is what makes it able to reach the capped-blind state;
the earlier symmetric sweep could not, and an independent reviewer found a real
divergence hiding in that blind spot. The big-blind option is exercised 336
times, and 270,380 nodes hold the state that separates the two possible all-in
rules (chips behind AND all in). Both counts are asserted positive rather than
assumed, because a sweep that never reaches a branch proves nothing about it. The option count is asserted positive rather than
assumed, because a sweep that never reaches the option proves nothing about it.

**Mutation verification is machine-run**, not hand-run:
`npm run mutation` applies twenty semantic mutations across two targets,
rebuilds only the affected target, classifies the result, and restores the file.
Sixteen are caught and four are equivalent, so the battery currently reports no
coverage gap. Fourteen of the sixteen cover the new stage-1 code -- including two
that preserve, as regression mutants, the exact mistakes the review found and the
one it did not -- and the other two cover `test_heads_up` itself, because its counts are what the rest of this
section cites and a citation needs a check rather than a print. Three of the
twelve were gaps the suite had before the battery ran:

- a rooted flop with fewer than two actionable seats, reachable only from an
  empty stack, which the settlement sweep never produced because its stack range
  started at 4;
- a settlement that awards the pot by seat order rather than by hand score,
  invisible while both fixture hands tied on a board straight flush;
- `after_card` opening action with a single actionable seat.

Two mutants are EQUIVALENT at two seats, proven by measuring reachability over
every node and not by argument: dropping the folded-skip guard in
`refund_unmatched` (a folded seat cannot hold the strict maximum commitment at
two seats, measured zero times), and dropping the `raise_rights` check from
`legal()` (zero reachable states where it is the only guard blocking a raise,
measured `rr_false=5820` states where it is genuinely exercised). Two further
mutants are equivalent for structural reasons the config records: omitting the
`all_in` derivation after an ANTE (the two-seat profile rejects a nonzero ante,
pinned by `ante_is_rejected_in_this_profile`), and stating the rooted `pending`
set as `stack > 0` rather than `!folded && !all_in` (`GameDef` declares no folded
seat, so at a root every seat is unfolded; measured green). All four are recorded
in the mutation config with their rationale, and the first two are pinned as
standing assertions in the oracle so that a fixture change reaching one of those
states turns the claim red instead of letting the mutant start passing for a new
reason.

**Two defects the oracle caught in the new code, both fixed.** The constructor
never seated the declared stacks, so the blind subtraction underflowed to
2^64-1. And the two-seat blind seats were derived as the three-handed rule with
the count turned down, which puts the big blind back on the button; heads-up the
BUTTON posts the small blind. Both are recorded in comments at the sites.

**One defect the oracle caught in its own ledger.** The reference encoded the
flop-deal rule as "neither seat is action-and-pending", but the shipped heads-up
rule closes the street the moment EITHER seat is all in
(`engine/src/poker/heads_up.cpp:271`). The two differ when one seat is all in:
the ledger's version would open a round in which the sole remaining seat may bet
into an opponent who cannot respond. The implementation was right and the ledger
was wrong; the ledger now states the two-seat rule it is meant to check.

**Gates.** Release ctest 38/38, ASan/UBSan 36/36, format clean, both benchmark
families, `npm run check`, `npm run proto:check`, and 156 replay decisions with
0 illegal and 0 JS fallbacks. The counts rose from 36/34 because this change
registers two new test targets (`game_definition` and, in the preceding commit
`dd1d4a6`, `unification_invariants`); both are new suites rather than preserved
ones.

What actually must not move is `test_heads_up`, and it did not: it still reports
`nodes=12226 folds=2852 showdowns=3002 refunds=3480 short-raises=120`, its ASan
run prints the same figures, and the commit does not touch the file. Those
figures are now asserted as EXACT pins rather than printed and compared by eye
(the reviewer's point: as written they were lower bounds plus a printf, so
"unchanged" was a human judgement, not a check). The pin was verified able to
fail: the mutation battery perturbs the expected node count and requires
`test_heads_up` to go red, and separately mutates `after_card` to open action
with one seat all in and requires the same. Both do, so the pin is a check and
not decoration.

**Residual limitations, stated rather than implied.**

- Two seats only. `validate` rejects any other seat count, and a rejection test
  pins that boundary so stage 2 has to widen it deliberately.
- Nothing routes through `GameState`. `HeadsUpState` remains the shipping rules
  type; this stage is a prerequisite, not a replacement.
- The ante field exists and is validated, but the two-seat profile rejects a
  nonzero ante outright. Ante behavior is therefore implemented and unreachable,
  which is why the ante mutation is equivalent here and why the mutation that
  omits deriving `all_in` after BLINDS is the one that must be caught.
- `TerminalDepth::Flop` (RFC 0007) is declared so the identity surface does not
  grow a field later, but only `River` is accepted.
- macOS/arm64 only; no Linux build was verified.

### The defect the independent review found, and what it cost

The rules-correctness reviewer found a real divergence from the shipped heads-up
rules, and it is worth recording precisely because the evidence above did not
have it.

**The rule.** A blind capped at its poster's stack makes that seat all in for the
REST OF THE HAND. `HeadsUpState` gets this from a flag that is set at the root
and never cleared. My `GameState` re-derived `all_in` from the stack at every
chip movement including `refund_unmatched`, so when the unmatched part of a
capped blind came back at street close, the seat revived, the preflop round
continued, and `after_card` opened a betting round the shipped rules never
enter. `engine/tests/test_heads_up_preflop.cpp:229` pins the shipped behavior in
words: "The board runs out with no action at any street."

**Reproduced independently before being accepted.** A lockstep differential
probe drove both implementations over every root both admit, and reported 12
divergences over 40 preflop roots at four-chip stacks. Minimum reproduction:
`stacks = {2, 2}`, `blind = 2`, `button = 0`. After the fix, 502 roots across
both profiles report zero divergences.

**Why the oracle could not see it, which is the part worth keeping.** Two
reasons, and the second is the instructive one. The ledger's `refund_unmatched`
shared the defect, so it agreed with the implementation by construction. But the
deeper reason is fixture shape: the preflop sweep used ONE stack value for both
seats, so no blind was ever capped and the state never arose. A symmetric-stack
sweep cannot test a rule about what happens when one seat is short and the other
is not. The sweep now varies both seats independently (144 roots, 1,026,792
nodes) and asserts the reachability of the state that separates the two rules:
270,380 nodes where a seat holds chips AND is all in. The comparison also changed
from `all_in == (stack == 0)` to a comparison against the ledger, because that
identity looked like the stronger check while holding in every reachable state
except the one that mattered.

**A claim in this stage's own header was wrong, and is corrected there.** The
header said the two all-in representations were "observationally identical across
1,990,808 reachable nodes". They are not. That measurement covered nodes reachable
from roots whose blinds were never capped, which is exactly the gap above. The
maintained representation was right, for the reason its own comment had given and
which the measurement had not covered.

**The reviewer's second finding did not hold up, and the difference matters.**
It described a state where only one seat can act and claimed the shipped rules
never enter it. They do: a bet that leaves its bettor all in keeps the street
open because the opponent must still answer it, so `HeadsUpState` assigns the
action to the opponent with `all_in_` set on the bettor. Both types agree there,
and since heads-up is the reference, no change was warranted. What the finding
DID expose is that my close rule asked the wrong question -- whether two seats
can still act, rather than whether anyone still owes an action. The named form
now states the rule directly, and preserving my original wrong rule as a mutant
turns the oracle red, so the mistake is a regression test rather than a note.

**Scope.** No production path constructs a `GameState`, so none of this could
have changed a live decision. It is still a real defect: the stage's acceptance
criterion is reproducing the shipped rules exactly, and this violated it for a
root the shipped suite pins.

**A second reviewer finding, and one the sanitizers could not have made.** The
test-vacuity reviewer found an out-of-bounds read in the oracle's own
reachability probe: it tested the STATE's phase and then indexed the LEDGER with
the ledger's actor, so a phase disagreement read `committed[-1]` and `stack[2]`
on two-element `std::array`s. No sanitizer covers that -- `std::array` is a plain
aggregate without container annotations, and libc++'s hardening modes pass it too
(verified by building against the buggy source with three different hardening
flags). It was found by reading. The probe now checks the ledger's own state
before indexing it and counts the combination it cannot index, asserted zero,
rather than skipping it quietly.

## Completed Checkpoint

Stage 1 evidence (2026-09-14):

- Public rules: `engine/include/bs/heads_up.hpp`, implemented in
  `engine/src/poker/heads_up.cpp`, registered under `bigshark_poker`.
- Native regression: `engine/tests/test_heads_up.cpp`, registered as
  `heads_up`. Its independent oracle covers 150 root configurations and
  12,226 nodes, including 2,852 fold and 3,002 showdown terminals.
- Independent static review by agent
  `6b80ad0f-375a-495c-98f5-94b45b556177` found no blocking findings.
  Test author `a71d2c08-badb-4caf-8d59-60269239771e` owned only the test;
  the main agent reviewed integration and executed verification.
- Release 17/17, ASan/UBSan 16/16, Node 32/32, Protobuf 7/7, existing
  benchmark families 3/3, and 156 replay decisions with no illegal actions
  or JS fallback. Format, documentation, and RFC checks passed.
- This is a working-tree checkpoint, not a commit or release. No live play
  occurred; the new Linux build is not yet verified.

Odd-chip split remainder is unreachable in the equal-contribution matched
heads-up profile. Broader pot and odd-chip coverage remains Stage 11.
The public state deliberately cannot validate future cards against hidden
hands; the Stage 2 chance dealer must enforce that invariant.

Stage 2 evidence (2026-09-14):

- Public solver facade: `engine/include/bs/heads_up_solver.hpp`, implemented
  in `engine/src/gto/heads_up_solver.cpp`, registered under `bigshark_solver`.
  Exact information keys, joint weighted private deals, the multi-size action
  abstraction, immutable policies with explicit coverage misses, and an exact
  modeled best response.
- Native regression: `engine/tests/test_heads_up_solver.cpp` (pure-response
  oracle enumerating complete pure policies, key/coverage/rollback checks) and
  `engine/tests/test_heads_up_allocations.cpp` (every-allocation fault
  injection and joint-deal/evaluation peak budgets).
- 8,192 full iterations on the fixed weighted two-combo game reach
  normalized NashConv `0.000821200905142` over 1,622,020 nodes and
  24 information sets.
- Independent static reviews found no P1 algorithm defects; one P2
  (best response over a non-fixed chance card uncounter-checked) and one
  P3 (average row literals unpinned) were carried into Stage 3 evidence.

Stage 3 evidence (2026-09-14):

- External-sampling MCCFR per the pinned OpenSpiel revision-48401890
  two-player `AverageType::kSimple`, plus pinned SplitMix64 PRNG revision 1,
  in `heads_up_solver.cpp`; the sampled entry is `train_sampled(iterations,
  seed, limits)`. The full-traversal average retains the pinned
  once-per-information-set own-reach convention; sampled and full averages
  are equal only after behavior normalization, as RFC 0004 lines 191-195
  require. Regret identity stays a raw equality.
- Test-only internals live behind a passkey in the private
  `engine/src/gto/heads_up_solver_debug.hpp`; the public facade no longer
  befriends a name any TU could define.
- Native regression: `engine/tests/test_heads_up_solver_sampled.cpp` covers
  pinned PRNG vectors, hand-derived full-sweep literals, enumerated sampled
  expected updates under non-uniform weights and a non-fixed public card,
  a fixed-policy enumeration with exact zero-probability actions at an
  opponent node (branch never drawn) and at a traverser node (still
  enumerated, value weight zero), repeatability/divergence, iteration and
  PRNG rollback under every cap, and an independent backward-induction
  chance-conditioned best response. A two-chip multi-size fixture exercises
  real three-action nodes (check/min-bet/jam and fold/call/jam; 47,076
  enumerated draw vectors); deeper complete trees were measured as
  intractable at this game size. `test_heads_up_allocations.cpp` adds
  sampled-path allocation faults including the debug-only raw-row export.
- Quality runners: schema-version-2 `benchmark-heads-up-blueprint`
  (Release-only CTest, 900-second timeout) and the manual, non-CTest
  `benchmark-heads-up-capacity` spending both sampled cases at the pinned
  1,000,000-iteration checkpoint. Reference machine: Apple M5 Pro, 48 GB,
  macOS 26.5.1, Apple clang 21. Capacity results, repeat delta zero on
  every published row:

  | Case | Seed 1 | Seed 17 | Seed 43 | Gate |
  | --- | ---: | ---: | ---: | ---: |
  | fixed sampled, 1,000,000 | 0.000932961259664 | 0.000779231105677 | 0.000664549996915 | 0.002 |
  | free-river sampled, 1,000,000 | 0.00271438222828 | 0.00298773380095 | 0.00264161588115 | 0.02 |

  Measured capacity run times (`elapsed_ms`, per-case final 1,000,000
  checkpoint including the exact evaluation, recorded on the reference
  machine Apple M5 Pro / 48 GB / macOS 26.5.1): fixed sampled 10,015.9 ms
  (seed 1), 10,108.9 ms (seed 17), 10,053.3 ms (seed 43), about 10.0-10.1 s
  per seed; free-river sampled 100,858.8 ms (seed 1), 95,709.8 ms (seed 17),
  97,118.0 ms (seed 43), about 95.7-100.9 s per seed. These are observations,
  not gates.

- Gate results: Release CTest 23/23 (196.6 seconds), ASan/UBSan CTest 21/21
  (the sampled test alone takes 1,289 seconds under instrumentation and
  uses a wall-clock-free completion budget; no reports), Node 32/32,
  Protobuf 7/7, 156 replay decisions with zero illegal actions or JS
  fallback, documentation and RFC checks pass. The legacy schema-1
  benchmark values are unchanged.
- Three adversarial review rounds (algorithm and engineering, with a
  skeptic re-check per finding) drove the fixes above; the final re-review
  is recorded in the session workflow evidence.
- These remain offline trainers and fixtures only. Strategy storage
  (Stage 5), resident lookup (Stage 6), production wiring, and preflop
  coverage (Stage 10) are still pending. Linux portable verification of
  the new targets has not yet been run.

Stage 4 evidence (2026-09-15):

- New versioned runner `engine/benchmarks/heads_up_matrix_benchmark.cpp`
  (schema_version 3 CSV) and targets `bigshark-heads-up-matrix-benchmark` /
  `benchmark-heads-up-matrix` in `engine/CMakeLists.txt`. It is deliberately
  not a CTest and reuses the unchanged `heads_up_benchmark_support.hpp`.
  The schema-1 and schema-2 benchmark files and the RFC were not modified.
- Twelve frozen fixtures (fourteen published final rows): a genuinely
  rainbow disconnected dry family Ks7h2c (the earlier Ks7s2c was two-tone,
  not unpaired rainbow; card collisions against the ranges and fixed runout
  are rejected by `HeadsUpTrainer` by construction), two-tone connected
  9h8h4d, paired QdQc6s, and monotone JhTh9h families; equal and asymmetric
  stacks; SPR 1, 4, and 10. One shared matched root (limped 10-chip pot,
  5 big blind, button seat 1), three weighted combos per side with one
  cross-blocked pair (eight joint deals), the standard multi-size schedule,
  fixed 3s/5h runout, and one free-river SPR-1 monotone fixture. Full
  catalog and CSV fields are in `docs/development/benchmarking.md`.
- Schema-3 CSV columns include `algorithm_revision`
  (`rfc0004-rev1-full-kSimple-prng1`), a stable FNV-1a `range_digest`
  (`9e1b8b78a66f91db`) of the ordered per-side range card ids and weights,
  and `missing_information_sets` (zero on every complete row). `metric_class`
  is derived: EXACT only for a finite full-traversal evaluation, INCOMPLETE
  on a failed evaluation (NaN exploitability), never EXACT on a FAIL.
  `nodes` counts TRAINING traversal nodes only; `peak_rss_bytes` is bytes on
  every platform (Linux KiB normalized x1024), -1 when `getrusage` is absent.
- Every fixture trains with FULL traversal. The runner's
  `--sampled-coverage` mode reproduces the reason at the pinned budgets:
  external sampling visits 251 of 252 fixed SPR-1 information sets after
  3,000,000 iterations (1 missing; 216,289.6 ms sampled time) and 4,028 of
  6,192 free-river sets after 100,000 iterations (2,164 missing;
  140,793.1 ms), so sampled policies are incomplete and the matrix instead
  publishes full-traversal policies certified by the exact `evaluate()`
  best response over the full betting/chance tree. Full rows are
  seed-independent; the dry SPR-1 fixture is published under seeds 1, 17,
  and 43 (identical), the larger matrix under seed 1; no held-out seeds.
- Gates: SPR-1 fixed rows use the pinned 0.002 gate and the free-river row
  the pinned 0.02 gate (final values 0.000606859664931 dry rainbow,
  0.001158562503 connected, 0.00144516124979 paired, 0.0112887585953 free
  river). SPR 4/10 rows do not claim 0.002; they require finite metrics,
  complete coverage, and strict first-checkpoint improvement behind
  fixture-specific gates frozen from the measured finals and rounded up
  with margin (SPR 4: 0.16/0.40/0.36/0.15 vs measured
  0.150113162264/0.365752065422/0.332260032957/0.137179422871; SPR 10:
  4.6/5.7/5.8/5.5 vs measured 4.37322736827/5.39491778572/5.5223726767/
  5.209779851; the rainbow dry-board change leaves these game values
  unchanged because no five-card flush/straight is reachable with these
  ranges and runout). Same-build repeat delta is zero on every row.
- The SPR 4/10 gates are REFERENCE-MACHINE gates (Apple M5 Pro, 48 GB,
  macOS 26.5.1, Apple clang 21), not portable equilibrium thresholds;
  benchmarking.md documents the `--freeze` rebase procedure (FREEZE status
  / `freeze-reference` basis, inspect deltas, round up 5-9%) so ordinary
  floating-point/library drift is distinguishable from a real regression.
  Linux verification remains an external gate.
- Capacity (reference machine Apple M5 Pro / 48 GB / macOS 26.5.1, Apple
  clang 21): complete-tree information sets 252 (SPR 1 fixed), 6,192
  (SPR 1 free river), 19,176 (SPR 4), 493,500 (SPR 10). SPR-10 conservative
  accounting peak 5,461,986,440 bytes (5.087 GiB / 5.462 GB) and largest
  forked-child peak RSS about 2,999,000,000 bytes (about 2.79 GiB / 3.00 GB),
  under an explicit 8 GiB accounting budget above the RFC 1 GiB default.
  Printed elapsed_ms totals about 303,982 ms (5.07 min) over the fourteen
  final rows and about 380,457 ms (6.34 min) over all printed rows; the whole
  run, including every independent same-build repeat and per-fixture fork,
  took 684.81 seconds wall. The per-row node/memory/time table is in
  `docs/development/benchmarking.md`. These are bounded frozen-matrix games,
  not full-game equilibrium coverage; the SPR 4/10 finals are explicitly
  unconverged large-game measurements.
- Documentation: new "Frozen Matrix Benchmark (schema 3)" section in
  `docs/development/benchmarking.md` (catalog, sampled-coverage evidence,
  rebase procedure, full field and measured tables), a short subsection in
  `docs/design/gto-engine.md`, and the manual target in
  `docs/development/build-and-test.md`. No new ctest was registered.

## Stage 5 Evidence (2026-09-15)

- New offline static library `bigshark_artifacts` under RFC 0005's approved
  `engine`-owned boundary. Public surface: `engine/include/bs/strategy_artifact.hpp`
  (domain records only; no SQLite or OpenSSL types); private implementation in
  `engine/src/artifacts/artifact_codec.cpp`, `strategy_artifact.cpp`,
  `artifact_digest.cpp`, and the private `artifact_internal.hpp`. The library
  links `bigshark_solver` PUBLIC for the domain types and the storage/crypto
  dependencies PRIVATELY; no host, service, client, platform, v0 protocol, or
  live path links it in this stage. The only solver-domain change is a
  sanctioned `HeadsUpPolicy` friend grant for the artifact record assembler and
  a test-only resume hook behind the existing debug passkey; the Stage 1-4
  CFR math and benchmarks are unchanged.
- Vendored SQLite: official public-domain amalgamation SQLite 3.50.0
  (`sqlite-amalgamation-3500000.zip`, published 2025-05-29), unmodified files
  `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h` under `engine/third_party/sqlite/`
  with the public-domain `LICENSE`; built privately as `bigshark_sqlite` with
  `-w`, `SQLITE_THREADSAFE=1`, and `SQLITE_OMIT_LOAD_EXTENSION=1`. The
  system/Homebrew SQLite is never linked; SQLite 3.37+ is asserted at both
  compile time and open time. SHA-256 uses OpenSSL 3 Crypto only: on this
  Apple arm64 machine CMake resolves `OpenSSL::Crypto` 3.6.3
  (`brew openssl@3 3.6.3`, headers `/opt/homebrew/opt/openssl@3/include`,
  `lib/libcrypto.3.dylib`) and links it PRIVATELY; a Homebrew keg-prefix and
  `OPENSSL_ROOT_DIR` fallback follows, and configure fails clearly when
  OpenSSL 3 is absent. Notices: `THIRD_PARTY_NOTICES.md`,
  `engine/third_party/sqlite/LICENSE`, and `engine/third_party/openssl/LICENSE`
  (verbatim Apache 2.0).
- Authoritative schema v1: `application_id = 0x42534754`, `user_version = 1`,
  STRICT tables, foreign keys on, prepared statements throughout. Tables
  `manifest`, `game`, `sizes`, `ranges`, `information_states`, `actions`,
  `training` (checkpoint only), `bounds`, and `measurements` (the last two
  exist, are validated, and are empty in this stage). INTEGERs are range
  checked to `2^53 - 1`; REALs reject NaN/Infinity; per-row action
  probabilities must sum to one within `1e-12`. The public information key is
  the RFC 0005 canonical ASCII `street|board_ids|events` (justified in
  `docs/design/gto-engine.md`), stored with separate player/own-combo columns;
  combo ids are a canonical zero-based index of the 1326 unordered hands.
  Checkpoint writes are one transaction per complete iteration with rollback
  journaling and `synchronous=FULL`; resume compares the full canonical
  identity and refuses published policies. Publication fills a unique temp file
  on the destination filesystem, fsyncs file and parent directory, links it
  exclusively into a new never-overwritten `0444` generation, verifies
  SHA-256/size, and reopens it read-only (`query_only`, defensive, untrusted
  schema, 64 MiB cache, extension loading compiled out). The reader validates
  physical header/database-size agreement before open, rejects files over the
  8 GiB bound, unknown schema objects, bad keys, and all non-finite or
  out-of-range values; rows are eagerly loaded, with bounded resident subsets
  explicitly deferred to Stage 6.
- Native evidence: `engine/tests/test_artifacts.cpp`, registered as
  `artifacts`, independently authored. It covers (1) full-traversal
  checkpoint round trip with exact sizes/cards/combo/target identity, bit-exact
  raw regret/average replay, tolerance-checked probabilities, an independent
  schema/STRICT/count oracle, and a forked second-process reader; (2)
  hand-authored canonical-key bytes and malformed-key rejection; (3) split-run
  equality: 60 then checkpoint/resume versus 120 uninterrupted sampled
  iterations, reloaded policies and raw rows matching with maximum policy
  probability delta exactly `0` (reported by the test) and byte-identical
  independently published generations; (4) canonical identity-mismatch resume
  rejection for stacks, button, pot, board, runout, range weights, size
  schedule, algorithm/PRNG id, and seed; (5) faults: deterministic
  `SQLITE_FULL` injected through a test-only SQLite VFS shim at create and
  commit (old-or-new, never partial, after hot-journal recovery), real
  `fork()`/`_exit()` killed-writer rounds pre- and post-commit, and
  journal-removal torn-file rejection; (6) page, magic, and page-size
  corruption, truncation, unknown `application_id`/`user_version`, foreign-key
  and STRICT rejection, byte-planted NaN/+Inf/-Inf REALs, bad probability
  sums, and integers beyond `2^53 - 1`; (7) immutability: no overwrite, `0444`
  mode, write refusal, no leftover temp names, digest mismatch on byte edit,
  and refusal to publish an empty policy.
- Gates: Release CTest 24/24 (203.57 seconds wall on this run, including the
  existing benchmark families); ASan/UBSan CTest recorded below; format,
  documentation, and RFC checks pass; `npm run check`, `npm run proto:check`,
  and `node bin/replay.mjs` are unchanged and pass. The new library is
  offline only; `apps/`, `clients/`, `platforms/`, the v0 protocol, and the
  Stage 1-4 benchmarks and solver math were not wired or altered (apart from
  the friend/passkey additions described above).
- Dependency portability: SQLite is vendored and OpenSSL is the only new
  system dependency. Native dependency builds were verified on macOS
  (Apple arm64, OpenSSL 3.6.3) only; Linux distro-OpenSSL verification via the
  existing `tools/ci` lane remains an explicit external gate.
- Working-tree checkpoint; nothing is committed.

ASan/UBSan result (initial Stage 5): 22/22 tests passed (exit 0, no ASan or
UBSan reports), including `artifacts` in 4.29 seconds under instrumentation;
the existing instrumented sampled test took 1,341.17 seconds as on prior
stages. The artifacts transaction, hard-link/fsync, VFS fault shim, and
corruption paths are all exercised under AddressSanitizer and
UndefinedBehaviorSanitizer.

### Stage 5 independent adversarial review hardening (2026-09-16)

An independent skeptic double-confirmed five defects (2 P1, 3 P2); all five,
plus the same-family audit, are fixed with adversarial regression tests that
fail (red) when each production fix is reverted:

- P1-1 fixed-array indexing from untrusted columns (`read_sizes`
  street/kind, and the same-family audit over `read_ranges` player/combo,
  `read_states` player/own-cards/ids, `read_actions`/`read_training`
  info_id/ordinal/kind, `read_game` button/flop/runout): every file-derived
  value now has an explicit C++ domain/range check before any fixed-size
  indexing. Root cause hardened in `verify_schema_objects`: each table's
  stored CREATE text must now be byte-identical to the canonical `kSchemaDdl`
  constant, the table set must be exactly the canonical set (no more, no
  fewer), indexes must be `sqlite_autoindex_*` with NULL CREATE text, and any
  view/trigger/other `sqlite_schema` row is rejected, so CHECK/FK/STRICT
  clauses can no longer be stripped. Cross-table orphan checks
  (actions->states, training->actions, every state has an action) were added.
  Mutation verification: removing the street check causes an immediate ASan
  `stack-buffer-overflow` abort in `read_sizes` on a writable_schema,
  street=16 fixture (release also rejects it).
- P1-3 untrusted reader now uses `file:<percent-encoded absolute
  path>?immutable=1` with `SQLITE_OPEN_READONLY|SQLITE_OPEN_URI` (no recovery,
  no sidecar access); before opening it rejects any `<path>-wal/-shm/-journal`
  sibling and the physical header check rejects WAL file-format bytes 18/19
  != 1. A committed sidecar generated by a forked child genuinely returns
  `completed_iterations=777` to an ordinary connection but is rejected by the
  artifact reader; the immutable reader never creates an `-shm` or touches the
  `-wal`, including with a pinned digest. The `commit_checkpoint` identity
  probe uses the same immutable/sidecar-rejecting read; writer crash/ENOSPC
  recovery is unchanged.
- P2-2 `decode_public_key` now requires all public board IDs to be pairwise
  distinct in addition to not overlapping the own combo; `0|3,3,3|` and a
  turn repeating the flop are rejected at load and publish.
- P2-4 a process-once `sqlite3_hard_heap_limit64(kReaderHeapBoundBytes)` with
  `kReaderHeapBoundBytes = 512 MiB` is installed before opening an untrusted
  reader (the 3.50 amalgamation exposes only the process-global API and this
  private SQLite has no other in-process consumer); `SQLITE_NOMEM` maps to a
  typed `CapacityExceeded` error. The regression exercises the view-zip-bomb
  rejection path (500 CREATE VIEW) and asserts the heap ceiling is installed;
  honestly, actually exhausting 512 MiB needs a >512 MiB fixture and is not
  fabricated, so only the bounded-design and rejection path is tested.
- P2-5 `open_writer` now fails closed: `apply_writer_pragmas` plus
  `verify_writer_pragmas` read `synchronous` and `journal_mode` back on the
  same writer connection and abort with a Sqlite error unless they are `2`
  (FULL) and `delete`; a whitebox test sets `synchronous=OFF` and asserts the
  verifier throws. The old cross-connection synchronous assertion (which read
  the compile default and could not detect an OFF change) is removed; the
  file-header `journal_mode` cross-connection check is retained.
- Cleanups: removed unused includes/using-decls and added required includes
  per the reviewer's clangd findings; no solver math or the `1e-12` tolerance
  was changed.

New evidence: the `adversarial` case in `engine/tests/test_artifacts.cpp`
(now 8 cases) constructs all malicious files with a separate ordinary
sqlite3 connection or the platform `unix` VFS, so the library's own code is
not used to vouch for the tampered input.

Post-hardening gates: Release CTest 24/24 (239.49 seconds wall; `artifacts`
2.13 seconds); ASan/UBSan recorded below; format-check, `npm run check`
32/32, `npm run proto:check`, `node bin/replay.mjs` (Decisions 156, Illegal 0,
JS fallbacks 0), `check-docs`, and `check-rfcs` all pass. Nothing committed;
HEAD remains 3ff6d86.

Post-hardening ASan/UBSan result: 22/22 tests passed (exit 0, no ASan or
UBSan reports); the hardened `artifacts` suite (now including the
`adversarial` case) passed in 5.93 seconds under instrumentation and the
instrumented sampled test took 1,363.18 seconds. All fixed-array domain
checks, byte-identical schema validation, immutable/sidecar rejection, board
distinctness, the heap bound, and the synchronous=FULL writer gate are
exercised under AddressSanitizer and UndefinedBehaviorSanitizer (the
reverted street-check mutation aborts with a confirmed
`stack-buffer-overflow` at `read_sizes`).

### Stage 5 second-round review: int64 narrowing audit (2026-09-16)

The second independent review found one residual P2 in the same P1-1 family:
`read_states` narrowed player/card0/card1 and `read_actions` narrowed kind to
`int` before the small-domain check, so `2^32`-congruent values (player
`4294967296` -> 0, card0 `4294967296` -> 0, card1 `4294967297` -> 1, kind
`4294967297` -> 1) passed after the information_states/actions CHECKs were
stripped via writable_schema and the byte-canonical DDL restored, binding a
probability row to a combo that never held it.

A complete same-family audit of every `column_i64` narrowing site was done.
All reads now validate the raw `std::int64_t` value before narrowing. Fixed
in this round: `read_states` player/own-cards, `read_actions` kind,
`read_game` button and fixed turn/river cards (the runout previously narrowed
before its 0..51 check, so `2^32` would silently become card 0), and the
manifest `information_key_revision` plus the reader's `application_id` /
`user_version` comparisons (now exact raw-int64 comparisons instead of
`uint32` truncation). Already correct and confirmed: `read_sizes`
street/kind/ordinal, `read_ranges` player/combo, `read_training` info_id/
ordinal, `read_actions` info_id/ordinal (raw checked first), all chip/
counter values through `read_bounded_i64` (raw range 0..2^53-1 before
cast), and the auxiliary `bounds`/`measurements` readers (values stay int64,
range checked, never narrowed for indexing).

New adversarial regressions (in the `adversarial` case): strip
information_states/actions CHECKs, plant `2^32`-congruent player/cards/kind
and a `2^32+52` negative control, restore canonical DDL, assert rejection.
Mutation verified: reverting read_states/read_actions to narrow-first makes
the congruence assertions fail red; the in-place fix restores green.

Gates after this change: format + format-check pass; full Release CTest
24/24 (314.63 seconds wall on the chained run, `artifacts` 14.86 seconds);
targeted ASan `ctest -R artifacts` passes in 8.96 seconds with no ASan/UBSan
report (no memory layout changed, only validation ordering, so the full
22-minute ASan suite was not rerun as scoped); `check-docs` and
`check-rfcs` pass.

A third, fresh independent skeptic (not the implementer or either prior
reviewer) rebuilt the current tree and executed the bypass end to end against
the real static library: `2^32` player/cards and `2^32+1` action kind are all
rejected with `InvalidSchema`/`InvalidValue`, the `2^32+52` negative control is
rejected, and an unmodified checkpoint still loads (71 states); reverting to
narrow-first made the planted files load and turned the adversarial regression
red at `test_artifacts.cpp:1514`. A full narrowing-site audit found every
`column_i64`/`pragma_i64` value raw-range-checked before narrowing. The only
residual was a non-exploitable P3 statement ordering in `read_training` (the
narrowed ordinal was dead until after the raw range check); that ordering was
moved below the check for consistency. Final Release CTest after that reorder:
24/24 (271.95 seconds wall; benchmark label 203.82 seconds), `artifacts` green,
format-check clean.

## Stage 6 Evidence (2026-09-16)

Preceding gate: commit `189ca31` (RFC 0005 Stage 5 SQLite strategy
artifacts), Release CTest 24/24, ASan/UBSan 22/22, clean tree.

Owned files (additive only):

- NEW `engine/include/bs/resident_policy.hpp`: public offline API and domain
  types only; no SQLite or OpenSSL types.
- NEW `engine/src/resident/resident_policy.cpp`: explicit supported-root
  construction, root advertisement and identity gate, the 256 MiB budget,
  and coverage-miss orchestration.
- NEW `engine/src/resident/public_reach.{hpp,cpp}`: hero-card-independent
  joint belief and public-reach propagation with exact joint-mass
  renormalization.
- NEW `engine/src/resident/resident_index.{hpp,cpp}`: compact immutable
  flat index (key/action/probability blobs, fixed row records, open
  addressing at a 50 percent load factor), contiguous probabilities, and
  honest byte accounting.
- NEW `engine/tests/test_resident_policy.cpp`, registered as CTest
  `resident`; links `bigshark_resident`, includes `engine/src`, and uses the
  GTO debug passkey plus the artifacts `PolicyAssembler` only to synthesize
  complete fixtures, as `test_artifacts` does.
- NEW `engine/benchmarks/resident_lookup_benchmark.cpp` and the
  `benchmark-resident-lookup` custom target; not a timing-gated CTest.
- MODIFIED `engine/CMakeLists.txt` only to add the static library, test,
  and benchmark. `docs/development/benchmarking.md`, `docs/design/gto-engine.md`,
  and this plan carry the evidence. No service, policy, decision, river_gto,
  v0, app, client, platform, host, or protocol file was modified.

The new static library `bigshark_resident` links PUBLIC
`bigshark_artifacts` (which brings solver and poker domain records). No
policy, service, host, v0, protocol, client, or platform target links it;
the layer is offline and unwired in this stage. Explicitly deferred: the
resolving gadget, certification and bounds production (Stage 9; the v1
`bounds`/`measurements` tables are validated and empty, so resident
continuations are advertised for BLUEPRINT lookup only and the public header
exposes no bound/guarantee/certification symbol), protocol minor 0/1
(Stages 7/8), eligibility, host wiring, and preflop (Stage 10).

Contract implemented:

- Startup takes an explicit list of `{path, pinned sha256}` supported roots.
  Each artifact is digest-verified and validated through `load_artifact`,
  flattened into immutable resident records, measured, and advertised only
  when its footprint fits the shared 256 MiB budget. Per-root results report
  `Advertised`, `LoadFailed`, `OverBudget`, or `DuplicateRoot`; one bad
  artifact never disables another. `kDefaultResidentBudgetBytes` is
  256 MiB.
- Root identity is the full canonical notion: ordered flop, stacks, matched
  contributions, pot, big blind, button (the query cannot alter range
  weights or the ordered rational sizing schedule because those come from
  the artifact); a pinned digest on a different root is
  `RootIdentityMismatch`; fixed turn/river divergence misses like
  `HeadsUpPolicy::lookup`.
- Honest byte accounting counts the resident key/action/probability blobs,
  row records, slot table, and immutable game copy at actual vector
  capacities; it never uses the SQLite page cache, file size, or manifest
  estimate as truth. The 493,500-set SPR-10 root measures 301.21 MiB against
  a 175.97 MiB file and is correctly withheld.
- Warm lookups make zero SQLite calls, take no lock, and allocate no heap
  memory (all per-query buffers are caller-owned `ResidentScratch`). The
  benchmark's counting allocator records 0 allocations over 100,000 hits and
  20,000 misses.
- A pre-load additive `artifacts::probe_artifact` performs the complete
  physical/schema validation and SQL aggregates (state, action, key-word
  counts) without materializing rows; `ResidentIndex::estimate_bytes` is a
  conservative tight upper bound (verified in tests to be at least and very
  near the exact footprint) used to refuse oversized roots before the
  map-plus-index transient. Accepted roots are still measured exactly after
  the index is built.
- Declared ranges with no positive card-compatible joint deal (zero weight
  or fully cross-blocked) are refused at startup as `InvalidRange`; a query
  whose observed path leaves zero joint mass fails closed with
  `EmptyJointRange` rather than returning NaN.
- Free-chance orphan combos (positive raw reach but no compatible opponent
  deal on the dealt branch) require no row and do not turn covered nodes
  into MissingHistory; a free-slot card reserved for a later fixed slot is
  rejected as `OffTree`, matching the solver's `public_cards` support; a hero
  combination with exactly zero conditioned reach never returns its row
  (`ZeroProbabilityHeroCombination`).
- Public belief is computed once per public node with no hero hole-card
  input; observed actions multiply actor-combo reach by the policy
  probability matched by kind plus exact target total; public cards zero
  combinations of both players and renormalize the exact card-compatible
  joint distribution. Missing row, action-set mismatch, and zero-probability
  observations are distinct misses. The hero-private blocker filter is
  separate and never renormalizes into a relabeled range. The miss enum
  covers RootNotSupported, RootIdentityMismatch, OverBudgetNotAdvertised,
  MissingHistory, OffTree, ZeroProbabilityObservedAction, OffTreeAmount,
  EmptyJointRange, UntrainedCombo, ComboBlockedByBoard, RunoutDivergence,
  OpponentRangeFullyBlocked, and ZeroProbabilityHeroCombination.

Correctness evidence:

- Hand-computed golden tests on a two-vs-two fixed-runout game with exact
  decimals: root weighted marginals (0.4/0.6 and 5/12, 7/12), check/check
  invariance, bet/call and bet/fold updates (4/11, 7/11), a mixed lead
  (5/19, 14/19), a partial zero-prob action removing one combo, and a
  zero-for-every-combo call reported as ZeroProbabilityObservedAction, all
  within 1e-12.
- Public-card blocker removal on a free-turn fixture (the As turn removes
  player 0's AsKs combination, joint deals renormalize, and the same hero
  combo is rejected as ComboBlockedByBoard).
- Solver convention cross-check: the independent oracle now zeroes joint
  deals that hold a newly dealt free card before applying 1/legal-cards,
  multiplies BOTH players' observed action probabilities (non-constant
  responder own factors), and feeds per-deal reaches to
  `HeadsUpSolverDebug::response_value_with_reach` next to reaches rebuilt
  from the resident raw factors; normalized response values agree within
  1e-10 at flop, turn, and river nodes (including a free-turn branch) for
  best and profile evaluation.
- Street conditioning: exact hand-derived fractions after observed turn and
  river bet/call/fold actions (turn bet 25/49, 24/49; turn call 4/11, 7/11;
  river jam 25/37, 12/37; fold/call reply collapsing one opponent combo),
  covering the event-street board-prefix slice; non-constant player-0 factors
  are asserted by these fractions.
- Orphan free-chance regression (3v3 free-turn artifact): an actor combo
  whose remaining opponents all share the newly dealt card has no row but the
  node stays covered; the orphan marginal is exactly zero, the other combos
  return rows, and replaying an orphan action into the response node does not
  produce MissingHistory. A free turn dealing the reserved river card is
  OffTree.
- Second independent review found the orphan partner gate was exact only for
  actor-0 observations: `renormalize` folded the joint mass into `raw[0]` but
  left the cached `card_mass[0]` at pre-fold scale, so when observing an
  actor-1 action `has_positive_partner(1,...)` mixed post-fold totals with
  pre-fold card masses and could read a true zero-partner player-1 combo as
  positive, forcing a missing row on a covered node. Fix: scale
  `card_mass[0]` by the same joint factor in `renormalize`, keeping the
  scratch representation internally consistent for both actors
  (`engine/src/resident/public_reach.cpp`). The symmetric regression
  `test_orphan_actor_one` (p0 KhKc/QhJd/As9d vs p1 KhQh/9sTs, free As turn,
  nonuniform player-0 turn checks) queries the boundary after player-1's turn
  check, asserts the KhQh orphan marginal is exactly zero, 9sTs is one, both
  live player-0 combos stay positive, and the folded
  `sum(card_mass[0]) == 2*sum(raw[0])` invariant; reverting the scaling makes
  the covered node fail (`node.hit`), and the existing actor-0 orphan test
  stays green.
- Empty-joint regression: a zero-weight and a fully cross-blocked single-combo
  artifact both validate on disk but start as `InvalidRange` and query as
  `EmptyJointRange` (never hit plus NaN).
- Shared two-card combination present in BOTH declared ranges exercises the
  inclusion-exclusion add-back; the self-pair contributes zero and the
  marginals match a brute-force oracle.
- Zero conditioned hero reach returns ZeroProbabilityHeroCombination even
  though the public belief update stays valid.
- Probe regression: `probe_artifact` aggregates match the full load exactly;
  the byte bound is an upper bound on and tight relative to the exact
  measured footprint; digest mismatch and checkpoint-kind probes are
  rejected.
- Public belief is asserted identical for every hero combination; the
  cross-blocked matrix root matches an independent nine-pair oracle; the
  compact index agrees with a brute-force `std::map` scan over 5,000 random
  keys including absent ones; the continuity walk requires rows only for
  combos with positive compatible joint mass (matching the training tree) for
  three published roots; truncated history yields MissingHistory and wrong
  flop order yields RootNotSupported/RootIdentityMismatch; empty
  `bounds`/`measurements` tables are asserted via a raw SQLite count and the
  public header contains no bound/guarantee/certification symbol outside
  comments. Every production fix above was verified red-on-revert by
  mutating the production source and rebuilding the suite.

Latency evidence (reference machine Apple M5 Pro / 48 GB / macOS 26.5.1 /
Apple clang 21, release preset, post-review corrected methodology): the
benchmark enumerates EVERY covered hero decision of all six roots
(115,056 queries, asserted equal to the published set count), runs an untimed
full-pass warmup, then times the batches. The six-root aggregate cold
construction (probe, verified loads, index builds) took 1,329.6 ms and
52.21 MiB resident; warm hits over the complete six-root working set p50
53.21 us, p95 69.67 us, p99 87.63 us (target <= 10 ms, MET), miss p99
16.54 us, zero unexpected miss hits, zero warm allocations. The SPR-10
single root measures 315,855,352 resident bytes and is refused by the probe
pre-gate. The full catalog and CSV contract are in
`docs/development/benchmarking.md`.

Gates (2026-09-16, reference machine Apple M5 Pro / 48 GB / macOS 26.5.1 /
Apple clang 21, after the independent adversarial review hardening):

- `cmake --preset release`: clean configure.
- `format` and `format-check`: pass.
- Full release build: clean.
- Release CTest: 25/25 passed (the expanded `resident` suite and the gated
  benchmark families included).
- `benchmark-resident-lookup`: builds and runs across the complete six-root
  115,056-query working set; p99 87.625 us, target <= 10 ms MET, 0/0 warm
  allocations.
- `benchmark-multistreet`: PASS for all three cases, repeat_delta 0.
- `npm run check`: 32/32 subtests pass; `npm run proto:check`: pass.
- `node bin/replay.mjs`: Decisions 156, Illegal 0, JS fallbacks 0.
- `check-docs` (164 text files) and `check-rfcs` (6 RFCs): pass.
- Targeted ASan/UBSan `ctest --preset asan -R "resident|artifacts"`: 2/2
  passed with zero ASan or UBSan reports (`artifacts` 5.50 s, `resident`
  20.07 s under instrumentation). The artifacts source gained the additive
  probe, so both run instrumented; no other shared solver/poker/service
  source changed, so the full instrumented suite was not rerun (scoped gate).
- No existing target (`bigshark_policy`, `bigshark_service`,
  `bigshark_v0_protocol`, host, clients, platforms) links
  `bigshark_resident`; existing v0 behavior is byte-identical.

Working-tree checkpoint; nothing committed.

## Stage 7 Evidence (2026-09-16)

RFC 0002 Stage 7 (minor-0 Protobuf production migration), rollout steps
3-6: C++ frame codec, strict semantic validator, request/response mappers,
opt-in framed host modes, framed Node client, River v1 mapper, and the v0/v1
golden differential. Preceding gate: commit `b8f7c06` (Stage 6), clean tree.

Owned files (additive; v0 paths untouched):

- NEW `engine/include/bs/v1_protocol.hpp`: protobuf-free public boundary
  (frame codec, byte-level envelope entry point).
- NEW `engine/src/protocol/v1_frame_stream.cpp`: canonical ULEB128 codec,
  five-byte prefix bound, 1 MiB rejection before payload allocation.
- NEW `engine/src/protocol/v1_mappers.hpp` (internal; includes generated
  Protobuf, confined to `engine/src/protocol/v1_*` and the host).
- NEW `engine/src/protocol/v1_semantic_validator.cpp`: hand-written
  protovalidate and poker-semantic checks returning `FieldViolation`s.
- NEW `engine/src/protocol/v1_request_mapper.cpp`: validated v1 request to
  the existing `bs::Ctx`, including v0 token-round line reconstruction.
- NEW `engine/src/protocol/v1_response_mapper.cpp`: degenerate minor-0
  strategy, deterministic selected-action membership validation, error and
  capability builders.
- NEW `engine/src/protocol/v1_envelope.cpp`: parse/dispatch/error boundary;
  never throws across the frame loop.
- NEW tests `engine/tests/test_v1_{frame_stream,semantic_validator,
  request_mapper,response_mapper}.cpp` and fuzz sources
  `engine/tests/fuzz_v1_proto.cpp` plus the Apple-Clang standalone driver
  `engine/tests/fuzz_main.cpp`.
- MODIFIED `apps/engine-host/main.cpp`: adds `--proto` / `--serve-proto`
  branches only; the default and `--serve` JSON paths are unchanged. The
  parse-error fold and v0 serialization are byte-identical.
- MODIFIED `proto/CMakeLists.txt`: declares `bigshark_v1_protocol` (sources
  stay in `engine/src/protocol`), the four v1 ctests, and the off-by-default
  fuzz target. Root ordering stays engine-before-proto because engine's
  vendored Abseil satisfies protobuf (reordering collided on duplicate Abseil
  targets, verified by configure failure).
- NEW `clients/node/proto-engine-process-client.ts`: Buffer varint codec,
  pre-allocation length cap, bigint-preserving binary envelopes, request-id
  map correlation, identical warmup/timeout/exit/restart semantics.
- NEW `clients/node/tests/fixtures/fake-proto-engine.ts` and
  `clients/node/tests/proto-engine-process-client.test.ts` (10 cases).
- NEW `platforms/river-club/src/v1-mapper.ts`: `RiverRoom` to
  `DecisionRequest` and `Strategy`/`EngineError` to `ExecutableDecision`.
- MODIFIED `platforms/river-club/src/engine.ts`: framed path selected only by
  `BIGSHARK_ENGINE_PROTO=1` or `EngineConfig.proto`; unset keeps the v0
  client. EngineError/transport failure routes to the existing operational
  `safeFallback`, never a strategic fold.
- MODIFIED `platforms/river-club/src/types.ts`: additive `RiverPot`,
  `EngineConfig.proto`, and `protoEngineClient` fields plus `pots` validation.
  `v0-normalizer.ts` is unmodified.
- NEW `platforms/river-club/tests/v0-v1-differential.test.ts`: every frozen
  fixture through the real binary on both transports.
- MODIFIED `tsconfig.json` (compile the committed-path generated TS output)
  and `package.json` (`typecheck`/`test:node` ensure generated TS exists).
- MODIFIED docs: `docs/reference/protobuf-engine-protocol.md`,
  `docs/development/build-and-test.md`, this plan.

`engine.proto`, `buf.yaml`, and the breaking baseline are unchanged; the
accepted IDL was not edited.

### v1 -> Ctx reconstruction rules

- Position: occupied seats sorted by seat, rotated from the button; heads-up
  button is `BTN` and the other seat `BB`; otherwise `BTN`, `SB`, `BB`, then
  `CO`/`HJ`/`UTG` counting back from the button (matching the v0 buckets).
- `effectiveStackBb`: the additive optional hint
  `options.preflop_effective_stack_bb` (tag 7) is used verbatim when present;
  otherwise the host falls back to a structural estimate (heads-up
  min-total/bb, multiway hero-depth/bb). The River adapter sends the platform
  value with v0 precedence for exact parity.
- `potOdds = to_call / (pot + to_call)`; pot, blinds, and inclusive
  bet/raise targets are absolute integers checked against `INT_MAX` and the
  2^53-1 profile.
- Cards use the exact v0 tokens: rank from `23456789TJQKA`, suit from
  `shdc` (spades, hearts, diamonds, clubs).
- Preflop `raises`, call `limpers`, and `openerPosition` are derived only
  from structured preflop history; they stay zero on later streets.
- River knobs replay the v0 normalizer's token-round grouping over the
  structured sequence (folds freeze transitions; calls and two passive
  actions close a round), then rebuild `flopLine`/`turnLine` actor tokens
  (`H`/`O` plus `x/c/b/r/f`) and the compact `riverLine` state machine
  (`""`, `c`, `b`, `cb`, `br`, `cbr`, with terminals disabling the solver),
  enabling the Nash solver only heads-up with five board cards and complete
  fold-free rounds.
- The TS adapter assigns history streets with the same replay, capped at the
  current street, so a lone early-river check (frozen fixture 4) stays in the
  turn round exactly as v0 groups it.
- Seed is full `uint64`; the v0 signed cast remains v0-only.

### Parity matrix

All six frozen `v0-golden.json` fixtures reproduce action and exact target
through the real engine binary on both transports:

| Fixture | v0 / v1 action | Target | River solver |
| --- | --- | --- | --- |
| preflop-facing-reraise | raise / raise | 2000 / 2000 | off |
| multiway-flop-check-option | check / check | 0 / 0 | off |
| multiway-turn-facing-raise | raise / raise | 340 / 340 | off |
| heads-up-river-check-option | check / check | 0 / 0 | on, river line `""` |
| multiway-river-facing-bet | fold / fold | 0 / 0 | off (multiway) |
| short-stack-heads-up-river | bet / bet | 250 / 250 | on, river line `c` |

`test_v1_request_mapper` independently asserts the mapped `Ctx` fields for the
same six hand-built requests. The only tolerated label difference is the
reason prefix (`cpp:` on the executable wrapper); action and amount compare
equal.

Representability notes: v1 cannot carry per-opponent `opponentPcts`, the
`riverRaiseFrac` override (the mapper uses the fixed 1.0 the heuristic
defaults to), or a style beyond tag/lag/station-hunter; none of the frozen
fixtures use those knobs, and unknown profiles/features fail validation rather
than silently defaulting. General actor attribution uses full display-name
prefix matching (hero precedence, longest-name tie-break); an unmatched actor
is treated as an opponent. The preflop opener intentionally replicates v0's
first-token exact-match quirk (multi-word opener -> no opener -> HJ default)
via an empty actor id; this adapter-only shim is removed with the v0 path.
The v1 player stack is the actual chip stack behind (`seat.stack`); the
platform preflop effective depth now travels through the additive hint field
for exact parity, with a structural estimate only as the hint-absent
fallback. Target legality is checked against `stack + street_committed`.

### Independent adversarial review (2026-09-16)

A 21-agent independent adversarial review (four paths plus a skeptic, zero
dismissals) confirmed 17 defects, several P1, all reproduced end to end on the
real release/ASan binaries. Disposition:

- Open-enum root cause: a single closed-set predicate per wire enum now runs
  before any indexing, mapping, or decision use — ActionType (the
  `std::array<bool,5>` index is now guarded; type 99/-1 rejected), Rank,
  Suit, decision/history Street (SHOWDOWN and unknown rejected, fixing river
  hands being played as flop semibluffs), PlayerStatus, ForcedContributionType,
  GameVariant, BettingStructure, GameType, SolverMode. Mapper conversion
  functions throw on unknown values instead of emitting empty streets,
  one-character cards, or dropped actions. Response-only enums are never read
  from a request.
- Integer chip narrowing: every chip field reaching the integer Ctx uses
  `chipFitsInt` (pot total/main/side, blinds, stacks, commitments, forced
  amounts, history targets, to_call, legal ranges), closing the 2^53-1 pass
  that narrowed to a possibly negative int.
- Bet/raise maximum is bounded by `hero.stack + hero.street_committed`
  (all-in boundary legal).
- Pot consistency: no-side-pot case requires total == main; the TS adapter now
  populates every River side pot with eligible seats and a multiway side-pot
  hand returns `UNSUPPORTED_FEATURE` -> EngineError -> safeFallback.
- Big-blind option: fold/check/raise with `to_call == 0` is accepted only in
  the structured unopened-big-blind state (hero posted exactly BB this street,
  no voluntary preflop raise yet); all other no-bet spots stay check/bet.
  Frozen-option regression added; the rule is not broadened to arbitrary
  raises.
- potOdds: investigation of the frozen platform data showed River rounds
  `to_call/(pot+to_call)` to four decimal places (0.3061 vs 0.306122...,
  0.0313 vs 0.03125). The mapper reconstructs that exact four-decimal value;
  all six fixtures now produce `Ctx.potOdds` equal to the frozen v0 context,
  and action/target parity is unchanged.
- solver_mode: forced RIVER_LP/RIVER_DCFR/MULTISTREET_CFR are
  `UNSUPPORTED_FEATURE` (never silently ignored), unknown values invalid;
  capabilities advertise exactly AUTOMATIC and HEURISTIC as selectable modes
  while the exact_lp/DCFR feature bits honestly report internal backends.
- Response amplification: `FieldViolation` is capped at 32 with an omission
  marker, so every error envelope fits one frame; the host logs and continues
  on an over-large response instead of killing the coprocess.
- Fuzz integrity: structured serialized decision seeds (valid plus
  type/street/rank/suit 99/-1, BB option, side pot, oversized pot, unknown
  status, forced solver) join the framing seeds in `fuzz_seeds.hpp`; a 100k
  ASan campaign over 17 seeds passed with zero crashes.
- TS name resolution: multi-word and unmatched actor names map as opponents,
  never as hard throws; dedicated multi-word, side-pot, and BB-option tests
  added. Delete-the-fix red tests accompany every change in
  `test_v1_semantic_validator.cpp`, `test_v1_request_mapper.cpp`, and
  `test_v1_response_mapper.cpp`.

### Fourth-round adversarial review (2026-09-17)

The final skeptic verdict (real release binary, fixed-state sweep) found two
latent full-input-space parity divergences. Closed with one authorized
strictly-additive IDL change and an adapter-only quirk shim.

- Effective stack (IDL additive). `DecisionOptions` gains
  `optional double preflop_effective_stack_bb = 7` (nothing existing moved);
  `buf breaking` against the unchanged baseline still passes. The C++ mapper
  uses the hint verbatim (validator rejects non-finite, non-positive, or
  >10000 bb) and otherwise falls back to the structural estimate. The River
  TS adapter sets it with v0-normalizer's exact precedence
  (`solver.effectiveStackBb ?? hero.effectiveStackBb ?? 100`). A 156-room plus
  six-fixture corpus study had already shown the server value is not
  structurally reconstructable (83/162 hero-depth, 35/162 min-total), so the
  hint is required for exact full-space parity; the fallback remains for
  generic hint-less clients and is documented as an approximation.
- Multi-word opener quirk. v0 derives the opener from the first whitespace
  token by exact seat-name equality, so a multi-word opener matches no seat
  and defaults to HJ; v1's correct full-name parse diverged. The TS adapter
  now applies that v0 rule only to the first preflop raise, emitting an empty
  `actor_player_id`; the C++ mapper accepts an empty id as an unresolved
  opener (raise counts accrue, opener position/last raiser unset) while all
  other actors keep correct full-name attribution. Shim is adapter-only and
  deleted with v0.

Differential honesty: 9 real dual-transport action+target parity cases (the
six frozen snapshots, the short heads-up hint jam, the cross-gate HU 260 /
server-12 room, and the four-way multi-word opener). The BB-option,
side-pot fail-closed, unmatched-name, and transport-failure cases are v1-only
rejection/validation semantics with no v0 counterpart and are labeled as
such, not counted as parity.

### Gates (2026-09-17; post-fourth-review)

- IDL: seven added lines, one additive optional field; baseline unchanged and
  `npm run proto:check` (lint/format/breaking/generate/typecheck + 7/7 golden
  vectors) green. No existing fixture sets the field, so existing wire bytes
  are unchanged.
- format + format-check pass; Release CTest 29/29; benchmark-multistreet 3/3.
- `npm run check`: 56/56 (14 differential cases); replay 156/0/0 with the
  unchanged mix `{raise 32, check 48, fold 68, bet 5, call 3}`.
- check-docs (185) and check-rfcs (6) pass.
- v0 identity: v0_json.cpp, v0-normalizer.ts, JSON client, IDL baseline
  unmodified; `decision.cpp` untouched this round. The opener shim is only in
  the new v1-mapper.ts and the empty-id wire marker.
- ASan: v1/v0/protobuf native targets green against the rebuilt instrumented
  binary; 27/27 passed in 1468.04 s, exit 0, zero sanitizer reports.

(Third-round gates: Release 29/29, ASan 27/27 in 1470.91 s, 120k/19 fuzz; the
80m arithmetic differential and 2,000,000,000 jam verification stand.)

A final review (with a 49-million-case old-vs-new arithmetic differential
proving the prior four widenings) found three P2 residuals:

- P2-1 preflop `roundBB` int multiply overflow. Complete money-arithmetic
  audit of `decision.cpp`: widened `roundBB` to int64, widened the RFI
  `(2.5+limpers*1.5)*bb` cast, and clamped all four preflop BB-sized sites
  (`RFI`, 3bet value, 3bet bluff, 4bet bluff) to the legal int range in a
  dedicated `clampRawLegal` (preserving the historical raw
  `[raiseMin,raiseMax]` bounds, which can include 0, so in-range output is
  bit-identical). Direct `raiseMax` jam sites need no arithmetic. The postflop
  targetTotal/reraiseto/MDF sites were widened in the prior round.
  Equivalence: an 80,000,000-case randomized old-vs-new differential
  (`/tmp/arith_diff.cpp`, discarded) matched every in-range value; the
  reviewer's frame (call 715,827,887, max 2,000,000,000, AA) now outputs
  `3bet value AA -> 2000000000` on both release and ASan binaries with empty
  stderr (zero UBSan). v0 replay stayed 156/0/0 with the unchanged mix.
- P2-2 effective stack parity. Platform source confirmed:
  `v0-normalizer.ts:192` passes the server-supplied
  `room.solver.effectiveStackBb` (estimate; default 100); v1 has no field.
  Corpus analysis (156 local rooms plus the six frozen snapshots) showed the
  server value is not a single reconstructable formula (best structural
  hypothesis matched only 96/162 exactly; min-total 35/162; deltas up to 166
  bb). The only consumer is the preflop `<=12bb` jam gate, so the C++ mapper
  reconstructs the physical effective stack decision-equivalently: heads-up
  `min(stack+street_committed, single live opponent total)/bb`, multiway
  `hero.stack/bb`. This matches five of six frozen fixtures and the reviewer's
  short heads-up room exactly (10 bb -> jam 200); zero jam-gate flips exist in
  the corpora (the one frozen deep-pot difference 96/97 is above the gate; the
  three server-0 session rooms hold garbage hands that fold either way). The
  rule and its evidence are documented in the reference protocol doc.
- P2-3 multi-word hero name. The TS replay kept the full event text and
  `resolveActor` now matches seats by full display-name prefix with hero
  precedence and longest-name tie-break (mirroring v0's
  `text.startsWith(heroName)`), so a multi-word hero's raise is attributed to
  the hero and a shared first token cannot shadow the hero. A multi-word hero
  plus multi-word opponent differential case asserts the derived ids.

The differential suite grew to 12 cases, adding the real-dual-transport
short-effective-stack jam (raise/200 on both transports) and the multi-word
hero attribution. Fuzz seeds remain 19 (the structured large-chip and INT_MAX
overflow seeds exercise the arithmetic fix).

### Gates (2026-09-16, reference machine Apple arm64 / macOS 25.5 / Apple
clang 21; post-third-review)

- `format` + `format-check`: pass; release build clean; Release CTest 29/29.
- `benchmark-multistreet`: 3/3 PASS.
- `npm run check`: 54/54 (12 differential cases included); `npm run
  proto:check`: lint/format/breaking/generate/typecheck + 7/7 vectors;
  `engine.proto` and the baseline unchanged.
- `node bin/replay.mjs`: 156/0/0, action mix
  `{raise 32, check 48, fold 68, bet 5, call 3}` unchanged.
- `check-docs` (185 files) and `check-rfcs` (6 RFCs): pass.
- v0 identity: `v0_json.cpp`, `v0-normalizer.ts`, JSON client, default host,
  IDL, and baseline unmodified; `v0_protocol` and `v0-golden` green.
- 80m-case old-vs-new arithmetic differential: exact in-range equality.
- Full ASan/UBSan `ctest --preset asan`: 27/27 passed in 1470.91 s, exit 0, zero AddressSanitizer/UBSan/signed-overflow reports;
  the reviewer large preflop frame returns the 2,000,000,000 jam with no
  UBSan. Structured fuzz: 120,000 inputs over 19 seeds, zero crashes.

(Earlier rounds: second review Release 29/29, ASan 27/27 in 1506.04 s,
120k/19 fuzz; first review 100k/17; initial 50k framing-seed campaign found
the UTF-8 hardening.)

Eleven of thirteen first-round fixes closed; one residual P2 and one P3
defense item were confirmed against the release/ASan binaries and fixed:

- P2 signed-int `pot_total + to_call` overflow (negative potOdds flipped a
  fold to a call; UBSan at the mapper and in shared policy). Fixed in two
  layers: (a) the validator and mapper now reject the combination in `uint64`
  before narrowing when the sum exceeds `INT_MAX`
  (`v1_semantic_validator.cpp` derived-sum block; mapper defense re-check),
  with delete-the-fix red tests at 2e9+2e9, INT_MAX+1, and an inclusive
  INT_MAX boundary; (b) the shared `engine/src/policy/decision.cpp` chip
  arithmetic was widened to `int64`/`double` with no threshold or constant
  changes — `targetTotal` (call + rounded geometry), both MDF denominators,
  and the river re-raise geometry.
- P3 defense: a preflop BET (not only RAISE) now closes the big-blind option;
  hand-frame regression added. Fuzz seeds grew to 19 with explicit
  near-INT_MAX and pot+to_call overflow combinations.

Equivalence argument for the shared-file change: the old code added two
non-negative ints in `int` then converted the ratio operands to `double`; for
every non-overflow input the exact integer sum is <= 2^53-1, so
`(double)a + (double)b` is exactly the same value as the widened
`int64`/`double` sum. `targetTotal` returns the same value because its result
is clamped to the int legal range; the intermediate `llround`/`*bb` are done
in `int64`. Zero v0 drift is proven by `test_v0_protocol`, the eight
`v0-golden` tests, and replay 156/0/0 with an unchanged action mix
`{raise 32, check 48, fold 68, bet 5, call 3}`.

### Gates (2026-09-16, reference machine Apple arm64 / macOS 25.5 / Apple
clang 21; post-second-review)

- `format` + `format-check`: pass; release build clean; Release CTest 29/29.
- `benchmark-multistreet`: 3/3 PASS.
- `npm run check`: 52/52; `npm run proto:check`: lint/format/breaking/
  generate/typecheck + 7 vectors; `engine.proto` and baseline unchanged.
- `node bin/replay.mjs`: 156/0/0 with the identical action mix;
  `check-docs` (185 files) and `check-rfcs` (6 RFCs): pass.
- v0 identity re-proven: `v0_json.cpp`, `v0-normalizer.ts`, JSON client,
  default host path, IDL, and baseline unmodified; `v0_protocol` and
  `v0-golden` green.
- Full ASan/UBSan `ctest --preset asan`: 27/27 passed in 1506.04 s, exit 0, zero AddressSanitizer/UBSan/signed-overflow reports.
- Structured fuzz: 120,000 mutated inputs over 19 seeds (including INT_MAX
  and pot+to_call overflow) under ASan, zero crashes/hangs.

(First review-round gates: Release CTest 29/29; ASan 27/27 in 1557.41 s;
100k/17-seed fuzz clean. Pre-review first pass: 50k framing-seed campaign
found and fixed the UTF-8 hardening.)

No live dry-run/canary, v0 removal, replay switch, or minor-1 full
distribution was performed; those remain explicit external gates. The v0
JSON host, NDJSON client, published binary default, and River defaults are
unchanged.

## Stage 8 Evidence (2026-09-17)

RFC 0002 Stage 8 (negotiated minor 1, full resident blueprint
distributions, opt-in), RFC 0005 328-359. Preceding gate: commit `f8b1654`
(Stage 7 minor-0 production migration), Release CTest 29/29, replay
156/0/0 with action mix `{raise 32, check 48, fold 68, bet 5, call 3}`,
clean tree.

### Additive IDL diff (buf breaking against the unchanged baseline: pass)

- `SolverMode`: `SOLVER_MODE_BLUEPRINT = 6`, `SOLVER_MODE_RESOLVING = 7`.
- `SolverSource`: `SOLVER_SOURCE_BLUEPRINT = 6`, `SOLVER_SOURCE_RESOLVING = 7`.
- `SolverMetadata`: `optional string artifact_sha256 = 9` (pattern
  `^[a-f0-9]{64}$`), `optional string guarantee = 10` (in
  `modeled_exact_bound|uncertified|baseline`).
- NEW message `ExpandedStrategy` (`repeated ActionPolicy actions = 1`
  1..32, `optional SelectedAction selected_action = 2`, required
  `SolverMetadata solver = 3`); `DecisionResponse.result` gains
  `ExpandedStrategy expanded_strategy = 3`. The minor-0 `Strategy` keeps
  its 1..5 entry cap; nothing was renumbered, retyped, or widened.
- NEW golden vectors, old fixtures byte-identical:
  `expanded-strategy` (7 actions, BLUEPRINT/uncertified/64-hex digest),
  `expanded-strategy-bound` (`modeled_exact_bound` wire pin, never emitted),
  `expanded-strategy-baseline` (`baseline` wire pin, reserved),
  `capabilities-minor1` (minors [0,1] with BLUEPRINT),
  `solver-enum-presence` (enum ordinals 6 AND 7 on the wire).

### Host-services seam

- MODIFIED `engine/include/bs/v1_protocol.hpp`: protobuf-free
  `V1HostServices` (`blueprintAdvertised`, const `blueprintHeroDecision`
  returning poker-domain `V1BlueprintRow`/`V1BlueprintResult`),
  `V1BlueprintMiss` mirroring the resident miss vocabulary plus
  `UnsupportedHandState`, `noResidentServices()`, and the two-arg
  `handleEnvelope(frame, services)`. The one-arg entry point delegates to
  the shared no-resident service. Minor 0 never touches services and keeps
  the exact Stage-7 call graph and response bytes (asserted against the
  captured Stage-7 capability envelope inside `test_v1_minor1`).
- MODIFIED `apps/engine-host/main.cpp`: the proto modes parse repeatable
  `--resident-root <path>=<64 lowercase hex sha256>`, build ONE
  `bs::resident::ResidentPolicySet` before serving, print per-root results
  to stderr only, and inject the resident-backed service into `runProto`.
  Default/`--serve` JSON paths, `--proto` without flags, v0 serialization,
  and the parse-error fold are unchanged.

### Negotiation, validator, response mapping

- Accept exactly minors 0/1; >1 is `UNSUPPORTED_PROTOCOL` echoing minor 0.
  Capabilities: minor 0 returns the frozen Stage-7 response byte-for-byte
  (minor 0 only, AUTOMATIC/HEURISTIC, build `v1.0.0`); minor 1 lists [0,1],
  build `v1.1.0`, and adds BLUEPRINT only with an advertised root.
  RESOLVING and guarantees are never advertised.
- Validator is minor-aware: BLUEPRINT/RESOLVING known ordinals
  (`isKnownSolverMode` now 1..7); BLUEPRINT on minor 0 and RESOLVING on
  either minor are UNSUPPORTED_FEATURE; forced LP/DCFR/multistreet remain
  rejected. Minor-0 diagnostic wording is preserved exactly.
- `mapBlueprintExpandedResponse`: 1..32 verbatim rows, probabilities
  finite/non-negative summing to 1 within 1e-12, target only on
  bet/raise, `all_in` derived as legal max == hero capacity, sampled action
  via one domain-separated SplitMix64 draw, selected membership checked
  against BOTH the row and the request legal set, source BLUEPRINT,
  cache_hit, reason `blueprint`, artifact digest, guarantee
  `uncertified`. `modeled_exact_bound` is unreachable.
- `mapHeuristicExpandedResponse`: minor-1 degenerate ExpandedStrategy with
  the heuristic's REAL source and no digest/guarantee (never relabeled
  BLUEPRINT).
- NEW `engine/include/bs/prng.hpp`: shared SplitMix64 with a fixed protocol
  sampler domain constant `0x425356312d73616d` XORed into the seed; lookup
  is seed-independent, one draw, first prefix bucket strictly greater than
  the top-53-bit point (zero-prob buckets can never be chosen).

### HandState -> HeadsUpState reconstruction

NEW `engine/src/protocol/v1_resident_mapper.{hpp,cpp}`: postflop heads-up,
no-ante/equal-matched profile only. Player index is occupied seat order and
`root.button` is the button player's index (matching artifact range/key
convention — verified against the trained fixture: player order is seat
order, the button is a root field, not a rotation). Flop root derived from
current stacks plus exact target-total payments and current pot minus
postflop payments (or the first event's `pot_before` when supplied);
preflop events only establish the root pot. Flop/turn/river voluntary
events replay through the exact poker engine with absolute targets and
dealt cards; final stacks/street commitments/pot/to_call/actor must match
the snapshot. Every reject class is a deterministic miss
(RootNotSupported/OffTree), never a clamp or invented root.

Independent oracle `test_v1_resident_mapper` builds expected roots/nodes
directly with the poker API and compares field by field: flop first action,
check-to-button and facing-lead, turn check-to-BB after bet/call, river
after turn raise/call, the all-in jam, the button-on-seat-1 player mapping,
plus rejects for preflop, multiway, antes, folded opponent, odd matched
pot, unresolved actor, missing target/increment, illegal check-after-bet,
tampered pot/to_call, hero/board card collision, and a turn event without
the turn card.

### Link and build

- `bigshark_v1_protocol` links `bigshark_resident` PRIVATE in
  `proto/CMakeLists.txt`: SQLite/OpenSSL/resident symbols never cross the
  public boundary into v0/service/policy/decision/host-public. v0, JSON,
  and default-host paths are byte-identical (proved below).
- NEW ctests `v1_resident_mapper`, `v1_minor1` (negotiation/cap
  filtering/forced-mode matrix/full 1..32 mapping/exact-seed
  sampling/membership/coverage misses/minor-0 byte identity), and the
  offline `bs_resident_fixture` publisher (also copied to `bin/` under the
  release publish preset) that trains a complete six-root-action postflop
  artifact for the real-binary matrix.
- Fuzz seeds extended (`fuzz_seeds.hpp`) with minor 0/1/2/huge capability
  frames, minor-1 BLUEPRINT/RESOLVING/AUTOMATIC/HEURISTIC decisions,
  BLUEPRINT-on-minor-0, open enum 8, a reconstruction-miss history, and a
  hand-built expanded_strategy response. Standalone ASan driver: 20,000
  inputs over 31 seeds cleanly.

### Node + River

- `ProtoEngineProcessClient` gains the explicit `negotiateMinor1` opt-in:
  warmup at minor 0, then the minor-1 capability probe;
  `negotiatedProtocolMinor`/`minor1Capable` expose the result; downgrade to
  0 on an old host; renegotiated after every hard restart. uint64 stays
  bigint. Non-opted-in callers never send minor 1.
- `v1-mapper.ts`: `decisionEnvelope(request, minor)`;
  `fromV1DecisionResponse` decodes both result oneofs and validates the
  sampled action against the FULL reported list (up to 32) before
  execution. Engine errors keep surfacing as `V1EngineError` -> operational
  `safeFallback`, never a fold.
- `engine.ts`/`types.ts`: `BIGSHARK_ENGINE_PROTO_MINOR1=1` negotiates at
  client construction; `EngineConfig.protoBlueprint` forces BLUEPRINT only
  on a minor-1 client. Minor-1 AUTOMATIC tries resident then heuristic.
- Fake hosts extended and a new old-host fake; 17 client tests including
  new-client/old-host downgrade, the >5 expanded hit, bad-member detection,
  and the minor-1 heuristic provenance. NEW real-binary matrix
  `v1-minor1-blueprint.test.ts` (10 tests) publishes a fixture through
  `bs-resident-fixture`, serves it with `--resident-root`, and verifies
  the handshake, a six-action blueprint hit with digest/uncertified and
  all-in jam row, seed-independent distributions, RootNotSupported on a
  changed flop, the wrong-pin hidden-BLUEPRINT miss, non-opted-in minor-0
  isolation, RESOLVING rejection, and mapper membership/legal-window
  validation.

### Explicitly deferred (not implemented)

Resolving gadget, certification bounds, modeled_exact_bound/baseline
emission, whole-range selection, eligibility gates, live River
minor-1 rollout (the River default stays minor 0), and v0 removal.

### Independent adversarial review fixes (4-way + skeptic, 0 dismissals)

Three findings were confirmed (#1/#2 share one root defect, demonstrated
end-to-end by two reviewers) and fixed without weakening any validation or
the minor-0 path:

- #1/#2 (P2, core) production adapter omitted postflop action amounts, so a
  forced blueprint could only ever hit the flop-open node. `v1-mapper.ts`
  fills the postflop `ActionEvent` chip fields the resident reconstructor
  replays. Source of amounts: the tokenized event text carries the only
  per-historical-action chip number (current `seat.bet`/`pot` are
  decision-snapshot values and cannot recover an earlier action), and the
  current-street snapshot supplies exact structured values. Three exactness
  rules (see the reference doc):
  1. exact-integer text ONLY is trusted — the last whitespace token must be
     pure digits; any decimal or K/M/B multiplier token ("1.2K", "ALL IN
     3.6K", "70.5") is a rounded display and is never sent as an exact target;
  2. current decision street: when the text is rounded, the exact value is
     recovered from the live structured snapshot (`seat.bet`, current street
     commitment) only for the actor's FINAL chip action this street, with a
     per-(street,actor) ledger-completeness guard so an actor with an earlier
     unfillable same-street action is never mis-subtracted;
  3. past/closed streets accept exact-integer text only; rounded display
     stays unset -> deterministic OffTree miss. fold/check carry no amount.
  No amount is ever guessed; an unparseable past-street amount is the honest
  fail-safe miss. Corrected, reproducible corpus coverage
  (`sessions/2026-09-11.jsonl`, keyed on every postflop chip action in a room
  snapshot with events): of **3357 postflop chip actions, 3318 (98.84%) are
  exact integer tokens and 39 are K-suffix rounded display** (zero plain
  decimal tokens). The 39 rounded events break down by room
  `solver.playersInHand` as `{3: 1, 4: 3, 5: 15, 6: 20}` — i.e. every one is
  multiway (no heads-up room); on actionable postflop decision snapshots only
  2 sit on the current decision street and both are structurally recoverable
  to the exact integer. No HU-specific subset claim is made (a prior draft
  claimed "1112/1135 HU exact / 16/17 current-street recovery", which is not
  reproducible and was retracted). Current-street structured recovery exists
  for the future heads-up case; the present corpus exercises it via
  production/unit tests rather than live frequencies.
  Tests: NEW `v1-amount-parsing.test.ts` (980/1225 exact; 1.2K/2.5K/1K/
  1.0K/ALL IN 2.5K/70.5 -> null) and a production `v1-minor1-walk.test.ts`
  case where the opponent's current-street bet is displayed with a K suffix
  but the exact structured seat commitment is recovered and the node still
  hits SOLVER_SOURCE_BLUEPRINT, plus a direct assertion that a "1.2K"/1225
  room emits `target_total = 1225`, never 1200. Mutation verified: restoring
  the old K-scaling parser fails both the parser unit test and the 1225
  regression. Existing walk tests (flop facing bet, turn bet/call, river
  facing raise, strip-amounts miss) all pass.
- #3 (P3) sampler tolerance mismatch. `sampleBucket` now scales the uniform
  draw onto the row's actual recorded prefix sum and clamps to the last
  positive bucket, so every row passing the 1e-12 probability-sum check
  deterministically samples (sums 1 +/- 1e-13 covered for 64 seeds), zero
  probability buckets remain unselectable, and the pinned golden vectors on
  exactly normalized distributions are unchanged (seed 42 -> bucket 3, 0 ->
  6, 1 -> 4).
- Hardening (P3, fail-closed): the reconstructor explicitly rejects any
  player with status ALL_IN at a postflop decision (previously only FOLDED),
  and verifies equal matched preflop contributions from forced blinds plus
  the full attributed voluntary ledger — 15+25=40 is rejected while 20+20=40
  reconstructs; even-pot with blinds-only and no attributed completion keeps
  the matched assumption (both players non-all-in, and an asymmetric shape
  cannot reach an action node).


## Stage 9 Evidence (bounded resolving gadget + independent certification)

Offline implementation; v0/minor-0 behavior and bytes are unchanged and no live
canary is wired (`AUTOMATIC` never resolves; the River default stays minor 0).

**Augmented-game / m(I)/b(I) normalization.** Exact chip terminals are reused
from `HeadsUpState::settle_fold/settle_showdown`. For deal
`w(h,r)=rangeW_hero(h)*rangeW_resp(r)*pi^prefix_hero(h)` (responder's own
reach excluded), `m(I)=sum_h w`, `b(I)=sum_h w C_base(h,r)/m(I)` with zero
mass recorded and never divided. Gadget root chance normalizes `w/sum w`;
`TERMINATE` pays the centered `b(I)` zero-sum and `CONTINUE` enters the exact
terminal subtree. Full-traversal CFR keeps RFC 0004 regret matching and the
two-player `kSimple` own-reach average, with Stage-9 linear iteration weighting
(algorithm id `rfc0005-stage9-gadget-linear-rev1`); the constant terminal
payoff matrix is computed once per resolve.

**Tiny-game cert tolerance and oracle cross-check.** Canonical fixed-runout
terminal game (pot 2, matched 1/1, responder jams 1; hero AA/88 vs QQ): engine
settlement fold `+1`, call `-2`/`+2`; `m=2`, `b(all-fold)=1`,
`b(all-call)=0`, deceptive fold-winner/call-loser `=1.5` (RFC 425-430). The
whole-range atomic certifier rejects 1.5 with `slack>0`; the equilibrium
candidate (call winner/fold loser) passes, worst `slack = -1e-9*pot`, and the
independently written `TerminalOracle` (`engine/tests/resolver_oracle.*`, its
own runout/reach/pure-BR enumeration) reports gadget NashConv `4.585e-10` at
100,000 linear-CFR iterations, inside the `1e-8*root_pot` gate. The
production certifier and the oracle agree on the candidate margin to 1e-10.
Zero-mass responder combos are recorded with mass zero and carry no deal.

**Deadline behavior.** One steady-clock deadline starts at receipt; >=5 ms and
10% are reserved for return processing, the remaining window is split solve
(70%) then independent certify, and every per-node `Budget` cancels inside the
traversal. Solve or certify limit => discard: complete validated resident
blueprint is emitted as source BLUEPRINT/`baseline`; otherwise forced
RESOLVING returns retryable `DEADLINE_EXCEEDED`. Non-terminal node / no root /
off-tree / digest mismatch / coverage miss => `UNSUPPORTED_FEATURE`.
Certified => source RESOLVING(7)/`modeled_exact_bound`, unreachable from any
other path (enforced in the response mapper).

**Files.** New static lib `bigshark_resolver`
(`include/bs/resolver.hpp`, `src/resolver/{continuation,counterfactual_reach,
gadget_cfr,certifier,resolver}.*`), terminal-only is a graph property (one
structural trace per action); resident `resolver_source()` accessor; minor-1
envelope/validator/capabilities/reconstruction (facing-all-in admitted on the
resolve path only); host owns one resolver + per-request scratch; TS
`ResolverEligibility` plus `classifyResolveResponse`; tests
`test_resolver`, `test_v1_resolving`, `resolver-eligibility.test.ts`; manual
benchmark `benchmark-resolver` (100k iterations solve+cert ≈ 69 ms on the tiny
game, worst slack `-1e-9` pot).

**Adversarial review round (2026-09-18).** An independent four-dimension
review with per-finding skeptics (17 agents, 1.38M subagent tokens) confirmed
2 P2 findings and refuted 11, including every P1 claim. One finder alleged the
linear-CFR `O(1/T^2)` rate was false; an independent four-decade measurement
(NashConv 4.58e-6 / 4.58e-8 / 4.59e-10 / 4.59e-12 at T = 1e3..1e6, with
`NashConv*T^2` constant at 4.585) refuted it. The digest-channel P1s were
refuted because RFC 0005 makes the pinned-digest comparison response-carried
and adapter-owned (the host reports its process-pinned `artifact_sha256` on
every expanded response) and forbids a request-side eligibility bit.

Both confirmed P2s are fixed with mutation-red regressions:

- **Range completeness over dealt combos.** `certify_candidate` previously
  required a candidate row for every board-unblocked `live_hero` combo, but a
  holding that blocks every declared opponent combo carries zero
  counterfactual mass, enters no responder infoset, and is trained by no
  gadget deal, so the size check failed and discarded a certifiable candidate
  (reproduced: `live_hero=2, deals=1` -> `CoverageMiss`). The completeness
  requirement now covers exactly the combos appearing in positive-weight
  deals; such holdings remain recorded in `live_hero` and are never
  fabricated into the candidate. Regression
  `test_hero_combo_without_compatible_opponent` (mutation-red: restoring the
  old check fails it).
- **Budget-derived iteration cap.** The gadget loop ran a fixed
  `limits.iterations` regardless of the request budget, so a short
  `solve_time_budget_ms` could only stop work by exhausting the wall clock.
  `Resolver::resolve` now derives a deterministic cap after `build_model`,
  where the cost drivers (joint deals x ordered actions) are known:
  `iteration_cap_for_budget` divides the solve share of the budget (70 percent
  after the return reserve) by a conservative per-unit cost times two traverser
  sweeps over every deal and action, with the configured count as an upper
  bound. A zero cap discards rather than publishing an untrained candidate.
  The derivation reads only public inputs (budget, deal count, action count),
  so every counterfactual hero combination derives the identical cap. The
  deadline-fallback baseline row is taken first, inside the same
  receipt-anchored window. Regressions
  `test_iteration_cap_scales_with_budget` (cap is monotone in budget, strictly
  decreasing in deals and actions, honors the configured upper bound, and is
  zero for degenerate shapes; a 1 ms budget discards with zero completed
  iterations) and the rewritten `test_deadlines_discard` (mutation-red: making
  the cap budget-independent fails the monotonicity assertions). The cache
  identity carries the DERIVED cap rather than the configured count, so two
  requests at the same public state with different budgets cannot share a
  candidate; regression `test_cache_identity_includes_budget` (mutation-red:
  keying on the configured count instead fails it). A focused follow-up review
  then showed the key also omitted `max_nodes`/`certify_max_nodes`, so a warm
  hit could silently bypass the documented certification safety valve; both are
  now keyed and the same regression asserts a small `certify_max_nodes` still
  discards through a warm resolver.

**Re-review round (2026-09-18).** An independent re-review of both fixes with
per-finding skeptics (6 agents, 655k subagent tokens) confirmed the range fix
outright and rejected the first budget fix: three finders independently
measured that a flat `kGadgetIterationCost = 12 us` is not conservative — real
per-iteration cost grows linearly with the joint-deal count (about 0.65 us at
2 deals, 140 us at 420 deals, ~29 ms at 300 combos per side), so the derived cap
was non-binding on exactly the wide profiles the wall clock already governed.
The fix was rebuilt around the measured cost model above
(`kGadgetIterationUnitCost = 250 ns` per deal x traverser walk step, roughly
fifteen times the measured cost) and re-verified: at 420 deals the model
estimates 630 us/iteration against a measured 139 us, and a 100 ms budget
derives a cap of 100 iterations that genuinely binds. No safety consequence
existed in the rejected version — every path already failed closed to the
whole-range baseline — so the correction is about the documented boundedness
claim actually holding.

**Explicitly deferred (external gates):** live/canary wiring, a real second
platform, Linux portable verification, and nested/multiway/general
off-tree/general custom-payoff solving. General multi-request resolving and
crash-safe external baseline recovery still need their execution contracts.

## Stage 10 Partial Evidence (heads-up preflop rules)

Delivered: the preflop rules profile and its independent native regressions.

- `engine/include/bs/heads_up.hpp`: `Street::Preflop` appended last (Flop/Turn/
  River keep their historical values), `HeadsUpRoot::preflop` and
  `blinds_posted`, `HeadsUpState::big_blind_option_` and `all_in_`.
- `engine/src/poker/heads_up.cpp`: the posted-blind preflop root, button-first
  order, the big blind option across a limp, one-card-at-a-time flop dealing,
  live-blind and all-in settlement handling.
- `engine/tests/test_heads_up_preflop.cpp`: independent public-API regressions
  for button-first order, the option after a limp (raise and check endings), a
  button open removing the option, both fold endings, exact chip conservation,
  the short/all-in blind root with its board run-out, blind-post and
  flop-carrying-root rejection, the unchanged flop profile, and the measured
  preflop tree size (C(48,3) = 17,296 flops from a single line).

Zero regression on the existing profile: the Stage 1 exhaustive oracle still
reports 12,226 nodes / 2,852 folds / 3,002 showdowns, artifact split-run byte
identity holds, and the replay suite is unchanged at 156 decisions / 0 illegal
/ 0 JS fallbacks with mix {raise:32,check:48,fold:68,bet:5,call:3}.

NOT delivered, with the blocker recorded rather than assumed: preflop training
and policy-derived continuation ranges. A full traversal from a preflop root
does not converge inside the declared limits (a bounded walk visits over
3,000,000 nodes and more than 1,000,000 distinct postflop information sets
while the preflop street has two), so no convergence gate and no preflop
coverage is claimed. The bounded abstraction such a profile needs (a
flop-terminal subgame) brings a dedicated preflop size menu, which changes
`SizeSchedule` and therefore the artifact byte-accounting contract; that needs
its own accepted design. The live six-max preflop charts remain in force and
are not replaced by any heads-up model.

## Next Implementation Checkpoint

Stage 9 is implemented offline and gated; live enablement still requires
explicit authorization. Keep v0 contexts, production policy routing, and
external commands unchanged.

Before each later stage, define exact owned files and record which preceding
gate passed. Use independent bounded reviewers for algorithm, persistence,
protocol, and execution-safety changes. Preserve unrelated workspace edits.

## Required Verification

Every C++ change follows the entire `AGENTS.md` build contract:

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target benchmark-multistreet
npm run check
npm run proto:check
node bin/replay.mjs
```

Memory, arithmetic bounds, traversal lifetime, persistence, and framing
changes additionally run ASan/UBSan with the root `asan` preset.
Run Linux portable verification and new artifact dependency checks before
publishing those features. The existing three benchmark families remain
unchanged; new fixtures get independently versioned semantics.

Record commit, test results, fixture version, supported game/utility/ranges,
measured resource use, residual limitations, review dispositions, and rollback
evidence per stage. Any inability to run a required gate leaves that stage
unverified.

## Stop and Rollback Conditions

- Stop for invalid actions, inconsistent chips, private-information leakage,
  oracle disagreement, unexplained replay drift, or failed quality thresholds.
- Keep new strategy routing opt-in; disable it without changing v0 behavior.
- Keep prior artifact generations and refuse unknown revisions or mismatches.
- Retain v0 until its separate removal checkpoint; no automatic destructive
  cleanup is included in this plan.
- External process and operational fallback execution has no resolving
  certificate. Record the failure and clear per-hand eligibility.

## Remaining Gates

General multi-request/off-tree resolving and crash-safe external baseline
recovery need subsequent execution contracts. Expanded production utility
needs the v2 protocol RFC and a provider-data audit. Larger training needs
an explicit resource budget; live validation needs operating authorization.
A real second platform remains unselected. These gates do not block Stage 1,
and none is considered complete by approving this plan.
