---
rfc: "0034"
subject: "Unversioned Single-Form Engine Protocol"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-24"
updated: "2026-10-25"
owners: "Proto, engine protocol, engine host, node client, river adapter, engine tests, docs"
supersedes: ""
superseded-by: ""
---

# RFC 0034: Unversioned Single-Form Engine Protocol

## Summary

The engine protocol already has exactly one live form: every in-repo
participant sends `Envelope.protocol_version == 3`, the only IDL is
`proto/bigshark/engine/v2/engine.proto`, and the old v1 mapper, validator, and
JSON service are gone. What survives is naming rot: the sole package is called
`bigshark.engine.v2`, the documented protocol fuzz target includes a deleted
generated header (`bigshark/engine/v1/engine.pb.h`) and no longer even defines
its fuzz entry point, and first-party comments still describe v0/JSON and
minor-0/1 paths that do not exist. This RFC removes the orphaned versioning
surface so the repository visibly contains one final form: package
`bigshark.engine`, proto at `proto/bigshark/engine/engine.proto`, one
regenerated descriptor baseline, one fuzz harness driving the current
envelope entry point, and no first-party code comment referencing a coexisting
protocol generation.

No message field, field number, enum value, frame encoding, capability tuple,
or persisted artifact format changes. The envelope's `protocol_version` field
and the external River Club wire protocol are explicitly out of scope.

## Motivation

The project directive is that no internal V1/V2 concept exists: when an old
form is replaced, its remnants are deleted in the same change, not carried as
comments, baselines, or build targets. Three concrete defects show the orphaned
naming is already costing correctness, not just aesthetics:

1. **The documented fuzz target is dead and unbuildable.**
   `engine/tests/fuzz_seeds.hpp:9` includes
   `<bigshark/engine/v1/engine.pb.h>`, uses `namespace bigshark::engine::v1`,
   and builds v1 messages (`set_protocol_minor`, the `HandState` /
   `GameDefinition` / `PlayerState` model). That generated header exists only in
   stale build directories; `proto/bigshark/` contains no v1 IDL, so a clean
   `cmake -DBIGSHARK_ENABLE_FUZZ=ON` cannot compile. Worse, `fuzz_main.cpp:24`
   declares `LLVMFuzzerTestOneInput` extern and calls
   `bs::fuzz::structuredFuzzSeeds()`, but no translation unit defines either —
   the harness lost its entry point when the v1 service was removed. The
   security coverage documented in `docs/development/build-and-test.md:170-193`
   therefore does not exist.

2. **Comments describe deleted paths as live alternatives.**
   `engine/include/bs/resolver.hpp:4` says the resolver "never changes
   v0/minor-0 behavior"; `engine/src/policy/decision.cpp:471-473` distinguishes
   "a validated v1 request" from "the v0/JSON path, which keeps its historical
   classification"; `engine/include/bs/resident_policy.hpp:16` says the stage
   "does NOT wire the host, service, policy, v0, or protocol layers". There is
   no v0/JSON path and no minor-0/1 dispatch in the tree —
   `engine/src/protocol/envelope.cpp:281` rejects any `protocol_version != 3`.
   A reader audits a dual surface that is not there.

3. **The sole package is named after a dead twin.** `package bigshark.engine.v2`
   (`proto/bigshark/engine/v2/engine.proto:3`) has no v1 sibling, and the C++
   alias `pv2` plus the generated paths `bigshark/engine/v2/engine.pb.{h,cc}`
   propagate the orphan name into 29 files across engine, host, node client,
   river adapter, tests, and docs.

The engine protocol is strictly internal: its only participants are
`apps/engine-host` and `clients/node` (consumed by the River runner). No
external provider speaks it. Renaming the package therefore cannot break an
external contract the way a River Club change could.

## Goals

- One proto package `bigshark.engine` at
  `proto/bigshark/engine/engine.proto`, generated to
  `bigshark/engine/engine.pb.{h,cc}` and
  `bigshark/engine/engine_pb.js`, with every in-repo include, import, and
  namespace alias updated atomically.
- One descriptor baseline `proto/baseline/engine.binpb` replacing
  `proto/baseline/v2.binpb`; `npm run proto:breaking` runs against it.
- A working protocol fuzz target: structured seeds in the **current** request
  schema, one defined `LLVMFuzzerTestOneInput` driving the real frame decoder
  and `bs::handleEnvelope`, building under the documented
  `-DBIGSHARK_ENABLE_FUZZ=ON` configuration.
- No first-party source or comment in `engine/`, `apps/`, `clients/`,
  `platforms/`, `bin/`, `tools/`, or `proto/` references a coexisting v0/v1
  protocol generation or minor-0/1 dispatch, except the stable identifiers
  listed in Non-Goals.
- Current-truth documentation (`docs/reference/protobuf-engine-protocol.md`,
  `docs/development/build-and-test.md`, `docs/integrations/river-club.md` where
  it names paths) describes the single form.

## Non-Goals

- **No change to `Envelope.protocol_version` or the capabilities handshake.**
  Per project rules the wire version field is a transport detail, not an
  internal versioning scheme; the host continues to accept exactly `3` and
  reject anything else. The field is not renamed and the capabilities tuple
  negotiation is untouched.
- **No change to the external River Club protocol.** `docs/SKILL.md`,
  `docs/openapi.json`, and `docs/upstream-STRATEGY.md` reference the provider's
  own v2.1 contract, which the project does not control. Upstream content is
  untouched.
- **No change to message schema.** Field names, numbers, types, enum ordinals,
  oneof layout, ULEB128 framing, and the maximum-frame constant are unchanged.
  Binary wire compatibility is preserved by construction (there is no
  `google.protobuf.Any` and no type URL in the IDL, so the package name is not
  present on the wire).
- **No rename of digest-pinned abstraction identities.** `suit-canonical-v1`
  (card-abstraction name inside digest `af83bd016a92d2d6`),
  `rfc0008-coarse-to-exact:v1` (frozen translator identity), and the
  `menu-v1` behavior-menu tag are stable algorithm identifiers, not coexisting
  protocol generations. They appear inside artifact digests and sealed
  checkpoints; renaming them would invalidate published artifacts for zero
  functional gain.
- **No change to the labeled operational fallback.** The C++ heuristic policy
  and the TypeScript operational outage fallback remain: they are legal-action
  safety paths required by project rule 5, not a parallel strategy
  implementation.
- **No rewrite of history.** RFCs and historical plan documents keep their
  text; status fields and the documentation index remain the record of what was
  true when. Only Current-truth documents are edited.
- **No change to Buf's own `version: v2`.** The string appears in three Buf
  configuration files — `proto/buf.gen.yaml`, `proto/buf.yaml`, and
  `proto/buf.lock` — and names Buf's configuration schema, not our protocol.
- **No change to the unrelated artifact-schema version axis.** Resident
  policy schemas (`schema-v2`, `schema-v3` checkpoint/projection names and
  their fixture filenames), packed-format magics (`BSFLIB02`), checkpoint
  magics (`BSWARM01`/`BSWARM02` per RFC 0030), and algorithm-revision strings
  are an independent versioning axis with real coexisting persistence
  semantics. They are explicitly out of scope.

## Current State and Evidence

Facts verified in the repository on 2026-10-24:

- `find proto -name '*.proto'` returns exactly one IDL:
  `proto/bigshark/engine/v2/engine.proto`, `package bigshark.engine.v2`.
- The envelope handler accepts one version:
  `engine/src/protocol/envelope.cpp:281` rejects
  `envelope.protocol_version() != 3`; the response always sets 3
  (`:108,:140`); capabilities advertise only `3`
  (`engine/src/protocol/response_mapper.cpp:440`).
- The old engine `v1_*` C++ files are already gone: `engine/src/protocol/`
  contains `resident_mapper.cpp`, `semantic_validator.cpp`,
  `request_mapper.cpp`, `response_mapper.cpp`, `envelope.cpp`,
  `frame_stream.cpp`, `mappers.hpp` — no v1-prefixed file. The TypeScript
  `v1-mapper.ts` referenced by older plans is also absent.
- Stale generated trees survive locally in every preset's build directory
  (`build/{debug,release,asan,generated}/.../bigshark/engine/v1` and matching
  `.o` files). They are not recreated by the current IDL: Buf generation runs
  with `clean: true`, so they are leftovers from preset build directories that
  have not regenerated since the v1 IDL was deleted, and the next generation
  removes them. They are gitignored; the implementation deletes the stale
  trees from all preset directories as part of the rename so no on-disk v1
  path remains.
- The fuzz target (`proto/CMakeLists.txt:152-180`, option
  `BIGSHARK_ENABLE_FUZZ`, default OFF, Clang-only) compiles
  `engine/tests/fuzz_seeds.hpp` (v1 messages and
  `set_protocol_minor`) and `fuzz_main.cpp`, which calls
  `bs::fuzz::structuredFuzzSeeds()` and `LLVMFuzzerTestOneInput`; neither
  symbol is defined anywhere in the tree (`grep` over `engine/`). The target
  can neither compile on a clean checkout nor link on a stale one.
- `proto/baseline/v2.binpb` is the sole `buf breaking` baseline
  (`package.json:29`). The IDL has no `Any`, so serialized frame bytes do not
  contain the package name.
- The generated surface is consumed in 30 generated-import files verified at
  review (25 code/test sites with `engine/v2`/`pv2` references, the two dead
  fuzz files, `engine/CMakeLists.txt:790`, the root `CMakeLists.txt:9`, and
  `docs/development/practice-table.md`). Comment/doc rewrites on files NOT in
  that set add five new touched files (`engine/include/bs/policy.hpp`,
  `engine/include/bs/service.hpp`, `engine/src/poker/multiway.cpp`,
  `engine/include/bs/prng.hpp` constant pinned/comment rephrased, and
  `docs/design/gto-engine.md:1811`); `envelope.cpp:284` and
  `response_mapper.cpp:127` are additional rewrite LOCATIONS on files already
  inside the 30.
- **The IDL itself carries 33 generation-labelled comments plus the package
  declaration to rename.** The sole IDL,
  `proto/bigshark/engine/v2/engine.proto`, contains a header note
  `// V0/V1 are frozen; v2 is an explicit opt-in ...` (`:10-12`), 24
  field/message `// V2: ...` documentation comments, and clauses such as
  "Required for v2; absent in v1" (`:307,521,677`) plus an Envelope note that
  references the deleted `v1_envelope.cpp` (`:748-751`) — 33 comment lines in
  all alongside the `:3` `package bigshark.engine.v2;` declaration. These are
  comments on the protocol about to be renamed; leaving them in a package
  called `bigshark.engine` would preserve the rot inside the RFC's primary
  deliverable.
- **Five more first-party files name a protocol generation outside the
  generated-import set:** the practice tier —
  `engine/include/bs/practice/engine_process.hpp:22,28,68`,
  `engine/include/bs/practice/engine_served_policy.hpp:5-6`,
  `engine/src/practice/engine_process.cpp:1`,
  `engine/src/practice/engine_served_policy.cpp:263-264,409` — and the
  configure-time boundary guard
  `engine/cmake/stage6_boundary_guard.sh:104` ("The decision service and
  v0/v1 protocols must not link ..."; the string has no test consumer).
- **A dead mutation battery references deleted v1 sources.**
  `tools/mutation/decision-stage5.json` has a live top-level target
  (`test_guarantee`, three mutations on the present `guarantee.cpp`) and a
  live `decision_provenance` additional target (mutations on the present
  `decision.cpp`), plus an additional target `v1_minor2` whose build target
  `test_v1_minor2` no longer exists and whose 19 mutations edit three deleted
  files (`engine/src/protocol/v1_{envelope,semantic_validator,response_mapper}.cpp`).
  The npm `mutation` script does not invoke this config; the block is dead and
  is deleted, not whitelisted. Its surviving labels mention
  "v1-unreachable"/"minor-2" and are reworded to generation-free audit names.
- Remaining v0/minor prose in first-party code:
  `engine/include/bs/resolver.hpp:4`,
  `engine/include/bs/resident_policy.hpp:16-18`,
  `engine/src/policy/decision.cpp:23` and `:471-473`,
  `engine/include/bs/stage6/practice_table.hpp:120`,
  `engine/include/bs/guarantee.hpp:29`,
  `engine/tests/test_decision_provenance.cpp:4,6,46,99,170`,
  `engine/tests/resident_fixture_publisher.cpp:47-48`,
  `engine/tests/test_unification_invariants.cpp:160`,
  `engine/tests/test_postflop_frontier.cpp:573`, and the entire
  `engine/tests/fuzz_seeds.hpp` minor-0/1/2 comment block (which the fuzz
  rewrite replaces wholesale).
- Current-truth docs with generation paths/labels beyond the renamed import
  set: `docs/reference/protobuf-engine-protocol.md:7,30,87-95` and
  `docs/development/build-and-test.md:273`.
- The operational entry point the fuzzer must drive is
  `bs::handleEnvelope(const std::string&, const HostServices&)`
  (`engine/include/bs/protocol.hpp:185-194`) plus the ULEB128 framer in
  `engine/src/protocol/frame_stream.cpp`; both are what the production host
  calls.

## Design Principles

- **One name for one thing.** A sole artifact must not be named after the
  generation it replaced.
- **Rename, do not version.** No compatibility package, no forwarding header,
  no twin alias: the project's no-legacy rule forbids a shim whose only purpose
  is to preserve the old spelling.
- **Transport framing is owned by the wire, naming by us.** The envelope
  version field stays because peers use it to reject mismatched transports; the
  package name goes because only our own generated code reads it.
- **Security tests are ported, not deleted.** The fuzz target exercises the
  parser, validator, and mapper with hostile bytes; deleting it because the
  seeds rotted would remove the only frame-level robustness campaign. It is
  rebuilt against the sole current entry point.

## Proposed Design

### Naming and layout

| Surface | Now | After |
| --- | --- | --- |
| Proto package | `bigshark.engine.v2` | `bigshark.engine` |
| IDL path | `proto/bigshark/engine/v2/engine.proto` | `proto/bigshark/engine/engine.proto` |
| C++ generated | `bigshark/engine/v2/engine.pb.h` | `bigshark/engine/engine.pb.h` |
| C++ namespace | `::bigshark::engine::v2` | `::bigshark::engine` |
| C++ alias | `namespace pv2 = ...` | `namespace pe = ::bigshark::engine;` |
| TS generated | `bigshark/engine/v2/engine_pb.js` | `bigshark/engine/engine_pb.js` |
| Baseline | `proto/baseline/v2.binpb` | `proto/baseline/engine.binpb` |
| package.json | `--against proto/baseline/v2.binpb` | `--against proto/baseline/engine.binpb` |

`proto/CMakeLists.txt` generated-file lists (`:61-62,:76,:93`) point at the new
paths. `buf.gen.yaml` and `buf.gen.cmake.yaml.in` need no edit: outputs are
derived from the moved IDL's package.

### Baseline regeneration and the descriptor-equivalence evidence

`proto/baseline/engine.binpb` is produced with `buf build -o
proto/baseline/engine.binpb proto` (the same command that produced v2.binpb),
and the old file is deleted in the same commit. The evidence that the rebase
hides no schema change is a **full normalized FileDescriptorProto comparison**,
not a hand-picked field census:

1. `buf breaking proto --against proto/baseline/v2.binpb` green **before** the
   rename (the current gate's evidence).
2. Build both descriptors without source info
   (`buf build --exclude-source-info -o /tmp/before.binpb proto` before, and
   the same after the move) and compare their normalized
   `google.protobuf.FileDescriptorProto` text. Exactly three difference
   classes are permitted: the `package` declaration, the file `name`, and
   fully-qualified cross-references that embed the package (message/field
   type names and any `java_package`-style option — there are none today).
   Everything else must be byte-identical in the normalized text, explicitly
   including: every message name and field name, number, type, **label
   (singular/repeated)**, oneof membership, `json_name`, `json_name` defaults,
   reserved ranges/names; every enum name and **every enum value name and
   numeric ordinal** (name-only equality is insufficient — renumbering an
   enum while keeping names is a silent wire break); default values; and every
   option including the three `buf.validate.oneof` field options. The
   reviewer's independent experiment on 2026-10-24 established the expected
   result: after source-info exclusion the two descriptors are identical
   except for the package-bearing lines; the implementation commits the exact
   commands and the resulting diff as stage evidence.
3. Same-language conformance runs unchanged: the C++ `test_conformance` suite
   and the Node protocol/client suites, which on the Node side exchange framed
   envelopes with a TypeScript fake engine (`clients/node/tests/fixtures/fake-proto-engine.ts`)
   generated from the same schema, exercise both language bindings.
4. Cross-language evidence is the two real-binary tests in
   `platforms/river-club/tests/w1-resident-wiring.test.ts` (the
   `t.skip()`-gated cases around `:486-540`) that spawn the published
   `bin/bigshark-engine`. They must be run **non-skipped, after the engine
   binary is rebuilt and republished in the same change**; a pass against the
   previously published binary proves nothing. The final argument for
   on-wire invariance remains step 2, because with no `Any` the package name
   never appears in serialized frame bytes.

After the rebase, `buf breaking` against the new baseline retains full
protection for every future change; the one-time package-name break against
the old baseline is the intended no-legacy migration.

### Fuzz harness port

`engine/tests/fuzz_seeds.hpp` and the harness are rewritten, not adapted:

- One include: `<bigshark/engine/engine.pb.h>`, namespace `bs::fuzz` with
  `namespace pe = ::bigshark::engine;`.
- Seeds construct **current-schema** messages: a capabilities request; a valid
  decision request modeled on the v3 envelope the engine tier produces
  (`engine/src/practice/engine_served_policy.cpp` is the reference for the
  G/F ledger, pot layers, transition path, forced contributions, legal
  actions, and provenance) and reusing the existing legal v3 envelope builder
  in `engine/tests/test_conformance.cpp:176-191` (the fuzz target already has
  `engine/src` on its include path; the helper is exposed to the target rather
  than copied, so a second rot-prone fixture does not exist); open-enum
  attacks on every validated enum (action type, street, rank, suit, guarantee
  token, solver mode); a capabilities response and a decision response frame
  (the host must reject responses); oversized integers; and a wrong
  `protocol_version` frame.
- **Two translation units, no conditional `main`:** the harness TU defines
  `LLVMFuzzerTestOneInput` (the libFuzzer entry, linked normally under
  `-fsanitize=fuzzer`); a separate standalone-driver TU defines `main` and is
  added to the target only when `BIGSHARK_FUZZ_LIBFUZZER` is OFF. CMake
  selects the driver source with a target-sources conditional instead of a
  macro, so both configurations link cleanly and libFuzzer never sees a second
  strong `main`. The mutated bytes are fed two ways — directly to
  `bs::handleEnvelope` and as ULEB128-framed payloads through the same
  `FrameStream` the production host reads.
- **The failure oracle is corpus-scoped, because a legal random request can
  legitimately receive a fold.** The fixed negative corpus (empty input,
  truncated/oversized length prefixes, garbage bytes, open-enum attacks, a
  response frame, a wrong `protocol_version`) must produce only a clean
  error-envelope oneof or a closed-stream indication — never a decision
  strategy response. The deterministic random mutation campaign asserts only
  no crash, no escaping throw, and no hang (seed requests carry a minimal
  solve budget); it does not assert the response kind, because a well-formed
  AUTOMATIC request without resident services legally reaches the labeled
  operational fallback. A pinned test proves the valid seed reaches the
  decision arm rather than an error arm.

### Comment and documentation cleanup

Each v0/minor reference listed in Current State is edited to describe the code
that exists (e.g. the resolver comment states it never touches the wire; the
decision comment states the no-hole branch is unreachable after validation and
is a defensive fold; the guarantee comment names "the negotiated wire
contract"). Current-truth docs get their paths and package names updated;
`docs/development/build-and-test.md` keeps the fuzz section with the repaired
target. Historical RFC text is never edited for this.

### The gate: a full token inventory, not a phrase list

Four review rounds proved hand-enumerated phrase patterns cannot be complete:
each round found protocol-generation prose the patterns missed, including
comments inside the IDL itself. The acceptance gate is therefore a **full
token inventory over the whole tree with closed classifications**, encoded as
one documented `rg` invocation committed to the implementation plan (a shell
snippet plus its full before/after output).

Tokens scanned, case-insensitive, whole-word:

- `\bv[0-3]\b`
- `minor[- ]?[0-2]`
- the namespace/path forms `engine/v[0-9]`, `engine::v[0-9]`,
  `bigshark.engine.v[0-9]`, and the aliases `\bpv[12]\b`

Scan scope: `engine/` (including `engine/cmake/` and `engine/tests/`),
`apps/`, `clients/`, `platforms/`, `bin/`, `tools/`, `proto/`, the
repository-root build files, and every Current-truth document (`docs/` files
with `Status: Current` plus the reference/build docs named in this RFC).
Historical RFCs, plans, and upstream documents are scanned separately but are
exempt as history (category 6).

Every hit must be classified into exactly one bucket; the change lands only
when the **residual (unclassified) inventory is empty**:

1. **Frozen behavior literals** (value pinned, comments rephrased):
   `suit-canonical-v1`, `rfc0008-coarse-to-exact:v1`, `menu-v1`,
   `kProtocolSamplerDomain`/`BSV1-sam` (`engine/include/bs/prng.hpp:23`,
   `engine/src/protocol/response_mapper.cpp:127`; XORed into sampler seeds),
   AND the action-abstraction digest literals `size-v1` (the canonical
   schedule header, `abstraction.cpp:57`, fail-closed at `:244`) and
   `rfc0007-pot-fractions` (the identity family name,
   `abstraction.cpp:80`; both participate in pinned artifact digests, so
   renaming them is a compatibility break rather than a label cleanup).
2. **Transport axis**: the `protocol_version` field and its rejection vectors
   (`set_protocol_version(0/1/3)`); misleading comments around those vectors
   are rewritten, the vectors stay.
3. **External tooling/provider versions**: Buf's `version: v2` in
   `proto/buf.yaml`, `proto/buf.lock`, `proto/buf.gen.yaml`; River Club
   references to the provider protocol (`v2`, `v2.1`) in upstream docs and
   River-facing files — whitelisted by path/context, since they name a
   protocol this project does not own.
4. **Artifact-schema axis** (real persistence semantics; matched by
   (whitelisted path) OR (enumerated literal anywhere)): paths
   `engine/src/artifacts/`, `engine/include/bs/strategy_artifact.hpp`,
   `engine/include/bs/unified_game.hpp`, plus the schema-axis comments in
   `engine/include/bs/seat_policy.hpp` and
   `engine/src/resident/resident_policy.cpp`; and the enumerated literals
   `schema-v2`/`schema-v3`, `BSFLIB02`, `BSWARM01`/`BSWARM02`,
   `rfc0016-…rev2`, `kRulesIdentifierV1/V2/Preflop`, `kSchemaV*`,
   `roundtrip-vN-checkpoint.db`, and the persisted rules literals
   `rfc0004-heads-up-flop-v1`, `rfc0009-unified-flop-v1`,
   `rfc0009-unified-preflop-v1`, and
   `rfc0010-multiway-preflop-v1` (`preflop_profile_builder.cpp:338`).
   Bare schema-axis prose outside both the paths and the literals (e.g. a
   test comment saying "a v2 game" without the canonical token) is residual
   and is rewritten TO the canonical literal ("a schema-v2 game"), not left
   free-form. The rule lets the many schema-axis hits in tests, benchmarks
   (`test_artifacts.cpp`, `test_resident_policy.cpp`, six benchmark files,
   `resident_index.hpp`, `sealed_checkpoint.hpp`, `mmap_library.hpp`) and
   Current docs classify by literal without path-whitelisting those files
   wholesale.
5. **Mermaid** syntax (`stateDiagram-v2`).
6. **History**: `docs/rfcs/`, `docs/plans/`, upstream docs, and this RFC.

Everything else is residual and is REWRITTEN in the change. The round-4
inventory (complete at review time; the implementation's before-scan output
is authoritative) classifies as residual: the 33 generation-labelled IDL
comments (the package declaration at `:3` is renamed, not rewritten), the five
practice/guard files, the `v0/minor` code comments listed in Current State,
the Current-doc path labels, the `engine/src/protocol/envelope.cpp:284`
handler string, the misleading conformance comment, and the fuzz-seed minor
block (deleted by the fuzz rewrite). Because the scan token is `v[0-9]`
rather than a guessed phrase, the `v2` sites (`envelope.cpp:284`,
`gto-engine.md:1811`) are caught by the same rule as the `v1` sites; the
artifact axis does not appear in residual because its path is bucket 4.

The dead `v1_minor2` block in `tools/mutation/decision-stage5.json` is
deleted, and the surviving mutation labels that say "v1-unreachable"/"minor-2"
are reworded to generation-free names; the live `test_guarantee` and
`decision_provenance` batteries and their mutations on present files are
untouched. The inventory output (before and after) is committed with the
implementation plan, so completeness is auditable mechanically rather than
asserted.

## Dependency Rules

- `proto/` remains the only IDL owner; nothing outside the generated tree
  hand-rolls message encoding.
- The fuzz target links only `bigshark_engine_protocol` + `bigshark_protocol`
  (as today); it never links the host binary or the practice tier.
- No new dependency and no new extension point are introduced.

## Compatibility and Migration

- Engine wire: byte-compatible (no Any/type URL, no field change). Both peers
  move in one commit; there is no deployed mixed-version fleet.
- Generated artifacts: stale `build/**/bigshark/engine/v1` and `.../v2`
  directories are removed by a clean build; the implementation may delete the
  stale trees in existing build directories to make the single form visible.
- Descriptor baseline: replaced, not migrated; the three-step evidence above
  is committed with the change.
- Artifacts and checkpoints: untouched; identity digests do not include the
  proto package, and the retained abstraction names are unchanged.
- External systems: none; the River Club provider never speaks this protocol.

## Security and Operational Impact

Positive: the protocol fuzz campaign — currently advertised but unbuildable —
is restored, so frame/envelope/validator robustness is exercised again. No
credential, trust-boundary, fairness, or live-operation change. The host's
rejection vocabulary and error envelopes are unchanged.

## Alternatives Considered

1. **Keep the v2 name and only fix the fuzzer.** Rejected: the user directive
   is explicit that the project has no internal V1/V2 concept, and a sole
   package named after a deleted sibling is exactly the rot the no-legacy rule
   exists to remove.
2. **Delete the fuzz target instead of porting it.** Rejected: it is the only
   hostile-bytes campaign over the frame decoder and validator. It rotted
   because it was optional, not because it was worthless. Porting to the sole
   current API is a small, well-bounded change.
3. **Rename the envelope's `protocol_version` field too.** Rejected: the field
   is the transport handshake shared with the framing layer and the external
   process boundary; CLAUDE.md explicitly classifies it as a transport detail
   outside project versioning. Renaming a wire field is exactly the kind of
   external-contract churn this RFC avoids.
4. **Rename the digest-pinned abstraction identifiers as well.** Rejected:
   they are algorithm names embedded in shipped-artifact digests; renaming
   invalidates artifacts and forces retraining for naming aesthetics, with no
   twin generation to remove.
5. **Introduce a `engine.pb.h` forwarding header under the old path.**
   Rejected: a compatibility shim is precisely what the no-legacy rule forbids.

## Risks

- **Rebasing the baseline could hide a real breaking change.** Mitigated by the
  ordered before/after evidence (green `buf breaking` first, descriptor diff
  limited to package fields, end-to-end conformance bytes unchanged).
- **Mechanical rename misses a site and breaks a generated include.**
  Mitigated by the full build contract: CMake generation, C++ build, TS
  generation/typecheck, and the Node conformance suites all fail loudly on a
  missed path; the change is one commit so no intermediate state exists.
- **Fuzzer port validates the wrong request shape.** Mitigated by constructing
  seeds from the production practice-tier request builder rather than inventing
  a second fixture; a valid seed must reach `handleEnvelope`'s decision arm,
  pinned by a test that asserts the seed is accepted rather than rejected.
- **Fuzz build cannot be exercised on GCC hosts.** The option requires Clang
  and CI's Linux lane is GCC. If no Clang is available locally, the harness is
  verified by a compile-only syntax check of the TU with the current generated
  header under the default toolchain, and the clang-only gate is explicitly
  recorded as not run there rather than faked.

## Verification Plan

- `npm run proto:check` (lint, format, breaking against the rebased baseline,
  generation, TS typecheck) and the same-language conformance suites: C++
  `test_conformance` and the Node suites against the generated-schema fake
  engine.
- Cross-language evidence: rebuild and republish `bin/bigshark-engine`, then
  run the two real-binary tests in
  `platforms/river-club/tests/w1-resident-wiring.test.ts` (`:486-540`)
  non-skipped.
- Full Engine Build Contract: release configure, format, build, `ctest`,
  `benchmark-multistreet`, `npm run check`, `node bin/replay.mjs`,
  `check-docs`, `check-rfcs`.
- ASan/UBSan on the touched protocol surface.
- Descriptor evidence: the normalized `--exclude-source-info`
  FileDescriptorProto diff with only the three permitted difference classes.
- Fuzz target (mandatory on this host — `/usr/bin/clang++` is Clang 14, so the
  "no compiler" escape does not apply here):
  `cmake -Bbuild/fuzz -DBIGSHARK_ENABLE_FUZZ=ON` builds `fuzz_proto` in
  standalone mode, and the same sources plus `-DBIGSHARK_FUZZ_LIBFUZZER=ON`
  link under libFuzzer with no duplicate `main`; `BS_FUZZ_MAX_RUNS=100000`
  completes cleanly; the fixed negative corpus is asserted to produce only
  error envelopes or stream closures. If Debian's lack of
  `libclang-rt.fuzzer` makes the libFuzzer configuration unbuildable, that is
  recorded honestly with the linker error — it is not faked by skipping the
  standalone campaign.
- Grep gate: the committed token-inventory `rg` invocation reports an empty
  residual after classification into the six closed buckets.

## Rollout Plan

1. Record the green pre-rename `buf breaking` result and produce the
   normalized descriptor pair and diff.
2. One commit: move the IDL and strip every generation label from it
   (header note, the 24 `V2:` field comments, the v1-contrast clauses, the
   deleted-file Envelope reference); update package/namespace/paths across all
   renamed sites; update CMake and package.json; rebase the baseline; rewrite
   the five practice/guard files, all residual code comments listed in
   Current State (`resolver.hpp`, `resident_policy.hpp`, `decision.cpp`,
   `practice_table.hpp`, `guarantee.hpp`, `test_decision_provenance.cpp`,
   `resident_fixture_publisher.cpp`, `test_unification_invariants.cpp`,
   `test_postflop_frontier.cpp`, `policy.hpp`, `service.hpp`, `multiway.cpp`,
   `prng.hpp`/`response_mapper.cpp` sampler comment, `envelope.cpp:284`, root
   `CMakeLists.txt:9`), the misleading conformance comment, and the
   Current-truth doc clauses (`gto-engine.md:1811`, the protocol reference,
   build-and-test baseline path). The `BSV1-sam` constant value is untouched.
3. Rewrite and land the fuzz harness (two-TU split, reused conformance
   fixture, scoped oracle), replacing the minor-0/1/2 seed block; delete the
   dead `v1_minor2` block from `tools/mutation/decision-stage5.json` and
   reword its surviving generation-labelled mutation names.
4. Delete the stale `engine/v1` and old `engine/v2` generated trees from all
   preset build directories; rebuild and republish `bin/bigshark-engine`.
5. Run the full contract plus ASan, both fuzz link configurations, the
   non-skipped real-binary tests, and the token-inventory gate; attach the
   descriptor diff AND the complete before/after token inventories (every hit
   classified into a bucket or rewritten, residual empty) to the change.

Stop condition: any same-language conformance exchange changes, a real-binary
test fails non-skipped, the descriptor diff shows anything beyond the three
permitted difference classes, or the after-scan has an unclassified token —
halt; the change is not a pure rename.

## Rollback Plan

Single-commit revert restores paths, package, baseline, and the previously
documented (broken) fuzz target. No persisted data and no deployed fleet are
affected, so rollback is source-only.

## Open Questions

- None. The exemption categories, the transport-field exemption, and the
  external-protocol exemption are decided above; if the independent reviewer
  rejects an exemption it becomes a blocking finding, not an open item.

## Acceptance Criteria

- The only IDL is `proto/bigshark/engine/engine.proto`, package
  `bigshark.engine`; generated C++/TS paths and all consumer sites use the
  unversioned names, and the IDL carries no generation-labelled comment.
- The committed token-inventory after-scan has an EMPTY residual set: every
  `\bv[0-3]\b`, `minor[- ]?[0-2]`, namespace/path, and alias hit in scan scope
  is classified into one of the six closed buckets or rewritten, and the
  before/after inventory outputs are attached.
- `proto/baseline/engine.binpb` exists, `v2.binpb` is deleted,
  `proto:breaking` passes, and the committed normalized descriptor diff shows
  only package, file name, and embedded-FQN differences — field numbers,
  types, labels, oneofs, json names, reserved ranges, enum ordinals, defaults,
  and all options (including `buf.validate`) byte-identical.
- `fuzz_proto` links in BOTH standalone and libFuzzer configurations (the
  driver is a separate TU, no duplicate `main`); defines exactly one
  `LLVMFuzzerTestOneInput`; the valid current-schema seed is pinned to reach
  the decision arm; the 100,000-input standalone campaign completes without
  crash/hang/throw on this host's Clang 14; the fixed negative corpus never
  yields a strategy response.
- The two real-binary w1 tests pass non-skipped against the rebuilt,
  republished engine binary.
- No stale generated `engine/v1` or pre-rename `engine/v2` tree remains in any
  preset build directory.
- The Engine Build Contract, same-language conformance suites, proto checks,
  and ASan all pass, and Current-truth documentation states the single form.

## Implementation Record

Status: Implemented (2026-10-25). Every acceptance criterion is met and the
final Engine Build Contract plus the full ASan/UBSan suite are green on the
final source.

- The sole IDL is `proto/bigshark/engine/engine.proto`, package
  `bigshark.engine`; generated C++/TS paths and every consumer use the
  unversioned names, and the IDL carries no generation-labelled comment.
- The committed token inventory
  (`docs/plans/0034-token-inventory-{before,after}.txt`) shows 2,660 hits
  before and 1,617 after with an EMPTY residual: every remaining hit falls in
  the six closed buckets (frozen literals, transport version, vendored and
  external tooling, artifact schema, diagrams, history).
- `proto/baseline/engine.binpb` is the only baseline (`v2.binpb` deleted);
  `proto:breaking` passes and the committed normalized descriptor diff
  (`docs/plans/0034-descriptor-normalized.diff`) shows only package, file-name,
  and embedded-FQN differences.
- `fuzz_proto` links in both configurations via the two-TU split
  (`fuzz_harness.cpp` plus `fuzz_standalone_driver.cpp`; `fuzz_main.cpp`
  deleted): one `LLVMFuzzerTestOneInput`, the legal seed pinned to the
  decision arm, a 100,000-input Clang-14 campaign clean, and the fixed
  negative corpus never yields a strategy response.
- Both real-binary w1 tests pass non-skipped against the republished engine.
- No stale `engine/v1` or `engine/v2` generated tree exists in any preset
  build directory.
- Verification (final source, 2026-10-25): release ctest 79/79 including
  `benchmark_multistreet` with all three quality gates PASS;
  `npm run check` 124/124 including `proto:check`; `node bin/replay.mjs` exit
  0; documentation and RFC checks pass. The full ASan/UBSan ctest is 77/77:
  the complete suite passed once with one wall-timeout on
  `stage6_measured_policy` (900 s watchdog; release runs ~150-183 s while the
  measured sanitizer overhead on the stage-6 walks is 30-40x), the watchdog
  was raised to 5,400 s under sanitizers only, and the isolated rerun passed
  in 2,905 s along with the resolver/subgame/conformance family rebuilt from
  the final source.

## Review history

### Round 1 — Changes Requested (2026-10-24)

Reviewer — `rfc0034-approval-1` (independent approval agent), 2026-10-24.
Outcome — **Changes Requested** — 3 major, 3 minor, 1 nit; all spec/evidence
wording, no architectural redesign. The reviewer independently reproduced the
package rename in `/tmp` with `buf` 1.73.0 and confirmed the descriptor diff
is limited to the package-bearing lines.

Resolved in this revision —

1. The bare-token grep gate was unsatisfiable against the legitimate
   artifact-schema version axis; replaced with the forbidden-pattern plus
   six-category exemption list, and the false "a fourth v-token fails review"
   sentence removed.
2. The descriptor-diff evidence is now a full normalized
   `--exclude-source-info` FileDescriptorProto comparison naming labels,
   oneofs, json names, reserved ranges, enum **ordinals**, defaults, and
   options as required-equal fields.
3. Conformance evidence no longer claims TS↔C++ byte exchange for the fake-
   engine suites; the two gated real-binary w1 tests are named as the cross-
   language evidence and must run non-skipped after the engine is republished.
4. The libFuzzer double-`main` hazard is resolved by a driver/harness two-TU
   split selected in CMake; both link configurations are acceptance criteria.
5. The fuzz oracle is scoped to the fixed negative corpus (legal random
   requests may legitimately reach the operational fallback), and the valid
   seed reuses the existing `test_conformance.cpp` v3 envelope builder rather
   than a copied fixture.
6. Exemptions now list `buf.yaml`/`buf.lock`; additional rewrite sites are
   enumerated (`envelope.cpp:284`, `engine/CMakeLists.txt:790`,
   `practice-table.md`); the stale-tree mechanism text is corrected and
   stale generated directories are explicitly removed; Clang 14 being
   present on this host makes the 100k fuzz campaign mandatory evidence.

### Round 2 — Changes Requested (2026-10-24)

Reviewer — `rfc0034-approval-1` (same agent), 2026-10-24. Outcome:
**Changes Requested** — 1 major, 1 minor; round 1's six findings were
independently re-verified as resolved, including a fresh
`--exclude-source-info` descriptor experiment showing zero normalized
differences outside the 274 package-bearing lines.

Resolved in this revision —

- **Finding A (major):** the forbidden-prose pattern included "v2 profile",
  whose only hits are the protected strategy-artifact rules axis. The pattern
  is now path-scoped to protocol-owned code/Current docs, explicitly excludes
  "v2 profile" naming, and exemption category 4 names the full
  rules-identifier axis (`kRulesIdentifierV1/V2/Preflop`, the persisted
  `rfc0004-…`/`rfc0009-…` literals, `validate_game_v*`, and the reader/writer
  prose under `engine/src/artifacts/`, `seat_policy.hpp`,
  `resident_policy.cpp`).
- **Finding B (minor):** the gate scope now includes the repository-root build
  files and names `CMakeLists.txt:9` ("RFC 0002 v1 protocol target") and
  `docs/design/gto-engine.md:1811` (stale "v2 protocol" clause); the prose
  patterns add `v2 protocol` with River-facing path qualification so the
  external provider references stay exempt.

### Round 3 — Changes Requested (2026-10-24)

Reviewer — `rfc0034-approval-1` (same agent), 2026-10-24. Outcome:
**Changes Requested** — 1 major (three residual protocol-axis comments the
closed list did not catch), 1 minor (the frozen `BSV1-sam` constant
unclassified), 2 nits. Round 1/2 dispositions re-verified as sound.

Resolved in this revision —

- **R3-1:** the exact-phrase prose list is replaced by a generalized
  path-scoped shape (`the v[01] (protocol )?(boundary|validator|handler|
  service|mapper|contract|wire|profile)` and `v[01] protocol`) over
  protocol-owned code and Current-truth docs, which catches
  `engine/include/bs/policy.hpp:13`, `engine/include/bs/service.hpp:10`, and
  `engine/src/poker/multiway.cpp:58` by construction. Path/namespace patterns
  are clarified to apply tree-wide; the renamed-surface count stays 30 with
  comment/doc rewrites listed separately.
- **R3-2:** `kProtocolSamplerDomain` / "BSV1-sam" (`prng.hpp:23`) is added to
  exemption category 1 as a byte-pinned behavior identifier (same class as
  `menu-v1`); the constant is frozen and only its "v1 protocol sampler"
  comment is rephrased.
- Nits: exemption 4 paths add `engine/include/bs/unified_game.hpp`; scope
  wording distinguishes tree-wide namespace patterns from path-scoped prose.

### Round 4 — Changes Requested (2026-10-24)

Reviewer — `rfc0034-approval-1`, 2026-10-24. Outcome: **Changes Requested** —
phrase-enumeration completeness failed a fourth time. Findings:

- **R4-1 (major):** 34 generation-labelled comments live inside the IDL
  itself (`V0/V1 frozen` header, 24 `V2:` field comments, v1-contrast
  clauses, a deleted-file Envelope reference), plus five practice/guard files
  (`engine/include/bs/practice/engine_process.hpp`,
  `engine_served_policy.hpp`, their `.cpp` files,
  `engine/cmake/stage6_boundary_guard.sh:104`) outside every gate and list.
- **R4-2 (major spec error):** the round-3 generalized shape used `v[01]`,
  which cannot match the very `v2` sites it claimed to catch
  (`envelope.cpp:284`, `gto-engine.md:1811`).
- **R4-3 (minor):** the rewrite dropped the `v0/JSON`/`minor-0/1` shapes,
  excluded `docs/` from the path/namespace scope, and left the live mutation
  fixture `tools/mutation/decision-stage5.json` unclassified; its `v1_minor2`
  target block was verified dead (19 mutations against the three deleted
  `v1_*.cpp` files, target `test_v1_minor2` absent).
- R4-4 (nit): the touched-file accounting double-counted two files already
  in the 30; corrected to five new files plus two additional locations.
- R4-5 (structural): replace phrase enumeration with a full token inventory.

Resolved in this revision — the gate is now a full `\bv[0-3]\b` /
`minor[- ]?[0-2]` / path-namespace-alias token inventory over all first-party
code, configs, and Current docs, with six closed classification buckets and a
committed before/after inventory whose residual must be empty; the IDL
comments, practice/guard files, code comments, doc labels, and the dead
mutation block are all enumerated in Current State and rollout steps 2-3;
the dead `v1_minor2` block is deleted (not whitelisted) and surviving labels
reworded.

### Round 5 — Changes Requested (2026-10-24)

Reviewer — `rfc0034-approval-1`, 2026-10-24. Outcome: **Changes Requested** —
the inventory gate itself was accepted as the terminal structure and verified
against 1,339 tree-wide hits; remaining findings were one major (three
digest/persistence-frozen literals missing from the closed buckets: `size-v1`,
`rfc0007-pot-fractions`, `rfc0010-multiway-preflop-v1` — rewriting them would
break published artifacts), one minor (bucket 4 needed an explicit
"whitelisted path OR enumerated literal anywhere" matching rule), and two
nits (IDL count is 33 comments plus the package declaration; residual
locations such as `apps/river-club-agent/main.ts:89`,
`stage6_link_negative.cmake`, `tools/replay` fixture labels, fake-engine
`fake-v2.0` strings, benchmark manifest tags, and local test variable names
are left to the implementation inventory rather than the RFC list). All are
applied in this revision: the three literals are in buckets 1 and 4, bucket 4
states its path-or-literal rule and canonicalizes bare schema prose, the IDL
count is corrected, and the implementation's before/after inventory is
required to classify each remaining hit line.

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0034-approval-1 (independent approval agent)
Decision date: 2026-10-24
Review outcome: Approved
Reviewed scope: The full RFC across six review rounds; the sole IDL and proto
  CMake/Buf configuration; envelope/frame_stream/protocol.hpp; fuzz_main and
  fuzz_seeds; test_conformance; the practice engine tier; engine-host; the
  node client and River adapter including the real-binary w1 tests;
  tools/mutation and tools/replay; the engine cmake boundary guards; all
  related Current-truth docs. The reviewer independently reproduced the
  package rename in /tmp twice (buf 1.73.0, normalized
  --exclude-source-info FileDescriptorProto diff of zero allowed differences
  beyond package/file-name/embedded-FQN lines), classified all 1,339
  tree-wide token inventory hits bucket by bucket, and parsed the mutation
  JSON to verify the v1_minor2 block targets three deleted files and a
  nonexistent build target.
Review summary: Six rounds of Changes Requested. Round 1 required a
  pattern-based grep gate, full normalized descriptor-equivalence evidence,
  honest same-language vs cross-language conformance wording, a two-TU fuzz
  split to avoid a duplicate main, a corpus-scoped failure oracle, and
  complete exemption/site lists. Rounds 2-4 proved hand-enumerated phrase
  patterns incomplete (the gate missed the artifact-rules axis, the root
  CMakeLists and gto-engine clauses, and finally 33 generation comments in
  the IDL itself, while the `v[01]` shape could not match its claimed v2
  sites). Round 5 replaced phrase enumeration with a full `v[0-3]` /
  `minor[- ]?[0-2]` token inventory over all first-party code, configs, and
  Current docs, six closed classification buckets, an empty residual set,
  deletion of the dead v1_minor2 mutation battery, and committed
  before/after inventory evidence. Round 6 added the last three
  digest-pinned frozen literals (size-v1, rfc0007-pot-fractions,
  rfc0010-multiway-preflop-v1), specified bucket 4's path-or-literal
  matching, and corrected the IDL count (33 comments plus the package
  declaration). Approval is design only; it authorizes no spending, live
  play, or training.
```
