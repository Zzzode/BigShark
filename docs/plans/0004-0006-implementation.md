# RFC 0004-0006 Implementation Plan

Status: Current

Execution state: RFCs 0004 and 0006 Implementing; Stages 1, 2, 3, 4, 11, and
the isolated Stage 12 ICM complete with recorded evidence. Stage 5 artifact
storage is next. RFC 0005 remains Accepted.

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

Stages 1, 2, 3, 4, 11, and the isolated Stage 12 ICM are complete;
Stage 5 and all other work remain Pending. Owners name existing
modules or the explicitly approved artifact boundary, not separate services.

| Stage | Roadmap IDs | Owner | Implementation | Required completion evidence |
| --- | --- | --- | --- | --- |
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

## Next Implementation Checkpoint

Stage 5 adds the RFC 0005 artifact boundary: the authoritative SQL schema,
transactional checkpoints, immutable export, digest, and bounded reader, with
crash/full-disk/corruption/version tests and macOS/Linux native dependency
builds. Keep v0 contexts, production policy routing, and external commands
unchanged.

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
