---
rfc: "0038"
subject: "Retire the schema-v1 heads-up artifact format and algorithm revision 1"
status: "Proposed"
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
  schema v1, `info_key_revision = 1`, `kNumericProfileV1`,
  `kRulesIdentifierV1`, `kDefaultAlgorithmRevision` (rfc0004-rev1), or
  `kLegacyAlgorithmRevisionV2` (nseat-rev1) remains in `engine/`.
- G2: opening a schema-v1 file through `load_artifact` or
  `probe_artifact` fails with `ArtifactErrorKind::UnsupportedVersion`;
  the resident reports `RootStatus::LoadFailed` with the diagnostic and
  never crashes; a pinned test presents a real v1 byte fixture.
- G3: `create_checkpoint` and `publish_policy` accept only
  `SeatTrainingResult`; the heads-up `TrainingResult` overloads and the
  v1 `LoadedArtifact.result` arm are deleted.
- G4: the resident policy has one artifact-to-game path (the
  `game_def`/`build_v2_game` arm); the three `to_unified_game(probe.game)`
  branches are gone.
- G5: tests and benchmarks build and pass with no v1 fixture; design
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
  runs `kSchemaDdlV1`; the public heads-up overloads
  `create_checkpoint(path, const TrainingResult&, …)`
  (`strategy_artifact.hpp:193`) and the v1 arm of `publish_policy`
  (`strategy_artifact.cpp:~2993,3030`) are its only callers. Outside
  `engine/tests`, `publish_policy` is called by
  `engine/benchmarks/preflop_profile_builder.cpp:333` (seat, v2) and
  twice by `engine/benchmarks/resident_lookup_benchmark.cpp` — once with
  a heads-up result (`:156-157`, the only non-test v1 producer) and once
  with a seat result (`:204-205`). The preflop profile builder, the only
  production artifact publisher, uses the seat overload.
- **Read surface.** `load_path` branches on schema version: v1 reads
  `game`/`sizes`/`ranges` plus v1 state/action/training tables with
  `read_states(..., legacy_domain = true)` (`:2631`); v2/v3 go through
  `load_path_seat`. `LoadedArtifact` carries either `result`
  (`HeadsUpTrainingResult`) or `nseat` (`:159-164`). `ArtifactProbe`
  carries a v1 `HeadsUpGame game` field and v2 `game_def`/`ranges`/
  `sizes` optionals (`strategy_artifact.hpp:~248-260`).
- **Identity surface.** v1 manifests default to
  `kDefaultAlgorithmRevision = "rfc0004-rev1-full-kSimple-prng1"`,
  `kNumericProfileV1`, and `kRulesIdentifierV1 =
  "rfc0004-heads-up-flop-v1"`. The seat line still decodes
  `kLegacyAlgorithmRevisionV2 = "rfc0009-nseat-rev1-sampled-splitmix64"`
  as revision `{1, 0}` in `decode_nseat_algorithm`, and
  `nseat_write_identity` can still emit it for an
  `algorithm_revision == 1` result; a test round-trips it
  (`engine/tests/test_artifacts.cpp:1120-1143`).
- **Shipped artifacts.** `artifacts/preflop-profile{,-3way-100bb,-4way,
  -5way,-6way}` are schema v2 with `rfc0016-nseat-rev2-frozen-batch-*`
  revisions; `artifacts/flop-library-6way-fullrange` is BSFLIB03 over v3
  class artifacts. Nothing under `artifacts/` is schema v1.
- **Resident consumer.** `engine/src/resident/resident_policy.cpp` has
  the v1 arm in three places — two probe paths (`:598`, `:677`) and the
  full lazy-load path (`:728-729`) — each via
  `solver::to_unified_game(...)`; the v2 path uses
  `build_v2_game(*probe.game_def, ...)`.
- **Test surface.** `engine/tests/test_artifacts.cpp` contains a
  heads-up `test_roundtrip` (`:954`) and v1-based
  split-run/resume/crash/corruption/immutable/adversarial cases that
  create v1 checkpoints with `trained.result`; the v1 algorithm-identity
  case lives at `:1095` (`test_nseat_algorithm_identity`, including the
  rev1 legacy round-trip). The seat-stream adversarial and v2/v3
  round-trips already cover the same invariant classes for current
  formats.
- **No external readers.** `grep` over `platforms/`, `clients/`,
  `apps/`, `bin/`, and `tools/replay` finds no schema-version-aware
  consumer; TypeScript and replay never open artifact SQLite files.

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

1. Delete the schema-v1 dialect: `kSchemaDdlV1`, `kAllowedTablesV1`,
   `kSchemaV1`, `write_schema`, the heads-up `write_all`,
   `insert_game`/`insert_sizes`/`insert_ranges`, the heads-up policy
   insert path, `read_game`/`read_sizes`/`read_ranges`/`validate_game`,
   `read_auxiliary_tables` if v1-only, and the v1 manifest validator.
2. `open_validated` dispatches only on versions 2 and 3; version 1 takes
   the existing `UnsupportedVersion` failure with a message naming the
   retired schema (`"unsupported schema version: 1 (schema v1 retired
   by RFC 0038)"`).
3. Delete the heads-up public surface:
   `create_checkpoint(path, const TrainingResult&, …)`, the v1 branch of
   `publish_policy`, `LoadedArtifact::result` (the heads-up arm), and
   `ArtifactProbe::game` plus its v1-only population path.
4. Delete the legacy identities and defaults:
   `kDefaultAlgorithmRevision` (rfc0004-rev1), `kNumericProfileV1`,
   `kRulesIdentifierV1`, `kLegacyAlgorithmRevisionV2`, the revision-1
   branch of `decode_nseat_algorithm`, and the revision-1 emit branch of
   `nseat_write_identity`. The shared `ArtifactManifest` default for
   `algorithm_revision`/`numeric_profile` becomes empty; every writer
   already populates both through `manifest_for_v2`, and the existing
   non-empty reader checks keep an unpopulated manifest invalid.
   `kSampledPrngIdentifier` (`splitmix64-rev1`) stays — it names the
   sampler, not the artifact schema, and v2 manifests carry it.
5. `read_state_fields`/`read_states` lose `legacy_domain`; the player
   domain is always `0..player_count-1`.
6. `CheckpointProvenance` loses the rfc0004 default; callers that need
   it construct the seat provenance explicitly (the preflop builder
   already does).

### Resident consumer

`engine/src/resident/resident_policy.cpp` keeps only the `game_def`
arm: the two probe sites and the lazy-load site stop constructing a
`to_unified_game(probe.game)` fallback. A v1 root now fails at
`probe_artifact`/`load_artifact` and lands in the existing
`RootStatus::LoadFailed` catch with the `UnsupportedVersion` message;
advertising, ladder behavior, and metrics are otherwise unchanged.

### Benchmarks and tests

- `resident_lookup_benchmark.cpp`: the heads-up benchmark case
  (`:156-157`) is deleted; the zero-fallback seat case (`:204-205`)
  remains and gains the lookup coverage the deleted case shared with it
  only if a distinct invariant is lost (expected: none — the lookup
  machinery is format-agnostic).
- `test_artifacts.cpp`: delete `test_roundtrip` (v1) and the v1-only
  split/resume/crash/corruption/immutable/adversarial cases; remove the
  revision-1 identity case from `test_nseat_algorithm_identity`. Where a
  deleted v1 case protects a format-agnostic invariant not already
  asserted by a v2/v3 case (physical corruption rejection, immutable
  open, adversarial probabilities), that assertion is added to the
  matching seat fixture instead of being dropped.
- Add a focused rejection test: write a minimal SQLite file with
  `PRAGMA user_version = 1` (or retain a tiny checked-in v1 header
  fixture), and assert `load_artifact` and `probe_artifact` both throw
  `ArtifactErrorKind::UnsupportedVersion`, and that the resident turns it
  into `LoadFailed`.
- `solver::to_unified_game` keeps its existing in-memory callers
  (resolver benchmarks and tests that build a `HeadsUpGame` directly);
  it is not removed.

### Documentation

- `docs/design/gto-engine.md`: the artifact section describes v2/v3 only;
  schema v1 gets one Historical sentence.
- RFCs 0004, 0005, 0009, 0016, and 0017 keep their records but their
  schema-v1 descriptions are annotated as Historical/superseded by this
  RFC; status fields are not rewritten.
- `docs/development/build-and-test.md` and any operational reference to
  a v1 artifact path are updated in the same change.

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
  `create_checkpoint`/`publish_policy` overloads or the
  `LoadedArtifact::result`/`ArtifactProbe::game` fields get a compile
  error and must use the seat API; the only such callers are the
  benchmark and tests in this change.
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
3. **Hide v1 behind a build flag.** Rejected by the no-flag rule in
   CLAUDE.md ("Do not introduce feature flags to select between old and
   new implementations").
4. **Fold this into RFC 0037's legacy retirement.** Rejected during
   planning: RFC 0037 owns action-abstraction caps; the artifact schema
   is a separate persisted-format contract with its own consumer
   (resident loading) and deserves independent review.

## Risks

- **A forgotten v1 consumer fails to compile.** Mitigation: the full
  Engine Build Contract builds every benchmark and app; the API removals
  are compile errors, not silent behavior changes.
- **A local resident entry points at a v1 file.** Mitigation: typed
  `LoadFailed` plus the existing ladder; documented above; no shipped
  configuration does this.
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
  unless one is warranted): no matches for `kSchemaV1`, `kSchemaDdlV1`,
  `kNumericProfileV1`, `kRulesIdentifierV1`, `kDefaultAlgorithmRevision`,
  `kLegacyAlgorithmRevisionV2`, `legacy_domain`, or
  `bundle.result` under `engine/`.
- The v1 rejection test and the migrated seat-fixture invariants pass.
- `node bin/check-rfcs.mjs` and `node bin/check-docs.mjs` green.

## Rollout Plan

1. Artifacts module removal (DDL, readers, writers, identities, public
   fields).
2. Resident single-arm conversion and the v1 rejection test.
3. Benchmark/test deletion and migration; grep audit.
4. Documentation updates; gates green.

Stop condition: any production (non-test, non-benchmark) caller still
requires a deleted symbol, or any v2/v3 artifact stops validating —
stop and reassess rather than reintroduce a compatibility path.

## Rollback Plan

Pure source/test/docs deletion with no format migration and no data
movement: `git revert` restores the v1 code and tests. No v2/v3 bytes
are rewritten, so no artifact rollback is needed.

## Open Questions

None.

## Acceptance Criteria

- **AC1**: No symbol or string for the v1 dialect remains in `engine/`:
  `kSchemaDdlV1`, `kAllowedTablesV1`, `kSchemaV1`, `kNumericProfileV1`,
  `kRulesIdentifierV1`, `kDefaultAlgorithmRevision`,
  `kLegacyAlgorithmRevisionV2`, the heads-up `create_checkpoint`/
  `publish_policy` overloads, `LoadedArtifact.result`,
  `ArtifactProbe.game`, `legacy_domain`, and the revision-1 branches of
  `decode_nseat_algorithm`/`nseat_write_identity` are all gone. A grep
  audit over `engine/` returns no match (excluding third-party).
- **AC2**: A SQLite file carrying `PRAGMA user_version = 1` makes both
  `load_artifact` and `probe_artifact` throw
  `ArtifactErrorKind::UnsupportedVersion`, and a pinned resident test
  shows the root reporting `RootStatus::LoadFailed` with the RFC 0038
  message rather than crashing.
- **AC3**: Every non-test caller of the artifact API publishes through
  the seat path; the only production publisher
  (`preflop_profile_builder`) is unchanged in behavior, and the five
  shipped v2 profiles plus the BSFLIB03 library still validate, load,
  and match their digest pins.
- **AC4**: The resident policy has one artifact-to-game path
  (`game_def`/`build_v2_game`); no `to_unified_game(probe.game)` call
  remains outside in-memory solver/benchmark construction.
- **AC5**: Deleted v1 test cases are accounted for: every
  format-agnostic invariant (physical corruption rejection, immutable
  open, adversarial probability rejection, split-run/resume) has a
  passing v2/v3 counterpart; ctest passes with no skipped placeholder.
- **AC6**: The full Engine Build Contract release chain passes
  (format, build, ctest, benchmark-multistreet, npm check, proto:check,
  replay), the artifacts/resident tests pass under ASan, and
  `check-rfcs`/`check-docs` pass with the gto-engine, RFC 0004/0005/
  0009/0016/0017, and operational doc updates.

## Decision

Pending independent approval-agent review.
