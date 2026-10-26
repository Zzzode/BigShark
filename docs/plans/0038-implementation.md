# RFC 0038 Implementation Plan

Status: pending approval. RFC 0038 is in round-2 independent review; no
engine source changes until the approval agent returns Approved.

RFC: `docs/rfcs/0038-retire-schema-v1-artifacts.md`.

## Acceptance-criteria map

| AC | Stage | Evidence |
| --- | --- | --- |
| AC1 (authoritative deletion set gone; grep audit) | 1 | grep output, source diff |
| AC2 (v1 file -> UnsupportedVersion; resident LoadFailed) | 3 | new pinned tests |
| AC3 (seat-only publishers; shipped artifacts still validate) | 1,4 | policy/verify-pack/replay green |
| AC4 (single resident game path) | 3 | source diff, resident tests |
| AC5 (bs_resident_fixture v2, JSON contract, w1 TS green) | 2 | w1-resident-wiring.test.ts |
| AC6 (one-to-one resident scenario migration; invariants relocated) | 2,4 | ctest, scenario table below |
| AC7 (benchmark migrated; new baseline recorded) | 4 | benchmark run + benchmarking.md |
| AC8 (full gates + ASan + docs) | 5 | Engine Build Contract output |

## Stage 1 — artifacts module removal

Delete from `engine/src/artifacts/strategy_artifact.cpp` and
`engine/include/bs/strategy_artifact.hpp`:

- v1 dialect: `kSchemaDdlV1`, `kAllowedTablesV1`, `kSchemaV1`,
  `write_schema`, heads-up `write_all`, `insert_game`/`insert_sizes`/
  `insert_ranges`, heads-up policy insert, `read_game`/`read_sizes`/
  `read_ranges`/`validate_game`, v1-only auxiliary readers, v1 manifest
  validator, the v1 `load_path` branch.
- public API: heads-up `create_checkpoint`, `commit_checkpoint`, the v1
  `publish_policy` arm, `LoadedArtifact::result`, `ArtifactProbe::game`.
- identities: `kDefaultAlgorithmRevision`, `kNumericProfileV1`,
  `kRulesIdentifierV1`, `kLegacyAlgorithmRevisionV2`, rev1 branches of
  `decode_nseat_algorithm`/`nseat_write_identity`; manifest defaults
  emptied for algorithm/numeric strings;
  `information_key_revision` defaults to `kArtifactSchemaVersionV2`;
  delete bare `kArtifactSchemaVersion`.
- codec (`engine/src/artifacts/artifact_codec.cpp`): remove
  `grammar_for_revision` and the revision parameters from
  `encode_public_key`/`decode_public_key`; hardwire the revision-2
  grammar (preflop wrap, 9-player domain).
- `CheckpointProvenance` struct deleted; `read_state_fields`/
  `read_states` lose `legacy_domain`; heads-up `PolicyAssembler`
  (`artifact_internal.hpp`, friend in `heads_up_solver.hpp`) deleted.
- `open_validated` accepts 2/3 only; version 1 throws
  UnsupportedVersion with the RFC 0038 message.

Expected intermediate state: module compiles; test/benchmark/fixture
consumers do not yet (fixed in stages 2/4). Build the artifacts library
target directly to stage the work.

## Stage 2 — v2 fixture layer and the fixture publisher

Shared helpers (new test-internal header if reuse requires it):

- `publish_game_v2(game_spec, iterations, overrides, dir, name)`:
  train with `train_nseat` over a 2-seat flop-rooted `GameDef`, apply
  overrides by seat information key, assemble with
  `SeatPolicyAssembler`, checkpoint+publish via
  `SeatCheckpointProvenance`.
- `publish_root_only_v2(...)`: publish the seat policy with the class
  section suppressed (same observable root-only artifact the v1 helper
  produced; `validate_content_v2` never enforces completeness).
- `publish_synthetic_policy_v2(...)`: assemble one forced row (and the
  forced zero-probability row) directly via `SeatPolicyAssembler`.

Migrate `engine/tests/resident_fixture_publisher.cpp` to these helpers;
preserve the printed JSON exactly (`path`, `sha256`, `root.flop`,
`root.stacks`, `root.contributions`, `root.pot`, `root.bigBlind`,
`root.button`) and the `bs_resident_fixture` target/bin copy.

### test_resident_policy.cpp one-to-one scenario map

| Scenario (current test) | v2 counterpart |
| --- | --- |
| golden reach (:818) | publish_game_v2, 2-seat flop root, golden margin query |
| free-turn (:961) | v2 root with turn conditioning in the query cursor |
| belief hook (:1039) + hook-free (:1169) | v2 policy row overrides at the hook node |
| matrix (:1229) | publish_game_v2 4 iterations, same query matrix |
| street conditioning (:1325) | v2 root, advance/round-end in queries |
| streets-zero-hero (:1370) | forced zero-probability row via synthetic helper |
| orphan actor/free chance (:1407) | v2 ranges omitting one seat's combos |
| empty-shared / empty-zero (:1487,:1501) | publish_synthetic_policy_v2 single-row artifacts |
| orphan p1 (:1565) / free slot (:1635) | v2 ranges + seat-mask variants |
| shared combo (:1674) | overlapping v2 combo ranges |
| probe api (:1715,:1753) | v2 publish_game_v2 probe byte accounting |
| checkpoint rejection probe (:1790) | seat create_checkpoint; rejection checks unchanged |
| miss golden/matrix (:2344-2346) | v2 unserved root enumeration |
| fully blocked (:2517) | v2 root with blocking overrides |
| root-only (:2539) | publish_root_only_v2 |
| continuation golden (:2730) | v2 multi-street query chain |

The pre-existing v2/v3/preflop/multi-root tests in the same file are
untouched.

## Stage 3 — resident single arm and rejection tests

- Remove the three `to_unified_game(probe...)` arms in
  `engine/src/resident/resident_policy.cpp` (`:598`, `:677`, `:728-729`).
- Add: a `user_version = 1` SQLite file fails `load_artifact` and
  `probe_artifact` with UnsupportedVersion, and a resident configured
  with it reports LoadFailed with the RFC 0038 message.
- Run `w1-resident-wiring.test.ts` against the rebuilt
  `bin/bs-resident-fixture`; both real-binary cases must pass.

## Stage 4 — test_artifacts and benchmark migration

- Delete v1 roundtrip/split/resume/crash/corruption/immutable/
  adversarial cases and the six `commit_checkpoint` sites; remove rev1
  identity cases and rev1 canonical-key assertions (keep rev2 coverage);
  rewrite the five arm-empty assertions to the nseat arm.
- Relocate format-agnostic invariants to v2/v3 fixtures: physical
  corruption rejection, immutable open, adversarial probability
  rejection, split-run/resume.
- Migrate `resident_lookup_benchmark.cpp`: both publishers become 2-seat
  flop-rooted v2; keep SPR-10 scan, six-root SPR-4 cold/warm +
  zero-allocation, and zero-prob miss sections; update the key codec
  call; record the first migrated run as the new baseline in
  `docs/development/benchmarking.md`.

## Stage 5 — documentation and gates

- gto-engine artifact section v2/v3-only + Historical sentence;
  benchmarking methodology/baseline; Historical annotations in RFCs
  0004, 0005, 0007, 0009, 0016, 0017, 0034.
- Full Engine Build Contract release; ASan on artifacts/resident;
  npm/proto/replay; check-rfcs/check-docs; the AC1 grep audit.

## Stop conditions

Any w1 fixture behavior or resident scenario that cannot be expressed on
the 2-seat v2 path; any production caller of a deleted symbol; any
shipped v2/v3 artifact that stops validating. Stop and amend the RFC; do
not delete an assertion or reintroduce a compatibility path.
