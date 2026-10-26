---
rfc: "0038"
subject: "Retire the schema-v1 heads-up artifact format and algorithm revision 1"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-26"
updated: "2026-10-26"
owners: "engine artifacts (engine/src/artifacts, engine/include/bs/strategy_artifact.hpp); engine resident consumer"
supersedes: ""
superseded-by: ""
---

# RFC 0038: Retire the schema-v1 heads-up artifact format and algorithm revision 1

## Summary

Remove read and write support for the original two-seat `user_version = 1`
SQLite artifact schema (the RFC 0004 heads-up blueprint format) and for
algorithm revision 1 (`rfc0004-rev1-full-kSimple-prng1` on the heads-up
line and `rfc0009-nseat-rev1-sampled-splitmix64` on the seat line) from
the engine. After this change the artifact module speaks only
schema-v2/schema-v3 with algorithm revision 2; a schema-v1 file fails at
open with the existing `UnsupportedVersion` error kind, which the
resident already maps to `RootStatus::LoadFailed` and the normal resolver
ladder. No shipped artifact, benchmark fixture, resident root, wire
protocol, or in-memory solver path uses schema v1.

## Motivation

The project keeps no legacy implementations: when a new approach replaces
an old one, the old code, tests, and documentation are deleted in the
same change. Schema v1 is the surviving compatibility layer from the
RFC 0004 heads-up blueprint era:

- it is a second, parallel SQLite dialect inside one 3,072-line file
  (`engine/src/artifacts/strategy_artifact.cpp`): its own DDL set,
  allowed-table set, manifest validation, game/sizes/ranges readers,
  policy/training writers, key grammar (`info_key_revision = 1`),
  numeric profile (`kNumericProfileV1`), and its own `LoadedArtifact`
  arm (`bundle.result`, a `HeadsUpTrainingResult`) mutually exclusive
  with the seat arm;
- every production training path now produces schema v2 (preflop
  profiles) or schema v3 (flop classes): the five shipped preflop
  profiles are schema v2 carrying `rfc0016-nseat-rev2-…` revisions, and
  the BSFLIB03 flop library packs v3 class artifacts;
- no code outside tests and one benchmark publishes or consumes a v1
  artifact (evidence below), and the `~/.bigshark-backups` tree that
  held the last v1-era copies was deleted on 2026-10-26 by maintainer
  direction;
- the dual arm propagates into the resident policy: three sites convert
  the heads-up probe/load result with `solver::to_unified_game(probe.game)`
  instead of the single `game_def` path, and `read_state_fields` carries
  a `legacy_domain` boolean that relaxes the player domain from
  `0..player_count-1` to `{0,1}`.

The layer is dead weight in a hot boundary that also parses untrusted
SQLite bytes: deleting it removes a parser dialect, two identity strings,
one public overload, one probe field, and three resident branches.

## Goals

- G1: `engine/src/artifacts` reads and writes only schema-v2/schema-v3
  artifacts, all under algorithm revision 2 identities; no symbol for
  schema v1, the revision-1 key grammar, `kNumericProfileV1`,
  `kRulesIdentifierV1`, `kDefaultAlgorithmRevision`,
  `kLegacyAlgorithmRevisionV2`, `CheckpointProvenance`, the heads-up
  `PolicyAssembler`, or the heads-up
  `create_checkpoint`/`commit_checkpoint`/`publish_policy` overloads
  remains in `engine/`.
- G2: opening a schema-v1 file through `load_artifact` or
  `probe_artifact` fails with `ArtifactErrorKind::UnsupportedVersion`;
  the resident reports `RootStatus::LoadFailed` with the diagnostic and
  never crashes; a pinned test presents a real v1 byte fixture.
- G3: `create_checkpoint` and `publish_policy` accept only
  `SeatTrainingResult`/`SeatCheckpointProvenance`; the heads-up
  overloads, `commit_checkpoint`, the v1 `LoadedArtifact.result` arm,
  and `ArtifactProbe.game` are deleted.
- G4: the resident policy has one artifact-to-game path (the
  `game_def`/`build_v2_game` arm); the three `to_unified_game(probe.game)`
  branches are gone.
- G5: every v1 producer is migrated through the rekey route rather than
  deleted: the `bs_resident_fixture` publisher keeps its JSON contract
  and the w1 real-binary TS tests stay green; the documented
  resident-lookup benchmark keeps all three measured sections as
  flop-rooted river-terminal v2 artifacts with a declared new baseline;
  and the ~20 `test_resident_policy` scenarios are mapped one-to-one via
  in-memory heads-up training and byte-identical key transfer.
- G6: tests and benchmarks build and pass with no v1 fixture; design
  docs and the affected RFCs record schema v1 as Historical.

## Non-Goals

- **No change to schema v2 or v3.** Their DDL, validation, digests, and
  on-disk bytes stay exactly as they are; v2 and v3 artifacts require no
  rebuild and their SHA-256 pins remain valid.
- **No change to the in-memory heads-up solver.** `HeadsUpGame`,
  `HeadsUpTrainer`, the L4 solve seam, and
  `solver::to_unified_game(const HeadsUpGame&)` are solver concepts used
  by in-memory benchmarks and the resolver; they are not the artifact
  format and are untouched.
- **No BSFLIB/BSWARM format change.** Packed libraries and sealed
  checkpoints are outside this RFC.
- **No wire-protocol change, no session/journal change, no replay
  change.** None of them read artifact SQLite files.
- **No v1-to-v2 migration tool.** No v1 artifact exists in the shipped
  set or in backups; a converter would be speculative.

## Current State and Evidence

### Facts (verified in this repository, 2026-10-26)

- **Version dispatch.** `open_validated` selects the schema set by
  `PRAGMA user_version` and currently accepts 1, 2, and 3
  (`engine/src/artifacts/strategy_artifact.cpp`, `kSchemaV1` at
  approximately `:2385`; dispatch at the `open_validated` definition).
  Any other value already fails with `UnsupportedVersion`.
- **Write surface.** `write_all` creates `PRAGMA user_version = 1` and
  runs `kSchemaDdlV1`; the public heads-up overloads are
  `create_checkpoint(path, const TrainingResult&, …)`
  (`strategy_artifact.hpp:193`), `commit_checkpoint(path, const
  TrainingResult&, …)` (`strategy_artifact.hpp:211`,
  `strategy_artifact.cpp:2902`, whose only callers are six v1
  split/resume tests in `test_artifacts.cpp:1768-1967`), and the v1 arm
  of `publish_policy` (`strategy_artifact.cpp:3025-3032`). The
  non-test callers are exactly two programs, both heads-up v1:
  - `engine/benchmarks/resident_lookup_benchmark.cpp` — the documented
    resident cold/warm lookup benchmark
    (`docs/development/benchmarking.md`): `publish_complete` (`:140`,
    called at `:363` and `:399`, publishes via the heads-up overload at
    `:153-157`) and `publish_zero_prob` (`:163`, called at `:457`,
    publishes at `:201-205`). The file contains no `SeatTrainingResult`,
    `train_nseat`, or `SeatCheckpointProvenance` reference: contrary to
    an earlier draft, BOTH publish sites are v1. Its three measured
    sections are the SPR-10 sizing scan, the six-root SPR-4 cold/warm
    latency and zero-allocation probe, and the zero-probability miss
    fixture; it is a default `all` build target
    (`engine/CMakeLists.txt:481`).
  - `engine/tests/resident_fixture_publisher.cpp:108-115` — builds a v1
    artifact with the heads-up `PolicyAssembler` and
    `CheckpointProvenance`. `proto/CMakeLists.txt:247-269` builds it as
    the `bs_resident_fixture` executable and copies it to
    `bin/bs-resident-fixture`; `platforms/river-club/tests/w1-resident-wiring.test.ts:38`
    runs that binary and exercises two real-binary blueprint serving
    paths against the actual engine (`:540-599`). Its JSON summary
    (root envelope, path, sha256) is the TS test's input contract.
  The preflop profile builder, the only production artifact publisher,
  uses the seat overload (`preflop_profile_builder.cpp:327-336`).
- **Read surface.** `load_path` branches on schema version: v1 reads
  `game`/`sizes`/`ranges` plus v1 state/action/training tables with
  `read_states(..., legacy_domain = true)` (`:1787-1806,1841`); v2/v3 go
  through `load_path_seat`. `LoadedArtifact` carries either `result`
  (`HeadsUpTrainingResult`) or `nseat` (`:159-164`). `ArtifactProbe`
  carries a v1 `HeadsUpGame game` field and v2 `game_def`/`ranges`/
  `sizes` optionals (`strategy_artifact.hpp:~248-260`). The resident's
  v1 arms are: two probe sites in `build()` (`resident_policy.cpp:598`
  and `:677`) and the eager preload path in the same `build()` try-block
  (`:728-729` — this is the build-time preload, not the on-demand lazy
  loader). The real lazy loader `ensure_lazy_loaded` (`:365-410`) is
  already seat-only (`:386-387`: a non-nseat bundle returns nullptr),
  and the probe pre-gates every lazy path, so the failure conclusion
  below holds for both.
- **Identity and key-codec surface.** v1 manifests default to
  `kDefaultAlgorithmRevision = "rfc0004-rev1-full-kSimple-prng1"`,
  `kNumericProfileV1`, and `kRulesIdentifierV1 =
  "rfc0004-heads-up-flop-v1"`. The shared `ArtifactManifest` defaults
  `information_key_revision = 1` (`strategy_artifact.hpp:116`). The
  information-key codec has a revision-1 grammar
  (`grammar_for_revision(1) -> {1,1,2,false}`,
  `engine/src/artifacts/artifact_codec.cpp:77-84`); every surviving
  caller outside the v1 paths passes revision 2
  (`encode_public_key(..., 2)` in `test_seat_policy.cpp`, v2 writers,
  and the v2 reader), with the exception of
  `resident_lookup_benchmark.cpp:296` and
  `test_resident_policy.cpp:2642`, both v1-fixture sites. The seat line
  still decodes `kLegacyAlgorithmRevisionV2 =
  "rfc0009-nseat-rev1-sampled-splitmix64"` as revision `{1, 0}` in
  `decode_nseat_algorithm`, and `nseat_write_identity` can still emit it
  for an `algorithm_revision == 1` result; a test round-trips it
  (`engine/tests/test_artifacts.cpp:1120-1143`).
- **Test fixture surface beyond test_artifacts.**
  `engine/tests/test_resident_policy.cpp` builds roughly twenty resident
  semantic scenarios on three v1 fixture helpers: `publish_game`
  (`:158`, used at `:818, 961, 1039, 1169, 1229, 1325, 1370, 1407,
  1565, 1635, 1674, 1715, 1753, 2344, 2517, 2730`), `publish_root_only`
  (`:244`, used at `:2539`), and `publish_synthetic_policy` (`:317`,
  used at `:1487, 1501`), plus direct heads-up `create_checkpoint`
  calls at `:591, 653, 762, 1790` — the first three are already seat
  (`SeatCheckpointProvenance` + `SeatTrainingResult`: the v2 projection
  fixture, the RFC 0020 hand-authored fixture, the preflop fixture) and
  stay untouched; only `:1787-1790` (the checkpoint-rejection probe) is
  heads-up and migrates. The scenarios cover golden margins, free-turn,
  the belief hook and matrix, streets/round-end, orphan seats,
  shared/zero-weight combos, probe byte accounting, miss coverage,
  continuation, the fully-blocked root, and root-only partial exports.
  The heads-up-only friend `detail::PolicyAssembler`
  (`artifact_internal.hpp:132-141`, friend of `HeadsUpPolicy` in
  `heads_up_solver.hpp`) is used only by these v1 producers; the seat
  analogue `SeatPolicyAssembler` is separate and stays.
- **v2/v3 tests touch the to-be-deleted arms.** Even current-format tests
  assert the mutually-exclusive v1 arm is empty or read its probe field;
  the mechanical edits are `test_artifacts.cpp:1270, 1317-1318, 1326,
  1423, 1541-1542` (`bundle.result`/`probe.game` references inside the
  v2/v3/preflop round-trips; each becomes the nseat-arm assertion).
- **Shipped artifacts.** `artifacts/preflop-profile{,-3way-100bb,-4way,
  -5way,-6way}` are schema v2 with `rfc0016-nseat-rev2-frozen-batch-*`
  revisions; `artifacts/flop-library-6way-fullrange` is BSFLIB03 over v3
  class artifacts; `flop_library_pack.cpp:85-87`,
  `flop_library_verify_pack.cpp:104-108`, and
  `preflop_nash_conv.cpp:50-52` all require `bundle.nseat`. Nothing
  under `artifacts/` is schema v1.
- **No external readers.** `grep` over `platforms/`, `clients/`,
  `apps/`, `bin/`, and `tools/replay` finds no schema-version-aware
  consumer; TypeScript and replay never open artifact SQLite files.
  BSFLIB pack/verify code and BSWARM checkpoints have no v1 identity
  coupling; the stage-6 frozen artifacts use `rows.bin`/`manifest.bin`.

### Assumptions

- A developer machine with a private `.runtime/config.json` resident
  entry pointing at a years-old local v1 file loses that root after
  upgrading; it surfaces as `LoadFailed` exactly like any other
  unreadable root and falls through the existing ladder. This is the
  intended behavior under the no-legacy rule, not a regression in any
  shipped configuration.

## Design Principles

- One artifact dialect. The module validates, writes, and exposes only
  the formats current training produces.
- Fail closed and explicitly. An unknown schema version already has a
  typed error path; schema v1 joins it instead of being guessed or
  converted.
- Local closure. The change is confined to the artifacts module, its one
  resident consumer, benchmarks, and tests. No wire, packed-library, or
  solver change.
- Delete in one change. No deprecation flag, no dual reader, no v1
  compatibility shim with a later removal date.

## Proposed Design

### Artifacts module

The authoritative deletion set (grep-audited in AC1):

1. The schema-v1 dialect: `kSchemaDdlV1`, `kAllowedTablesV1`,
   `kSchemaV1`, `write_schema`, the heads-up `write_all`,
   `insert_game`/`insert_sizes`/`insert_ranges`, the heads-up policy
   insert path, `read_game`/`read_sizes`/`read_ranges`/`validate_game`,
   v1-only auxiliary readers, and the v1 manifest validator.
2. `open_validated` dispatches only on versions 2 and 3; version 1 takes
   the existing `UnsupportedVersion` failure with a message naming the
   retired schema (`"unsupported schema version: 1 (schema v1 retired
   by RFC 0038)"`).
3. The heads-up public surface:
   `create_checkpoint(path, const TrainingResult&, …)`,
   `commit_checkpoint(path, const TrainingResult&, …)` (whose only
   callers are the deleted v1 split/resume tests), the v1 branch of
   `publish_policy`, `LoadedArtifact::result` (the heads-up arm), and
   `ArtifactProbe::game` plus its v1-only population path.
4. The legacy identities and defaults:
   `kDefaultAlgorithmRevision` (rfc0004-rev1), `kNumericProfileV1`,
   `kRulesIdentifierV1`, `kLegacyAlgorithmRevisionV2`, the revision-1
   branch of `decode_nseat_algorithm`, and the revision-1 emit branch of
   `nseat_write_identity`. The shared `ArtifactManifest` default for
   `algorithm_revision`/`numeric_profile` becomes empty; every writer
   already populates both through `manifest_for_v2`, and the existing
   non-empty reader checks keep an unpopulated manifest invalid.
   `ArtifactManifest.information_key_revision` stays a stored v2/v3
   field but its default becomes `kArtifactSchemaVersionV2`; the raw
   `kArtifactSchemaVersion = 1` constant is deleted.
   `kSampledPrngIdentifier` (`splitmix64-rev1`) stays — it names the
   sampler, not the artifact schema, and v2 manifests carry it.
5. The revision-1 information-key grammar:
   `grammar_for_revision` loses its `revision == 1` branch, and
   `encode_public_key`/`decode_public_key` drop the revision parameter
   and speak only the revision-2 grammar (preflop wrapping,
   9-player domain). The remaining callers already pass revision 2.
   `read_state_fields`/`read_states` lose `legacy_domain`; the player
   domain is always `0..player_count-1`.
6. The heads-up-only friend class `detail::PolicyAssembler`
   (`artifact_internal.hpp`) and its friend declaration in
   `heads_up_solver.hpp` are deleted; `SeatPolicyAssembler` and the
   in-memory `HeadsUpPolicy` itself are untouched.
7. `CheckpointProvenance` (the heads-up provenance struct) is deleted
   wholesale: every construction site disappears with the heads-up
   checkpoint/commit paths and the two migrating v1 producers. The seat
   `SeatCheckpointProvenance` is the only provenance struct that
   remains.

### Resident consumer

`engine/src/resident/resident_policy.cpp` keeps only the `game_def`
arm: the two probe sites in `build()` (`:598`, `:677`) and the eager
preload site (`:728-729`) stop constructing a
`to_unified_game(probe.game)` fallback. A v1 root now fails at
`probe_artifact`/`load_artifact` and lands in the existing
`RootStatus::LoadFailed` catch with the `UnsupportedVersion` message;
the seat-only lazy loader needs no change, and advertising, ladder
behavior, and metrics are otherwise unchanged.

### Migration mechanism: rekey, not retrain

The v1 test/benchmark producers train on **fixed runouts** — concrete
turn and river boards (for example 9h/8s and 3s/5h) whose rows the
scenarios and the benchmark then query. The seat training path cannot
replace that producer: `GameDef` has no fixed-runout field,
`train_nseat` is chance-sampled SplitMix64 MCCFR, and its
`export_seat_policy`/`export_dfs` emits concrete rows only for nodes the
sampled walk visited (`engine/src/solver/seat_policy.cpp:152-167`, the
`scratch.covered` gate), so the exact boards would have no rows; full
chance materialization is also impossible (the benchmark's single-board
SPR-10 tree is 493,500 sets; times roughly a thousand turn/river boards
it crosses the 1,000,000-node default limit).

The migration therefore rekeys the existing in-memory training into a
seat artifact:

1. keep training in memory with `HeadsUpSolverDebug::train_full` (the
   fixed-runout full-traversal solver, retained by the Non-Goals);
2. derive the seat identity from `solver::to_unified_game(game)`: its
   `GameDef`, per-seat ranges, and `SizeSchedule`;
3. create a `SeatPolicy`, `SeatPolicyAssembler::set_identity(...,
   action_id, card_id)` with the schedule and a matching
   `AbstractionId` minted from `unified.sizes` (the identity must
   reproduce the converted menu exactly — the schedule-conversion
   requirement in Risks), and transfer every trained and override row
   with `SeatPolicyAssembler::add_row` using the same
   `InformationKey` words. The heads-up and seat key layouts are
   byte-identical — pinned by `test_two_seat_byte_identity`
   (`engine/tests/test_seat_policy.cpp:439,458`:
   `information_key(HeadsUpState) == make_information_key(...)` for both
   a flop state and a checked-down river state);
4. publish the result as a **flop-rooted, river-terminal schema-v2**
  artifact with `SeatCheckpointProvenance`
  (`prng_identifier = kFullTraversalPrngIdentifier`, matching the full
  traversal that produced the rows). The rekey helper stamps a genuine
  revision-2 seat identity on the result: `algorithm_revision = 2`,
  `batch_size = 1`, `completed_epochs = completed_iterations` (the
  `epochs_from_samples` invariant with batch 1), and
  `information_sets` equal to the count of transferred rows (the v2
  reader requires manifest count to equal stored rows). The only
  exception is a hand-authored sparse fixture that deliberately uses an
  explicit custom algorithm string (the
  `test_folded_root_identity` route); such a string is named in the
  test and never carries a revision-2 claim. The resident accepts
  flop-rooted (`board_size == 3`) v2 sources and `build_v2_game`
  (`resident_policy.cpp:118-135`) is an identity copy at that shape, so
  the resident view and every fixed-board query are unchanged.

`train_nseat` is used only by the scenarios that already train through
it today (the preflop fixture and the v2 projection fixtures); it is not
introduced into the fixed-runout producers. The test and benchmark
programs keep depending on the in-memory heads-up solver; this is a
test/benchmark-only dependency and adds nothing to the production
artifact or resident path. Hand-assembled sparse rows follow the
existing `test_folded_root_identity` precedent
(`test_resident_policy.cpp:600-660`).

### Fixture publisher migration (required, not optional)

`engine/tests/resident_fixture_publisher.cpp` is rewritten through the
rekey route above: keep its in-memory `HeadsUpSolverDebug::train_full`
run, assemble rows through `SeatPolicyAssembler` on identical key words,
and publish with `SeatCheckpointProvenance` and the seat
`create_checkpoint`/`publish_policy`. The program's JSON
summary contract (path, sha256, root envelope with flop/stacks/
contributions/pot/bigBlind/button) is preserved field-for-field so
`platforms/river-club/tests/w1-resident-wiring.test.ts` needs no
contract change; `bs_resident_fixture` stays a `proto/CMakeLists.txt`
target copied to `bin/bs-resident-fixture`, and the two real-binary
blueprint serving TS cases must stay green. If any asserted numeric
fixture behavior (zero-probability miss, exact root) cannot be
expressed, the migration stops and the scenario is analyzed rather than
weakening the TS assertion.

### Test migration

- `engine/tests/test_resident_policy.cpp`: the three v1 fixture helpers
  (`publish_game`, `publish_root_only`, `publish_synthetic_policy`) are
  rewritten through the rekey route (in-memory `train_full` ->
  `to_unified_game` identity -> `SeatPolicyAssembler` row transfer on
  identical key words -> flop-rooted river-terminal seat publish), not
  through `train_nseat`; the single heads-up rejection probe
  (`:1787-1790`) is rebuilt on a seat checkpoint, and the three
  existing seat-provenance sites (`:591, :653, :762`) are untouched.
  Every one of the ~20 scenarios is migrated, not deleted. Two shapes
  need explicit treatment and are feasible because
  `validate_content_v2` (`strategy_artifact.cpp:1246-1290`) validates
  rows individually and never enforces range completeness:
  (a) the root-only partial export (`publish_root_only`) is reproduced
  by publishing a rekeyed seat policy with the class section
  suppressed; (b) the synthetic single-row and forced
  zero-probability rows are assembled directly through
  `SeatPolicyAssembler` using byte-identical seat keys. Scenario roots
  (golden margin, free-turn, hook/matrix, streets, orphan, shared/zero
  combos, probe byte accounting, miss coverage, continuation,
  fully-blocked, root-only) are mapped one-to-one in the implementation
  plan; a scenario whose fixed-runout rows cannot be rekeyed is a
  stop condition, not a deletion.
- `engine/tests/test_artifacts.cpp`: delete `test_roundtrip` (v1) and
  the v1-only split/resume/crash/corruption/immutable/adversarial cases
  and the six `commit_checkpoint` call sites; remove the revision-1
  identity case from `test_nseat_algorithm_identity`; delete
  `test_canonical_keys`'s revision-1 assertions while retaining full
  revision-2 round-trip coverage. The five v2/v3/preflop arm-empty
  assertions (`:1270, 1317-1318, 1326, 1423, 1541-1542`) are rewritten
  to the nseat arm. Where a deleted v1 case protects a
  format-agnostic invariant not already asserted by a v2/v3 case
  (physical corruption rejection, immutable open, adversarial
  probabilities), that assertion is added to the matching seat fixture
  instead of being dropped.
- `engine/tests/test_seat_policy.cpp`: the codec revision-parameter
  removal breaks its five revision-1 call sites
  (`:373, 379, 381, 412, 441, 461`) and the grammar-1-only rejection
  block; delete the grammar-1 assertions, retarget the
  unsupported-revision case to the single grammar, and KEEP the two
  heads-up-vs-seat byte-identity checks
  (`information_key(HeadsUpState) == make_information_key(...)`) on the
  grammar-2 codec — they are the proof the rekey route depends on.
- Add a focused rejection test: present a minimal SQLite file with
  `PRAGMA user_version = 1` and assert `load_artifact` and
  `probe_artifact` both throw `ArtifactErrorKind::UnsupportedVersion`,
  and that the resident turns it into `LoadFailed`.
- `solver::to_unified_game` keeps its existing in-memory callers
  (`test_resolver.cpp`, `resolver_benchmark.cpp`, and in-memory resident
  test setup that builds a `HeadsUpGame` directly), plus the new rekey
  helpers; it is not removed.

### Benchmark migration (required, not deleted)

`engine/benchmarks/resident_lookup_benchmark.cpp` is migrated, not
removed: it is the only cold/warm resident latency and zero-allocation
regression runner documented in `docs/development/benchmarking.md`. Both
publishers keep their in-memory `HeadsUpSolverDebug::train_full`
training (fixed runouts are essential to the benchmark's full-coverage
query invariants at `:441-447` and the warmup requirement at
`:518-535`) and publish through the rekey route to flop-rooted
river-terminal v2 artifacts with the converted schedule's
`AbstractionId`; the three measured sections (SPR-10 sizing scan,
six-root SPR-4 cold/warm and zero-allocation probe, zero-probability
miss) issue the same resident queries through the v2 game view. Chance
materialization is not attempted. Historical v1 numbers are explicitly
not comparable to the migrated run; the benchmark output and
`benchmarking.md` mark the first post-migration run as a new baseline
with its root shape named. The former `encode_public_key(..., 1)` call
at `:296` is deleted with the codec revision parameter.

### Documentation

- `docs/design/gto-engine.md`: the artifact section describes v2/v3 only;
  schema v1 gets one Historical sentence.
- `docs/development/benchmarking.md`: the resident-lookup benchmark
  description gains the v2 2-seat methodology and the new-baseline
  note.
- RFCs 0004, 0005, 0007 (`:406` coexistence/error text), 0009, 0016,
  0017, and 0034 (the fixture publisher description) keep their records
  but their schema-v1 descriptions are annotated as Historical/
  superseded by this RFC; status fields are not rewritten.
- Any operational reference to a v1 artifact path is updated in the same
  change.

## Dependency Rules

Unchanged. The artifacts module continues to depend on SQLite wrappers
and the solver types; the resident depends on artifacts; nothing new
depends on the artifacts internals. The resident's dependency on
`solver::to_unified_game` for artifact loading disappears; its other uses
are untouched.

## Compatibility and Migration

- **Persisted data:** schema-v1 SQLite artifacts become unreadable. This
  is the intended breaking change. No shipped artifact, resident root,
  packed library section, or backup contains schema v1 (verified;
  backups deleted 2026-10-26). v2/v3 artifacts are byte-identical and
  keep their digests.
- **C++ API:** callers compiling against the deleted heads-up
  `create_checkpoint`/`commit_checkpoint`/`publish_policy` overloads,
  `CheckpointProvenance`, the heads-up `PolicyAssembler`, or the
  `LoadedArtifact::result`/`ArtifactProbe::game` fields get a compile
  error and must use the seat API; such callers are exactly the
  fixture publisher, the lookup benchmark, and tests covered by this
  RFC. The `encode_public_key`/`decode_public_key` signatures lose
  their revision parameter; every surviving call passes revision 2
  today.
- **Test/benchmark artifacts:** the regenerated
  `resident-fixture-policy.db` and the benchmark's scratch artifacts
  become schema v2; the fixture's JSON summary is unchanged and the
  benchmark's historical numbers are superseded by a declared new
  baseline.
- **Runtime:** an operator-configured v1 root transitions from
  loadable (heads-up projection) to `LoadFailed` with a typed message;
  the resolver ladder and operational fallback already own that outcome.
- **No migration stage or dual-read window** is provided, per the
  project's no-legacy rule; rollback is `git revert`.

## Security and Operational Impact

The artifact reader parses untrusted SQLite bytes under bounded
allocations and disabled extensions. Removing the v1 dialect deletes a
parser path (v1 table grammar, v1 key encoding, v1 manifest checks) from
that boundary; the security posture strictly improves. No credentials,
network surface, fairness path, or decision semantics change. The new
operational signal is a `LoadFailed`/`UnsupportedVersion` detail naming
RFC 0038 if a stale v1 path is configured.

## Alternatives Considered

1. **Keep the reader, delete only the writer.** Rejected: every reason
   for the dual arm (separate DDL, key grammar, legacy_domain, probe
   field, three resident branches) lives in the read path; a reader kept
   "just in case" is exactly the legacy layer the project rule removes,
   and there is no v1 artifact left to read.
2. **Ship a v1-to-v2 converter.** Rejected: there is no input to convert
   — no shipped v1 artifact and no backup. Building and testing one
   would be speculative generality.
3. **Delete the resident-lookup benchmark together with the v1
   publisher.** Rejected: it is the documented cold/warm latency and
   zero-allocation regression runner for the resident layer, and the
   resident query machinery it measures is format-agnostic; rekeying its
   publishers to v2 preserves the measurement with a declared new
   baseline, which costs one fixture rewrite and removes no coverage.
4. **Hide v1 behind a build flag.** Rejected by the no-flag rule in
   CLAUDE.md ("Do not introduce feature flags to select between old and
   new implementations").
5. **Fold this into RFC 0037's legacy retirement.** Rejected during
   planning: RFC 0037 owns action-abstraction caps; the artifact schema
   is a separate persisted-format contract with its own consumer
   (resident loading) and deserves independent review.
6. **Retrain the fixed-runout fixtures with `train_nseat` instead of
   rekeying.** Rejected by evidence: the seat trainer is
   chance-sampled, exports only visited rows, and cannot full-chance
   materialize the benchmark tree (see "Migration mechanism").

## Risks

- **A forgotten v1 consumer fails to compile.** Mitigation: the full
  Engine Build Contract builds every benchmark and app, including the
  `bs_resident_fixture` proto target; the API removals are compile
  errors, not silent behavior changes.
- **A local resident entry points at a v1 file.** Mitigation: typed
  `LoadFailed` plus the existing ladder; documented above; no shipped
  configuration does this.
- **Fixture semantic loss.** A migrated rekeyed fixture may not
  reproduce every heads-up fixture behavior. The key words are
  byte-identical (pinned), but the `AbstractionId` and `SizeSchedule`
  stamped on the seat artifact must reproduce the converted menu
  exactly: the rekey helper must mint the action id from
  `to_unified_game(game).sizes` and its matching digest rather than a
  default schedule, because a mismatched schedule would silently change
  served menus; the fixed-runout terminal depth must be River at a
  flop root. Each of the ~20 scenarios is mapped one-to-one in the
  plan, and the real-binary w1 TS assertions are the final guard; an
  unexpressible scenario or a menu mismatch is a stop condition
  requiring RFC amendment, never a deleted test.
- **Benchmark continuity.** The migrated resident-lookup benchmark
  produces a new, non-comparable baseline; mitigating factors are that
  the resident query machinery under measurement is format-agnostic and
  the three measured sections keep identical shapes, and the new
  baseline is explicitly labeled rather than silently replacing the old
  numbers.
- **A format-agnostic test invariant is lost when v1 cases are
  deleted.** Mitigation: each deleted case is mapped to a v2/v3
  assertion in the same change; the rejection test pins the new
  fail-closed behavior.

## Verification Plan

- Engine Build Contract (release):
  `cmake --preset release && cmake --build --preset release --target
  format && cmake --build --preset release && ctest --preset release
  && cmake --build --preset release --target benchmark-multistreet
  && npm run check && npm run proto:check && node bin/replay.mjs`.
- ASan on the artifacts/resident surfaces
  (`--preset asan` focused tests).
- A source grep audit (committed as a documented command, not a new tool
  unless one is warranted): no matches under `engine/` (excluding
  third-party) for `kSchemaV1`, `kSchemaDdlV1`, `kAllowedTablesV1`,
  `kNumericProfileV1`, `kRulesIdentifierV1`, `kDefaultAlgorithmRevision`,
  `kLegacyAlgorithmRevisionV2`, `kArtifactSchemaVersion` (the bare rev1
  constant), `CheckpointProvenance`, `legacy_domain`, `bundle.result`,
  `probe.game`, `PolicyAssembler` (the heads-up class name;
  `SeatPolicyAssembler` remains), or a `revision == 1` key-grammar
  branch; `encode_public_key`/`decode_public_key` have no revision
  parameter.
- The v1 rejection test and the migrated seat-fixture invariants pass.
- The w1 real-binary TS tests run the regenerated
  `bin/bs-resident-fixture` and pass (`npm run check`, which includes
  `w1-resident-wiring.test.ts`).
- The migrated resident-lookup benchmark builds and runs its three
  sections; its first post-migration run is recorded in
  `docs/development/benchmarking.md` as the new baseline.
- `node bin/check-rfcs.mjs` and `node bin/check-docs.mjs` green.

## Rollout Plan

1. Artifacts module removal: the authoritative deletion set above
   (dialect, readers, writers, identities, codec, structs, public
   fields).
2. Shared v2 fixture layer: migrate `resident_fixture_publisher.cpp`
   (JSON contract preserved) and the three `test_resident_policy.cpp`
   helpers, mapping the ~20 scenarios one-to-one.
3. Resident single-arm conversion and the v1 rejection test; w1 TS
   real-binary tests green.
4. test_artifacts migration (v1 case deletion, v2/v3 arm edits,
   invariant relocation) and the benchmark migration with the new
   baseline record; grep audit.
5. Documentation updates; full gates green.

Stop conditions: any production (non-test, non-benchmark) caller still
requires a deleted symbol; any w1 fixture behavior that cannot be
expressed on the 2-seat v2 path; any resident semantic scenario without
a one-to-one v2 mapping; or any v2/v3 artifact stops validating — stop
and amend the RFC rather than reintroducing a compatibility path or
deleting an assertion.

## Rollback Plan

Pure source/test/docs deletion with no format migration and no data
movement: `git revert` restores the v1 code and tests. No v2/v3 bytes
are rewritten, so no artifact rollback is needed.

## Open Questions

None.

## Acceptance Criteria

- **AC1**: The authoritative deletion set is gone from `engine/`
  (excluding third-party): `kSchemaDdlV1`, `kAllowedTablesV1`,
  `kSchemaV1`, `kNumericProfileV1`, `kRulesIdentifierV1`,
  `kDefaultAlgorithmRevision`, `kLegacyAlgorithmRevisionV2`, the bare
  `kArtifactSchemaVersion` rev1 constant, `CheckpointProvenance`,
  `legacy_domain`, the heads-up `PolicyAssembler`, the heads-up
  `create_checkpoint`/`commit_checkpoint`/`publish_policy` overloads,
  `LoadedArtifact.result`, `ArtifactProbe::game`, the revision-1
  branches of `decode_nseat_algorithm`/`nseat_write_identity` and
  `grammar_for_revision`, and the `encode/decode_public_key` revision
  parameter. A grep audit over `engine/` returns no match; the codec
  speaks only the revision-2 grammar.
- **AC2**: A SQLite file carrying `PRAGMA user_version = 1` makes both
  `load_artifact` and `probe_artifact` throw
  `ArtifactErrorKind::UnsupportedVersion`, and a pinned resident test
  shows the root reporting `RootStatus::LoadFailed` with the RFC 0038
  message rather than crashing.
- **AC3**: Every non-test caller of the artifact API publishes through
  the seat path; the only production publisher
  (`preflop_profile_builder`) is unchanged in behavior, and the five
  shipped v2 profiles plus the BSFLIB03 library still validate, load,
  and match their digest pins (including flop-library pack and
  verify-pack and the preflop NashConv tool, which all require the
  nseat arm).
- **AC4**: The resident policy has one artifact-to-game path
  (`game_def`/`build_v2_game`); no `to_unified_game(probe.game)` call
  remains in the artifact/resident production code outside in-memory
  solver/benchmark/test construction.
- **AC5**: The `bs_resident_fixture` publisher builds as the same CMake
  target copied to `bin/bs-resident-fixture`, keeps its in-memory
  fixed-runout training, and emits its existing JSON summary fields
  from a flop-rooted river-terminal v2 artifact produced by the rekey
  route (`to_unified_game` identity + `SeatPolicyAssembler` +
  byte-identical key transfer, `AbstractionId` minted from the converted
  schedule); both real-binary blueprint serving cases in
  `platforms/river-club/tests/w1-resident-wiring.test.ts` pass without a
  JSON contract change.
- **AC6**: Every one of the ~20 `test_resident_policy.cpp` v1 scenarios
  has a named rekeyed v2 counterpart in the implementation plan,
  including root-only partial exports and synthetic single-row / forced
  zero-probability fixtures; the three existing seat-provenance sites
  are untouched and only the `:1790` heads-up rejection probe is
  rebuilt; every deleted v1 `test_artifacts.cpp` invariant (physical
  corruption rejection, immutable open, adversarial probability
  rejection, split-run/resume) has a passing v2/v3 counterpart; the
  five arm-empty assertion sites are rewritten to the nseat arm;
  `test_seat_policy.cpp` keeps its byte-identity checks on grammar 2 and
  loses only grammar-1 assertions; ctest passes with no skipped
  placeholder.
- **AC7**: The resident-lookup benchmark builds and runs all three
  measured sections against rekeyed flop-rooted river-terminal v2
  artifacts (in-memory `train_full` retained; full-coverage and warmup
  invariants still satisfied; no chance materialization); its
  methodology and the labeled new baseline are recorded in
  `docs/development/benchmarking.md`.
- **AC8**: The full Engine Build Contract release chain passes
  (format, build, ctest, benchmark-multistreet, npm check, proto:check,
  replay), the artifacts/resident tests pass under ASan, and
  `check-rfcs`/`check-docs` pass with the gto-engine, benchmarking, and
  RFC 0004/0005/0007/0009/0016/0017/0034 documentation updates.

## Implementation Evidence

Status: Implemented 2026-10-26.

The deletion set is gone from the engine (`kSchemaDdlV1`,
`kAllowedTablesV1`, `kNumericProfileV1`, `kRulesIdentifierV1`,
`kDefaultAlgorithmRevision`, `kLegacyAlgorithmRevisionV2`,
bare `kArtifactSchemaVersion`, heads-up `CheckpointProvenance`/
`create_checkpoint`/`commit_checkpoint`/`PolicyAssembler`,
`LoadedArtifact::result`, `ArtifactProbe::game`, `legacy_domain`,
`grammar_for_revision`, and the encode/decode revision parameter). The
codec speaks only the revision-2 four-field event grammar; a fold is not
terminal in that grammar (multiway hands continue), matching the
pre-RFC-0038 revision-2 behavior exactly. `nseat_write_identity` accepts
revision 2 and an explicitly named revision-0 custom identity (test
fixtures) and preserves an unrecognized provenance string verbatim;
revision 1 is unrecognizable.

One transport detail needed an explicit design decision during
implementation: the rekeyed fixed-runout policies had to keep carrying
their conditional turn/river cards. The schema-v2 `game.fixed_turn`/
`fixed_river` columns previously meant only the turn/river-rooted board
prefix and were forbidden on a flop-rooted root, so the in-memory
`UnifiedGame.fixed_runout` used by the resident chance-support and
runout checks had nowhere to round-trip. The columns now carry the
CONDITIONAL runout for a flop-rooted, river-terminal policy (NULL on a
free street), still the board prefix for a turn/river-rooted root, and
always NULL for preflop and schema-v3 class policies. The value lives on
`SeatPolicy::fixed_runout` and `ArtifactProbe::fixed_runout`, is set by
the test-only rekey helper and the artifact reader, and is copied by the
single `build_v2_game` path. A board-class artifact carrying one is
rejected by load and probe.

Verified 2026-10-26:

- Release build clean; `ctest` 79/79 passed (artifacts suite rewritten to
  v2/v3 fixtures with the physical-corruption, immutable-publication,
  ENOSPC/killed-writer, WAL-sidecar, zip-bomb, writer-pragma,
  probability/stream-adversarial, and byte-deterministic-publication
  invariants relocated off the retired v1 cases; 26 resident scenarios,
  including the new `user_version=1` resident `LoadFailed` pin).
- New rejection tests: an empty SQLite file with the application id and
  `user_version` 1 (also 0 and 99) fails both `load_artifact` and
  `probe_artifact` with `UnsupportedVersion` and the
  "schema v1 retired by RFC 0038" message; the resident reports
  `LoadFailed` with that message and occupies no advertised slot.
- `bs_resident_fixture` emits `user_version=2`, the conditional runout
  columns populated, 10,080 states, and the unchanged JSON contract; the
  w1 real-binary TS cases pass inside `npm run check` (133/133).
- All five shipped preflop profiles (2/3/4/5/6-way) load through the new
  reader and run one frozen-deviation evaluation (rc=0); the BSFLIB03
  library advertises under `--verify-binary` with its pinned digest.
- The migrated resident-lookup benchmark runs all three measured
  sections against rekeyed schema-v2 artifacts: 493,500 SPR-10 sets and
  the 115,056-set six-root aggregate (enumerated queries asserted equal
  to the published count), zero timed-batch allocations; the labeled
  Linux baseline is recorded in benchmarking.md.
- `benchmark-multistreet` PASS; `npm run check`, `npm run proto:check`,
  and `node bin/replay.mjs` green; the RFC 0037 policy audit (6/6) and
  ablation checker (matrix CSV) pass; `check-rfcs` and `check-docs`
  green.
- AddressSanitizer + UndefinedBehaviorSanitizer: `test_artifacts`,
  `test_seat_policy`, and `test_resident_policy` all passed with zero
  sanitizer findings (the resident run is slow under instrumentation
  because the pre-existing 3-4 seat 200-iteration fixtures expand to
  ~600k-1m rows and are exercised train/publish/load/index; those
  reader/index/reach paths are unchanged by this RFC).

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0038-approval-1 (independent approval agent, auto_model/alwaysday1_max;
              same agent across all three rounds)
Decision date: 2026-10-26
Review outcome: Approved
  Two minor non-blocking nits were folded into the Accepted text: the
  rekey helper must stamp a genuine revision-2 seat identity
  (algorithm_revision 2, batch_size 1, completed_epochs =
  completed_iterations, information_sets = transferred row count;
  custom algorithm strings only for explicitly named hand-authored
  sparse fixtures), and two editorial fixes (a duplicated "free-turn"
  phrase and a malformed article).
Reviewed scope: Three rounds. Round 1 returned Changes Requested with
  one blocker (both resident-lookup benchmark publishers are heads-up
  v1, not a seat/v1 mix; deleting the benchmark would lose the only
  resident latency/zero-allocation regression runner), three majors
  (the bs_resident_fixture binary and its two real-binary w1 TS tests
  were missing; roughly twenty test_resident_policy scenarios depend on
  v1 fixtures; the deletion set omitted commit_checkpoint, the
  revision-1 key codec, CheckpointProvenance, the heads-up
  PolicyAssembler, and manifest defaults), and three minors. Round 2
  returned Changes Requested with one blocker: the prescribed
  train_nseat migration is infeasible for fixed-runout producers
  (chance-sampled training emits only visited rows and cannot
  full-chance materialize the benchmark tree); the revision requires
  the rekey route (retain in-memory train_full, convert identity via
  to_unified_game, transfer rows through SeatPolicyAssembler on
  byte-identical information keys proven identical by
  test_two_seat_byte_identity, publish a flop-rooted river-terminal v2
  artifact), plus the test_seat_policy.cpp grammar-1 inventory, a
  checkpoint-site factual correction, and numbering. Round 3 verified
  the rekey route end to end in source (to_unified_game shape and
  schedule copy, identity_action_id cap-0 menu preservation,
  assembler emplace-only insertion, the v2 per-row validator, the
  byte-identity proof, and the hand-authored sparse precedent),
  re-audited the complete deletion and migration scope against the
  plan's one-to-one scenario map, and ran both documentation checkers
  green (38 RFCs, 433 files). The approval is design approval only; it
  authorizes no spending, credentials, live play, or destructive
  operations.
Review summary: A locally closed, evidence-based removal of the last
  persisted-format compatibility layer, with every v1 producer
  migrated rather than deleted, objectively testable acceptance
  criteria, and fail-closed UnsupportedVersion handling for stale
  files. Remaining risk is implementation discipline (rev2 identity
  fields, schedule-matched action id, row counts), guarded by the
  one-to-one scenario map, the w1 real-binary TS tests, and the full
  Engine Build Contract.
```
