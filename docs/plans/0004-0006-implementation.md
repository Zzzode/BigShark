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

## RFC 0008 Stage 2 Evidence (2026-09-21)

Stage 2 widens the one unified `GameState` from two seats to the full 2..10
range the RFC names: 3..6 seats now reproduce the shipped `MultiwayState`
exactly, and 7..10 seats ride the same rules with no shipped type to compare
against. The contribution ledger (`settle_contributions`) widens from 6 to 10
seats. No adapter forwards anything to either old type, so the widening is
verified by equivalence rather than by production calls -- the RFC's own gate
wording: "multiway test suite plus the settlement grid."

**Rules decisions made explicit.** The Stage 1 machine branched only for
heads-up; this stage states the multiway forms. Four rules genuinely differ by
profile and the seat-count branch picks each:

- **Refunded all-in.** At 3+ seats a refund RE-DERIVES `all_in` from the stack,
  so a seat whose all-in excess comes back is live again on the next street;
  only the two-seat profile preserves the flag (the capped-blind rule). The
  shipped `MultiwayState` already does this.
- **Mid-hand fold.** A fold in a 3+ game only marks the seat; the hand
  continues and the unmatched excess returns at the street close. Only the
  two-seat fold ends the hand and refunds immediately.
- **No big-blind option, no raise-rights clear at close.** A 3+ street closes
  when no seat owes an action (`any_pending`), not from a big-blind option
  (which does not exist), and `close_street` leaves raise rights as the street
  set them; the next street's `after_card` is what resets them.
- **Street close with one live seat.** At 3+ seats the last actionable seat,
  facing only all-in or folded opponents, still OWES its call/fold: the round
  closes only when there is no actionable seat at all. The two-seat rule closes
  the moment either seat is all in.

Two further shipped defects were found by the independent review lanes and
fixed, both in `MultiwayState` construction (the new machine already handled
both, so these are corrections to the 3..6 reference, not to the unified
rules):

- **Rooted-board all-in.** The rooted-board constructor did not re-derive
  `all_in` for a seat already all in from an earlier street (it derived it on
  the ante path but not the rooted path), so a zero-stack seat could be handed
  the action with a free check. Pinned by
  `test_rooted_zero_stack_seat_is_skipped`.
- **Preflop non-blind all-in.** On a plain no-ante preflop root, a NON-blind
  seat sitting on zero stack posts neither ante nor blind, so neither posting
  loop derived its `all_in`; it kept the default and the opener search handed
  it a phantom check. The rooted path (above) had the same shape and was fixed
  first; this is the preflop analogue the review proved with a differential
  `/tmp` driver (`MultiwayState` actor=0/all_in=0 vs `GameState`
  actor=1/all_in=1). Pinned by
  `test_preflop_zero_stack_nonblind_seat_is_skipped`; the lockstep ledger's
  `ref_start` derives it the same way, so the 3..6 sweep also reaches it.

**Validation shape.** `validate()` keeps one type across 2..10 with the two
profiles written out: two seats retain the strict heads-up contract (full
nominal blinds, no ante, equal closed-street contributions, flop-only rooted
board); 3..10 accept antes (capped to the stack), capped small AND big blinds
by post-ante stack, arbitrary per-seat rooted dead money, and 3/4/5-card
boards. Multiway roots select preflop from board emptiness (no separate flag
is declared -- the ledger never had one); blind fields on a rooted board must
be zero because those blinds are already in `contributions`.

**Equivalence oracles.**

- `test_game_definition_multiway` (new) is the stage-2 gate. It writes an
  independent N-seat ledger from the rules and from `MultiwayState`'s contract
  (the LEDGER CODE itself never includes `multiway.hpp`; only the lockstep
  harness below the ledger in the same TU does) at 3..6 seats it drives the
  real `MultiwayState` in lockstep with `GameState` at every node (fields,
  phase, actor, board, the full legal-action set, and both fold and showdown
  settlement), and at 7..10 it rides the same ledger. The walk enumerates
  every legal action at every node; to keep it bounded it holds exactly three
  ACTIVE seats per fixture (the width the shipped suite stays inside) -- extra
  seats post all in (a preflop ante shape) or carry a prior-street all-in (a
  rooted board), so they still traverse the ten-element arrays, the clockwise
  actor skip, and the multi-way pot layers without branching the action tree.
  Three seats rotate the cheapest fixture through every button position; 4..10
  ride two representative buttons. Non-vacuity counters pin mid-hand folds,
  hand-end folds, showdowns, ties, refunded-all-in revivals, short raises,
  capped blinds, antes, rooted roots, flop deals, and the 7..10 range itself.
- The 2-seat Stage 1 oracle's exhaustive walk is untouched and still green:
  the stage-2 change to `test_game_definition.cpp` is confined to the
  construction-rejection case (it now builds a valid 3-seat root and keeps the
  11-seat rejection); no heads-up enumeration moved.
- `test_settlement` keeps the bit-identical 2..6 exhaustive grid
  (435,000 accepted / 676,100 rejected) and adds a deterministic LCG
  extension over 7..10 seats (three fixed seeds, 400 trials each = 4,800):
  1,903 accepted cases compared per-seat award vector, rake, layer COUNT, and
  chip conservation against an independent ledger (the 2..6 grid alone compares
  full per-layer contents); the remainder are structurally rejected inputs the
  library also rejects. The ledger's seat constant lives in L0 as
  `kMaxContributionSeats` and is statically tied to `kMaxUnifiedSeats`.

**Two shipped defects fixed.** The rooted-board and preflop non-blind all-in
omissions above both changed shipped `MultiwayState`; the shipped multiway
rules the new machine targets were therefore not already correct. Both fixes
are in the old type itself, each has its own shipped-suite regression, and the
rooted case is additionally reached by the 3..6 lockstep oracle while the
preflop case is reached by the lockstep sweep and its ledger derivation.

**Independent-review hardening (post-commit 0294d72).** Three adversarial
review lanes (rules correctness, oracle/test rigor, evidence audit) drove four
test-side and reference fixes, none changing the unified rules:

- a **differentiated three-way river showdown** case
  (`differentiated_three_way_showdown_matches_shipped`, A/K/Q on the rainbow
  board 2c 3d 7h 9s Jd) sits BESIDE the bounded walk rather than changing its
  fixed club runout: that board `5c6c7c8c9c` plays for every fixed hand, so
  every contested showdown the walk reaches is an all-way tie and a mutant
  rotating live seats' hole cards went uncaught. The new case drives all three
  machines to a differentiated showdown; a rotate-holes mutant goes red on it
  while the all-tie sweep stays green, which is the empirical proof it bites.
- the legality sweep no longer compares the production
  `LegalActions::contains` against itself; an independent ledger predicate
  (`ref_accepts`) recomputes membership from the ledger fields, so a
  lower-bound off-by-one in `contains` is visible at 3..10 (verified red with
  a strict-`>` probe).
- an **anti-bypass** `CHECK` asserts the real `MultiwayState` is present
  exactly at 3..6 seats, so the lockstep cannot silently degrade to
  ledger-only.
- a **scripted five-seat preflop fold-down** reaches the unified
  `GameState`'s `settle_fold` at n>=5, which the bounded three-active fixtures
  structurally cannot (their extra seats are all in and stay live).
- the real `MultiwayState::legal()` is now compared structurally against the
  independent ledger at every lockstep node (fold/check/call, call amount, and
  the bet/raise interval), inside `compare_shipped`. Previously the shipped
  legality was observed only through the transitions it allowed, so a
  reported-only field like `call_amount` (which `contains` and every
  transition ignore) could drift silently; a `call_amount+1` probe is now red
  at the first action node.
- the one **equivalent** mutant is pinned by a structural `CHECK`
  (`!(folded && pending)` at every walked node), not by a case that merely
  stays green under it.

A fresh, separate re-review pass (three new adversarial agents that did not
author the change) re-derived every rule, re-ran the probes, and audited the
numbers: F1-F3/N1 and G1-G6 all RESOLVED with no blocking issue. The rules
lane additionally drove a differential the bounded walk cannot -- 6,000
all-seats-active random walks (65,513 nodes) plus 200 refund-revival hands at
3..6 -- with zero divergences. The audit lane caught one real reproducibility
defect (a clang-format reflow had stale-anchored the settlement mutant in the
mutation JSON); the anchor was re-synced and the battery re-run to 11/1.
Two accepted non-behavioral divergences are recorded below (F2, F3).

**Gate results (release).**

- full release `ctest`: 39/39 pass.
- new multiway oracle: 55,296,168 walked nodes over 3..10 (121,820 in the
  7..10 range); every non-vacuity counter positive; ~82s.
- settlement grid: 2..6 grid byte-identical; 7..10 extension 4,800 cases
  (1,903 accepted vs the independent ledger, rest structurally rejected).
- ASan/UBSan full preset: 37/37 pass. The exhaustive multiway oracle is too
  slow fully sanitized (measured ~23x slower per node instrumented, i.e.
  tens of minutes for the full walk), so under the sanitizer preset it
  compiles to a SUBSET (`BS_MULTIWAY_SANITIZED`): the full three-seat
  lockstep plus the ten-seat preflop-orbit, capped-blind, ante, zero-stack,
  and rooted sub-fixtures (112,240 nodes together), which still exercises the
  ten-element arrays and the new construction, action, and settlement paths
  under sanitizers; the release sweep is the full 55M-node combinatorial gate.
- mutation (`node dist/tools/mutation/verify.js --config tools/mutation/game-definition-stage2.json`
  after `npm run build:ts`; note `npm run mutation` hardcodes the stage-1
  config and does NOT take a config argument, 12 mutants): 11 caught / 1
  equivalent. The one equivalent drops the folded-seat check from
  `any_pending`; it cannot change behavior because a folded seat's `pending`
  is always already false (the constructor and `after_card` set pending as
  `!folded && !all_in`, and `after_action` clears the acting seat's pending
  before it marks the seat folded). The pin is that structural invariant,
  measured to hold at EVERY walked node (0 of 55,296,168 have folded==true and
  pending==true); it is NOT `fold_as_last_pending_action_closes_street`, which
  is green under the mutant precisely because its folding seat already has
  pending false. The caught set adds the rotated live-seat hole-card mapping
  (differentiated showdown) and the preflop non-blind all-in omission on top of
  the rooted all-in regression, the 7-player settlement bound, ante
  acceptance, mid-hand folding, refund semantics, and the street-close rule.

**Accepted non-behavioral divergences (rules review F2/F3/N1).**

- **F2 numeric-bound shape.** The unified `validate()` bounds the WHOLE hand as
  `pot + sum(all stacks) <= kMaxHeadsUpChips` (the heads-up form), while
  shipped `MultiwayState` bounds `pot() + each seat's stack` PER SEAT without
  accumulating. The two differ only for physically impossible chip totals
  (around the 2^53-1 ≈ 9e15 per-value cap aggregated across seats); at every
  reachable stack the per-seat bound is the looser one and the aggregate check
  dominates. Settlement also rejects any pot that cannot be expressed in its
  exact numeric profile, so no such root can reach a decision. Left as-is and
  recorded; tightening to a per-seat bound would weaken the heads-up equality
  the stage-1 oracle pins.
- **F3 `can_raise` outside the Action phase.** On an all-in runout the unified
  `after_card` resets raise rights before the close test, so a read-only
  `can_raise()` query in a non-Action phase can report a value the shipped
  heads-up type (whose `close_street` zeroes rights) does not. This is
  observability only: no transition reads rights outside Action, the legal
  action set is empty in those phases, and no settlement input changes. Inherited
  from Stage 1; recorded for the Stage 7 type-removal rather than patched now.
- **N1 fixed.** A rooted-path comment claimed a folded root seat was
  constructible; `GameDef` has no folded field, so none is. The comment now
  states that pending keys off the stack alone.

**Scope boundary.** This stage changes no protocol, no strategy/solver surface,
and no adapter. The old types remain the sole live callers of their own rules;
Stage 3 (abstraction) and Stage 7 (removal) are untouched.

## RFC 0008 Stage 3 Evidence (2026-09-21)

Stage 3 lands the L2 abstraction layer (§L2, rollout step 3): a new
`bigshark_abstraction` component that depends ONLY on `bigshark_poker`, holding
the declared ordered action menu and card bucketing, each behind an
`AbstractionId`. Nothing routes the live decision path through a new map; the
existing solver menu is lifted into the component and called through a thin
adapter, so decisions are unchanged (the gate: identity error zero and the
existing schedules reproduce their current decisions).

**What landed.**

- `engine/include/bs/abstraction.hpp`, `engine/src/abstraction/abstraction.cpp`
  (new CMake static target `bigshark_abstraction`, linked only to
  `bigshark_poker`; `bigshark_solver` gains a PUBLIC link to it).
  - **Action abstraction.** `Fraction`/`StreetSizes`/`SizeSchedule`/
    `default_size_schedule()` move here as the explicit identity action
    abstraction (`rfc0007-pot-fractions:v1`); the state-neutral ordered-menu
    rule is `build_action_menu(legal, street_sizes, MenuContext)`, which takes
    only `LegalActions`/`Street` plus chip scalars (no rules state type), so L2
    stays independent of both profiles. Fold/check/call, the legal-minimum and
    effective opponent-matching cap seeds, ceil pot fractions, clamp,
    sort/unique, and the 32-action cap are the exact shipped computation.
    `heads_up_solver.hpp` re-exports the types and `abstract_actions` is now an
    adapter that fills `MenuContext` from a `HeadsUpState`; `Fraction` layout is
    unchanged, so the persisted size rows, `same_size_schedule`, and the frozen
    `kGameCopyAccountingBytes=368` do not move.
  - **Card abstraction.** `Identity` buckets a holding by its seven-card
    evaluator score (strictly strength-order-preserving: zero merge of distinct
    strengths); `CategoryTiersV1` is the first declared lossy family (hand
    category 1..9). Bucketing takes the two hole cards contiguously after the
    3/4/5 public cards.
  - **Identity.** `AbstractionId{name,version,parameters,digest}` with a
    deterministic FNV-1a digest of the canonical parameters; it stores NO
    claimed error. `require_same_abstraction` throws a typed
    `abstraction_mismatch` on a policy trained under a different id -- the RFC
    0008 typed refusal. The id is in-memory only: RFC 0007 schema v2 (preflop
    persistence) has not landed, so this stage does NOT touch the frozen v1
    artifact DDL, digests, the 368-byte charge, or resident keys.
- Hard-coded preflop charts are deliberately NOT relocated: they remain a
  declared policy source, not an abstraction (§L2).

**Measured coverage.**

- `test_abstraction` links `bigshark_abstraction` (which brings only the rules
  layer; no solver/storage/IO symbol), so the dependency rule is a link error
  if violated: menu math incl. the short-opponent effective cap and a preflop
  opener whose declared fractions produce interior targets (8/10/14 between the
  minimum 4 and all-in 20), fraction validation and overflow (rejection of an
  unreduced or non-positive fraction at BOTH menu-build and id declaration),
  deterministic identity with reduced-form normalization (2/4 mints the same id
  as 1/2), a **golden identity digest** `0x422c245239c7a527` and exact pinned
  shipped preflop fractions `{{3,2},{2,1},{3,1}}`, the typed refusal, the
  3/4/5-card board fail-closed guard, the identity card bucket against the
  evaluator over the non-blocked combos of a board (1176 holdings -> 91
  identity buckets), and a nine-category CategoryTiersV1 witness (one hand per
  evaluator category 1..9) plus a published tier merge statistic (one flop:
  1176 holdings -> 91 identity vs 4 reachable tier buckets).
- `test_abstraction_equivalence` links the solver and walks every reachable
  state of bounded heads-up games comparing the lifted menu to shipped
  `abstract_actions` element-for-element, with **pinned** node counts (asserted,
  not printed). The flop-rooted trees (stacks 2/4/12) cover 2720 states under
  the identity schedule and 1384 under a non-default schedule (together the
  pre-stage-3 4104). A DEEP-stack (10 chips = 5 BB at 1/2) preflop root adds
  1156/908 states, 20 of them on `Street::Preflop` and, critically, 2 INTERIOR
  nodes whose fraction-derived target lands strictly inside the legal interval
  (8, with minimum 4 < 8 < cap 10). The interior count is the load-bearing pin:
  a shallow stack-4 preflop root used initially made every preflop interval
  all-in-only (minimum == cap == 4), so every fraction clamped to one seed and
  the Preflop=3 lists never influenced a compared menu -- that vacuous coverage
  was found by the second independent review and replaced. Zero menu
  differences. Equal menus mean the solver builds the identical abstract game,
  so the identity abstraction's exact exploitability error is zero on these
  enumerable fixtures by construction; the pinned `test_heads_up_solver`
  sizing vectors and the replay suite cover the end-to-end decisions.
- The same-schedule equivalence harness structurally cannot detect a wrong
  shipped DEFAULT (both sides receive the same schedule), so a golden identity
  digest plus the exact preflop fraction assertion in `test_abstraction` pin
  the default independently: changing `preflop.bets 3/2 -> 1/1` was confirmed to
  fail BOTH gates (digest mismatch, and the preflop tree shifting 1156 -> 1192).
- The 7..10-seat estimated-NashConv regime (labeled estimate vs declared
  reference opponents) is NOT built here: no multi-seat best-response
  evaluator exists yet and it belongs to the stage-6 measured-policy gate. The
  lossy card family's large-table error is therefore not yet measured; it ships
  only as a declared, currently-unwired bucket. The Identity card bucket's
  zero-error claim is scoped to river-terminal fixtures: on the flop/turn two
  holdings can share a current made-hand score while differing in runout equity
  (draws), and the parameters string records `lossless=river-terminal`.

**Independent review and hardening.** Two adversarial review passes were run by
agents that did not author the change. The first confirmed the menu lift is
element-identical (an independent 849,692-state harness plus 3,000,000 fuzz
vectors, zero mismatches) and the frozen layout (Fraction 16 B, StreetSizes
48 B, SizeSchedule 192 B, HeadsUpGame 352 B; the 368-byte charge intact); its
findings drove: exact per-root CHECKs in place of a printed node total; a
preflop root and a preflop-safe Deal index; removal of a dead
`MenuContext.call_amount`; fail-closed `invalid_argument` for the >32 and
board-size guards; reduced-form id normalization; river-terminal lossless
scoping; and a nine-category lossy-tier witness. The second pass (read-only,
no mutation run) confirmed three defects, all fixed: (1) **major** -- the
initial shallow preflop root made the Preflop=3 fraction coverage vacuous,
fixed with a deep-stack root, the interior-node pin, the state-neutral preflop
opener case, and a golden identity digest; (2)/(3) **minor** --
`canonical_schedule` reduced `std::gcd(0,0)==0` and so dividing by it was
integer UB at ActionAbstraction declaration (ahead of menu-build validation),
fixed by validating positivity in a new `canonical_fraction` helper so a
`{0,0}`/`{n,0}` fraction throws `invalid_argument` at declaration. A
card-id-range note and an ephemeral-harness citation were independently
refuted (the former is the evaluator's existing trusted-input precondition; the
latter is correctly attributed to the review pass) and left unchanged.

**Mutation verification.** `node dist/tools/mutation/verify.js --config
tools/mutation/abstraction-stage3.json` (15 mutants, 2 targets): **15 caught / 0
equivalent**. The first battery run was 8/9: the effective-cap mutant survived
because no fixture made the opponent unable to match the legal maximum; a
short-opponent case was added and the mutant now goes red. The battery now also
pins the canonical gcd normalization, the declaration-time zero guard (a
deterministic invalid_argument mutant, not a UB crash), and the shipped
preflop default (3/2 -> 1/1, the reviewer's exact attack), alongside the
cap/minimum/base/clamp/ceil menu semantics, the contiguous-hole-card bucket
layout (a real bug the unit test caught -- fixed slots 5/6 read zero padding
on the flop), constant-digest identity, the typed refusal, the board guard,
the tier mask, and the shipped solver sizing vector through the adapter.

**Gate results (release).**

- full release `ctest`: 41/41 (two new tests; all solver/resident/artifact/v1
  suites unchanged).
- replay: 156 decisions / 0 illegal / 0 JS fallbacks, action mix identical.
- npm run check 94/94; proto:check 16/16; check-docs and check-rfcs green.
- ASan/UBSan affected targets: `abstraction`, `abstraction_equivalence`,
  `heads_up_solver`, `heads_up_solver_sampled`, `heads_up_allocations` pass
  (the zero-fraction declaration test runs clean under UBSan); the menu is
  heap-light and the id is declaration-time, so no rules hot-path allocation
  changed.
- mutation 15/15.

**Residual limitations / scope boundary.** No protocol, storage, live routing,
or strategy change; the lifted menu executes on the live solver path but
produces byte-for-byte identical decisions, and the card buckets/id are not yet
consumed outside tests. Persisting `AbstractionId` waits for the RFC 0007
schema-major v2 work; the abstract tree (L3), unified solver interface (L4),
guarantee levels/decision service (L5/L6), and the measured >6/=10 policy
(stage 6) are untouched.

## RFC 0008 Stage 4 Design Brief (2026-09-21, revised after independent review 2026-09-22)

Goal (RFC rollout step 4): introduce the shared L3 abstract tree and the L4
unified `solve`, and bring the existing solvers behind it. Gate (verification
plan, RFC 0008:525-527): every existing solver is **reachable through `solve`**,
**refuses unsupported tree shapes explicitly rather than approximating**, and a
solver whose model IS the identity tree **reproduces its previous results
bit-for-bit under the identity abstraction**.

### What the read-only maps and the adversarial review established

- The heads-up multistreet CFR has no materialized tree: it re-walks
  `HeadsUpState` by value and its pins are FP-exact (`==` policies, SplitMix64
  golden vectors, hand-derived regret literals, 24 infosets). Rerouting its
  numerics through a newly allocated graph would change FP summation order and
  break bit-for-bit even when mathematically equivalent.
- Every FP-exact pinned fixture is a CONDITIONED game (`HeadsUpGame.fixed_runout`,
  e.g. `{Js,9c}`); there is no fully free-runout golden fixture. GameDef has no
  runout field (runout is documented as non-root identity "composed by its
  owner", game_definition.hpp:133-139). So the request, not the tree, must
  carry conditioning, or `solve()` cannot express the games whose numbers are
  pinned.
- The river LP/DCFR game CANNOT be an identity L3 tree: a 2-seat GameDef root
  must be a 3-card flop (`game_definition.cpp:98` rejects a river root at two
  seats), and the six-node toy uses fractional float geometry (`a=pot/2`,
  `B=0.75*pot`, one bet/one raise, analytic double payoffs) with none of the
  identity schedule's integer min/all-in seeds. Claiming it "maps onto an
  identity tree" (v1 of this brief) was wrong.
- L2's menu context is single-opponent; multiway `legal()` is existential over
  all opponents with the actor's own all-in as maximum (multiway.cpp:210-231)
  and has no pot-fraction cover rule. A 3-seat cover cap must be declared, not
  silently chosen as "the next actor".

### Decision

**1. L3 `bigshark_tree` (new static lib; depends ONLY on bigshark_poker +
bigshark_abstraction; never on a solver).** `AbstractTree` OWNS a copy of its
source `GameDef` and `ActionAbstraction` (values, not references) and exposes
their identities: `const GameDef& def()`, `const AbstractionId& action_id()`,
digest. Nodes live in one `std::vector<Node>` built with capacity reserved up
front and addressed ONLY by stable integer index (parent/children are indices;
no pointer/Node& retained across a push_back). Each node carries a stable
**public-path id**: the FULL ordered public prefix from the root (not
street-scoped — a per-street id would collide across streets) of `{actor seat,
action type, target total}` on action edges and the dealt card id on chance
edges — exactly the public-history prefix the current `InformationKey`
generalizes (it already spans the whole hand, heads_up_solver.cpp:567-584).
Node kinds:

- `Action` — actor seat, street, legal actions, children = the L2 menu in order,
  each edge labeled with its concrete `Action` (type + target total).
- `Chance` — the flop is THREE sequential single-card Chance nodes (board sizes
  0->1->2->3 asserted), then one per turn/river. Chance edges are CONCRETE card
  ids over the UNCONDITIONED public set `{c in 0..51 not on the board}` and
  carry NO probability: per-deal conditioning (subtracting that deal's four hole
  cards and runout reservations) and uniform `1/k` mass stay in L4, exactly as
  `heads_up_solver.cpp:140-162` does today. Concrete ids now; a future public
  card abstraction wraps on the L4 side, so L3 does not bake in a bucket scheme.
- `TerminalFold` — carries the settled `chip_utility` VECTOR (one per seat) plus
  the live-seat set; the fold payoff needs no holdings and is checkable now.
- `Showdown` — carries the terminal ledger snapshot for ALL N seats: per-seat
  contributed/refunded/stack with a folded flag (folded seats' gross
  contributions are required to build N-way side pots, not only the live
  prefix), the live seats in ASCENDING order (the order `settle_showdown`
  packs holdings), and the five-card board; the payout vector is evaluated
  L4-side only once concrete holdings are supplied.

L3 is hole-card-free by construction: no range data, no private-chance nodes,
no holdings evaluator (a link-negative test asserts L3 never references the
transitional state/root types or range/holdings-eval symbols). The guard must
be TOKEN/SYMBOL based, not a raw `#include` ban: `game_definition.hpp` itself
transitively includes `bs/heads_up.hpp` for the shared `Action`/`LegalActions`
types, so L3 sources may receive those headers indirectly but must not NAME
`HeadsUpState`/`HeadsUpRoot` or solve via a two-seat state — only
`GameDef`/`GameState`. Runtime N, no `[2]`, no `1-actor`.

**2. Multiway cover cap — a declared L2 extension, done before 3-seat menus are
pinned.** L2 gains a neutral menu entry for the existential multiway rule: the
caller supplies an explicit `cover = min(legal.maximum, max over every other
LIVE, non-folded seat of street_committed + stack)` computed from GameState;
fractions clamp to `[minimum, max(bounds.minimum, cover)]`, otherwise identical
to the heads-up rule. The 2-seat path is unchanged (its single opponent IS the
max). A unit test pins a fixture where the next seat is short but a third seat
can cover, so picking "the next actor" would clamp wrongly. This is additive L2
work inside stage 4; the identity schedule and digest for the 2-seat path do not
move.

**3. Independent fidelity oracle (the load-bearing, non-circular gate).** A
test-local reference enumerator that includes NEITHER the L3 builder header NOR
`ActionAbstraction::menu` (the stage-1 independent-oracle pattern). From raw
`GameState::legal()` it independently codes the fold/check/call prefix, the
fraction/ceil/clamp math, min/cap seeds, sort/unique, and (for 3 seats) the
max-cover cap; classifies node kind/actor/street itself; computes chance sets
independently as `0..51 minus board`; and evaluates terminal payout vectors
through GameState settlement. It drives the reference and the built tree in
lockstep over bounded fixtures at BOTH 2 and 3 seats (preflop + rooted flop;
shallow and deep stacks; mid-hand folds; an all-in runout; asymmetric stacks)
and asserts, as exact CHECKs with pinned counts: node id/kind/actor/street,
every menu element-for-element, every chance set, terminal classification, AND
fold `chip_utility` vectors element-for-element plus showdown ledgers against
holdings the oracle deals independently. At 2 seats it additionally asserts
menus equal shipped `abstract_actions`. Chance-set coverage includes the
partial-flop board sizes 0/1/2. A separate L4-side check asserts the trainer's
per-deal conditioned chance set equals the L3 public set minus that deal's
holdings and runout reservations.

**4. L4 `solve` surface (in bigshark_solver, which links bigshark_tree).**

- `SolveRequest{ const AbstractTree& tree; Ranges ranges; SolveMode mode;
  SolveLimits limits; std::uint64_t seed; RunoutConditioning runout; }`.
  - `Ranges`: one weighted-holding vector PER SEAT over runtime N (reuse the
    WeightedHand family), explicitly validated; joint mutually-compatible deal
    enumeration and normalization are L4-owned (the lifted `joint_deals`), not
    L3.
  - `SolveMode { FullTraversal, ExternalSampling }` selects `train()` vs
    `train_sampled()`; both are conformance-gated. `SolveLimits` maps the
    trainer's five caps (nodes/infosets/depth/bytes/time) so ResourceLimit is
    part of the reproduced behavior.
  - `RunoutConditioning`: declared optional per-street fixed turn/river (the
    `fixed_runout` owner slot), with its own composed identity. `nullopt` is the
    free runout. This is what makes the shipped golden games requestable.
- `SolveResult` returns a MOVE-ONLY concrete result wrapping the existing
  `HeadsUpTrainingResult` (status, completed iterations, node/infoset/byte
  counters, seed/prng_state, and `HeadsUpPolicy` moved with zero per-row
  copies) plus the tree's `AbstractionId` and composed runout identity. No
  polymorphic base is added to `HeadsUpPolicy`; its map is never copied at the
  boundary; a conformance test scopes the `test_heads_up_allocations`
  operator-new fault window to the TRAIN phase only (request pieces and the
  tree are built outside the window, exactly as the existing test constructs
  the trainer before opening the window), so the train-time allocation ordinal
  is unchanged while tree-build and range-copy allocations sit outside it. No closed `Guarantee` enum is defined in L4 — the
  normative source->level mapping is L6 and lands in stage 5; stage 4 reports
  only solver status/cost.
- Typed refusals are NEW exception types that do NOT derive from
  `std::invalid_argument` (so host invalid-argument handlers cannot misclassify
  an expected solver-selection refusal): `unsupported_tree_shape` and
  `tree_resource_exhausted`.

**5. Heads-up adapter = zero-numeric-change dispatch, proven against the
existing golden fixtures, not a same-call-twice comparison.** The adapter
accepts only `player_count == 2` with the tree's identity action schedule
(compared by `AbstractionId`, never by re-calling `default_size_schedule()`),
projects the validated GameDef to a `HeadsUpRoot` by field copy (including the
three `-1` flop sentinels, both blinds, stacks-before-blinds), and calls the
UNCHANGED `HeadsUpTrainer`. Conformance:

- the direct-call side builds its `HeadsUpGame` from an INDEPENDENT root literal
  and ranges asserted `same_game_def`-equal (never pointer-identical) to the
  tree's def;
- it re-runs the EXISTING pinned fixtures through `solve()` — the 24-infoset
  one-iteration policy, all three runout variants `{Js,9c}/{Js,null}/{null,9c}`,
  and BOTH modes — and asserts the committed golden literals, raw `==` policy
  doubles, and the SplitMix64/prng_state streams; plus negative tests (n==3, a
  lossy/non-identity action id, a malformed projection) each throw the typed
  refusal.
- the existing `{20,20}`/`{100,100}` DEEP-STACK shapes are menu/projection-only
  (test_heads_up_solver.cpp:367-385 pins menus, not a trained policy), and
  materializing the full unconditioned deep tree is out of budget; they are
  asserted via the L3 oracle's menu equality and the projection round-trip, NOT
  counted as trained bit-for-bit goldens. Every TRAINED golden uses 1-2 chips
  behind, whose full unconditioned trees are small enough to build.
- the short-big-blind deal-out (`{2,10}`, nominal blinds, see below) is added
  to the solve()-route conformance set as a zero-action rules/payout fixture
  (no trained policy is pinned on it): the projected game reaches the same
  refunded-but-still-all-in state and deal-out showdown.

Representability was checked empirically, not assumed (an earlier draft of this
brief got it WRONG and an independent reviewer caught it): at two seats BOTH
`GameDef` and the legacy `HeadsUpState` require the big blind to post its full
NOMINAL amount (`game_definition.cpp:89-90`, `heads_up.cpp:60-61`); there is no
under-nominal shape accepted by one and rejected by the other. The shipped
"short big blind" fixture (`test_heads_up_preflop.cpp:207-240`, stacks
`{2,10}`, pot 2, nominal blinds `{1,2}`, the BB seat posting its whole stack
exactly equal to nominal) DOES construct a valid `GameDef` — verified: it opens
directly in `Deal`, seat 0 is all-in with one chip of uncalled blind refunded,
commitments level at 1 — and the stage-1 sweep already pins it through
`GameState` (`test_game_definition.cpp:819-825,858-861`). An under-nominal
post (BB posts 1 when nominal is 2) is rejected by BOTH profiles. So the full
two-seat representable set — full-nominal preflop (including a zero-action
all-in blind deal-out) and complete-flop roots — is in scope for the route; no
two-seat shape is carved out to stage 7 on representability grounds.

**6. Every other existing solver is reachable as a registered REFUSE-ONLY
adapter (never "not routed").** River LP, river DCFR, and the experimental
multistreet trainer each have an adapter that `solve()` dispatches to and that
throws `unsupported_tree_shape` for every L3 tree (their models are not L1
games / not identity trees this stage); a conformance test asserts each typed
refusal, and their bit-for-bit numbers remain gated on their UNCHANGED direct
entry points. The resolver is documented as the deliberately separate RFC 0005
subgame/blueprint interface with a different request shape (it is not a policy
producer over an L3 tree) and likewise refuses the L4 request; `multiway_sampler`
is documented as a sampling utility, not a policy solver. This satisfies
"reachable + explicitly refuses rather than approximating"; their actual numeric
ports are later stages that declare a dedicated abstraction/river model.

**7. Build boundaries and a time-boxed transition exception.** `bigshark_tree`
links only poker+abstraction and has a link-negative test (linking without the
solver; an include guard forbidding solver/state-root/range-eval headers). The
`HeadsUpTrainer` still calls L2 `build_action_menu` directly from stage 3; fully
moving L4 off L2 requires the stage-7 trainer rewrite (which would perturb FP),
so the L4->L2 edge is recorded as a deliberate transition exception EXPIRING at
stage 7, with an architecture test that fails once the replacement trainer can
source menus from the tree. No L3 edge is added.

**8. `TreeLimits`.** Node/depth/byte caps (no time cap on deterministic
construction), each checked BEFORE the corresponding allocation using the
overflow-safe budget pattern; a bound throws `tree_resource_exhausted`
(distinct from unsupported shape and from `std::bad_alloc`) with strong
exception safety (a failed build yields no tree). A refusal test hits each cap;
the measured full-expansion node count of the smallest representable game is
published.

### Scope boundary

L5 storage identity, L6 normative guarantee levels/wire disposition, the
public-card lossy abstraction, and the measured >6/=10 policy (stage 6) are not
touched. The resolver internals, river LP/DCFR numerics, and the experimental
multistreet trainer are unchanged. Stage 4 produces the seat-count-agnostic
public game tree, the typed solver seam, and the bit-for-bit heads-up route,
and makes every other solver a reachable, explicitly-refusing adapter.


### Stage 4 implementation status (complete, gate evidence pre-commit)

All six TODOs above are done on disk; the stage awaits the single clean commit
after independent re-review.

- L2 additive multiway cover cap: `MultiwayMenuContext{cover}` +
  `build_multiway_action_menu`; the 2-seat `build_action_menu` is unchanged
  (shared core, cap math identical). Unit test `multiway_cover_cap` pins
  deep-vs-short cover and heads-up/multiway agreement. Identity digest unmoved.
- L3 `bigshark_tree` static lib (links only poker+abstraction):
  `engine/include/bs/abstract_tree.hpp`, `engine/src/tree/abstract_tree.cpp`.
  Index-only nodes, explicit-stack DFS, per-leaf TerminalPayload (fold carries
  the exact settle_fold vector; showdown carries the ledger incl. folded
  seats), probability-free chance edges, deterministic TreeLimits checked
  before allocation, `tree_resource_exhausted` distinct from invalid_argument.
- L4 `engine/include/bs/solve.hpp`, `engine/src/gto/solve.cpp`: move-only
  SolveResult, SolveMode full/sampled, RunoutConditioning on the request,
  field-copy GameDef->HeadsUpRoot (incl. -1 preflop sentinels and the {2,10}
  nominal-BB deal-out), typed unsupported_tree_shape. River LP/DCFR and the
  experimental multistreet CFR are reachable and refuse-only; Auto routes only
  the 2-seat identity tree.
- Gates: independent fidelity oracle `test_abstract_tree_fidelity` (own
  enumerator, never calls the builder or L2 menu) pins at 2 seats
  {1,1}: 31124 nodes (A9608/C248/F4804/S16464) and 3 seats
  {1,1,1}: 102827, {1,1,2}: 162316, and the deepest-cover fixture
  {2,3,3}: 547535 (A216752/C3309/F35826/S291648, depth 15). The {2,3,3}
  shape was chosen after a reachability probe proved shallower asymmetric
  shapes do not distinguish first-cover from max-cover (the minimum-raise
  floor masks it). `test_solve_conformance` bit-for-bit pins both drivers over
  all three fixed-runout variants incl. the SplitMix64 PRNG state, plus every
  typed refusal and the short-blind projection. `test_tree_link_negative`
  links the tree without the solver; an unconditional build-time include-
  allowlist + forbidden-identifier guard covers both L3 files.
- Mutation battery `tools/mutation/abstract-tree-stage4.json`: 20 mutants /
  3 targets, 20 caught, 0 equivalent, 0 gap.

#### Independent adversarial review (two rounds) and fixes

Three fresh reviewers (L3/L2, L4 seam, boundary+rigor) returned
CHANGES-REQUESTED. Confirmed findings fixed and re-verified:

- BLOCKER byte cap did not bound retained memory (children/actions/payout
  slabs and vector-growth slack uncharged; preflop build ran ~1.8x the cap).
  Reworked accounting so every retained slab is grown through a budgeted
  `grow_to_fit` that charges the whole doubled capacity before `reserve`;
  `accounted_bytes()` now equals requested retained capacity exactly (pinned on
  the 3-seat fixture) and the preflop build aborts at ~0.92 GiB peak RSS under
  the 1 GiB cap. Allocator rounding (~1-2% physical overshoot) documented as
  out of portable scope.
- MAJOR boundary guard gaps: the first attempt (configure-time single-file
  regex) and its build-time successor were both shown by two independent
  adversarial reviewers to be beatable by the preprocessor (dotted
  bs/./eval.hpp, `evaluate (` spacing, char-literal flanking, then
  macro-##-pasted includes/identifiers, backslash-newline splice, digraph
  %:include, no-space #include<...>, #import, all compiling and emitting the
  inline weak evaluator symbol past the link-negative test). Final control:
  `engine/cmake/l3_boundary_guard.sh` runs the real compiler in dependency mode
  (-MMD -MF) on each L3 source and allowlists the RESOLVED post-preprocessor
  engine-header closure (only L1 game_definition/heads_up/settlement, L2
  abstraction, and its own header). Every demonstrated spelling now fails while
  the real pair and standard/L1/L2 includes pass; unconditional, not BUILD_TESTING
  gated. A repo-local shadow header can only enter the non-globbed target via
  the reviewed CMakeLists source list (accepted residual).
- MAJOR runout collisions leaked the trainer's invalid_argument: L4 now
  validates fixed cards (range, distinct, off board, off both seats' hole
  cards) with unsupported_tree_shape, distinguishing a PRESENT negative id from
  nullopt via has_value() (a present -1 was the one residual the second review
  caught). Preflop conditioning is refused with the typed error (defense in
  depth; a preflop public tree is unmaterializable).
- MAJOR preflop projection arm untested and unreachable end-to-end: extracted
  the pure GameDef->HeadsUpRoot projection to
  `include/bs/detail/solve_projection.hpp` + `src/gto/solve_projection.cpp` and
  unit-tested BOTH arms directly against the legacy HeadsUpState (the {2,10}
  deal-out: Deal phase, no actor, all-in, refund 1, street_committed 1).
- MAJOR byte/depth caps and preflop exhaustion claimed but untested: added
  fidelity cases for depth (cap 4 vs depth-15 tree), byte (1024 B), and
  preflop (pinned 52*51*50*49*48 = 311,875,200 river leaves vs the 2,000,000
  default node cap; small cap exercises the typed throw).
- MINOR: unreduced schedule fractions (2/4) minted the identity id yet reached
  the trainer and threw; ActionAbstraction now reduces to lowest terms at
  declaration (`reduced()`), identity digest unchanged. Showdown ledger test
  now asserts refunded and the folded flag. Sampled conformance covers all
  three runout variants, every result field incl. information_sets/
  accounted_bytes/prng_state, and a repeatability re-solve. Wrong range count
  (1 and 3), explicit-HeadsUpCfr-3-seat, and present-negative fixed-card
  refusals added. Removed the default SolveResult ctor (self-reported identity)
  and dead includes; removed the tautological same_game_def(def,def).

Final matrix after the fixes: release ctest 44/44 (incl. benchmarks);
ASan/UBSan clean on all new and touched targets and the non-benchmark ASan
preset (42/42); mutation battery 20/20; replay 156/0/0; npm check 94/94,
proto:check 16/16, check-docs 250, check-rfcs 8 green.



## RFC 0008 Stage 5 Design Brief (2026-09-22)

Goal (RFC rollout step 5): source-based routing with the typed guarantee level
on every decision. Gates (RFC verification plan): "every decision path reports
the level its source warrants; a test asserts a weaker level is never reported
as a stronger one, and that a request for a stronger level than available is
typed", plus the replay suite with UNCHANGED decisions (156 / 0 illegal /
0 JS fallbacks).

### What the read-only maps established (facts that constrain the design)

- Domain routing is `bs::evaluatePolicy(Ctx)` (engine/src/policy/decision.cpp:
  461-473): no-hole-cards fold, `preflop()` on street, `riverGto()` on a
  river+GTO-enabled spot (LP exact / DCFR bounded), else `postflop()`. It
  returns a bare `Decision{action,amount,reason,equity,mdf}` with NO source.
  `bs::decide` (bigshark_service, src/service/decision_service.cpp) is a
  pass-through.
- The wire source is INFERRED FROM TEXT afterward:
  `solverSource(reason)` (v1_response_mapper.cpp:30-42) prefix-matches
  "gto-cfr"/"gto"/12 chart prefixes, default POSTFLOP_HEURISTIC. Source is
  otherwise set only for blueprint/resolved rows in the minor-1 path.
- `SolverMetadata.guarantee` (field 10) is a closed string
  {modeled_exact_bound, uncertified, baseline}, present ONLY on minor-1
  ExpandedStrategy rows (blueprint/resolved), pair-discipline enforced in
  mapResolvedExpandedResponse. Minor-0 and minor-1 heuristic responses never
  set it (test pins `!has_guarantee()`).
- Minor negotiation is per-frame `Envelope.protocol_minor`; host accepts
  <=1, rejects >1 with UNSUPPORTED_PROTOCOL; the validator receives the
  negotiated minor and gates the minor-1-only solver modes; the write side is
  gated structurally (separate dispatch/mappers per minor). TS negotiates 1
  only behind BIGSHARK_ENGINE_PROTO_MINOR1=1 and probes gracefully.
- Live reachability of sources: PREFLOP_CHART, RIVER_LP (HiGHS build),
  RIVER_DCFR (non-HiGHS / bounded path), POSTFLOP_HEURISTIC, BLUEPRINT,
  RESOLVING (certified / deadline-blueprint). MULTISTREET_CFR is offline-only
  and never produced on the decision path. On a validated v1 request the
  heuristic ALWAYS answers, so no engine v1 response today is a no-source
  fallback; the genuine operational fallback is the TS `safeFallback` when the
  engine is unreachable and the engine-host JSON-mode parse-error fold.
- Freeze surface: minor-0 v1 bytes are hex-pinned (kStage7Hex); minor-1
  behavior is pinned by test_v1_minor1/test_v1_resolving and TS golden/walk
  suites; proto/baseline/v1.binpb is the Buf FILE baseline (minor 1 already
  added enum values 6/7 against it, so additive enum values are an established
  non-breaking change).

### Decision 1: the L6 core is protocol-free and lives in bigshark_service

New header `engine/include/bs/guarantee.hpp` (+ src/service/guarantee.cpp):

- `enum class Guarantee { OperationalFallback, Approximate, AbstractSolved,
  ExactSolved, CertifiedBound }` declared in ASCENDING strength;
  `int guaranteeRank(Guarantee)` (0..4) and
  `bool guaranteeMeets(Guarantee achieved, Guarantee floor)`.
- `enum class DecisionSource` is added to `engine/include/bs/decision.hpp`
  (the policy layer must be able to NAME the source it selected):
  PreflopChart, PostflopHeuristic, RiverLp, RiverDcfr, MultistreetCfr,
  Blueprint, Resolving.
- Normative mapping in guarantee.cpp, one place, an EXHAUSTIVE switch
  (-Wswitch, no default label so a new enumerator fails the build):
  Resolving+certified -> CertifiedBound; Blueprint -> Approximate;
  PreflopChart/PostflopHeuristic/RiverLp/RiverDcfr/MultistreetCfr ->
  Approximate; Resolving without certification -> Approximate (total mapping;
  today the deadline answer is emitted under the Blueprint source, so this arm
  exists for completeness). ExactSolved and AbstractSolved have NO source
  today - a test pins that no mapping arm returns them, so promoting a source
  is a deliberate, reviewed change (stage 6 is what can earn
  AbstractSolved).
- `const char* guarantee_token(Guarantee)` returns the five canonical L6
  spellings ("operational_fallback", "approximate", "abstract_solved",
  "exact_solved", "certified_bound"). The boundary fail-closed rule is a
  real function: `Guarantee guaranteeForWireSource(int wire_source, bool
  certified)` maps the seven known pv values via the core table and maps
  UNSPECIFIED / any out-of-range value to OperationalFallback (the RFC: "a
  source that is not listed ... fails closed to operational_fallback rather
  than being assigned a level by default"). It lives in guarantee.cpp as an
  int-taking function so the rule itself is protobuf-free and unit-testable;
  the mapper forwards the pv enum's integer.

### Decision 2: source is DECLARED at the routing branch, never inferred

- decision.cpp gains an internal `PolicyAnswer { Decision decision;
  DecisionSource source; }`; each branch tags itself (preflop chart, river
  exact LP, river bounded DCFR, postflop heuristic including the no-hole-cards
  fold, which the text inference today classifies as POSTFLOP_HEURISTIC).
- New public `evaluatePolicySourced(Ctx)` in policy.hpp and
  `decideSourced(Ctx)` in service.hpp returning
  `SourcedDecision { Decision decision; DecisionSource source; }`. The
  cascade is written as one explicit ordered preference sequence for
  AUTOMATIC (the RFC's "explicit preference order"): resident storage
  sources are attempted first by the envelope (unchanged), then within the
  domain policy: river LP/DCFR (river, eligible) > preflop chart (preflop)
  > postflop heuristic catch-all. Behavior is identical; the chosen step now
  EMITS the source.
- `evaluatePolicy`/`decide` remain thin wrappers returning `.decision`; the
  v0 path and minor-0 path are untouched byte-for-byte.
- An INDEPENDENT provenance test re-codes the old reason-prefix inference
  (its own prefix table, never calling the mapper) and asserts the declared
  source agrees for fixtures exercising every branch; this is the
  non-circular guard that the refactor cannot silently relabel a decision.

### Decision 3: wire disposition = negotiated MINOR 2 on the v1 package

Recorded resolution of the RFC open question: a new minor, not
bigshark.engine.v2. Rationale: RFC 0006 reserves v2 for coherent full-state
support, which does not exist yet; the change is one additive enum + two
additive fields, the exact shape minor 1 established; and the RFC's two
immovable constraints (minor-0 bytes frozen; minor-1 peers keep exactly
modeled_exact_bound | uncertified | baseline with unchanged meaning) are
satisfied structurally.

Proto changes (all additive; proto/baseline FILE check stays green):

- New enum
  `enum GuaranteeLevel { GUARANTEE_LEVEL_UNSPECIFIED=0;
  GUARANTEE_LEVEL_OPERATIONAL_FALLBACK=1; GUARANTEE_LEVEL_APPROXIMATE=2;
  GUARANTEE_LEVEL_ABSTRACT_SOLVED=3; GUARANTEE_LEVEL_EXACT_SOLVED=4;
  GUARANTEE_LEVEL_CERTIFIED_BOUND=5; }` numeric order = strength.
- `DecisionOptions.minimum_guarantee = 8` (optional GuaranteeLevel),
  negotiated-minor-2 only; the validator rejects it on minor 0/1 exactly like
  the minor-1 mode gate. UNSPECIFIED/absent = accept any level.
- `SolverMetadata.guarantee_level = 11` (optional string, buf in: the five
  canonical tokens), negotiated-minor-2 only. It is a string (not the enum)
  to match the field-10 vocabulary style and journal-friendliness; the enum
  is the request-side vocabulary.
- `ErrorCode.ERROR_CODE_GUARANTEE_BELOW_REQUEST = 9` (next free),
  non-retryable (retrying an unchanged state cannot help), with a
  messageFor entry.
- Field 10 is NOT widened. Per-minor vocabulary is disjoint and enforced by
  separate mappers as today: minor 0 sets neither field; minor 1 sets field
  10 with exactly the legacy three tokens and never field 11; minor 2 sets
  field 11 on EVERY decision (heuristic included) and never field 10. The
  semantic bridge certified_bound <-> modeled_exact_bound (same meaning,
  different token per minor) is documented in the proto comment and pinned by
  a test; minor-1 "uncertified"/"baseline" both correspond to core
  Approximate.
- Host accepts minors 0..2; >2 stays UNSUPPORTED_PROTOCOL. Capabilities at
  minor 2 advertise [0,1,2] and build version "bigshark-engine-v1.2.0"; the
  minor-0 and minor-1 capability bytes are unchanged (kStage7Hex plus the
  existing minor-1 golden).

### Decision 4: floor enforcement is response-side and never alters the action

handleMinor1Decision is left exactly as is; a parallel
handleMinor2Decision shares its low-level tryBlueprint/tryResolving helpers
but:

1. obtains the best available answer via the SAME mode routing
   (AUTOMATIC: storage first, heuristic cascade; forced BLUEPRINT/RESOLVING:
   unchanged coverage-miss/deadline errors);
2. computes the core Guarantee of the answer (declared source for heuristic;
   certified flag for resolving; Approximate for blueprint/deadline);
3. if `minimum_guarantee` is present and guaranteeMeets(level, floor) is
   false, responds ERROR_CODE_GUARANTEE_BELOW_REQUEST INSTEAD of serving the
   weaker decision (the RFC: report, do not silently downgrade; the host does
   not validate what the caller claimed and provenance rules are unchanged);
4. maps the response through new minor-2 mapper entry points that always set
   field 11, keep artifact_sha256 (field 9), and never set field 10.

With all live sources at Approximate (or CertifiedBound via a certified
resolve), floors at exact_solved/abstract_solved are typed refusals today;
that is the honest current state and the mechanism precedes promotion.

### Decision 5: operational_fallback surface

- Engine core defines and tests the level and the unknown-source fail-closed
  mapping (UNSPECIFIED and an out-of-range cast both -> OperationalFallback).
- The TS `safeFallback` (platforms/river-club/src/engine.ts), which is the
  live "no strategy source available" path when the engine binary/transport
  is unavailable, labels its local decision guarantee `operational_fallback`
  in the platform metadata surface.
- Evidence states plainly that no engine v1 response carries
  operational_fallback: a validated v1 request always has the heuristic
  source, and an empty heuristic action remains NO_DECISION (an error, not a
  downgraded fallback). This is asserted rather than implied.

### Decision 6: TS adapter changes (same stage as the protocol, per RFC L7)

- Regenerated protobuf only; clients/node ProtoEngineProcessClient learns
  0|1|2 negotiation (new opt-in BIGSHARK_ENGINE_PROTO_MINOR2=1; probe 2,
  graceful stay-below against an older engine), leaving the minor-1 opt-in
  and all minor-1 tests exactly as they are.
- v1-mapper: decode solver.guaranteeLevel on minor 2 (prefer it over field
  10 by negotiated minor), expose it in the platform decision metadata, and
  map error code 9 to a typed result; classifyResolveResponse on field 10 is
  unchanged for minor 1.
- New real-binary TS test (gated on bin presence like the existing walks):
  minor-2 capabilities; a heuristic decision carrying
  guaranteeLevel="approximate"; floor exact_solved -> code 9; floor
  approximate -> decision; a published blueprint row -> field 11
  "approximate", field 10 absent; minor-1 walk suite and goldens unchanged.

### Verification and gates

- New C++ tests: `test_guarantee` (full normative table, rank/order, every
  known source + unknown fail-closed, no arm today returns exact/abstract,
  token set); `test_decision_provenance` (declared source per routing branch
  vs an independent re-coded prefix inference; sourced == legacy decision on
  every fixture); `test_v1_minor2` (negotiation/capabilities, field 11 on
  every decision, field 10 never at minor 2, floor matrix incl. forced
  blueprint/resolve via injected V1HostServices fakes, request field
  rejected at minor 0/1, minor 3 rejected, certified<->modeled bridge).
- Freeze proofs: kStage7Hex minor-0 bytes, test_v1_minor1,
  test_v1_resolving, test_v0_protocol, TS v0/v1 goldens unchanged.
- Mutation battery `tools/mutation/decision-stage5.json`: every source->level
  arm, the rank/order comparison, floor short-circuit, minor gating of fields
  8/11, and the field-11-vs-10 selection must go red; equivalent mutants
  recorded with pins, per standing practice.
- Full matrix: release ctest, ASan/UBSan, format BEFORE the final mutation
  battery, benchmark-multistreet, npm run check, proto:check (baseline stays
  green with additive fields/enum values), replay 156/0/0, check-docs,
  check-rfcs. Format is applied before the final battery so mutation anchors
  cannot rot.
- Then INDEPENDENT adversarial review (fresh read-only agents,
  implementer != reviewer), fix confirmed findings with mutation-red
  regressions, re-review, and only then the single clean stage commit.

### Decision 7: the heuristic stays in bigshark_policy (open question resolved)

The RFC asked whether the heuristic should move into its own target. It stays:
bigshark_policy already IS the heuristic+chart target with its own fixture
coverage through the replay/v1 suites, and moving files now would couple the
L6 guarantee work to an unrelated build-graph change. What DOES move is the
routing/source machinery: guarantee.cpp and the sourced entry point live in
bigshark_service (L6), which links bigshark_policy privately exactly as
today. The heuristic remains a DECLARED source (POSTFLOP_HEURISTIC), not an
unnamed catch-all; extracting a standalone heuristic test target, if ever
wanted, is a post-removal cleanup, not stage 5.

### Independent design review round 1 (2026-09-23): findings

Three fresh read-only reviewers (RFC conformance, wire/protocol freeze,
architecture/testability) all returned CHANGES-REQUESTED. The wire schema
itself was cleared: field numbers verified free, Buf FILE breaking
empirically passes against the unchanged baseline, minor-2 probe is harmless
to an old persistent host, enum strength order matches the RFC ladder, the
seven-source table is complete (MULTISTREET_CFR's offline-only arm is
normative, not an evidence gap), RIVER_LP=approximate is correct under the
RFC's strict exact_solved definition (live cap 36, decision.cpp:353-355),
and minor-2 opt-in is RFC-sanctioned given the frozen minor-0/minor-1
constraints. Confirmed defects, all fixed in Revision 1 below:

- BLOCKER (both protocol and architecture reviewers independently): the
  current tryBlueprint/tryResolving do not return answers - they build
  minor-1 protobuf responses through mappers that unconditionally set field
  10 and throw for any other token. Sharing them as written forces minor 2
  to infer provenance back out of a field-10 response and clear/rewrite it.
- MAJOR: DecisionSource in decision.hpp pollutes the v0/frozen include
  surface; the int-taking wire mapping is a type-safety hole with two
  driftable tables.
- MAJOR: no -Werror, so a default-less exhaustive switch is not enforced.
- MAJOR: the "re-code old reason inference" provenance oracle is circular
  and provably wrong on four live branches: preflop chart FOLDS ("fold pre",
  "fold vs open", "fold vs 3bet", "fold vs 4bet", decision.cpp:104,122,133,
  138) are inferred POSTFLOP_HEURISTIC today although preflop() selected
  them. The brief only noticed the v1-unreachable "no hole cards" arm.
- MAJOR: the RIVER_DCFR tag arm has no deterministic fixture in HiGHS
  builds; and cache_hit (true on every blueprint row) is a certification
  mutant no test kills.
- MAJOR: the platform "metadata surface" does not exist
  (ExecutableDecision is action/amount/reason only), so the RFC's
  journal-visibility payoff and the operational_fallback label have no
  carrier; and the TS decide() blanket catch would convert a code-9 floor
  refusal into a silent fallback.
- MAJOR/wire: no closed-set predicate for the request enum (proto3 open
  enums), "field 11 on every decision" scope unresolved for error
  envelopes, shared TS/C++ golden vectors unenumerated, TS negotiation
  cascade/types underspecified, real-binary test gated on binary presence
  instead of negotiated minor, fuzz oracle not extended.
- Plus minors: floor precedence, minor-2 success oneof, documentation
  change list, test_v1_minor1's existing "minor 2 rejected" pin (it must
  move to minor 3 - it cannot stay unchanged), forced experimental backends
  must stay rejected at minor 2, probe order 2->1->0.

### Revision 1 (2026-09-23): superseded decisions

**R1. Types and the single normative table (supersedes Decision 1).**
- `Guarantee` stays in a NEW `engine/include/bs/guarantee.hpp` but is OWNED
  BY bigshark_policy (not the service): policy already names every routable
  source and links the solver; the enum is routing vocabulary, and the v0
  target never includes the new header. `decision.hpp` gains NOTHING (it
  stays the frozen platform-neutral leaf included by v0_json.cpp).
- `DecisionSource` and `SourcedDecision` live in `engine/include/bs/policy.hpp`
  (policy's own public surface): four policy-producible values
  PreflopChart/PostflopHeuristic/RiverLp/RiverDcfr, plus MultistreetCfr for
  switch totality. Storage sources Blueprint/Resolving do NOT enter the
  policy enum; the boundary has its own small label type (R3).
- The normative mapping exists EXACTLY ONCE, typed and protobuf-free, in
  guarantee.{hpp,cpp} (bigshark_policy):
  `Guarantee guaranteeFor(DecisionSource)` with a default-less exhaustive
  switch (MultistreetCfr/PreflopChart/PostflopHeuristic/RiverLp/RiverDcfr
  -> Approximate). Certification is NOT a parameter of this table:
  CertifiedBound is reachable only from the storage/resolve boundary (R3).
  `int guaranteeRank`, `bool guaranteeMeets`, `const char* guaranteeToken`.
- Exhaustiveness is enforced for real with
  `target_compile_options(bigshark_policy PRIVATE -Werror=switch)` (narrow;
  the repo builds -Wall -Wextra without -Werror), plus a constexpr
  visitor-instantiated check over every enumerator.
- A test pins that no `guaranteeFor` arm returns AbstractSolved or
  ExactSolved (promotion is stage 6's measured job).

**R2. Declared sources and the structural provenance oracle (supersedes
Decision 2).**
- decision.cpp internals return `PolicyAnswer{Decision, DecisionSource}`.
  Tagging rule pinned now: EVERY return reached THROUGH `preflop()` is
  PreflopChart, including the four fold exits and "BB option" (a chart fold
  is a chart decision: the branch that selected it is the chart). The
  river answer carries RiverLp vs RiverDcfr from ONE read of
  `sol->result().exact` at the single answer-construction site (:385), with
  a deterministic bounded-CFR fixture forced through the existing
  `allow_exact=false` seam; every riverGto nullopt fall-through
  (ineligible, unknown line, !ok, combo absent, illegal translation) is
  tagged PostflopHeuristic AT THE CALL SITE. The pre-dispatch no-hole-cards
  fold stays PostflopHeuristic; it is v1-unreachable (validator requires
  two hole cards, v1_semantic_validator.cpp:486) and is covered at the core
  evaluatePolicySourced unit level only.
- Public API: `evaluatePolicySourced(Ctx)->SourcedDecision` (policy.hpp)
  and `decideSourced(Ctx)->SourcedDecision` (service.hpp). Legacy
  evaluatePolicy/decide remain one-line `.decision` wrappers; v0/JSON and
  minor 0 do not change at all.
- The provenance oracle is STRUCTURAL, not textual: an independent test
  reconstructs the expected source from the Ctx shape and routing
  conditions (street, riverGtoOn, forced solver seam for LP vs DCFR,
  chart-vs-heuristic street dispatch) and drives fixtures that cover every
  preflop() exit INCLUDING THE FOUR FOLDS, river eligible/ineligible,
  forced-bounded DCFR, and the postflop catch-all. The old 12-prefix reason
  inference survives only as a JOURNAL-COMPATIBILITY cross-check with an
  explicitly pinned discrepancy set {fold pre, fold vs open, fold vs 3bet,
  fold vs 4bet}: minor 0/1 freeze keeps emitting POSTFLOP_HEURISTIC for
  them; minor 2 truthfully emits PREFLOP_CHART. test_v1_minor2 pins BOTH
  sides of that divergence (levels are identical: approximate), and the
  proto comment on field 11 records it.
- Minor 2 always populates SolverMetadata.source (field 1) from the
  declared source; reason-text inference (solverSource) remains wired ONLY
  into the minor-0/minor-1 mappers.

**R3. Lookup/mapping split (fixes the BLOCKER; supersedes Decision 4's
sharing model).** In v1_envelope.cpp the lookup core becomes protobuf-free:
- `lookupBlueprint(services, request) -> {hit, row, miss}` (reconstruction
  + service call + blueprintRowIsLegal; NO mapper).
- `lookupResolving(services, request, deadline_ms) ->
  {outcome, row, miss}` reusing V1ResolveOutcome (Certified vs
  DeadlineBlueprint already distinguished at the boundary).
- handleMinor1Decision keeps its EXACT current behavior by calling the
  existing minor-1 mappers (mapBlueprintExpandedResponse /
  mapResolvedExpandedResponse with pair discipline) on those results.
- handleMinor2Decision calls NEW minor-2 mapper entry points
  (mapGuaranteedHeuristicResponse, mapGuaranteedBlueprintResponse,
  mapGuaranteedCertifiedResponse, mapGuaranteedDeadlineResponse) that set
  field 11 from typed boundary labels, keep field 9, and never touch field
  10. The certified label is constructible ONLY with source RESOLVING +
  outcome Certified; deadline/blueprint rows -> approximate (mirrored
  boundary guard, not a minor parameter). No clear_guarantee() erasure
  exists anywhere; the field-10/11 split is a construction invariant, and
  a mutation deleting the discipline goes red on test_v1_minor2.
- Boundary label helper lives in the protocol TU:
  explicit pv::SolverSource -> storage label table; UNSPECIFIED/out-of-range
  -> OperationalFallback fail-closed there (the only place unknown wire
  values can actually arrive). L6 stays free of pv names and magic ints.

**R4. Wire contract precision (supersedes Decisions 3-4 details).**
- Enum `GuaranteeLevel { UNSPECIFIED=0; OPERATIONAL_FALLBACK=1;
  APPROXIMATE=2; ABSTRACT_SOLVED=3; EXACT_SOLVED=4; CERTIFIED_BOUND=5 }`,
  numeric order = strength. `DecisionOptions.minimum_guarantee = 8`
  (optional enum), `SolverMetadata.guarantee_level = 11` (optional string,
  buf `in` five tokens), `ErrorCode = 9`. Buf schema empirically verified
  FILE-green against proto/baseline/v1.binpb; baseline is NOT regenerated.
- Validator: `isKnownGuaranteeLevel` closed-set predicate (proto3 open
  enums: values 6+/int-max are parseable and must be rejected
  INVALID_REQUEST). Semantics: ABSENT field = accept any level; PRESENT
  value 0 or out-of-range = INVALID_REQUEST; presence at minor 0/1 =
  UNSUPPORTED_FEATURE ("requires negotiated protocol minor 2"), including
  an explicit 0. Forced RIVER_LP/RIVER_DCFR/MULTISTREET_CFR stay rejected
  at minor 2 (pinned; the new minor widens nothing).
- Scope of "every decision": EVERY SUCCESSFUL strategy/expanded oneof at
  minor 2 carries field 11; minor 2 always returns the expanded_strategy
  oneof (never the 5-capped Strategy). Error envelopes structurally carry
  no metadata - code 9 IS the report; it carries no strategy payload,
  retryable=false (pinned), static message text (no coverage detail, no
  achieved-level leak beyond what the served approximate decision would
  itself reveal).
- Floor precedence PINNED: validation/coverage/deadline errors first
  (forced BLUEPRINT miss = UNSUPPORTED_FEATURE, forced resolve no baseline
  = DEADLINE_EXCEEDED, unchanged); code 9 only when a complete weaker
  answer exists. AUTOMATIC blueprint hit + floor certified_bound -> 9
  (AUTOMATIC never resolves): the cache_hit-vs-certified mutant.
- Host accepts minors 0..2; echo formula and malformed-id paths become
  <=2; the >2 rejection gets new wording, the old message stays byte-fixed
  for existing cases. Capabilities: minor-2 query advertises [0,1,2],
  build "bigshark-engine-v1.2.0", identical solver-mode set to minor 1;
  minor-0 (kStage7Hex) and minor-1 capability bytes are untouched. The
  existing test_v1_minor1 "minor 2 rejected" assertion MOVES to minor 3
  (this is the one deliberate test edit; minor-1 decision/response pins
  otherwise stay).

**R5. Platform adapter and journal surface (supersedes Decisions 5-6).**
- `ExecutableDecision` gains an OPTIONAL `guaranteeLevel?: string`
  (additive; reason strings are never repurposed, so replay's
  safe-fallback counting is unaffected). v1-mapper populates it from
  field 11 at negotiated minor 2 (at minor 2 the mapper READS field 11
  exclusively, never falling back to field 10); every LOCAL fallback
  producer sets it to "operational_fallback": safeFallback (single
  function covers engine.ts call sites), and the separate
  `{action:'fold',reason:'no-legal'}` site. The engine-host JSON parse-
  error fold stays unlabeled: frozen v0, removed at stage 7 (stated in
  evidence, not silently omitted).
- The runner (apps/river-club-agent/main.ts) threads the field into the
  .runtime/results.log entries (the RFC's journal-visibility payoff);
  behavior/action selection is unchanged.
- Floor refusals: code 9 is a typed V1EngineError at the clients/node
  boundary (with code-name/retryable), and platform decide() does NOT
  substitute safeFallback for it - a caller-declared floor refusal is a
  contract response, not an operational outage; it propagates. The shipped
  adapter itself never SENDS a floor (no production floor policy), so live
  behavior is unchanged; request encoding exists for explicit callers.
- Negotiation: type widening to 0|1|2 everywhere (client getter,
  V1EnvelopeClient, decisionEnvelope, engine.ts); opt-in
  BIGSHARK_ENGINE_PROTO_MINOR2=1 with explicit probe cascade 2 -> (if
  MINOR1 enabled) 1 -> 0, restart re-negotiation rule kept; mode
  selection changes `minor1Capable ? ...` to `minor >= 1`.
- Tests: real-binary minor-2 walk GATED ON negotiated minor === 2 (skip
  message otherwise, so a stale published binary cannot fail npm check);
  PLUS a new fake-proto-engine minor-2 fixture giving non-gated coverage of
  negotiation fallback, field-11 decode, and code 9; plus a runner-level
  test that an engine decision logs its level and a fallback line logs
  operational_fallback.

**R6. Goldens, fuzz, docs.**
- proto/tests/fixtures: add a minor-2 decision-response JSON/binpb
  (guarantee_level present, field 10 absent), a field-8 request vector
  (separate from the minor-0 request golden), and an error-code-9 vector;
  extend BOTH the TS golden round trip and the hardcoded C++ list in
  test_generated.cpp. Add a test asserting the C++ five-token set equals
  the proto `in` list.
- fuzz_v1_proto.cpp: minor-2 expanded strategies must carry field 11 and
  not field 10; requestIsValid gains the minor argument so the
  strategy=>valid-request implication holds for minor-2 frames.
- Docs same change: docs/reference/protobuf-engine-protocol.md (minor 2,
  fields 8/11, code 9, v1.2.0, certified_bound<->modeled_exact_bound
  bridge, chart-fold source divergence), docs/integrations/river-club.md,
  docs/README.md status, docs/design/gto-engine.md module notes, and mark
  BOTH RFC 0008 open questions (v2-vs-minor; heuristic target) resolved in
  the RFC itself and docs/rfcs/README.md as required by the doc rules.
- CMake: guarantee.cpp joins bigshark_policy; new test_guarantee is
  registered against bigshark_policy (engine/CMakeLists.txt);
  test_v1_minor2 joins the v1 test group in proto/CMakeLists.txt.

Everything else in the original brief (mutation battery targets, full gate
matrix, independent re-review before the single commit) stands.

### Independent design review round 2 (2026-09-23): findings

Two fresh reviewers (RFC/layering and wire/TS) verified Revision 1 against
the code. They cleared the schema, the R1 placement (-Werror=switch,
v0 include freeze, link graph), R2's fold enumeration (all 16 preflop
returns; four folds at decision.cpp:104,122,133,138; five nullopt sites at
:321,:344,:368,:371,:456), the cross-minor divergence, R4 precedence, and
the TS cascade/old-engine probe. Remaining defects, fixed in Revision 2:

- BLOCKER (both reviewers independently): R2's deterministic DCFR fixture
  cites river_gto.hpp:47 allow_exact, but no POLICY path can set it
  (decision.cpp:361-366 builds literal options; Ctx is frozen; SolveRiver is
  a non-virtual free function). At live settings the LP estimate is
  <=~268ms vs the 825ms gate (river_gto.cpp:222-227), so HiGHS release
  builds always take LP; the RIVER_DCFR tag arm is unfalsifiable.
- MAJOR: R3's boundary table, read literally, re-creates a second seven-row
  normative table; field 11 must provably DERIVE from guaranteeFor; the
  DecisionSource->pv::SolverSource conversion is unnamed; arm flips must
  go red at the wire, not just in test_guarantee.
- MAJOR: R5's runner-level results.log test has no seam (main.ts exports
  nothing).
- MAJOR: floor-vs-mapper-throw precedence (malformed row + high floor must
  stay INTERNAL, never become code 9) with an exact ordering and fixture.
- Minors: exactly-once service invocation; pin cache_hit/reason_code per
  outcome; mapper signatures (fromV1DecisionResponse negotiated-minor arg;
  named floor option in EngineConfig); fuzz symmetric oracles + structured
  seeds; v0 deepEqual field-absence pin; runner crash-path acknowledgment;
  lookup legality predicate reuse (no domain/pv duplicate); enum-conversion
  exhaustiveness on the v1 target as well; old-minor message wording and
  the full echo-site list; golden generator's THIRD fixture list
  (generate-golden.ts envelopeFixtures); GuaranteeLevel ordinal-presence
  vector; classifyResolveResponse minor-1-only doc.

### Revision 2 (2026-09-23): final design decisions

**R7. Policy-level solver seam (fixes the BLOCKER).** decision.cpp
gains an internal answer builder used by BOTH public entry points:
`SourcedDecision evaluatePolicySourced(const Ctx&,
const RiverBackendHint&)` where a NEW policy-owned
`struct RiverBackendHint { bool allow_exact = true; }` is DECLARED IN
policy.hpp (NOT decision.hpp; the frozen Ctx/Decision leaf gains nothing).
It is threaded to exactly one place: the `opt.allow_exact` assignment at
decision.cpp:361-366; every other RiverSolveOptions field stays exactly as
today. The production overloads are hard-wired to the default
(`evaluatePolicy(Ctx)`, `evaluatePolicySourced(Ctx)`, `decide(Ctx)` all
pass `{}`), so live/v0/minor-0 behavior is unchanged and the LP schedule is
untouched. Deterministic provenance fixtures: hint{false} -> RiverDcfr in
EVERY build (including HiGHS); a small eligible live fixture with the
default hint -> RiverLp iff gto::hasExactRiverLp(), else RiverDcfr
(conditional on the compile flag, pinned either way). The structural oracle
predicts LP/DCFR from the hint + hasExactRiverLp() ONLY, never from
result().exact, killing the same-origin trap. The five nullopt fall-through
expectations (:321,:344,:368,:371,:456) are covered; one representative per
class plus the :456 illegal-translation arm are oracle fixtures.

**R8. One normative table, consumed at the boundary (fixes the MAJOR).**
- Field 11 for heuristic/river answers is computed in the minor-2 mapper by
  CALLING the policy core: `guaranteeToken(guaranteeFor(declared))`. The
  protocol TU has NO table mapping the five policy sources to levels; a
  flipped guaranteeFor arm therefore changes wire bytes and goes red in
  test_v1_minor2 (mutation battery explicitly asserts the wire-level red).
- Storage labeling is a TWO-ARM function of the lookup OUTCOME, in the
  envelope: Certified (only from V1ResolveOutcome::Certified on a forced
  resolve) -> certified_bound; blueprint hit / DeadlineBlueprint ->
  approximate. There is no pv::SolverSource-keyed seven-row table.
- One typed conversion `toWireSource(DecisionSource) ->
  pv::SolverSource` (exhaustive, no default) lives in the protocol TU and
  is the ONLY writer of field 1 at minor 2; bigshark_v1_protocol gets the
  same narrow -Werror=switch on its enum-conversion switches (the fail-
  closed unknown-source arm keeps an explicit default by design and is
  table-tested directly with UNSPECIFIED and an out-of-range cast).
- The fail-closed function is a table-tested defense only; evidence and
  test assert no host-produced minor-2 response is operational_fallback.

**R9. Lookup split precision and the floor ordering (fixes MAJOR/MINORS).**
- lookupBlueprint/lookupResolving are PURE MOVES out of
  v1_envelope.cpp:92-162: same advertised checks, same reconstruction
  calls, exactly one service invocation (the returned row is reused by the
  floor check and BOTH per-minor mappers - never called twice), same
  blueprintRowIsLegal pv predicate (it is NOT reimplemented against the
  domain state; the mapper and lookup share the one existing request-keyed
  predicate), and the miss struct carries the verbatim miss code/detail so
  blueprintMissResponse and the DEADLINE path serialize identically. The
  DeadlineExceeded early return at :147-149 stays BEFORE the row legality
  check; the :149-153 off-tree outcome overwrite is preserved verbatim.
  Minor-1 byte preservation is guaranteed by the existing pins; the move
  is reviewable line-by-line against the cited ranges.
- Minor-2 exact order: (1) validate (minor-2 rules); (2) mode dispatch +
  lookup exactly like minor 1; (3) mapper-independent COMPLETENESS
  preconditions on the returned row (legality + 64-hex digest + sampler
  liveness), factored from mapExpandedResponseImpl and shared by both
  minors; (4) ONLY THEN the floor comparison -> code 9 with no strategy
  payload; (5) serialize through the minor-2 mapper. Fixture: malformed
  row (bad digest) + floor exact_solved -> INTERNAL, never code 9.
- The four mapGuaranteed* responses share the mapExpandedResponseImpl
  construction body parameterized ONLY by (label, field 10 vs 11); they
  re-derive nothing. Pinned per outcome: cache_hit = source==BLUEPRINT
  (true for blueprint and deadline rows, false for certified resolve and
  heuristic), reason_code resolving/blueprint, solve_time_us=0, field 9
  present on storage rows, sampler/all_in/hero-capacity derivation
  unchanged, heuristic fields 3/4/8 via fillHeuristicMetadata.

**R10. Platform test seam and floor contract (fixes the remaining
MAJORS/MINORS).**
- apps/river-club-agent: extract a pure exported
  `resultsLogEntry({kind, hand, street, source, decision, ok})` (and the
  decision->guaranteeLevel projection, which is just the optional field)
  into runner-state.ts; main.ts's two log sites call it. runner-state.test
  asserts an engine decision carries its decoded level and a fallback entry
  carries operational_fallback. main.ts keeps ZERO exports otherwise.
- TS floor surface is NAMED: EngineConfig gains an optional
  `minimumGuaranteeLevel?: GuaranteeLevel`; toV1DecisionRequest encodes it
  ONLY when negotiated minor === 2; fromV1DecisionResponse gains a
  negotiated-minor parameter and at minor 2 reads field 11 exclusively
  (field 10 is never consulted; classifyResolveResponse is documented
  minor-1-only). engine.ts rethrows code 9 out of the blanket catch; the
  shipped runner never sets the option, and the option is documented as
  explicit-caller-owned: decideWithinBudget has no handler, so a floor
  refusal surfaces as a typed process error by contract rather than a
  played fallback.
- v0 freeze detail: validateEngineDecision keeps building a fresh object
  WITHOUT guaranteeLevel (v0-golden deepEqual stays green); only the shared
  safeFallback gains the field, and replay counts by reason prefix so
  156/0/0 is unaffected (asserted, not assumed).

**R11. Goldens, fuzz, negotiation final details.**
- THREE golden lists get the new vectors (generate-golden.ts
  envelopeFixtures, generated-roundtrip.test.ts list, test_generated.cpp):
  minor-2 decision response (field 11 set, field 10 absent), field-8
  request, code-9 error, capabilities-minor2, and a
  guarantee-level-enum-presence vector pinning ordinals 0..5 (analogous to
  solver-enum-presence). Existing vectors stay byte-identical.
- fuzz_v1_proto: requestIsValid takes the frame minor; symmetric presence
  oracles (minor 2 => field 11 and NOT field 10; minors 0/1 => never field
  11); structured fuzz_seeds.hpp cases for cap2, field-8 at minor 1
  (UNSUPPORTED_FEATURE), present 0 and out-of-range at minor 2
  (INVALID_REQUEST), floor approximate served, floor exact -> code 9,
  malformed row precedence; response-size and FrameReader oversize traps
  re-pinned unchanged.
- Negotiation: env BIGSHARK_ENGINE_PROTO_MINOR2=1; probe order strictly
  2 -> 1 (only if MINOR1 enabled) -> 0; restart re-negotiation retained;
  the mode expression becomes minor >= 1; types widen to 0|1|2 on the
  client getter, V1EnvelopeClient, and decisionEnvelope. A fake-proto-engine
  minor-2 host (extension of clients/node/tests/fixtures/fake-proto-engine.ts)
  covers cascade fallback, field-11 decode, and code 9 without a binary;
  the real-binary walk skips unless negotiated minor === 2.
- Echo formula changes at every existing <=1 site
  (v1_envelope.cpp:307,312,316 and the catch echo paths) to <=2; the >2
  rejection message is replaced outright (the old "0 and 1" wording has no
  surviving case and is not byte-pinned), and test_v1_minor1's rejection
  assertion moves from minor 2 to minor 3.

R1-R6 stand as revised. The design is now ready for the final independent
re-review before implementation begins.

### Revision 3 (2026-09-23): final amendments after round 3

The third reviewer empirically compiled every v1 protocol TU with the
planned flag and found the single remaining build defect plus two nits;
verdict was that no further substantive review is needed after these:

- R12. -Werror=switch placement (fixes the MAJOR). The flag is applied to
  bigshark_v1_protocol target-wide AND the one pre-existing non-exhaustive
  switch is made explicit: v1_resident_mapper.cpp:27-37 wireStreet gains
  `case poker::Street::Preflop: return pv::STREET_UNSPECIFIED;` (runtime-
  identical: the trailing return already yields UNSPECIFIED, and
  reconstruction is postflop-only). A pin asserts the preflop
  reconstruction path is unchanged (existing test_v1_resident_mapper
  coverage). The reviewer confirmed this is the ONLY -Wswitch break in the
  whole tree and the other protocol TUs compile clean.
- R13. resultsLogEntry keeps the FULL current journal schema: its parameter
  set includes the optional failure `raw` payload (main.ts:524,548 append
  raw only when !ok); the extraction is byte-preserving for results.log.
- R14. Token single-sourcing: even the storage "approximate" outcome arm
  emits via guaranteeToken(Guarantee::Approximate), never a literal; only
  certified_bound has its own token.
- R15. Completeness ordering covers the heuristic arm too: before the floor
  comparison, the chosen answer - row OR heuristic - passes all preconditions
  its own mapper enforces (heuristic legality via actionType/actionIsLegal;
  storage rows via the factored row checks). An un-mappable heuristic action
  + high floor yields INTERNAL, never code 9.
- R16. Minor-2 error responses carry echo minor 2: the factored miss
  serializer takes the negotiated minor (the minor-1 call site keeps literal
  1; minor 2 passes 2); forced BLUEPRINT miss stays UNSUPPORTED_FEATURE with
  the same code/detail, only the echo differs.
- R17. The five riverGto nullopt arms are covered where deterministically
  reachable (ineligible :321, unknown line :344, combo absent :371, illegal
  translation :456); the !ok arm (:368) is a solver-failure outcome that is
  not forceable through a deterministic fixture, so it is pinned at the
  sourced-builder unit level structurally (tag assigned at the call site),
  like the v1-unreachable no-hole fold.

DESIGN APPROVED for implementation subject to R12-R17 (all prescriptive).

### Stage 5 implementation notes (2026-09-23)

- C++ L6: `engine/include/bs/guarantee.{hpp}` plus `src/policy/guarantee.cpp`
  in bigshark_policy (`-Werror=switch`): five-level ladder, one normative
  `guaranteeFor(DecisionSource)` (all five policy sources -> approximate),
  and `evaluatePolicySourced(Ctx, RiverBackendHint)` with every routing
  branch declaring its source (the four preflop chart folds included).
- Wire: minor 2 accepted; field 8 floor, field 11 level token, error code 9;
  lookup/mapping split in v1_envelope.cpp with the five-step floor ordering
  (validate -> lookup -> mapper completeness -> floor -> serialize).
- L7 TS: 0|1|2 negotiation behind BIGSHARK_ENGINE_PROTO_MINOR2=1, strict
  2->1->0 probe cascade, ExecutableDecision.guaranteeLevel, code-9 typed
  propagation, operational_fallback labels, pure resultsLogEntry extraction.
- Tests: test_guarantee, test_decision_provenance (structural oracle plus
  the legacy-inference discrepancy set), test_v1_minor2, extended fuzz
  oracles/seeds, five new golden vector classes across all three golden
  lists, fake minor-2 host and platform/runner-level TS tests.

### Stage 5 gate evidence (2026-09-23)

Design: three rounds of fresh independent review before implementation
(six agents total); the approved brief is the "Revision 3 (R12-R17)"
section above.

Implementation gate results (all measured on this date):
- `cmake --preset release` + full `ctest --preset release`: 47/47 passed.
- `cmake --preset asan` + full `ctest --preset asan`: 45/45 passed.
- `npm run check` (typecheck, TS build, source policy, proto lint/breaking
  FILE against the unchanged v1.binpb baseline, proto generate/typecheck,
  docs, RFCs, node tests): green; node tests 107/107 (the probe-failure
  respawn test was the 107th; the original matrix was 106/106).
- `benchmark-multistreet`: PASS.
- `node bin/replay.mjs`: 156 decisions, 0 illegal, 0 JS fallbacks.
- Mutation battery `tools/mutation/decision-stage5.json` via
  `tools/mutation/verify.ts`: 23/23 caught, 0 equivalent, 0 gaps across
  three suites (test_guarantee, test_decision_provenance, test_v1_minor2).
  Key mutants: floor comparison removal, completeness/floor reordering,
  certified/approximate resolve mislabel, AUTOMATIC-resolves mutation,
  field 8 gate, field 10/11 vocabulary split, declared-source inference
  regression, fail-closed arm, negotiation gate widening, retryable code 9.
  Hardened after independent review to 27/27 (four R16 echo mutants, see
  the review subsection below).
- Fuzz: structured seeds extended to 45 (field-8 gates at each minor,
  present-0/out-of-range floor, forced-backend rejection, code-9,
  minor-2/3 caps); a 30k-input ASan mutation campaign passed clean.
- Goldens: five new vector classes (minor-2 capabilities, field-8 request,
  guaranteed strategy, code-9 error, GuaranteeLevel ordinals 0..5) added to
  all THREE golden lists (generate-golden envelopeFixtures, TS roundtrip,
  C++ test_generated); existing vectors unchanged.
- The real-binary minor-2 walk
  (platforms/river-club/tests/v1-minor2-walk.test.ts) ran and passed
  against the freshly published bin/bigshark-engine.

Open/known: no stage-5 host-produced response is operational_fallback by
construction (asserted in C++ and fuzz oracles); the operational_fallback
surface is the TS local fallback and is labeled. The `!ok` solver arm
(decision.cpp:368) is structural and pinned at the sourced-builder unit
level rather than via a deterministic fixture (R17).

#### Independent implementation review (2026-09-23): outcomes and fixes

Three fresh read-only reviewers (C++ implementation, wire/golden/fuzz
contract, TS/L7) reviewed the finished implementation. Verdict: no
blocker, no remaining major. The TS/L7 reviewer's MINOR (an out-of-band
negotiation-probe failure aborted start() instead of settling at minor 0)
was fixed with a try/catch cascade plus a custom-spawn respawn test before
this evidence was first written. Two further findings were closed after
the final reports:

- F1 (contract reviewer, MAJOR-grade, empirically proven): R16 prescribed
  that minor-2 error responses echo minor 2, but only success and code-9
  paths asserted the echo; mutating both `blueprintMissResponse(..., 2)`
  sites to 1 left test_v1_minor2, test_v1_minor1, and the fuzz campaign
  green. Fix: test_v1_minor2 now pins echo 2 on every minor-2 error arm
  (forced BLUEPRINT coverage miss, forced RESOLVING with the resolver
  unadvertised, DEADLINE_EXCEEDED, INTERNAL completeness, INVALID_REQUEST
  floors, forced-backend rejections, AUTOMATIC/deadline code 9), and the
  mutation battery gained four echo mutants (forced BLUEPRINT miss, forced
  RESOLVING miss, validation errors, DEADLINE_EXCEEDED). The reviewer
  re-ran its exact mutant against the hardened suite (2 FAILs) and the
  battery now reports 27/27 RED.
- M1 (C++ reviewer, MINOR; R6): the promised test that the C++ five-token
  set equals the proto `in` list was only a hardcoded comparison.
  test_v1_minor2 now reads the compiled buf.validate FieldRules extension
  off the SolverMetadata.guarantee_level descriptor and asserts each
  guaranteeToken is a member of the descriptor's string `in` list, so a
  .proto/C++ vocabulary drift goes red.

Additional contract-reviewer MINORs: the response-shaped fuzz seed's
comment falsely claimed oracle coverage (comment corrected; the oracle is
exercised by host-output seeds automatic2/floorapprox2), and the C++
presence-vector loop did not assert has_minimum_guarantee on ordinal 0
(now asserted; the TS side and byte vectors already pinned presence).
NO_DECISION-vs-floor precedence stays structurally pinned (the empty
heuristic action is not deterministically forceable), matching the R17
treatment; the minor-1-era golden list subset and the root-less vs
resident capabilities shapes were accepted as pre-existing.

Final gate re-run after the hardening: release 45/45 functional tests plus
benchmark-multistreet PASS (standalone, 5.2 s), ASan 45/45 clean,
npm run check green (node 107/107), npm run proto:check green,
replay 156 decisions / 0 illegal / 0 JS fallbacks, mutation battery
27/27 RED post-format, and the real-binary minor-2 walk passing
non-skipped against the republished bin/bigshark-engine.

### RFC 0008 stage 6 precursor: ten-seat equity memory-safety fix (2026-09-23)

While mapping the stage-6 surface, a reachable stack out-of-bounds WRITE was
found in the Monte Carlo equity routine that the pinned postflop baseline
calls on every flop/turn/river decision.

- Defect: `equityVsAll` (engine/include/bs/equity.hpp) stored each drawn
  opponent hand in a fixed `std::array<std::array<int,2>, 6> opp`, but the
  number of opponents is `opts.minPct.size()`, which
  `postflop()` (engine/src/policy/decision.cpp:195,205) sizes to
  `playersInHand - 1`. The v1 validator accepts 2..10 seats and nothing
  upstream rejects large tables, so a postflop request at 8/9/10 seats set
  7/8/9 gates and `opp[nDrawn++]` wrote past the six-slot stack buffer on the
  first Monte Carlo iteration that drew a seventh opponent. `opponentPcts`
  is also an untrusted external JSON array (v0_json.cpp) with no length
  bound, giving a second overflow path independent of seat count.
- Why it blocks stage 6: the RFC pins the engine's own heuristic+chart
  policy as the comparison opponent and requires a measurement at exactly
  10 seats; that baseline had undefined postflop behavior (memory
  corruption) there, so it could not be pinned or measured honestly.
- Fix: the opponent buffer is sized to the true capacity, single-sourced
  from the L0 constant
  `kMaxOpponents = poker::kMaxContributionSeats - 1` (9); equity.hpp now
  includes bs/settlement.hpp (no cycle: settlement is a bigshark_poker
  sibling). `nOpp` is clamped to `[1, kMaxOpponents]` so an oversized gate
  list can never overflow, while the normal 1..9 path is unchanged.
  Widening the fixed buffer changes no indexed value, so <=7-seat results
  are bit-stable; the existing multiway equity figures (AA heads-up 0.845
  vs 3-way 0.605) are unchanged and replay stays 156/0/0.
- Verification: new engine/tests/test_equity_capacity.cpp (registered in
  engine/CMakeLists.txt) fills opponent slots 1..9 with gate 0, passes a
  50-entry oversized gate list AND an empty gate list, and asserts seed
  reproducibility. It trapped `stack-buffer-overflow WRITE ... overflows this
  variable 'opp'` at equity.hpp under AddressSanitizer on the unpatched code,
  and passes after the fix. The independent reviewer reproduced that trap
  against the committed header via a /tmp include overlay (repo untouched).
  Mutation battery tools/mutation/equity-capacity-stage6.json = 3/3 RED: the
  buffer-shrink mutant and the short-gate-list guard-removal mutant (a
  container-overflow READ on an empty list) are witnessed under the asan
  preset (the decisive witness for memory-safety regressions), and the
  clamp-removal mutant under release. The short-list guard also closes a
  pre-existing latent out-of-bounds READ (nOpp floors at 1, so an explicitly
  empty minPct read past the vector; no shipped caller produced it).
  Gates: release 46/46 (the new test is the 46th), ASan/UBSan 46/46 clean,
  npm run check 107/107, replay 156/0 illegal/0 JS fallback. Independent
  read-only reviewer: APPROVE (bounded indexing, no other opponent-indexed
  array, no include cycle, <=7-seat behavior unchanged, legal-v1 reachability).

The larger stage-6 machinery (a seat-indexed behavior-policy interface over
GameState/AbstractTree, an N-seat sampled unilateral-deviation estimator,
paired confidence-interval statistics, a scalable joint-deal/runout/action
sampler, the parsed-chart digest and baseline pin, seat permutations, and a
10-seat policy that cannot materialize a full identity tree and therefore
needs abstraction/traversal) is designed separately below; none of it is
implemented in this precursor.

## RFC 0008 Stage 6 Design Brief Revision 0 (2026-09-23) — FOR INDEPENDENT REVIEW, NOT YET APPROVED

Status: draft. Nothing in this section is implemented beyond the ten-seat
equity precursor (b430a09). This brief exists to be challenged; it does not
authorize the build. The RFC's own rule governs: stage 6 MEASURES, it does not
have to win, and a negative result reported honestly completes the stage while
blocking only the Summary's capability claim (RFC 0008:619-627).

### What the three read-only machinery maps established (with file:line)

**Exists and is reusable.**
- 2..10-seat rules/transitions/exact N-vector payout through unified
  GameState (`after_action`, `after_card`, `settle_fold`,
  `settle_showdown(span<array<int,2>>)`; game_definition.hpp:194-206) and
  settle_contributions (side pots, ties, rake).
- The seat-agnostic L3 AbstractTree (2..10), with fold leaves carrying exact
  chip_utility vectors and showdown leaves carrying ledger+board; chance edges
  carry no probability (per-deal conditioning is L4's job).
- Joint PRIVATE-hole distribution with zero card duplication:
  enumerate_joint_deals / sample_joint_deal (multiway_sampler.hpp:55-72),
  caller-owned SplitMix64 (prng.hpp). Its documented scaling dead end:
  sample returns an index into the fully ENUMERATED table, so it is only for
  small validation games, and it conditions on a static board (no runout, no
  action sampling).
- The exact 2-player BR recursion (heads_up_solver.cpp:481-546, evaluate
  :700-729) and the external-sampling traversal pattern
  (SampledTraversal::walk :434-478; enumerate traverser actions, sample one
  opponent action) as the algorithmic template, plus pinned unbiased
  bounded_index/weighted_index and top-53-bit unit doubles (:369-405).
- The pinned baseline source: charts.{cpp,hpp} + preflop()/postflop()/
  evaluatePolicySourced in decision.cpp; preflop is already seat-count aware
  (rfiBucket/openerBucket keyed on playersInHand; a 9-seat chart case already
  exists in test_decision_provenance.cpp:144) and postflop has explicit
  multi = playersInHand>=3 branches (decision.cpp:181,245,281,294,301).

**Does not exist and must be built.**
1. A seat-indexed BEHAVIOR POLICY interface over GameState/AbstractTree:
   state+own-hole -> distribution over the legal menu. Today the heuristic is
   reachable only via string-context evaluatePolicy(Ctx); HeadsUpPolicy is
   2-seat and HeadsUpState-keyed. Need adapters: pinned-baseline policy,
   uniform-random reference, and the candidate abstract policy, all behind one
   interface.
2. An N-seat sampled UNILATERAL-DEVIATION estimator (general-sum NashConv:
   sum_i E[u_i(BR_i, sigma_-i) - u_i(sigma)]), using sampled joint deals,
   sampled public runouts conditioned on all N hands, sampled opponent
   actions, and traverser-action maximization, with exact terminal settlement.
   This is the documented unbuilt design (plan lines ~34-56).
3. Paired-sample mean/variance/confidence-interval statistics under COMMON
   RANDOM NUMBERS (same joint deal + opponent-action draws for sigma vs
   deviator), with Student-t critical values for the published seed-list size.
   No stats code exists anywhere in the repo.
4. A scalable joint-deal/runout/action sampler that does NOT enumerate the
   full 1326-combo support (the existing sampler cannot serve full ranges),
   and a full-table hand simulator (game + per-seat policies + deal -> payoff
   vector). Nothing maps GameState+holes+policies to payoffs today.
5. A deterministic digest of the 24 PARSED chart ranges (sorted 169-key sets),
   to pin the baseline by content as the RFC requires alongside the SHA/files.
6. A seat-permutation driver over a fixed published seed list.

**The hard constraint the maps measured (do not hand-wave this).** A full
IDENTITY L3 tree does not materialize at the target seat counts. Empirical
rooted-flop node counts (identity menu, equal stacks): 2p 31k, 3p 103k,
4p 289k, 5p 744k, 6p 1.81M (already past the 1 GiB default), 7p 4.25M /
~3 GB, 8p 9.75M / ~6 GB, 9p 21.98M / ~12 GB; 10p projects ~50M / ~27 GB and
exhausted even a raised 24 GiB/60M-node probe. Deeper stacks explode faster
(7 seats at 4-unit stacks exhausted 16 GiB). Therefore the candidate policy
CANNOT be "CFR over a materialized full identity tree" at 7/10. It must use
either (a) a COARSER action+card abstraction whose tree fits, trained by a
streaming/external-sampling multiplayer CFR that does not retain the full
public tree, or (b) on-the-fly tree traversal without materialization. This is
the central design decision this brief must settle, and it is exactly the
"abstraction" the RFC says the larger-table policy is allowed to earn
abstract_solved through (RFC 0008:291-297, 619).

### Open decisions Revision 0 proposes (each to be confirmed or overturned in review)

D1. **Candidate policy = coarse-abstraction multiplayer external-sampling
CFR (vanilla CFR+), trained offline, stored as abstract-node -> averaged
distribution; NOT a materialized full tree.** Propose a deliberately coarse,
declared abstraction sized so the TRAINED tree fits even at 10 seats (e.g. a
single bet/one raise per street with a small pot-fraction set plus an all-in
edge, and the existing L2 CategoryTiersV1 card buckets), and a streaming
trainer that walks GameState on demand and retains only infoset regret/sum
tables keyed by the L3 public-path id + the acting seat's L2 card bucket.
Reason: this is the only path that fits the measured memory envelope and it
matches Pluribus-style abstracted self-play the RFC cites. The RFC does not
commit to a training budget and explicitly keeps large-scale training out of
scope (RFC:448-451), so this stage uses a SMALL, declared, reproducible
iteration budget on LOCAL hardware; the point is the measurement harness and
an honest number, not a strong policy.

D2. **Estimator = Monte Carlo external-sampling best response per seat, on the
EXACT game (not the training abstraction), reporting estimated NashConv in
chips/pot with a paired CI.** The evaluator must walk the real L1 GameState
with the real legal menu (so a candidate trained on a coarse tree is measured
against true poker, not against its own coarse rules), draw joint holes and
runouts via the RFC 0006 joint distribution, sample the FIXED opponents from
the policy under test, and maximize over the deviator's true legal actions.
Estimate is explicitly NOT a bound at 7..10 (RFC:291-297).

D3. **Estimator validation before any large-table number.** RFC 0006:298-299
requires a small three-player game with INDEPENDENTLY ENUMERATED unilateral
deviations and a complete utility vector. Build that exact 3p oracle (full
enumeration, deterministic) and require the Monte Carlo estimator to agree
with it within a tight tolerance on the enumerable 3p game before trusting
any 7/10 estimate. This is the same "validate the estimator on a game where
the exact answer exists" discipline as Stage 3's zero-error identity and the
2p ExactEvaluation.

D4. **Baseline pin (RFC:568-580).** Record (i) commit SHA a9584b4 plus the
equity fix b430a09 (the baseline is only defined at 10 seats after the fix),
(ii) the exact files, and (iii) a content digest of the 24 parsed chart
ranges. The baseline adapter drives evaluatePolicy through a GameState->Ctx
adaptor (new) and is wrapped by the same behavior-policy interface as the
candidate, so the comparison is like-for-like at the same seats.

D5. **Protocol of the published table.** Seat counts {2, 3, 6, 7, 9, 10}
(>=6 included to bracket the engine's old 6-max ceiling; >6 and =10 satisfy
the RFC). At every count: abstract-candidate estimate vs pinned-baseline
estimate vs uniform-random and (where meaningful) a previous-stage reference,
on IDENTICAL fixtures/seeds; candidate occupies EVERY seat (RFC:605-609);
seats permuted across the seed list; each figure with a paired CI at a
declared level (propose 95%) and the seed-list size justified by the interval
it produces (start from RFC 0006 seeds 1/17/43 and ADD seeds until the
interval width is reported; a one-element list is rejected). Report resident
bytes, train wall clock, and per-decision latency in the same table
(RFC:444-447). State "estimate, no convergence guarantee" beside every
multiplayer figure.

D6. **Negative result is the default-honest expectation and is fine.** The
coarse, locally-trained candidate is plausibly WEAKER than the tuned
heuristic; if intervals do not separate in the candidate's favor the stage
reports that, does not promote/enable the policy, and proceeds to stage 7 on
the architecture alone. The deliverable that cannot be skipped is the
falsifiable harness, the pinned baseline, and the seat-by-seat curve.

### Explicit questions for the independent reviewers

Q1. Is on-the-fly streaming CFR keyed by L3 public-path id + L2 card bucket
SOUND at 3+ seats (the public-path id is defined on the materialized tree; a
streaming trainer that never materializes must derive the SAME id from a
GameState walk — is that identity provable, or should the trainer materialize
only a COARSE tree that fits at 10 and key by node index)? Which is the
correct primary path given the measured memory envelope?

Q2. General-sum NashConv is a measure, not a convergence certificate. Is the
proposed per-seat sampled BR estimator unbiased given the RFC 0006 joint
distribution and exact settlement, and what is the correct variance unit
(per-seat gain vs the sum) for the paired CI?

Q3. Is measuring the candidate on the EXACT game while it was trained on a
COARSE tree the right (and feasible) design, or must evaluation also stay on
the training abstraction (which would be circular and is rejected)?

Q4. Is the proposed coarse abstraction + local training budget too weak to be
worth building, given the RFC explicitly says stage 6 does not need to win?
Is there a smaller valid stage (harness + baseline pin + measurement of the
baseline against references, candidate optional) that still satisfies every
acceptance criterion at 7 and 10?

Q5. What is the minimal set of NEW targets/files, and does the dependency
graph stay acyclic (estimator/trainer must not be linked by the live decision
service or v1 paths; this is offline-only tooling)?

## RFC 0008 Stage 6 Design Brief Revision 1/2 (2026-09-23) — DESIGN APPROVED BY TWO INDEPENDENT LANES

Approval record: the RFC-fidelity lane APPROVED Revision 1 and re-APPROVED
Revision 2 (F1-F14 all RESOLVED, new-defect scan clean; explicit rulings that
the composed profile is an RFC-permitted reading and the frozen-matrix hash
gate satisfies reproducibility). The algorithm lane returned CHANGES REQUIRED
on Revision 1 (three blockers), then APPROVED Revision 2 after verifying the
flop-geometry matrix, the distinct (kind,total) branch pin, the
chart-union-deviation-grid reach, the corrected marginal scoping, the uniform
primary deal, the solver-target dealer placement, and the sampled-root-board
ordinal alignment against source. Both approvers were fresh read-only agents,
neither authored this brief. Implementation proceeds under R12; design changes
after this point require a new dated revision and re-review.

Status: Revision 0 was reviewed by two independent read-only agents (an
algorithm/estimator skeptic and an RFC-fidelity/architecture reviewer). Both
returned "sound/compliant WITH mandatory changes" (24 findings: A1-A10,
F1-F14). Revision 1 resolved all 24; the RFC-fidelity lane then APPROVED,
while the algorithm lane returned CHANGES REQUIRED with three blockers
(undefined flop-root geometry matrix; a marginal assertion false under
nonuniform weights; no defined BB dealing range). Revision 2 (same date)
resolves all three via R1-D10/R1-D11, the new R3b geometry matrix, the R3
uniform-primary dealing distribution, and the corrected R5 marginal scoping,
plus the R2 solver-target placement note; it awaits the algorithm lane's
re-review. Every finding was checked by the implementer against source before
being resolved here; one reviewer sub-claim that was factually wrong (that
TrainingLimits lacks max_depth/max_bytes; it has both at
heads_up_solver.hpp:105-106) did not change the underlying requirement and is
noted. Revision 1/2 supersede Revision 0 in full; Revision 0 is retained as the
draft history. Nothing below is implemented beyond the ten-seat equity
precursor (b430a09).

### R1. Decision summary (what changed since Revision 0)

- R1-D1 **Materialize the coarse tree; do not stream.** There is no "L3
  public-path id": TreeNode carries only an allocation-order position index
  (abstract_tree.hpp:88-115; assigned in deterministic DFS order at
  abstract_tree.cpp:244), and GameState is explicitly historyless
  (game_definition.hpp:20-26). Infosets are keyed by materialized node index
  plus the acting seat's own card bucket. Coarse-tree size at every seat count
  must be MEASURED and within declared caps before the trainer is built
  (A1, A2, F14). Streaming is removed from the stage scope.
- R1-D2 **Preflop is composed, not trained.** L2 card bucketing legally throws
  on boards under three cards (abstraction.cpp:222-223) and CategoryTiersV1 has
  no preflop image. The candidate is therefore a declared COMPOSED PROFILE:
  preflop actions come from the pinned charts (identical source to the
  baseline, in every seat), postflop actions from the coarse-trained policy.
  Training trees remain rooted-flop, matching L3. This is labeled on every
  row of the published table as part of the policy's identity (A3, F10).
- R1-D3 **Two-phase frozen best-response estimator.** Naive per-episode
  "max over one-sampled continuation per action" is upward biased. Phase 1
  accumulates per-infoset Q with traverser-action enumeration under common
  random numbers, freezes BR-hat once; phase 2 measures on disjoint fresh
  seeds with paired best=true/false rollouts (A7).
- R1-D4 **Pre-registered, disjoint seed lists.** Pilot seeds estimate variance
  and fix sample count against a predeclared target half-width; the
  confirmatory list (containing RFC 0006 seeds 1/17/43) is committed and
  frozen before any confirmatory run. No adaptive seed addition. The trained
  artifact is frozen and digested before confirmatory evaluation (F1, F2).
- R1-D5 **An explicit, versioned exact<->coarse translator is built and named
  as a declared confound.** No nearest-size mapper exists today
  (v1_response_mapper.cpp:87-97 is only an interval predicate) (A9).
- R1-D6 **Baseline pin hardened:** content digest over expanded chart sets +
  raw specs + the Chen percentile table; a byte-identical decision gate
  between the deployed parseRequest path and the new adapter; deterministic
  seed derivation; river solver disabled for the harness baseline (F4-F7).
- R1-D7 **Exact recurrence, guarantee, and oracle discipline written out**
  (external-sampling MCCFR, RM+ regrets, uniform average strategy, abstract
  CCE only, sampled-vs-full averaging gate on the enumerated 3p game; A5,
  A10).
- R1-D8 **Offline target graph with four link guards**; new targets
  bigshark_behavior / bigshark_stage6_train / bigshark_stage6_eval, never
  linked by service/protocol/host (F14).
- R1-D9 **Candidate is required, strength is not.** Q4 is settled against the
  acceptance language (RFC:556-557, 684-690): the stage must produce and
  measure an abstract policy at >6 and 10 seats; a harness measuring only the
  heuristic and uniform policies fails. Losing honestly is the accepted
  outcome (F-scope).
- R1-D10 **Flop geometries are an enumerated matrix, not one tree per seat
  count (Revision 2).** A rooted tree's node indices and node.actions carry
  CONCRETE chip totals (abstract_tree.hpp:88-99), so limped pots, single-raised
  pots, 3-bet pots, and 4-bet-call pots with different live-seat subsets are
  different training artifacts. The finite set of flop chip geometries
  reachable under the composed preflop profile AND the declared preflop
  deviation menu is enumerated, sized, cap-tested, and frozen before training
  (see R3b). An uncovered geometry at simulation time is a typed harness
  failure with no fallback.
- R1-D11 **Sampler/statistics corrections (Revision 2).** The balanced
  marginal assertion is scoped to the symmetric uniform control (nonuniform
  weights shift marginals by suffix completion mass); the primary distribution
  test stays joint-frequency vs enumerate_joint_deals. The PRIMARY dealing
  variant is uniform 1326 combos per seat including BB (the game's own
  unconditional chance distribution); chart weights exist only as an explicit
  node-CONDITIONAL optional variant with BB defined. The scalable dealer lives
  in bigshark_solver, linked by both train and eval targets.

### R2. Architecture and target graph (offline only)

New targets, all links point downstream; the graph stays acyclic:

1. `bigshark_behavior` — `engine/include/bs/behavior_policy.hpp` plus
   implementations under `engine/src/behavior/`. Interface:
   given GameState, acting seat, that seat's two hole cards, and the legal
   menu, return a probability distribution over the legal actions. Links only
   `bigshark_poker`. Ctx/policy vocabulary stays out of L0/L1 (no
   poker<-policy edge).
2. `bigshark_stage6_train` — coarse-tree training driver, infoset regret/
   average store. Links poker, abstraction, tree, solver. Never links policy,
   service, or either protocol.
3. `bigshark_stage6_eval` — behavior-policy implementations (pinned-baseline
   adapter, uniform, candidate-composed), the GameState->Ctx adapter,
   full-hand simulator, two-phase deviation estimator, exact 3p oracle, stats
   module, digest generator, translator. Links poker, abstraction, tree,
   solver, policy, behavior. The adapter is the ONE place allowed to include
   both bs/game_definition.hpp and bs/decision.hpp.
3a. The scalable joint dealer (R5) lives in `bigshark_solver` next to
    multiway_sampler, which both new targets already link; one implementation,
    no duplication (Revision 2 placement correction).
4. Harness binary in `engine/benchmarks/` (manual target, never a timing-gated
   CTest; precedent: heads_up_matrix_benchmark.cpp) emitting a deterministic
   CSV + markdown table. tools/replay (TypeScript) is not used.

Guards, replicating the stage-4 L3 discipline:

- CMake configure/CTest assertion over LINK_LIBRARIES of bigshark_service,
  bigshark_v0_protocol, bigshark_v1_protocol, and the engine-host executable:
  fail if any stage6/behavior target appears.
- Resolved-include allowlist guard (the engine/cmake/l3_boundary_guard.sh
  -MMD pattern) over service/protocol sources excluding behavior and stage-6
  headers.
- A link-negative test binary that links service+v0+v1 and references no
  stage-6 symbols.
- The frozen trained artifact is harness-local files only; it is never
  registered with the decision service, resident storage, L5, v0, or v1. The
  harness performs no network access, reads no credentials, and reads no
  sessions/ journals (RFC:455-460).

### R3. Fixtures, ranges, positions, permutations

Frozen before any confirmatory result (F3):

- Seat counts {2, 3, 6, 7, 9, 10}. Primary endpoints for claims are 7 and 10
  (the RFC-required >6 and =10); {2,3,6,9} are secondary curve points.
- Cash fixture: 100 BB effective stacks, blinds 1/2 (big blind = 2 chips),
  rake zero, no antes. Training uses rooted-flop games (board 3 cards); the
  measured full hands run preflop->river through the simulator composed
  profile.
- Dealing distribution, PRIMARY variant: every seat is dealt uniformly from
  all 1326 combos (weight 1 each), joint-conditioned on zero card duplication
  by the R5 restart sampler. This is the game's own unconditional preflop
  chance distribution and the correct measure for whole-hand NashConv against
  a fixed profile; it also has a defined BB marginal, which the RFI chart does
  not (charts.cpp:55-60 has no BB key). OPTIONAL secondary variant, labeled
  and never used for headline claims: node-CONDITIONAL chart weights — when the
  simulator reaches a preflop decision, subsequent conditional analysis may
  weight a seat's combos by the pinned chart range appropriate to its exact
  situation (RFI for unopened seats UTG/HJ/CO/BTN/SB; the vs-open value/bluff/
  call sets facing an opener; BB = all 1326 combos, matching the deployed BB
  check-option baseline). These conditional weights never overwrite the
  unconditional deal and are reported separately. vs-open/3-bet continuations
  do not modify the unconditional dealing weights.
  Runout cards are uniform conditional on the board and ALL 2N hole cards,
  including seats that later fold (all hole cards are removed from the deck at
  deal time; 2p precedent heads_up_solver.cpp:151-153).
- Position identity: every GameDef is canonicalized before tree build by
  rotating seats so seat 0 is the button. Tree actor indices are therefore
  button-relative, and one trained policy serves every permutation. The
  permutation driver rotates real assignments across the seed list with
  balanced position coverage (an assertion verifies each seat occupies each
  canonical position equally). The canonicalization is pinned by a
  round-trip equivalence test against unrotated builds.
- Seat->chart-position vocabulary ({UTG,MP,HJ,CO,BTN,SB,BB} per seat count,
  with the n=3/6/8 bucket switches at decision.cpp:38-55) is a committed
  mapping table and is proven identical to the deployed adapter (F7).

### R3b. Flop-geometry matrix (Revision 2; resolves algorithm blockers 1 and 3)

A rooted AbstractTree is not parameterized by pot or stacks — the root
GameDef's concrete contributions, pot, and per-seat stacks are baked into
every node index and into the chip totals in node.actions
(abstract_tree.hpp:88-115; game_definition.hpp:75-125). Limped pots,
single-raised pots, 3-bet pots, and 4-bet-call pots, crossed with the set of
seats that reaches the flop and the seated count, are therefore DISTINCT
artifacts; one tree per seat count cannot serve the simulator's flops, and R7
action translation cannot bridge a pot-geometry mismatch. The stage therefore
operates on an enumerated, frozen geometry matrix:

1. Enumeration is by deterministic exhaustive preflop ROLLOUT, not by
   hand-picked cases, and the per-seat ACTION SUPPORT SOURCE is explicit.
   Candidate artifacts are queried only where the composed candidate acts
   postflop, so the matrix must cover the flop geometries reached by:
   (i) every profile seat playing the pinned charts
      (evaluatePolicySourced, riverGtoOn=false); and
   (ii) ONE designated deviator seat playing the full R8 deviation grid at
      each of its preflop decisions, with every other seat on chart support,
      iterated over every canonical deviator position relative to the button
      (seat permutations collapse under canonicalization).
   The uniform-random and previous-stage all-seat reference measurements need
   NO candidate artifacts: in those profiles every seat is the reference, the
   candidate is never queried, and settlement is exact on the exact game.
   The pinned charts are deterministic for a fixed hand (one action per Ctx;
   postflop-only rng draws do not occur preflop), but DIFFERENT hole cards map
   to different actions: the chart support at a preflop state is computed from
   the range memberships (rfi / vs value,bluff,call / four-bet sets) and the
   <=12bb jam/fold branch (decision.cpp:76-138). Both sources emit a set of
   DISTINCT (action kind, chip total) pairs; the enumerator branches over
   exactly that union. Kind alone is insufficient: one state can emit two
   different raise totals — short-stack jams go to raiseMax while ordinary
   members get the fixed sized open (2.5+1.5*limpers BB), and 4-bet value
   jams (raiseMax) while 4-bet bluffs use 2.2x the call. Fold and check carry
   total zero; calls carry the state's call amount. The deviation grid is the
   finite R8 raise set (declared pot fractions including min-raise and
   all-in), so the rollout set stays finite and card-independent; the
   enumerator never samples cards for this step. Folded seats' blinds/calls
   remain in the pot exactly as the rules settle them. If the cap test in
   step 3 fails because the deviation grid explodes the matrix, the grid may
   be coarsened ONLY in a declared revision whose final grid is frozen and
   named alongside the estimate (the quantity is explicitly an estimate over
   that declared menu, R8); chart-support geometry may not be coarsened away.
2. Each distinct flop state is canonicalized (button rotation, R3) and reduced
   to its geometry signature: player_count, per-seat remaining stack, street
   contribution, total pot, and live-seat set relative to the button. The
   concrete flop CARDS are deliberately NOT part of the signature: the public
   tree topology is board-value-independent (chance degrees are 49 then 48 for
   any three-card root; the pinned fixture uses one representative board), and
   board identity enters training and lookup only through the L2 card bucket
   and the sampled joint deals. Two flops with the same signature share one
   GameDef root topology and one artifact regardless of their cards; the
   enumerator emits the deduplicated signature list with the originating
   preflop action line(s) for audit. To keep regret rows from conditioning on
   the representative board, TRAINING samples the three root board cards per
   iteration uniformly from the deck conditioned on that iteration's joint
   holes (the R5 dealer supplies holes + root board together as the deal),
   while the single materialized tree for the signature supplies node indices
   and abstract menus — the MCCFR walk advances a GameState carrying the
   sampled board and aligns it to the topology by construction order. Index
   alignment at chance nodes is ordinal, not by card identity: chance children
   are ordered by card id in the representative tree, so the walk maps a
   sampled card to the child with the same ordinal k among the conditional
   deck sorted by id (the post-chance ACTION subtree is card-value-
   independent, so every k selects the same action topology). Action nodes and
   terminal leaves therefore share one index space across sampled boards. The
   ordinal mapping is pinned by a test: walks with identical action lines on
   two different sampled boards visit identical action-node index sequences,
   and the k-th conditional card round-trips. Root-board sampling is declared
   as part of the abstraction's identity and named with the translator in the
   table's confound label.
3. Every signature is built under the R6 coarse ActionAbstraction at its seat
   count and measured (nodes, action/chance/terminal counts, infoset-row
   estimate, charged retained bytes) against the unchanged TreeLimits caps
   (2,000,000 nodes / 1 GiB; never raised). If a signature misses a cap the R6
   menu escalation coarsens the schedule and the ENTIRE matrix is rebuilt and
   re-measured; the frozen config records one menu schedule per signature if
   escalation diverges.
4. The frozen matrix (signatures, action lines, counts/bytes, menu id, and the
   exact artifact key each simulator flop must select) is committed in the
   config directory before training and content-hashed; R10's confirmatory
   gate hashes it together with the seed list.
5. Simulation-time lookup is total and typed: the simulator computes a flop's
   signature and MUST find its artifact; an uncovered signature throws a
   typed `stage6_geometry_uncovered` error (never a fallback tree, clamp, or
   skip), because a silent omission would bias the measured profile.
6. Coverage of the enumerator itself is pinned by a test asserting, per seat
   count, that (a) every generated line terminates legally under GameState
   transitions; (b) a fixed sample of full-hand simulator runs for EVERY
   measured profile maps to enumerated signatures with zero uncovered events,
   and in particular best=true phase-1/phase-2 deviator rollouts (one
   designated deviator seat exercising the full R8 preflop deviation grid,
   opponents chart) produce zero uncovered flop signatures; (c) the per-state
   support computation returns distinct CONCRETE (kind, total) branches,
   pinned on the raises==2/heroWasRaiser node where fourValue combos map to a
   raiseMax jam and fourBluff combos to the sized 2.2x total
   (decision.cpp:125-130): the two totals must appear as separate branches in
   the matrix, and jam-vs-sized lines must produce distinct signatures; (d)
   the matrix equals the union of chart-support reach and deviation-grid reach
   asserted against an independent brute-force rollout on the smallest seat
   counts.

### R4. Baseline pin (F4-F8)

Stage-start evidence records, in the plan and the table header:

- Full 40-char SHAs a9584b4 and b430a09, with the statement that the pinned
  baseline is only defined at 10 seats after b430a09.
- Exact file list: engine/src/poker/charts.cpp,
  engine/include/bs/charts.hpp, engine/src/policy/decision.cpp, plus decision
  dependencies engine/include/bs/equity.hpp, eval.hpp, range.hpp.
- A deterministic digest generator (registered CTest linking bigshark_poker),
  SHA-256 over a canonical serialization of: the raw spec strings and the
  sorted post-expansion Range169 sets for all 24 chart fields, each labeled by
  canonical field path (rfi per position; vs per opener bucket
  value/bluff/call; fourValue, fourBluff, vs3Call, vs4Continue), and the 169
  entries of preflopPctTable in key order (it drives the <=12bb jam branch at
  decision.cpp:76-85). The digest value is recorded before training starts.
- Byte-identical decision gate: a committed corpus covering seats
  {2,3,6,7,9,10}, all streets, raises 0/1/2, multiway branches, and
  Monte-Carlo-sensitive spots, each with a fixed deterministic seed; every
  state is decided both through the deployed string path
  (v0_json parseRequest -> evaluatePolicySourced) and through the
  GameState->Ctx adapter, asserting identical action AND amount. An
  adapter-produced action not present in GameState::legal() is a typed harness
  failure, never a clamp. The existing 156-decision replay
  (node bin/replay.mjs: 156 / 0 illegal / 0 JS fallbacks), test_heads_up_preflop,
  and test_v0_protocol are named and run unchanged in the same gate.
- Stochasticity is fully pinned: the Ctx.seed for each corpus/table decision
  is a deterministic SplitMix64 function of (seed, seat, decision index). The
  harness baseline sets riverGtoOn=false (so no figure depends on a
  HiGHS-present build; exact-vs-DCFR build dependence is removed), and the
  RiverBackendHint is recorded in evidence.

### R5. Scalable joint dealer (A6)

A new dealer in bigshark_solver (R2 item 3a) implements the SAME construction the
enumeration-bound sample_joint_deal proves exact, minus its O(D) table-index
lookup (multiway_sampler.cpp:135-158): build one marginal cumulative-weight
table per seat over its fixture range, then per deal independently propose one
combo per seat via the pinned unbiased weighted selection
(heads_up_solver.cpp:369-405), and ACCEPT only when no card is duplicated
across seats or the board; on any conflict discard the ENTIRE proposal and
restart from seat 0. A positive-weight proposal is accepted with probability
proportional to the product of its per-seat weights, so the restart-on-conflict
construction (not conditioning) yields exactly the joint distribution
proportional to the product of range weights conditioned on mutual
compatibility (RFC 0006:192-195). The forbidden construction is different:
sampling seats independently and renormalizing only the LAST seat's pool
conditioned on the others, which changes every earlier seat's marginal; the
restart sampler restarts symmetrically and its marginals are exactly those of
the enumerated product-conditional joint table (which under nonuniform
per-seat weights are NOT the raw marginals — see the validation below). Worst-case acceptance at ten seats with full 1326-combo
uniform ranges is about 1.8 percent (~57 restart attempts per accepted deal),
measured, so no support enumeration is ever required; with sparse chart ranges
the rate is lower and the measured rate is reported. Validation on the small
game: empirical JOINT frequencies must match enumerate_joint_deals within a
declared tolerance — this is the primary distribution test, and it is the only
correct equality claim under nonuniform weights, because restart-on-conflict
acceptance shifts each seat's marginal by the suffix completion mass of the
seats drawn after it. The balanced-marginal assertion (each seat's empirical
marginal equals its declared weights) is therefore scoped to a symmetric
uniform-weights control fixture where every combo blocks exactly the same
number of opponent combos and the equality is exact; in every other fixture
the asserted comparison is empirical marginals vs the marginals derived from
enumerate_joint_deals. A last-seat-renormalization control must produce a
measurably different joint distribution and is rejected. Per-deal
normalization and zero-duplication are asserted on every accepted deal. RNG
streams are caller-owned SplitMix64, deterministically derived per (seed,
public node, role, phase).

### R6. Candidate trainer (A4, A5, F13)

- Sizing measurement FIRST: for EVERY flop geometry signature in the frozen
  R3b matrix (not one game per seat count), build the declared coarse
  ActionAbstraction (AbstractionId published; initial menu: fold/check/call
  backbone plus one bet and one raise per street over a small declared
  pot-fraction schedule plus an all-in edge; card abstraction
  CategoryTiersV1 postflop) rooted at that signature and record node counts,
  infoset-row counts, and charged retained bytes against TreeLimits. If any
  signature misses its predeclared caps (max_nodes 2,000,000 / max_bytes 1 GiB
  defaults are NOT raised for training), the menu is coarsened per the declared
  escalation order in R3b and the ENTIRE matrix is rebuilt and re-measured;
  the menu id per signature is frozen before training. A build that hits a cap
  reports tree_resource_exhausted typed, never a truncated tree (RFC:466-468).
- Artifact and infoset key: one artifact per frozen geometry signature. Within
  an artifact the infoset key is materialized TreeNode::index + the acting
  seat's own L2 card bucket; globally a row is addressed by (geometry
  signature hash, node index, bucket). Row actions are literally node.actions;
  no key derivation from historyless state exists or is attempted.
- Recurrence: external-sampling MCCFR (Lanctot), applied per geometry root
  with its rooted flop (postflop training only; preflop is the pinned composed
  source, R1-D2). One iteration = N traverser sweeps, one joint conditional
  board-and-private deal per sweep through the R5 dealer; at chance nodes
  sample one conditional card; at each of the N-1 non-traverser seats sample
  one action from that seat's current regret-matched policy; at traverser
  nodes enumerate all abstract actions; sampled regret
  r[I,a] += v(child_a) - v(I) unweighted; regrets are positive-clipped after
  update (RM+); the reported strategy is the UNIFORM average strategy
  (visit-weighted action sums), not CFR+'s linear-weighted average. Claim: at
  most convergence to a coarse correlated equilibrium of the abstract
  imperfect-recall game; never a Nash equilibrium claim and never a zero-
  NashConv claim (RFC 0006:202-208).
- Per-geometry iteration counts and TrainingLimits
  (max_nodes, max_information_sets, max_depth, max_bytes, wall time) are
  predeclared constants in the frozen config; the total local budget is the
  sum over the matrix and is reported as such. No game-copy/footprint
  accounting constant is rebased (RFC:439-443). RFC 0006:197-199 gate: on the
  enumerated 3p game the sampled multiway average must match full
  average-policy updates within a pinned tolerance before scaling.
- Output: one frozen artifact per geometry signature (infoset table +
  AbstractionId + translator id + training config), content-digested before
  confirmatory evaluation. No retraining after confirmatory results.

### R7. Exact<->coarse translation (A9)

A deterministic, versioned translator, unit-tested against LegalActions:

- coarse->exact: fold/check/call map directly; an aggressive total maps to the
  nearest legal integer target in the inclusive [raiseMin, raiseMax] interval
  (reuse the v1 actionIsLegal interval test), ties break to the SMALLER total;
  when two coarse targets translate to the same exact action their probability
  mass merges; if no aggressive action is legal, aggressive mass follows a
  declared rule (shift to call; fold mass unchanged).
- exact-history->coarse path: when the candidate acts as an OPPONENT inside
  the exact traversal, each past exact action is replayed through the
  as-of-that-node translator to locate the coarse node for the row lookup.
- The translator id is part of policy identity; every exact-game figure names
  it as a declared confound: the measured quantity is NashConv of the
  TRANSLATED candidate, and unseen exact sizings carry zero candidate mass by
  construction.

### R8. Two-phase deviation estimator (A7, A8, F9)

- Deviation menu: the exact raise continuum is not enumerable, so the
  deviator maximizes over a declared finite deviation menu —
  fold/check/call plus a fine raise grid (pot-fraction steps denser than the
  training menu, always including min-raise and all-in). This makes the
  quantity a unilateral-deviation estimate over a declared superset menu; it
  is labeled as such next to "estimate, no convergence guarantee".
- Phase 1 (learn, pilot/training seeds): at deviator nodes enumerate deviation
  actions and recurse each subtree, sampling chance and the N-1 fixed-profile
  opponents, using sibling-shared streams (CRN) for prefixes; accumulate
  Q_i(I,a). Freeze BR-hat_i = argmax_a Q_i once. Naive per-episode noisy
  max is explicitly rejected (upward Jensen bias); frozen-argmax on fresh data
  is unbiased for the gain against BR-hat and consistent for the gain against
  the true BR.
- Phase 2 (measure, confirmatory seeds only): BR-hat is now a fixed policy;
  paired best=true/false rollouts exactly as heads_up response()/evaluate()
  (heads_up_solver.cpp:481-546, 715-720), sharing deal, runout, and opponent
  draws. g_{i,s} = u_i(BR-hat_i, sigma_-i) - u_i(sigma) per seed s.
- Terminals settle through the exact N-vector paths (settle_fold /
  settle_showdown with live-seat-ascending holes; game_definition.hpp:194-206).
- Statistical unit: one seed = one vector-valued replicate (g_{1,s}..g_{N,s})
  in declared units (chips per pot, per hand). Per-seat CIs: Student-t on the
  S replicates (t implementation validated against tabulated critical
  values). NashConv CI is computed on the scalar Y_s = sum_i g_{i,s} —
  never by summing per-seat half-widths. Candidate vs baseline: PRIMARY
  statistic is the paired difference d_s = Y_candidate,s - Y_baseline,s on
  identical draws with its own CI; the RFC-literal "beats" rule (non-overlap
  of the two marginal 95% intervals, RFC:596-604) is ALSO reported, and the
  word "beats" is used only when that literal rule separates. Primary claims
  are at 7 and 10 seats with Holm correction over the two comparisons; the
  other counts are descriptive. All per-seed values and spreads are published
  beside the intervals. Estimation error (MC vs exact on the oracle) and
  numerical error (floating residual) are reported separately per RFC
  0006:203-205.
- Estimator gates before any large-table number:
  1. Exact 3p oracle agreement (R9) within tight tolerance, with a residual
     bias test whose CI includes zero.
  2. Two-seat cross-check: the sampled estimator against the existing exact
     heads-up ExactEvaluation::nash_conv on the same rooted game, both
     definitions reported where both are computable.
  3. Same-build 1e-12 repeatability; sampler zero-overlap/normalization
     checks.

### R9. Exact 3p oracle (A10)

- Primary fixture: the pinned three_seat_rooted({1,1,1}) identity tree
  (test_abstract_tree_fidelity.cpp:251-263; 102,827 nodes / 28,824 action /
  941 chance / 7,206 fold / 65,856 showdown — inside default TreeLimits),
  with declared 2-4 combo per-seat ranges chosen to (a) force card-overlap
  conflict rejection, (b) allow an all-three-to-river line. Complete
  3-component utility vectors asserted at every leaf with sum_i u_i = 0 for
  both fold and showdown leaves; RFC 0006 seeds 1/17/43, permutation, and
  1e-12 repeatability.
- Unequal-stack variant three_seat_rooted({1,2,2}) to exercise side-pot
  layers and multi-raise geometry (s=1 menus collapse nearly to
  fold/call/all-in); node count measured and pinned, offline oracle caps
  raised only for this validation binary if needed.
- The oracle serves two gates: estimator bias (R8) and sampled-vs-full CFR
  averaging (R6).

### R10. Seeds and pre-registration (F1, F2)

- Pilot list: a committed fixed set of constants (published in the harness
  config before any run), disjoint from the confirmatory list, used ONLY for
  variance estimation, the sample-size decision, and any hyperparameter
  choice.
- Predeclare level 95% and target half-width h = 0.05 pot for the paired
  NashConv difference; pilot variance fixes N per fixture/seat. The
  confirmatory list (values, including 1/17/43, count, declared space, and the
  per-purpose stream derivation) is then committed in a frozen config file and
  hashed together with the R3b geometry matrix and the fixture/range configs;
  the harness refuses to run a confirmatory table unless every hash matches
  its committed value. Every figure is reproducible from seed + frozen
  fixture + frozen geometry matrix alone.
- Train/test separation: no abstraction, budget, bucket, or translator change
  after a confirmatory run; a negative or indistinguishable result is
  reported as-is, the artifact is neither promoted nor enabled, and stage 7
  proceeds on architecture alone (RFC:618-627).

### R11. Reference opponents and published table (F11, F12)

- Every seat-count row reports, on identical fixtures/seeds: the composed
  abstract candidate in EVERY seat (permuted), the pinned baseline, fixed
  uniform-random, and a previous-stage reference named per count: at 2 seats
  the RFC 0004 blueprint policy if loadable in the behavior interface, else
  the literal entry "no previous-stage policy exists"; that same literal entry
  for all counts >2.
- Columns: per-seat gains and summed NashConv with 95% CIs, paired-difference
  CI vs baseline, RFC-literal separated/intervals-overlap verdict, estimator
  and translator ids, "estimate — no convergence guarantee".
- Cost columns, one row per seat count (10 mandatory): frozen policy resident
  bytes via the counting-allocator precedent, train wall clock with hardware
  and thread count declared, per-decision candidate latency at 10 seats over
  fixed states; estimator peak bytes reported as informational.

### R12. Build sequence (each step has pinned tests; no step may merge on a later step)

1. Chart digest generator + recorded stage-start pin evidence + preflop
   rollout geometry enumerator and the frozen, cap-tested R3b geometry matrix
   (per-signature node/infoset/byte measurements) + frozen fixture/range/menu
   configs.
2. bigshark_behavior + baseline/uniform adapters + GameState->Ctx adapter +
   byte-identical corpus gate + named baseline tests.
3. Scalable restart-on-conflict joint dealer in bigshark_solver +
   distribution-match tests vs enumerate (joint-frequency primary,
   uniform-control marginals, renormalization negative control).
4. Full-hand N-seat simulator (with total typed geometry lookup) + exact 3p
   oracle fixtures (both stack shapes) with conservation/full-vector
   assertions + R3b enumerator coverage test.
5. Two-phase estimator + stats module + oracle agreement + 2p exact
   cross-check + repeatability.
6. Translator with legality/projection tests.
7. MCCFR trainer over every frozen geometry signature + sampled-vs-full
   averaging gate on the 3p oracle + per-signature frozen artifacts.
8. Pilot -> freeze confirmatory list/matrix hashes -> frozen artifact digest
   -> confirmatory table -> markdown report with every column and the
   negative-result framing.

End state: one stage commit after the full gate matrix and independent
approval review of the IMPLEMENTATION (separate from this design approval).

### R13. Answers to Revision 0's questions (resolved)

- Q1: materialize the coarse tree keyed by TreeNode index + own bucket;
  streaming rejected (no derivable key, and it does not escape the
  regret-table memory wall).
- Q2: estimator unbiased only in the two-phase frozen form (R8); variance
  unit is the per-seed vector, NashConv CI on its sum, paired difference
  primary.
- Q3: exact-game evaluation correct and required; feasible only WITH the
  declared translator (R7), which is part of the measured policy's identity.
- Q4: candidate mandatory, winning optional; no smaller deliverable satisfies
  RFC:556-557/684-690.
- Q5: targets and guards as R2; graph acyclic; offline-only enforced by four
  guards.

### Stage 6 stage-start baseline pin evidence (R4, recorded 2026-09-23)

- Baseline commit (policy/heuristic at stage start):
  `a9584b4df7466287a121938b56ae4942f44671ca`
- Ten-seat baseline-defining equity precursor:
  `b430a099f2da565cca52f3e003ad872dc19b2d5c`
- Pinned source files: engine/src/poker/charts.cpp,
  engine/include/bs/charts.hpp, engine/src/policy/decision.cpp, plus decision
  dependencies engine/include/bs/equity.hpp, eval.hpp, range.hpp.
- Parsed-chart content digest (SHA-256 over the 24 sorted expanded range sets
  + raw specs + the 169-entry Chen percentile table), pinned in
  test_stage6_chart_digest:
  `cb2da0d1b99f6e3ef1912fcd83c14234dc71c118eb954fe3da3d875ef6898bda`
  Proven mutation-sensitive (removing one RFI token changes the digest and
  turns the golden test red).
- Byte-identical corpus gate test_stage6_adapter_corpus: 63 decision points
  across seats {2,3,6,7,9,10}, preflop raises 0/1/2 and rooted flop
  snapshots (non-PFA and PFA), three hole sets each; adapter-derived Ctx and
  the deployed parseRequest path return identical action AND amount, and the
  shared decision is legal in GameState. The gate proved itself red while it
  was being built: it caught two adapter derivation errors against the real
  platform normalizer (v0-normalizer.ts) — raises/limpers/openerPosition are
  preflop-only fields on later streets, and heroWasRaiser/heroPreflopAggressor
  are a single "last raise across every street is this seat" boolean.
- Offline boundary: bigshark_behavior (poker-only interface + uniform
  reference) and bigshark_stage6_eval (adapter, baseline, digest). Three
  guards all proven red-green: configure-time transitive-link assertion over
  service/v0/v1/host; a -MMD resolved-include shell guard (compile_commands
  flags per source); an nm scan of a v0+v1 link-negative binary.
- The stage-4 L3 guard had been passing vacuously (a ../../.. root
  overshoot); fixed in precursor commit
  `9d6e5d58f600abce5d1a2f62d7bfd4a78be62edf`, independently reviewed and
  approved.

### Stage 6 implementation progress (R12), 2026-09-23

- Step 2 DONE: bigshark_behavior target (BehaviorPolicy interface over
  concrete legal poker::Actions, declared finite behavior menu,
  UniformBehaviorPolicy, sample_distribution); bigshark_stage6_eval target
  (GameState->Ctx adapter, pinned BaselineBehaviorPolicy, SHA-256 chart
  digest). test_behavior_policy pins menu ordering/interval/dedup. The
  byte-identical corpus gate test_stage6_adapter_corpus pins 63 decisions
  (seats {2,3,6,7,9,10}, preflop raises 0/1/2 + rooted flop PFA/non-PFA),
  adapter vs deployed parseRequest equal on action AND amount, all legal;
  while being written it caught two real adapter semantic errors that were
  corrected against the live normalizer (raises/limpers/openerPosition are
  preflop-only; the two hero-raiser flags are one all-street last-raise
  boolean). test_stage6_chart_digest pins golden
  cb2da0d1b99f6e3ef1912fcd83c14234dc71c118eb954fe3da3d875ef6898bda,
  proven mutation-sensitive.
- Offline boundary: three guards all proven red-green — configure-time
  transitive-link assertion, -MMD resolved-include shell guard, nm scan of a
  v0+v1 link-negative binary.
- Step 3 DONE: sample_scalable_joint_deal in bigshark_solver (independent
  marginal proposals + full-restart rejection; no joint-support
  enumeration). test_multiway_sampler extended with: joint-frequency match
  vs enumerate (nonuniform, <0.01), determinism, symmetric-uniform marginal
  equality (1326 combos x 3 seats), a hand-computed last-seat-renormalization
  negative control (correct 3/4 vs renormalized 1/2, gap 1/4), board
  filtering and the attempts counter. Release and ASan green.
- Precursor commit 9d6e5d5 fixed the vacuous stage-4 L3 guard.
- NEXT: R12 step 1 flop-geometry enumerator + sizing (and the frozen config
  artifacts); then step 4 simulator + exact 3p oracle.

### Stage 6 Revision 3 (2026-09-23): geometry bucket finding — FOR INDEPENDENT REVIEW

Measured while implementing R12 step 1. The implemented enumerator
(enumerate_chart_flop_geometries, chart-only reach, exact concrete chip
signatures per R3b) produced this exact-geometry matrix:

| seats | exact geometries | (bb-pot x live-count x seats-behind) buckets |
|-------|-----------------:|--------------------------------------------:|
| 2     | 2                | 2 |
| 3     | 12               | 9 |
| 6     | 732              | 63 |
| 7     | 2,478            | 91 |
| 9     | 25,731           | 150 |
| 10    | **80,438**       | **184** |

Revision 2's "tens-to-low-thousands per seat count" assumption held only up to
about 7 seats. At 100bb/1-2 the exact signature (per-seat remaining stack,
per-seat contribution, exact pot, exact live subset) is essentially unique:
87-273 distinct pots and the fold subsets fan out, so at 10 seats there are
80,438 exact geometries. Building and training one materialized coarse tree
per exact geometry is structurally infeasible under the declared local budget.

Proposed Revision-3 change (the standard abstraction this design already
commits to in principle): QUANTIZE the geometry key. Strategy is pot-scale
invariant, so a rooted artifact is identified by
  (pot rounded to whole BB, count of live seats, count of live seats with
   postflop stack behind)
rather than exact per-seat chip vectors. The representative rooted GameDef for
a bucket derives per-seat stacks/contributions from the bb pot (even split of
dead money among the live count; each acting seat the same effective
stack-behind). Collapsing exact signatures onto this key gives 184 buckets at
10 seats — a trainable matrix under the local budget. Exact chip action at
evaluation time still goes through the R7 translator; the bucket tree is the
abstraction, the discrepancy is a declared confound exactly like card
bucketing.

Open questions the independent review must rule on:
1. Is collapsing dead-money distribution to an even split, and per-seat
   stack-behind to the counted set, a faithful abstraction, or does folding in
   WHO put the money (positions) lose information the card/strategy needs?
   (Mitigation already present: button-canonical position; but bucket trees
   would no longer distinguish, say, 3 live with the opener UTG vs on the
   button at equal pot.)
2. Should the bucket key retain the opener-position class in addition to the
   three fields above (small increase in count), given the postflop heuristic
   itself does not read opener position but the candidate might benefit?
3. Does training on a representative even-split pot while evaluating on exact
   pots break the R3b "simulator flop must find its artifact; uncovered throws"
   guarantee, or is every exact geometry guaranteed to map to a bucket (it is,
   by construction — bucketing is total)?
4. Is 184 x per-bucket-tree still within the declared local training budget,
   given measured per-tree node counts (step 1 tree sizing must be run on the
   184-bucket matrix, not the exact one, before approval)?

No implementation beyond the read-only enumerator and its measurement binary
is committed to this change; the trainer/simulator stay unwritten until the
bucketed signature is approved.

### Stage 6 Revision 3 APPROVED-WITH-CONDITIONS (independent review, 2026-09-23)

The fresh geometry-bucketing reviewer verified the measurement (reproduced
80,438 at n=10 / 184 buckets; confirmed the fan-out is pot x fold-subset, not a
dedup or termination bug) and returned APPROVE-WITH-CONDITIONS. The direction
(pot-scale bucket key) is accepted; the trainer is not built until these
conditions are met. Recorded decisions:

- **The 184 is mostly non-actionable.** 160 of 184 buckets are all-in-at-flop
  with zero acting seats (n=6: 51/63); at most 24 n=10 buckets need trained
  postflop trees. All-in buckets are typed "no candidate queried" (pure
  runout settlement), NOT uncovered and NOT trained.
- **Representative GameDef = a REDUCED live-count rooted game.** The first
  attempt (rooted_def_for in the benchmark) crashed with "positive pot layer
  has no eligible winner" because GameDef has no folded-seat field: a rooted
  game cannot carry folded players with live stacks. The representative for a
  bucket is a new GameDef whose player_count is the bucket's LIVE count, seats
  reindexed clockwise from a canonical button, equal acting stacks set to the
  MINIMUM acting depth among the bucket's members (never deeper — that emits
  illegal targets R7 would have to clamp; never shallower — that drops legal
  sizings), dead money reconciled to an exact pot the 3+ validator accepts
  (odd chip declared), street commitments zeroed.
- **No-mix guard (pinned test):** a bucket must not mix an all-in-for-less live
  seat with acting members, nor acting-depth classes beyond a declared
  tolerance; the ≤2-chip folded-blind redistribution and ≤1bb pot rounding are
  named confounds. Measured acting-stack spread in n=3 deviation reach was
  <=1 chip, and equal-97bb equal-contribution is structural at the uniform
  100bb fixture, so the collapse is near-lossless FOR THE FROZEN FIXTURE only.
- **Opener/last-raiser position is NOT a key field now.** Own relative seat is
  retained (node index encodes it). Condition: train over the actual
  member-geometry mixture and run a predeclared pilot sensitivity test (split
  vs merged opener class at 6/7 seats on a CI threshold); promote to a fourth
  key field only if it fires.
- **Totality:** stage6_geometry_uncovered fires on key-not-in-FROZEN-set;
  all-in buckets are "no candidate queried". The deviation-reach buckets
  (n=3 already grows 9->39) must be enumerated at every seat count and unioned
  into the frozen matrix BEFORE any budget/sizing claim.
- **Geometry quantization is a NEW AbstractionId.** Node/sizing counts measure
  cost, not error. The quantizer is a named/versioned AbstractionId parameter
  in the frozen rehashed matrix, superseding Revision 2's "chart-support
  geometry may not be coarsened away" under the measured-infeasibility
  rationale. Before the trainer: (i) structural menu-displacement rate across
  all buckets (zero-displacement fraction, no training needed); (ii) on the
  n=3 oracle where all 12 exact geometries are feasible, bucketed-vs-exact
  policy/deviation-gain delta with CIs. Without (ii) the table may still be
  published but only at guarantee level approximate, so labeled.
- **R7 alignment pin:** ordinal node alignment between representative tree
  and exact-state replay for sampled lines, per bucket.

Revised R12 step 1 therefore: enumerate exact reach (chart union deviation) at
all seats -> quantize to the conditioned bucket key -> split actionable vs
all-in-runout -> build REDUCED representative GameDef per actionable bucket
(min-depth equal stacks, reconciled pot) -> tree sizing against caps ->
no-mix + node-alignment + menu-displacement tests -> freeze/rehash matrix with
quantizer id. Then step 4 (simulator/oracle) and the n=3 error measurement
precede the trainer (step 7).

### Stage 6 measured wall (2026-09-23): deep postflop public tree cannot be
### materialized even with a passive-only menu

While validating Revision 3's reduced representative trees
(bigshark-stage6-geometry-benchmark --trees), EVERY actionable representative
exhausted the caps. Isolation measurements on 2-seat rooted-flop games
(board 2c3d7h, even contributions), with the most aggressive-free menu
possible (passive only: fold/check/call, no bets/raises):

| stack/chip | nodes |
|-----------|------:|
| 2  | 31,124 |
| 4  | 164,416 |
| 8  | 1,168,352 |
| 16 | 9,286,720 |
| 32 | >24 GiB (exhausted) |
| 64/194 | >24 GiB |

Control: the pinned 3-seat stack-1 identity tree is 102,827 nodes because all
players are immediately all in and there are no postflop action rounds.

This is the public CARD fan-out, not the action fan-out: the materialized
tree enumerates every public-card combination (49/48/47 conditional runouts)
under each action history, and with even one check/call round per street the
cross product explodes. Coarsening the ACTION menu does not help (passive-only
still exhausts), so this wall is independent of the R6 coarse action
abstraction and of geometry bucketing.

Consequence for the design:
- Revision 2's D1 fallback ("materialize the coarse tree; stream only if it
  misses the cap") is INFEASIBLE for deep postflop, not just at 7-10 seats but
  at TWO seats. The trainer must be streaming external-sampling CFR from the
  start (it never retains the public tree), as the Revision-1 algorithm lane
  recommended.
- The R3b per-bucket tree SIZING step and the R6 "measured coarse-tree size in
  caps" claim cannot be produced by AbstractTree for deep games. Sizing must
  instead measure (a) the STREAMING trainer's retained footprint (infoset rows
  visited, not tree nodes) and (b) wall time, which is what R11/R12 actually
  report anyway.
- The R3b enumerated matrix remains correct and needed for the set of flop
  CHIP geometries the simulator/translator must cover, but it must not be used
  to build materialized trees.
- The R9 3p oracle at {1,1,1} is unaffected (immediately-all-in, 102k nodes,
  fully enumerable). The {1,2,2} variant and any deeper oracle fixture must be
  checked the same way before use.
- This also bounds the n=3 bucketed-vs-exact equivalence error gate (Revision
  3 condition ii): the "exact" side at deep n=3 cannot be a materialized tree,
  so exact NashConv there must itself be sampled/MC, weakening the oracle
  comparison to MC-vs-MC. The only HARD exact comparison available is the
  stack-1 pinned 3p game.

This finding awaits independent design review. The streaming CFR trainer was
already the approved algorithm; what changes is (1) deleting the
materialize-if-fits fallback, (2) replacing tree-node sizing with
streaming-footprint sizing, (3) scoping the exact equivalence oracle to the
enumerable all-in pinned fixture and labeling every deep comparison estimated.

### Stage 6 wall CORRECTED after node-kind analysis (2026-09-23)

The earlier "deep postflop public tree cannot be materialized even passive"
finding was WRONG about the cause and overstated the conclusion. The
independent verifier stalled but flagged that the so-called passive tree still
contained 4,804 ACTION nodes, which led to the correction:

1. The L2 menu builder `build_menu_with_cover` ALWAYS seeds `{bounds.minimum,
   cap}` (a minimum bet and the all-in) even when a StreetSizes fraction list
   is empty (abstraction.cpp:185). There is no SizeSchedule value that removes
   aggressions, so my "passive-only" menu was actually min-bet+jam at every
   node — unbounded aggressive branching. That, not public-card fan-out,
   produced the 31k->9.3M->>24GiB growth with stack depth.

2. A TRULY passive tree (fold/check/call only; measured by a direct GameState
   DFS) is tiny and STACK-INDEPENDENT: 4,804 action nodes, 2,352 showdown
   leaves at every stack 8..194. Card fan-out alone (49*48*47 runouts) is
   only ~2.4k terminals — it is NOT the wall.

3. With a genuinely coarse action menu (passive + one pot-sized wager, at most
   one raise per street) measured by the same controlled DFS:
   - 2 seats: 140k nodes at stack16, 950k at stack194 — fits the 2M cap.
   - 3 seats: 102,827 at stack16; 9.06M at stack194 — materializable in memory
     but over the default 2M-NODE cap; use a raised cap or streaming.
   - 6 seats: 1.8M at stack16; >80M at stack194 — genuinely too large to
     materialize at 6+ seats deep, so streaming external-sampling CFR is
     required for the high-seat end (7/9/10) regardless.

Corrected design consequences (supersede the prior "measured wall" text):
- Add an abstraction capability to build a coarse menu WITHOUT forced
  min/cap: an explicit menu mode (e.g. a "declared fractions only" flag or an
  option suppressing the mandatory minimum/all-in seeds). Without it the
  candidate cannot be given the coarse one-wager menu R6 specifies. This is an
  L2 change with its own identity/zero-error consideration and independent
  review — it must not silently change the existing identity schedule.
- Materialized coarse rooted trees are viable for 2 seats (and shallow 3-6);
  the trainer can materialize there and must STREAM for 7/9/10 (and deep 3-6
  if node caps bind). The Revision-2 "materialize if it fits, else stream"
  structure is therefore CORRECT after all, conditioned on the new coarse-menu
  capability.
- R3b sizing must use the corrected coarse menu; the earlier capped counts are
  void.
- The R9 exact oracle: 2-seat and shallow-3 exact coarse trees are enumerable,
  restoring a stronger-than-MC equivalence gate at low seats; only the
  high-seat end is MC-estimated.

### L2 declared-only coarse menu implemented; wall verification CONFIRMED (2026-09-24)

Independent verifier agent aad1823b8870c442b returned **CONFIRMED** on all
three corrected claims:

- **A** `abstraction.cpp:185` unconditionally seeded `{bounds.minimum, cap}`;
  an empty fraction list still returned `check0 bet2 bet194`.
- **B** the truly passive DFS is exactly 4,804 action / 50 chance / 2,352
  showdown / 7,206 total nodes, stack-independent at every depth 8..194.
- **C** the shallow pins matched element-for-element (2p st16 = 140,498; 3p
  st16 = 102,827; 6p st16 = 1,808,828); 2p st194 ≤ 967k under every menu
  variant (fits the 2M cap), 6p st194 = 370,849,562 (unmaterializable). The
  ~950k/9.06M point estimates vary by one jam-reopen branch but never cross
  either bound.

The verifier prescribed the API, which is now implemented:

- `enum class CoverSeeds { MinAndCap, DeclaredOnly }` threaded into
  `build_menu_with_cover` through both `build_action_menu` and
  `build_multiway_action_menu` (default `MinAndCap`, so every existing caller
  is byte-unchanged).
- `ActionAbstraction::declared(schedule, CoverSeeds)` is a NEW factory minting
  identity `name="rfc0008-declared-coarse"`, version 1, with the seed rule
  serialized into `parameters` (`cover-seeds=declared-only`); MinAndCap and
  DeclaredOnly at equal fractions mint distinct digests, and neither collides
  with `rfc0007-pot-fractions`. The identity constructor, `identity()`,
  `identity_action_id()`, and the golden digest `0x422c245239c7a527` are
  untouched. The AbstractTree multiway path now calls
  `action.multiway_menu(...)` so the mode reaches tree construction.
- Coarse id measured: `rfc0008-declared-coarse:v1:84e07a99ff1ee719`.
- Tests: the golden digest and the equivalence node-count pins
  (kIdentityFlopNodes 2720 / kIdentityPreflopNodes 1156) still pass with zero
  rebaselining; a new `declared_only_coarse_menu` case covers the fresh id,
  empty-fraction passive menus (check-only and fold/call), exact {1/2,1/1}
  fraction-only targets with no min/cap, clamp-reaches-cap survival, and
  HU==multiway declared-only menu equality.

R3b was re-measured with the corrected declared-only menu (bets {1/2}, raises
{1/1}, no forced seeds). The geometry matrix is unchanged (2/9/63/91/150/184
buckets; actionable 1/3/12/15/21/24), and the materialization boundary is now
deterministic:

| seats | actionable buckets | materialized | capped (>2M nodes) |
| --- | --- | --- | --- |
| 2 | 1 | 1 (1,010,162 nodes) | 0 |
| 3 | 3 | 2 (each 1,010,162) | 1 (the live-3 deep `potbb9`) |
| 6 | 12 | 3 (all live-2, 1,010,162 each) | 9 (every live≥3 deep) |
| 7 | 15 | 3 | 12 |
| 9 | 21 | 3 | 18 |
| 10 | 24 | 3 | 21 |

Only heads-up (live=2) representatives materialize under the default 2M-node
cap; every live≥3 deep representative exceeds it (the representative always
uses the minimum = deepest 194 acting stack, a conservative bound). The
materialize/stream switch is therefore a deterministic pure function of
(live count, GameDef, abstraction id), as the verifier required: materialize
the exact full-width tree at live=2 (the R9 exact equivalence oracle), stream
external-sampling CFR for live≥3.

A real representative-construction bug surfaced when the 6-seat sizer first
ran these trees: the representative force-split the *rounded* bucket pot
evenly across the reduced live seats, inventing uneven contributions such as
`in{6,7,7}` for a pot whose real members are three callers of 6 plus a folded
1-chip small blind. Settlement rejects that (the 6-chip layer then has a single
contributor: "positive pot layer has no eligible winner"). In every real hand
the non-folded, non-all-in live seats reach the flop having called the SAME
preflop-close total. The representative now gives every reduced seat the
shallowest member's equal contribution and sets `pot = equal × live` — a
legal, realizable rooted game; the folded-dead/rounding residual is exactly
the pot-scale quantization the Revision-3 bucketed-vs-exact delta test must
measure, not chips to fabricate. A mutation-RED regression in
test_stage6_geometry materializes the folded-dead representative tree; under
the old even-split it fails on both the invented 7 and the settlement throw.

Status: full release build and 54/54 ctest green; the stage6 offline
boundary guards (configure closure, -MMD resolved-include, nm "6stage6"
scan) and tree link-negative pass; format applied. Awaiting INDEPENDENT
review of the L2 API + representative change before R12 step 4.

### Independent L2 review APPROVE-WITH-CONDITIONS; conditions resolved (2026-09-24)

Fresh reviewer (agent a568f90e9ae93082e, not the author) returned
**APPROVE-WITH-CONDITIONS, no P1**. It independently: reproduced the golden
digest in Python from the exact FNV-1a blob; proved only `declared()` can
produce DeclaredOnly and no deployed source calls it; probed every
declared-only edge (empty-target, clamp-to-min/cap, 32-guard skip); confirmed
the three id digests are pairwise distinct and unparseable so no round-trip
collision exists; scanned 121,760 chart + 12,457 deviation signatures across
n=2..10 and found ZERO live acting-seat unequal contributions or pot
mismatches (the equal-contribution invariant); mutation-tested BOTH offline
guards RED→GREEN; and reproduced the exact sizing (2p 1,010,162 fits, every
live≥3 capped).

Conditions, now resolved:

- **P2 (latent, unreachable at the uniform fixture):** the cross-member
  minimum-depth reduction borrowed a `first` sentinel reset only per member,
  so across the FIRST member's seats it overwrote the minimum with each seat
  and ended at that member's LAST stack (e.g. ascending {50,100} → 100). Fixed
  with a dedicated `have_min_stack` sentinel. Mutation-RED regression added
  (ascending {50,100} + {100,100} in one bucket must reduce to 50); reverting
  the sentinel fails it.
- **Nit:** removed the dead `acting_stack_min` computation in
  `bucket_key_for`.

Gate state after resolution: release 54/54, debug 52/52, ASan 52/52 (the one
ASan failure was a not-yet-built scan binary, since resolved), format clean,
offline guards green. The L2 coarse-menu + equal-contribution representative
is the settled foundation for R12 step 4.

### R12 step 4 implemented: full-hand simulator + exact 3p oracle (2026-09-24)

New offline components in bigshark_stage6_eval (nothing linked by the service
or protocols):

- `stage6/simulator.{hpp,cpp}` — `HandSimulator` drives one preflop GameDef to
  an exact terminal over per-seat `BehaviorPolicy`s. The five board cards and
  all 2N hole cards are fixed BEFORE the first action (the R5 joint dealer
  supplies both), so chance is predetermined and the loop only advances
  `Phase::Deal` via `after_card`; runouts are therefore conditioned on the
  full deal including seats that fold. It validates the deck (distinct/in
  range), re-checks every policy action with `LegalActions::contains` and
  throws `stage6_sim_error` on an illegal/non-distribution (never clamps),
  settles with the exact `settle_fold`/`settle_showdown`, and asserts the
  per-seat chip-utility vector is zero-sum at every terminal. A
  `GeometryCoverage*` makes flop lookup TOTAL: the first 3-card flop with
  ≥2 live seats computes `flop_signature(state)` and `require_covered`
  throws `stage6_geometry_uncovered` on a miss (no fallback/skip).
- `stage6/geometry.{hpp,cpp}` gains `flop_signature(const GameState&)` (one
  construction shared with the enumerator) and `GeometryCoverage`, a sorted,
  deduplicated exact-signature set with total lookup and a deterministic
  FNV-1a content hash for the frozen matrix.
- `stage6/exact_oracle.{hpp,cpp}` — `exact_deal_utility` (full recursion over
  the real L1 tree for one fixed joint deal: policy expectation at Action,
  uniform average over every runout card at Deal conditioned on board AND all
  holes, exact settlement at terminals) and `exact_oracle_utility` (enumerates
  the RFC 0006 joint table over small per-seat ranges and averages the
  per-deal expectations by the joint weights). No sampling; `OracleCounts`
  surfaces action/chance/fold/showdown/node and joint-deal counts.

Tests: `test_stage6_simulator` (new, registered as `stage6_simulator`) pins
2-seat fold and 3-seat showdown conservation with exact utility values,
seed/hand_id/board determinism, empty-vs-covered typed coverage, illegal-
policy rejection, coverage dedup/hash/membership, and:
- the R3b ENUMERATOR COVERAGE gate: 400 uniform-joint full hands with the
  pinned `BaselineBehaviorPolicy` in every 3-seat chair run under total lookup
  against `enumerate_chart_flop_geometries` (12 EXACT geometries); 21 hands
  reached showdown and none escaped, so the card-free enumerator matches real
  chart-baseline reach (non-vacuous);
- the exact 3p oracle at `three_seat_rooted({1,1,1})` and `{1,2,2}` with
  2-combo ranges that force card-overlap rejection (6 compatible joint deals,
  33,300 action / 264 chance / 10,836 showdown leaves), zero-sum expected
  vector; plus an aggressive {1,2,2} walk that explores a different tree than
  the check-down and a single-deal zero-sum check.

Release 55/55 green after format. Awaiting INDEPENDENT step-4 review and the
debug/ASan gates before R12 step 5 (two-phase estimator).

### Independent step-4 review APPROVE-WITH-CONDITIONS; conditions resolved (2026-09-24)

Fresh reviewer (agent a064bf1a1df5880eb, not the author) returned
**APPROVE-WITH-CONDITIONS, no P1**. It empirically verified every high-risk
claim and could not refute them: everyone-all-in preflop runs out all five
cards purely through Phase::Deal and settles exactly (2p u=[200,-200], 3p
chop); the oracle's used-card set matches L1/2p so it can never request a
board or folder card; no partial-flop/auto-runout loop or missed street;
joint weights do NOT double-count (a 2:1 weighted range reproduced the
weighted average with error 0.0); stochastic recursion is linear (50/50
mixture error 0.0); the pinned counts 33,300/264/10,836 independently
re-derived; dealing the flop leaves chip state byte-identical to the
preflop-close snapshot (measured), so flop_signature timing is exact; the
all-in flop does NOT skip total lookup (the matrix contains that signature);
zero-sum held across {1,2,2} side pots and all-in-for-less.

Conditions, now resolved:

- **P2-1 oracle NaN gap:** the oracle's manual probability check omitted
  `std::isfinite`, so a NaN mass made every guard false and poisoned the
  exact R9 value. Fixed to the canonical `!isfinite || <0` check used by the
  simulator and require_finite_distribution. Mutation-RED regression
  `test_oracle_rejects_nan` (reverting the check fails it).
- **P2-2 weak pinning:** the 33,300/264/10,836 oracle counts are now ASSERTED
  (not just printed), and a new `test_deviator_reach_covers_jam_hands`
  rotates one always-jam deviator across all three seats with the others on
  the pinned baseline for 360 hands under TOTAL lookup against
  enumerate_flop_geometries (78 deviation geometries); 67 called-jam
  showdowns exercise the all-in-at-flop/short geometries the baseline-only
  sample never reached. Higher seat counts (>3) remain for the step-5/7
  seat-curve work and are recorded as a deliberate scope boundary, not a
  defect.

Step 4 gate state: release 55/55; debug 53/53 and ASan 53/53 green before the
one-line NaN hardening, with the final ASan/debug rerun after that change in
progress; format clean; offline guards green. Step 4 is the settled base for
R12 step 5 (two-phase frozen best-response estimator).

### R12 step 5 foundation: exact best-response R9 reference (2026-09-24)

Before the sampled estimator, the exact R9 quantity it must reproduce was
added to exact_oracle: `exact_best_response_utility(def, ranges, policies,
traverser)` generalizes the oracle recursion with an optional BR traverser.
At every traverser ACTION node it takes the MAX over the declared finite
deviation menu (`declared_behavior_menu`: fold/check/call + the fine
pot-fraction grid including min-raise and all-in); opponent action nodes
follow their fixed policy; Deal averages uniformly over runout cards
conditioned on the full joint deal; terminals settle exactly. Ties resolve to
the first menu entry, so the exact BR is a deterministic reference policy
(not the biased per-episode max the R8 design explicitly rejects). The joint
average is shared with the profile expectation; the exact per-seat gain is
BR_utility[seat] − profile_utility[seat].

Test `test_exact_best_response_gain` on the enumerable {1,1,1} fixture pins:
for every traverser the unilateral gain is ≥ −eps and BR utility ≥ profile
utility; against jam opponents the BR vector is finite and zero-sum. The
sampled two-phase estimator (frozen Q-argmax phase 1, paired best=true/false
rollouts phase 2), the MC-vs-exact oracle agreement tolerance, the 2p
ExactEvaluation cross-check, and 1e-12 repeatability remain the next focused
increment — they are statistically subtle and get their own independent
review rather than being rushed onto this checkpoint.

### Exact BR review found omniscience P1; corrected to infoset-pooled BR (2026-09-24)

An independent review of the first exact-BR cut returned
APPROVE-WITH-CONDITIONS with TWO P1s:

- **P1-1 (critical):** the first BR maximized PER joint deal, E_z[max_a Q],
  which lets the traverser condition on opponents' unrevealed hole cards — an
  OMNSCIENT deviation. The reviewer built a strict 3p counterexample (a
  fold/call infoset whose preference splits across opponent holdings;
  per-deal value exceeded the best fixed infoset action by 2.57 chip/hand). A
  correct frozen estimator (which fixes one argmax per infoset) would have
  been wrongly flagged biased against that reference.
- **P1-2:** BR>=profile non-negativity requires the profile's action support
  to be contained in the declared deviation menu; the baseline can emit
  off-grid totals.

Correction (modeled line-for-line on the proven 2p `response()` reach-vector
walk, generalized to N seats):

- The correct quantity `exact_best_response_utility` groups joint deals by the
  traverser's OWN holding (own hand + public history = information set) and
  runs `br_group`, which carries a per-view (weight, reach) through ONE public
  GameState walk: traverser nodes force a single action across every pooled
  view and compare UNNORMALIZED summed values (max_a E_z[Q]); opponent nodes
  multiply each view's reach by that view's policy probability; deal nodes
  split reach over the runout cards available conditional on that view's FULL
  fixed deal; per-group unnormalized sums combine and normalize by total
  mass. Mass conservation is enforced at every node.
- The per-deal quantity survives only under the honest name
  `exact_omniscient_deviation_utility`, documented as the E_z[max_a Q] upper
  bound and explicitly NOT the estimator target.
- Non-negativity is documented as holding only for menu-contained profiles
  (check/call, always-jam).

Tests: menu-contained pooled BR >= profile; omniscient >= pooled everywhere;
and a multi-holding stacks-10 fixture where the information-set argmax splits,
asserting a STRICT omniscient-minus-pooled gap (measured 0.371601 chip/hand) —
empirical proof the pooled BR no longer peeks. Release 55/55 green; an
independent re-review of the pooled recursion and the ASan/debug gates are in
progress before any step-5 sampled estimator is built.

### Pooled-BR re-review APPROVED, no P1 (2026-09-24)

A fresh reviewer (agent aa7a952d861ad7f9a) returned **APPROVE**, verifying the
correction three independent ways: (1) a from-scratch brute-force enumerator
over EVERY pure deviation strategy (one declared-menu action per traverser
infoset keyed by own hand + public history) matched the library BR, omniscient,
and profile full 10-vectors to 1e-9 for all three traversers on a 24-deal 3p
game with a 0.6/0.4 stochastic mix AND under non-uniform 5:1:3:2/7:1/4:1:2
weights — proving the pooled value is achieved by an explicit legal strategy
(no omniscience, no factor-of-P(own-hand) normalization error); (2)
instrumented mass-conservation and zero-sum invariants at every node type over
~1.4M action nodes / ~2.5M terminals, clean to 1e-9/1e-7; (3) ASan/UBSan clean.
It confirmed chance conditions on board + all 2N holes (folders included),
views with identical own hand never split at a traverser node, and the strict
0.371601 gap is genuine pooling. One optional P2 (redundant avail-count
recompute per chance node; immaterial offline) left unfixed.

Gate state for the corrected exact reference: release 55/55, ASan 53/53,
debug 53/53, format clean. The exact R9 best response is now a sound target
for the R12 step-5 sampled two-phase estimator (frozen per-infoset argmax,
paired rollouts, MC-vs-exact agreement).

### R12 step 6 translator + step-5 foundations via Ultracode (2026-09-24)

A four-agent workflow (one writer + three read-only contract agents,
implementer separate from the later reviewer) produced:

- **Step 6 translator (implemented, release 56/56 then 57/57):**
  `stage6/translator.{hpp,cpp}` + `test_stage6_translator.cpp` in
  bigshark_stage6_eval. `TranslatorId rfc0008-coarse-to-exact:v1`;
  `translate_coarse_to_exact` does passive passthrough, nearest legal integer
  aggressive snap with round-half-DOWN ties to the smaller total, same-action
  probability-mass merge, aggressive->call/check/fold fallback when no raise is
  legal, full fail-closed re-validation (illegal/NaN/non-summing/unknown-id
  throw, never clamp); `project_exact_to_coarse_index` does same-type nearest
  index for candidate-as-opponent node location. Tests build REAL GameState
  legal intervals (preflop UTG [4,200], short-BB call-only no-reopen,
  check-only all-in). Independent adversarial review launched.
- **Step-5 deterministic foundations (implemented, 57/57):**
  `stage6/infoset_key.{hpp,cpp}` — InfosetKey = traverser's own SORTED pair +
  canonical length-prefixed public-history tokens (board + per-street
  seat/type/target triples); explicit vector key (no hash collisions); the key
  contains NO opponent/folder cards or undealt board, structurally preventing
  the omniscient pooling regression. `stage6/crn_streams.{hpp,cpp}` —
  content-addressed draws crn_u64/crn_unit(seed, public_token, purpose, seat,
  counter) via mix64 composition + a seed-only crn_deal_rng, so paired
  best/false legs and deviation siblings share draws on a common public spine
  independent of call order. `test_stage6_infoset_crn` pins own-pair order
  independence, no-opponent-card pooling, action/seat/target key sensitivity,
  CRN determinism/sensitivity/order-independence, and golden vectors for seeds
  1/17/43.
- **Three detailed contracts returned** (in the workflow journal, not yet
  code): step-5 br_estimator (epoch policy-iteration learn over FIXED learn
  seeds to avoid upward max bias, one frozen argmax per InfosetKey, disjoint
  confirm seeds, paired recursion, MC-vs-exact-pooled-R9 + 2p
  ExactEvaluation::nash_conv gates + 1e-12 repeatability); step-7 trainer
  (materialize live=2 / stream live>=3, RM+, uniform average, public-path key +
  ordinal chance alignment, RFC 0006:197-199 sampled-vs-full average gate);
  and an integration adjudication (acyclic core/train/eval target split,
  shared BehaviorPolicy/InfosetUuid vocabulary, per-purpose stream table,
  frozen lock.json hashing, ordered T0..T12 driver tasks).

Next: implement the step-5 br_estimator learn/confirm recursion in the main
thread (the statistically subtle component), gated on the exact pooled oracle;
then await/merge the translator review.


### Step-6 review closure + step-5 estimator implementation (2026-09-24, main thread)

**Translator review (fresh independent agent, APPROVE-WITH-CONDITIONS, no P1):**
independently brute-forced nearest snapping over all intervals in [0,60] plus
8,405 REAL GameState action nodes (1,290 all-in-only, 2,700 no-aggressive, 3p
+ HU), ASan/UBSan-clean instrumented translator, guards green, zero stage6
symbols in service/host. P2s fixed with mutation-RED pins: (1) corrected the
factual header comment (std::invalid_argument derives from std::logic_error,
not std::runtime_error — callers must catch std::exception); (2) added a real
all-in-only [9,9] pin (three coarse aggressive entries collapse to Raise 9 at
0.6 mass at the short BB's own node); (3) added fail-closed pins for illegal
Check at a due>0 node, illegal Call at a check-to node, and +Inf mass; (4)
added heads-up real-node pins (SB-first preflop + BB option); (5) fixed the
sign-conversion nit in project_exact_to_coarse_index.

**Step-5 two-phase deviation-gain estimator (implemented, release 58/58):**
`stage6/br_estimator.{hpp,cpp}` + `test_stage6_br_estimator.cpp` (16 gates) in
bigshark_stage6_eval; supporting additions are public_history_hash and
seed_list_hash in the infoset/CRN foundations, declared_menu_identity_hash in
bigshark_behavior, and a behavior-preserving ExactBrChoiceSink defaulted
out-parameter on exact_best_response_utility (single source of pooling truth
per the contract's chosen option).

- Phase-1 LEARN runs policy-iteration epochs over the same keyed seeds: full
  declared-menu bush at every traverser node with counterfactual-unchanged
  reach (exactly the br_group device, so unweighted accumulation at every
  fanned node is unbiased — no on-policy gating needed), one predecessor-frozen
  continuation returned to the ancestor, first-menu-order strict argmax,
  lazy profile-bootstrap, stabilization fixed-point detection with a typed
  stage6_br_not_stabilized. CRN keying guarantees identical visited nodes
  across epochs, so epoch-over-epoch table comparison is sound.
- Phase-2 CONFIRM is a paired best/profile recursion over disjoint seeds:
  shared walk at Deal/opponent nodes (identical recorded CrnDrawEvents),
  one-leg merge when frozen == profile draw, two legs after divergence with
  content-addressed re-pairing; stage6_br_infoset_miss is fail-closed with a
  declared RecordMiss fallback. FrozenBestResponsePolicy wraps a table as a
  legal point-mass BehaviorPolicy for exact evaluation.
- Gate design learned empirically: MC learn SATURATES only on river-rooted
  fixtures (the flop-rooted runout key space is own-combo x 43 x 42), so
  MC-vs-exact gains are pinned on river-rooted {1,1,1} passive / {1,2,2} jam /
  {2,2,2} stochastic opponents and a one-card turn-rooted runout gate uses
  the EXACT freeze; the flop-rooted stacks-10 fixture pins the strict 0.371601
  omniscient separation with 40k pinned confirm seeds (CI [10.312,10.519] vs
  omni 10.796). The 2p cross-check trains a real HeadsUpPolicy (2-chip
  all-in-only stacks where abstract and declared menus coincide exactly),
  replays it through GameState, and the estimator's exact freeze + MC confirm
  matches HeadsUpTrainer::evaluate nash_conv: exact unified oracle 0.272441 ==
  solver to 6 decimals; MC 0.285 with residual CI [-0.047,+0.072]. A bug found
  during validation was in the TEST (negative gains truncated into uint64),
  not the engine.
- Other gates: disjoint/empty seed rejection, seed-list hashing, miss
  Throw/RecordMiss, corrupted frozen action rejection, wrong-seat refusal,
  max_epochs=1 stabilization throw vs report, off-menu profile labeled
  (negative gains allowed), zero-sum paired settlements, identical-CRN
  shared-spine check, deal fingerprint equality across per-traverser runs,
  bitwise repeatability of tables/gains/Y series, joint-deal frequency vs the
  table measure, scalar NashConv stats. Resolved-include offline guard
  re-verified green with the three new sources; nm shows zero stage6 symbols
  in libbigshark_service.a and the host binary.

Next: independent adversarial review of the step-5 estimator (separate agent,
gated on exactness/pooling/bias), then step-7 MCCFR trainer.

**Step-5 independent review verdict: APPROVE, no P1.** A fresh adversarial
agent traced both walks, the exact oracle, CRN/key/content addressing,
settlement and legal paths, and ran six empirical scratch probes (nested
traverser decisions under chance, two-card flop runouts, learn-count decay
to 40k, 2p reached-node census, epoch flag, preflop smoke). It could not
construct a bias; the flop-rooted residual decayed monotonically to ~0 as
learn seeds grew (undercoverage, not bias), and frozen actions matched the
exact pooled table on every high-visit bucket. All P2/P3 findings fixed:
- P2-1 added the missing MC-LEARN-with-chance gate: a turn-rooted game
  (one 42-card river draw above a descendant traverser decision) runs full
  Monte Carlo learn at 6000 seeds on THREE independent learn lists with
  RecordMiss; zero confirm misses, residual CIs all include zero, all
  stabilized. Previously every chance-bearing gate used the exact freeze.
- P2-2 added the traverser < policies.size() guard to
  mc_learn_frozen_best_response (was reachable OOB).
- P2-3 addressed by the three-independent-learn-list gate above: a persistent
  small learn bias would shift all three residual means the same way.
- P3: stabilization flag now compares the first epoch against the lazily-
  seeded bootstrap (a one-epoch confirmation of b_0 reports stabilized);
  duplicate seeds within a list are now rejected (iid claim); the frozen
  legality test now performs a real root point-mass query (freeze for the
  seat-1 opener, whose own root key exists) instead of dead code; the CRN
  pairing assertion is global over all equal-coordinate events; stale
  8-deal comment corrected to six; documented why the runout stream hardwires
  seat=0 (cross-traverser pairing must not be personalized).

Gate matrix after closure: release 58/58; debug/asan rebuilt and rerun after
the engine-logic changes (stabilization/seed/bounds). Step 5 is fully
reviewed and closed; next is the R12 step-7 MCCFR trainer per the returned
trainer contract.

### R12 steps 7-8 implementation (2026-09-26): MCCFR trainer + composed candidate — kFull correction, coarse-space R11, honest null measurement — REVIEWED (two independent APPROVE verdicts; P1/P2/P3 resolved)

**Step 7 — external-sampling multiplayer MCCFR trainer**
(`engine/src/stage6/mccfr_trainer.cpp`, new `bigshark_stage6_train` static
target). One iteration runs N traverser sweeps; each sweep draws one
product-conditional joint deal (RFC 0006 restart sampler over uniform
1326-combo ranges) and one residual-deck root flop, then an external-sampling
walk: at a traverser node it enumerates every menu action over one shared
sampled continuation and applies the unweighted RM+ update
`R[a]=max(0,R[a]+v_a-v)`; at an opponent node it samples one action; at a deal
node it samples one legal runout card; terminals use exact L1 settlement with
a zero-sum assertion.

**kFull average (P1-1 correction).** The average is the OWN-REACH-weighted CFR
full average: at the traverser's OWN node, once per sweep,
`sums[a] += own_reach * sigma[a]`, with `own_reach` the product of the
traverser's own probabilities threaded through enumerated actions (opponent
actions, chance and the joint deal are externally sampled and carry no reach
factor). The earlier per-visit `sums += sigma` snapshot equals the full CFR
average ONLY at N=2; for N>=3 the sampled other-players reach contributes a
history-varying elementary-symmetric factor e_{N-2} and biases the average.
Header, TU and test comments were corrected to state kFull; CFR+ linear
weighting is still not used.

Two cursors share one walk: live==2 materializes the coarse AbstractTree and
keys rows by TreeNode index (chance children indexed by board-only ordinal,
tree topology board-value-independent); live>=3 builds no tree and keys rows by
PublicPath FNV hash. Rows are sealed with an artifact-identity stamp and an
FNV rows hash; the manifest records provenance and `iterations_completed`.

Guards: `stage6_train_edge_guard.{cmake,sh}` enforce the trainer<->eval sibling
edge in both directions and a resolved-include whitelist. The resolved-include
half now scans EVERY source in the `bigshark_stage6_train` target rather than a
hand-maintained list, so a new trainer TU inherits the edge automatically
(P2-5). Depth accounting is RAII (`DepthGuard`, P2-6).

Gates:
* `test_stage6_trainer`: independent external-sampling reference reproduces
  every RM+ regret and kFull average cell at 1e-9 for BOTH cursors;
  determinism; distribution validity; 100k stability and the
  `BS_STAGE6_TRAINER_LONG=1` 100k->600k mature-row stability gate; exhaustive
  cursor alignment (2p 9,608 / shallow 3p 28,824 action nodes). New: P1-2
  refuses zero completed iterations before the wall cap; P2-1
  `test_stream_order_sensitivity` proves `derive_stream` distinguishes key
  order (a commutative XOR mutation is RED). Mutation-RED: RM+ clip, kFull
  weighting (an unweighted `sums += sigma` mutation fails at the first own node:
  got 0.5 vs ref 0.25), runout deck, chance-token shift, stream commutativity.
* `test_stage6_multiway_average` (new): the RFC 0006:197-199 enumerable
  three-player parity gate. On a RIVER-rooted 3-seat fixture (one chip behind,
  six dead, five public cards, eight equal-weight joint deals from two disjoint
  combos per seat; 25 public nodes, ZERO chance nodes) an independently written
  full-traversal RM+ CFR is run against the production streaming sampler
  through the new test-only `debug_run_fixed_world_sweep` seam (production walk
  and RowStore verbatim; caller supplies the root def + joint deal). The
  independent reference accumulates BOTH the vanilla full average and the
  external-measure (pi_own*pi_opponents) average; the sampler is an unbiased
  estimator of the latter row-for-row, mass-weighted, and the SEALED profile is
  checked on the economically meaningful invariant — general-sum NashConv from
  exact infoset-consistent best responses on the restricted game: at 12k and
  30k checkpoints both independently produced profiles reach the same
  near-equilibrium (Y<0.01), agree with each other within 0.005 chip, and both
  contract with iterations, across seeds 1/43/777/999. Key sets are identical
  (no row on either side the other lacks). SCOPE: this aggregate gate does NOT
  itself discriminate the kFull-vs-unweighted weighting — the tiny fixture
  converges to near-pure equilibria (pi_opponents ~ {0,1}), so the e_{N-2}
  bias vanishes and an unweighted mutation leaves its NashConv green (verified
  on seeds 1/43/777). Its role is the end-to-end N>=3 property; the weighting
  is pinned at the measure level only by the 1e-9 single-sweep algebra gate.
  A future mixed-equilibrium enumerable fixture could add aggregate
  discrimination but is not needed for correctness.
* `test_stage6_frozen_artifact` (new, P2-2): write->load round-trip is
  byte/struct-identical for rows and manifest, serialization is deterministic,
  and a one-byte payload flip, bad magic, truncation, trailing bytes and a
  missing file are all rejected by the content-hash check.

**Step 8 — composed candidate** (`candidate_policy.{hpp,cpp}`, shared
`chart_preflop.{hpp,cpp}` — one pinned chart path). Charts preflop; postflop
the candidate replays the hand onto the bucket's reduced representative
shadow, locates the sealed row (TreeNode index / PublicPath hash) and projects
coarse mass onto concrete legal actions through the R7 translator. Binds by
reduced-representative def identity (reduced pot drops folded money and does
NOT round-trip to bucket.pot_bb) or by real flop signature. Added
`CandidateMissPolicy`: deployed default is fail-closed `Throw`; the coarse
NashConv measurement uses `UniformOnUnvisited`, which answers a
structurally-VALID coarse node no sampled sweep sealed with the SAME uniform
value an all-zero average row freezes to (`average_row`), counting the miss —
genuine abstraction violations (no binding, wrong stamp, off-menu concrete
action, desync, uncovered geometry) still throw in every mode. Gate
`test_stage6_candidate_policy` covers both cursors, preflop==baseline, deployed
fail-closed totality, and the new uniform/throw distinction.

**R11 deviation space — RESOLVED: coarse-game NashConv.** The frozen best
response gained an explicit deviation space
(`BrEstimatorConfig::deviation_space`, P1 of the R11 design question):
`Declared5Fraction` (the R8 menu; prior behavior) or `CoarseAbstraction`, where
the deviator is restricted to the SAME coarse action abstraction the artifact
was trained in (`bs::tree::abstract_node_menu`), with a distinct tagged menu
identity hash. The measurement driver measures BOTH profiles in the coarse
space, so the paired difference is apples-to-apples; opponents still play
their native policy. This is the only equilibrium claim an artifact with zero
off-menu mass can honestly make. Exact-sizings robustness stays a separate,
fail-closed Proposed question; the candidate does not project onto a
nearest-coarse node it never solved. Gate `test_coarse_deviation_space`
(`test_stage6_br_estimator`): null-action refusal, distinct/deterministic
identity, structural subset proof (coarse frozen menus are exactly
{check, half-pot bet} at check-to nodes and never contain an R8-only 1/3,3/4,
3/2 size), L3-menu equality, and the monotonicity bound coarse-NashConv <=
declared-menu NashConv (measured 1.02 <= 2.04 on the river fixture).

The measurement driver (`bigshark-stage6-measurement-driver`, a non-ctest leaf
joining eval+train) gained measurement-scale caps (`--max-infosets`,
`--max-gib`, `--wall-seconds`) and reports two coverage columns:
`br_confirm_misses` (unfrozen learn infosets hit in confirm) and
`candidate_uniform_rows` (structurally-valid coarse nodes answered uniform).

**R11 measured result so far — NOT a measurable advantage at feasible sampled
coverage; published, not promoted.** Running the coarse R11 table over uniform
1326-combo ranges is coverage-bound: a real 100bb 2p coarse tree has 1.43M
public nodes (553k action), and external sampling over 1326 holdings cannot
densely cover it at feasible iteration/seed budgets. At 200k iterations, 256
learn / 128 confirm seeds, the n=2 table (4 actionable buckets) shows
br_confirm_misses ~600-710 per traverser, candidate_uniform_rows ~2k-3k, and
every paired-difference 95% CI STRADDLES ZERO (e.g. n2 potbb4 d=-1.56 mean but
CI [-4.52,+1.39]); the same on n=3. Pushing one 2p bucket to 500k iterations
and 2000 learn seeds (5.3 min/bucket) still left ~1000 confirm misses and a
candidate CI of +-6.7 chips. The honest conclusion is that the sampled
uniform-range artifact is statistically INDISTINGUISHABLE from the pinned
baseline under coarse-game NashConv at any budget that completes in the test
window — with coverage counters showing WHY (the estimate is unsaturated), not
a tight equivalence. Per the project rule, no candidate is promoted and this
null is recorded as data. A meaningful larger-table table needs either
restricted (non-uniform) ranges, a narrower coarse tree, or a coverage
mechanism (result sampling / VR-MCCFR / chance-sampled average that does not
require every holding-path to be visited); that is future work, not a reason to
loosen the metric. Full n in {6,7,9,10} screening tables were not run because
each has 44-83+ actionable buckets with multi-minute per-bucket cost and the
n=2/n=3 signal already establishes the coverage-bound null with its diagnosis;
the driver runs them on demand. Geometry enumeration (offline, not a gate):
n=2 4 actionable, n=3 11, n=6 44, n=7 56, n=9 83, n=10 98 (1.81M exact
geometries at n=10; that enumeration alone takes tens of minutes and is a
one-time offline count, not a gate).

**P1 streaming-addressing bug found by the larger-table run — PublicPath hash
collision, FIXED.** Running the streamed (live>=3) table at n=6 threw
"trainer row address merged nodes with different menus" at live>=4 (it never
appeared at n=2/3 because those paths are shallow). The row address for a
streamed infoset is FNV-style `PublicPath::hash()` over the uint16 edge-token
sequence; that hash used the boost::hash_combine idiom
`h ^= value + golden + (h<<6) + (h>>2)` over uint16 tokens drawn from a small
~84-symbol alphabet, which is LOSSY on deep small-alphabet sequences. Two
genuinely distinct production paths captured from the n=6 run —
geo;seat1,2,3 all action0;turn;seat1 a1,seat3 a1 then {seat3 a2, river o355,
seat2 a0} vs {seat1 a0, river o340, seat2 a1} — hashed to the identical
64-bit value, merging a check-to node with a facing-a-bet node. Fix:
`PublicPath::hash()` is now FNV-1a over the little-endian BYTES of every token
plus the sequence length (the same canonical hash used for rows and the
geometry matrix), so distinct sequences differ in both content and length.
This is an artifact row-key identity change (acceptable pre-promotion: no
artifact has shipped) and is pinned by `test_public_path_hash_injectivity`
(the exact captured pair, a trailing-zero length case, determinism);
mutation-RED proven (restoring hash_combine fails on the captured pair). After
the fix n=6 live3/live4 train cleanly (3.35M / 6.7M distinct infosets vs the
silently-collapsed counts before) — the collision had been collapsing real
nodes, which also explains why n>=4 streamed artifacts looked smaller than
they are. The n=6 live5 streamed artifact exceeds the 8M-infoset driver cap at
100k iterations: streaming keys by concrete public cards does not share
card-bucket trees across streets, so high-live flop-rooted artifacts are very
large. That scale bound, together with the coverage null, is the practical
ceiling on the uniform-range larger-table policy; producing a tight,
separated NashConv at n>=4 needs restricted ranges or a coverage mechanism,
not a bigger map.

Also: unified the duplicate `bs::stage6::TranslatorId` onto the single core
type (eval fills the rule digest; projection gated on name+version).
`GeometryBucket` gained its big-blind denomination; the manifest gained
`artifact_content_hash` and `iterations_completed`. Full release matrix is
62/62; debug and ASan presets are run in the verification step.


**Independent review (2026-09-26, two reviewers, implementer != reviewer).**
Both returned APPROVE with no P1/P2 blockers. Reviewer findings and resolutions:
* comments that called kFull the "exact/unbiased full average for every N" were
  tightened to the precise statement — the sealed per-row policy is an unbiased
  estimator of the EXTERNAL-MEASURE average (pi_own*pi_opponents), coinciding
  with the vanilla full average at N=2 / convergence (trainer.hpp,
  mccfr_trainer.cpp header + walk_action);
* the aggregate multiway gate's non-discrimination of the weighting on the
  near-pure fixture is stated in the test SCOPE block and here (the weighting
  rests on the 1e-9 single-sweep algebra gate; a mixed-equilibrium enumerable
  fixture is a future optional addition);
* deleted the unused `WalkArgs` struct;
* added an explicit artifact duplicate-key/repeated-record rejection case and
  an end-to-end mutated-sealed-POLICY-under-stale-manifest case to
  `test_stage6_frozen_artifact` (the loader's content hash rejects it).
Reviewers additionally live-verified: the external-measure math and world-set
BR infoset consistency, the FNV path fix, the coarse DeviationSpace null/menu
guards (opponents never use the coarse menu), UniformOnUnvisited being
unreachable by a genuine abstraction miss, and the train-edge guard catching a
forbidden include live (including a transitive one). Final matrices: release
62/62, debug 60/60 (release-only benchmarks excluded), ASan 60/60 with zero
sanitizer findings.

### Offline practice table + adapter bet/raise verb fix (2026-09-27)

User-requested local training tool (not a measurement step): one human seat at
a 2..10-seat offline table, every other seat a `BehaviorPolicy` bot —
`bigshark-practice` interactive leaf (`engine/benchmarks/practice_simulator.cpp`)
over the new reusable `bs::stage6::PracticeTable` core
(`engine/include/bs/stage6/practice_table.hpp`,
`engine/src/stage6/practice_table.cpp`, library `bigshark_practice` ->
`bigshark_stage6_eval`; added to the offline-guard forbidden prefixes).
Difficulty `easy` = `UniformBehaviorPolicy`, `medium` = the pinned chart +
equity heuristic baseline; labels are explicit that neither is "GTO". The
core shuffles with SplitMix64, posts the n==2 vs n>=3 blind layouts, runs the
unified GameState action/deal loop, settles with the exact L1 rules with a
per-hand zero-sum assertion, rotates the button, and keeps cumulative P/L.
Information exposure is enforced in the core: the human callback/observer see
only the human's holes, the actually-dealt board prefix, and showdown live
holes only. Gate: `test_stage6_practice` (seat matrix 2/3/6/9/10 for both
difficulties, blind layouts, fold privacy, illegal-action rejection, bad
configs, seeded reproducibility).

The simulator exposed a latent adapter defect: `adapt_to_ctx` advertised
EVERY aggressive legal action as `"raise"`, even postflop when nothing is
owed, where the deployed policy's lead branches gate on `L.has("bet")` — the
pinned baseline therefore never led postflop in any stage-6 measurement
rollout. Session journals pin the live server vocabulary: preflop
check-available states (the big blind option after a limp) advertise
`raise`; postflop check-available states advertise `bet`. The adapter now
emits that street-dependent verb; `map_deployed_decision` accepts exactly
the advertised token — preflop only `raise` (reconciled onto the state's
Bet type at the BB option; a literal preflop `bet` throws), postflop an
exact bet/Bet or raise/Raise pairing — and throws on every mismatch. The
corpus gate pins a real one-limper BB-option isolation raise (1326-hand scan)
and both negative token/state pairings. The adapter corpus harness reference leg was updated
to the same vocabulary (63/63 identical). Artifact identity is unaffected:
geometry enumeration walks preflop states only. Baseline R11 figures change
(fidelity increases) and must be regenerated when measurements are re-run.

### R11 Item 3: exact-sizings EDGE projection for exact-game NashConv (2026-09-27)

Goal: report NashConv of the TRANSLATED candidate in the real declared
five-pot-fraction action space (DeviationSpace::Declared5Fraction), not only
the trainer coarse game, without changing any sealed artifact.

- `project_exact_to_coarse_edge` (translator.hpp/.cpp) joins the existing
  strict `project_exact_to_coarse_index`: passives keep the exact-entry rule;
  an aggressive exact action maps to the nearest aggressive coarse EDGE
  regardless of Bet/Raise type (within a street the reduced menu is uniformly
  one type), ties to the smaller total; -1 only when no aggressive edge
  exists. Unit-gated (cross-type match both directions, tie-smaller, no-edge,
  empty menu, and strict still refuses what edge accepts).
- `CandidateBehaviorPolicy` gains `CandidateProjectionMode`
  {StrictCoarse (deployed default, unchanged), NearestCoarseEdge} and an
  `off_tree_misses()` counter. Node addressing still advances with the
  matched COARSE menu action, so materialized TreeNode / streaming PublicPath
  never leave the sealed tree and the sealed distribution is unchanged; an
  unseen exact size keeps zero candidate mass except via the disclosed
  fallbacks.
- Empirical finding the original design note under-specified (confirmed by
  independent review instrumentation, ~11% of 2p and ~22% of 3p decision
  nodes at 100bb): nearest-edge chip OVERSHOOT (an exact 3/4 pot maps onto
  the coarse 1x edge, committing more than the real line) desyncs the
  reduced representative shadow from the real game at three places — mid-log
  (passives-only shadow while an observed action is aggressive, different
  actor, or a runout card the real line has not dealt) AND at the candidate's
  own final decision node (the reached coarse node's aggressive-option
  presence differs from the real state in either direction). At ALL of these
  there is no sealed row modeling the real decision; in NearestCoarseEdge +
  UniformOnUnvisited the candidate answers that concrete state with zero
  information (uniform over the exact declared menu) and counts it as the
  distinct `candidate_offtree_rows` CSV column. Every other mode/pairing
  stays fail-closed (a passive mismatch and a deployed/strict divergence
  still throw). Same-type off-coarse SIZES that land on a sealed edge are not
  off-tree. The reviewer also established the type-agnostic translator arm
  is currently-unreachable defensive breadth (actor alignment implies the
  Bet/Raise label aligns; overshoot changes amounts only), now documented on
  project_exact_to_coarse_edge; strict and edge shadow traversals are
  identical, so EDGE only unlocks the disclosed off-tree fallback.
- Driver: `measure --exact` runs the declared-space pass (candidate built
  NearestCoarseEdge); CSV gains `candidate_offtree_rows`. `hashes` is
  byte-identical, proving addressing-only change: rows, FrozenManifest,
  translator id, action digest unchanged, no retrain.
- Gates: translator EDGE unit cases; candidate test_nearest_coarse_edge_line
  walks a deep representative game along exact 3/4-pot aggressions, proves
  strict fails at an off-tree point while edge answers with a legal unit
  distribution and increments the counter, and that on-coarse passive lines
  still work. release 63/63.
