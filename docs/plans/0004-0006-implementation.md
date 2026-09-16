# RFC 0004-0006 Implementation Plan

Status: Current

Execution state: RFCs 0004 and 0006 Implementing; Stages 1, 2, 3, 4, 5, 6,
11, and the isolated Stage 12 ICM are complete with recorded evidence.
Stage 7 (protocol minor-0 migration) is next. RFC 0005 remains Accepted.

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

Stages 1, 2, 3, 4, 5, 6, 11, and the isolated Stage 12 ICM are complete;
all other work remains Pending. Owners name existing
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

## Next Implementation Checkpoint

Stage 7 begins the RFC 0002 minor-0 protocol migration; it is independent of
solver strategy changes. Keep v0 contexts, production policy routing, and
external commands unchanged. The resident layer stays opt-in and unwired
until the Stage 7/8 protocol and Stage 9 eligibility work land.

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
