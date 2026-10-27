# RFC 0034 Implementation Plan — Unversioned Engine Protocol

Status: Historical

Retired as an operational document on 2026-10-27 by [RFC 0040](../rfcs/0040-retire-river-club-agent-api.md): its provider v2.1 references describe the deleted integration. The RFC 0034 unversioned-protocol implementation evidence below is retained verbatim as a dated record.
Created: 2026-10-24
Owner: Engine protocol

## Outcome

The single IDL is `proto/bigshark/engine/engine.proto`, package
`bigshark.engine`; there is no second schema form anywhere in source,
generated paths, tests, docs, or fuzzing. This plan records the evidence the
RFC requires: the normalized descriptor equivalence, the full token
inventory with closed classifications, and the fuzz campaign result.

## Commands (reproducible)

```bash
# Descriptor equivalence (run before and after the move)
node_modules/.bin/buf breaking proto --against proto/baseline/v2.binpb --exclude-imports
node_modules/.bin/buf build --exclude-source-info -o /tmp/before.binpb proto
node_modules/.bin/buf build --exclude-source-info -o /tmp/after.binpb proto
# normalize both FileDescriptorProtos (sorted-key JSON) and diff

# Token inventory
rg -ni --no-heading \
  -e '\bv[0-3]\b' -e 'minor[- ]?[0-2]' \
  -e 'engine/v[0-9]' -e 'engine::v[0-9]' -e 'bigshark\.engine\.v[0-9]' -e '\bpv[12]\b' \
  engine apps clients platforms bin tools proto docs CMakeLists.txt package.json \
  | grep -v node_modules | grep -v build/
```

## Descriptor equivalence

Normalized `FileDescriptorProto` comparison of the pre-rename
(`bigshark/engine/v2/engine.proto`, `bigshark.engine.v2`) and post-rename
(`bigshark/engine/engine.proto`, `bigshark.engine`) descriptors, source info
excluded, shows exactly the three permitted difference classes:

1. one `package` line (`bigshark.engine.v2` -> `bigshark.engine`);
2. one file `name` line;
3. fully-qualified embedded cross-references in message `typeName` fields
   (`.bigshark.engine.v2.X` -> `.bigshark.engine.X`), 68 occurrences.

No difference in field numbers, types, labels, oneofs, json names, reserved
ranges/names, enum names/ordinals, defaults, or options (including the three
`buf.validate.oneof` field options). `buf breaking
--against proto/baseline/engine.binpb --exclude-imports` is green against the
rebased baseline; the old `proto/baseline/v2.binpb` is deleted in the same
change. With no `Any` messages, the package name never appears in serialized
frame bytes, so same-language conformance and the real-binary w1 suite are
the behavioral cross-checks.

## Token inventory

Before-scan total: 2,660 hits. After-scan total: 1,617 hits. The after-scan
partitions completely into the six closed buckets; the residual
(unclassified) set is empty.

| Bucket | After-scan hits | Content |
| --- | --- | --- |
| 1. Frozen behavior literals | 13 (+ scheme identifiers) | `size-v1`, `suit-canonical-v1`, `rfc0007-pot-fractions`, `rfc0008-coarse-to-exact:v1`, `menu-v1`, `BSV1-sam`; the `CardBucketKind` names (`CategoryTiersV1`, `TiersDrawV1`, `Preflop169`) are API identifiers, not generation labels |
| 2. Transport axis | on dedicated field lines | `protocol_version`, `supportedProtocolVersions`, accepted value 3; comments rewritten, field unchanged |
| 3. External tooling/provider | 5 + 442 vendored | `version: v2` in Buf config files; River Club provider `v2.1` references; vendored `yyjson`, `sqlite`, abseil, open_spiel variable names (never rewritten) |
| 4. Artifact schema axis | 244 | `schema-v1/v2/v3`, `BSFLIB02`, `BSWARM01/02`, `rfc0004-…-v1`, `rfc0009-…-v1`, `rfc0010-…-v1`, `rfc0016-…rev2`, `kRulesIdentifierV*`, `kSchemaV*`, `postflop-onthefly/declared-table` tags, `user_version`/`card_abstraction_version` fields |
| 5. Mermaid | 1 | `stateDiagram-v2` |
| 6. History | 909 | `docs/rfcs/` (583), `docs/plans/` (326); never edited |

Bare generation prose that previously appeared in artifact-path files
(`engine/src/artifacts/`, `engine/include/bs/strategy_artifact.hpp`,
`unified_game.hpp`, `seat_policy.hpp`, `engine/src/resident/resident_policy.cpp`,
`resident_index.hpp`, `mmap_library.cpp`, and the artifact/resident tests)
was rewritten to the canonical `schema-vN` literal, not whitelisted
free-form. User-facing error strings were also canonicalized
("the seat-generic schema-v2 profile", "schema-v2 writer/reader ...").

## Structural changes

- IDL moved to `proto/bigshark/engine/engine.proto`; 33 generation-labelled
  IDL comments and the package declaration rewritten; `buf.yaml` adds the
  `PACKAGE_VERSION_SUFFIX` lint exception with an RFC 0034 rationale.
- C++ namespace `::bigshark::engine`, alias `pe`; generated header
  `<bigshark/engine/engine.pb.h>`; TypeScript path `bigshark/engine/engine_pb.js`;
  `proto/CMakeLists.txt` generated-file lists and IDL dependency updated.
- Baseline regenerated as `proto/baseline/engine.binpb`; `package.json`
  breaking gate updated.
- Fuzz target split into three files: `fuzz_harness.cpp` (the single
  `LLVMFuzzerTestOneInput`), `fuzz_standalone_driver.cpp` (`main`, linked
  only when `BIGSHARK_FUZZ_LIBFUZZER=OFF`), and `fuzz_seeds.hpp` (current
  schema seed sets, legal/negative split). The legal builder is shared with
  `test_conformance` through `conformance_fixtures.hpp`; no second fixture.
  CMake selects the driver TU by option, so both configurations link with no
  duplicate `main`.
- The dead `v1_minor2` battery is deleted from
  `tools/mutation/decision-stage5.json`; surviving labels are
  generation-free.
- Current-truth docs updated: the protocol reference, build-and-test fuzz
  section, `gto-engine.md`, practice table, docs index, and the root README
  link.

## Verification evidence

- `npm run proto:check`: lint, format, breaking (against `engine.binpb`),
  C++/TS generation, generated-TS typecheck — green.
- `npm run check` (typecheck, TS build, source-policy, docs/rfc compiled
  checks, Node suite) — green.
- Compiled Node suite including the two real-binary w1 tests: 119/119 pass,
  0 skipped, against the rebuilt and republished `bin/bigshark-engine` and a
  refreshed `bin/bs-resident-fixture`.
- Same-language conformance: `test_conformance` green after the shared
  fixture extraction.
- Fuzz standalone build (`build/fuzz`, Clang 14, ASan/UBSan,
  `BIGSHARK_FUZZ_LIBFUZZER=OFF`): the 100,000-input deterministic campaign
  completed cleanly ("100000 inputs over 24 seeds"); the fixed negative
  corpus (framing garbage, open-enum attacks, a response frame, wrong
  transport versions) produced no strategy response; the valid HEURISTIC
  seed is pinned to reach the decision arm.
- Fuzz libFuzzer build (`build/fuzz-libfuzzer`,
  `BIGSHARK_FUZZ_LIBFUZZER=ON`): links cleanly with exactly one
  `LLVMFuzzerTestOneInput` and no driver `main`; a 500-input smoke run
  completed.
- Stale generated `engine/v1` and pre-rename `engine/v2` trees removed from
  the debug, release, generated, and test-JS build directories; the ASan
  directory is regenerated by the Clang ASan build.
