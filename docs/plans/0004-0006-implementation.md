# RFC 0004-0006 Implementation Plan

Status: Current

Execution state: RFCs 0004 and 0006 Implementing; Stages 1, 2, 3, 11, and
the isolated Stage 12 ICM complete with recorded evidence. Stage 4 frozen
matrix is next. RFC 0005 remains Accepted.

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

Stages 1, 2, 3, 11, and the isolated Stage 12 ICM are complete;
Stage 4 and all other work remain Pending. Owners name existing
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

## Next Implementation Checkpoint

Stage 4 adds the frozen release matrix from RFC 0004: at least dry,
two-tone connected, paired, and monotone flops; equal and asymmetric stacks;
and stack-to-pot ratios 1, 4, and 10. It reports exact versus estimated
metrics and node, memory, elapsed-time, and coverage capacity per fixture,
with new independently versioned fixtures and no full-game claim.
Keep v0 contexts, production policy routing, and external commands unchanged.

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
