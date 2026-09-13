# RFC 0001 and RFC 0002 Implementation Plan

Status: Current

Execution state: Stages 0-4 complete; Stage 5 ready

Approved RFCs:

- [RFC 0001: BigShark Engineering Architecture](../rfcs/0001-engineering-architecture.md)
- [RFC 0002: Protobuf Engine Protocol](../rfcs/0002-protobuf-engine-protocol.md)
- [RFC 0003: Strict TypeScript Application and Integration Layer](../rfcs/0003-typescript-application-layer.md)

## Objective

Migrate BigShark from a River Club-shaped repository into a platform-neutral
poker engine architecture, then replace the internal JSON v0 process contract
with the accepted Protobuf protocol.

The migration must preserve River Club behavior, current engine decisions,
public command paths, session replay, and rollback capability at every stage.

## Governing Rules

1. Change RFC status to `Implementing` only when the first implementation
   stage begins.
2. Keep architecture movement and protocol migration in separate logical
   changes.
3. Freeze behavior before moving implementation.
4. Preserve `bin/` compatibility entry points until canonical replacements
   pass all gates.
5. Do not add a shared platform base class before a second adapter exists.
6. Do not introduce Protobuf-generated types into poker, solver, or policy
   domain APIs.
7. Stop a stage when unexplained decision parity, legality, or replay drift
   occurs.
8. Use strict TypeScript for application, adapter, client, and tool
   implementation; keep JavaScript only in thin compatibility launchers.

## Completed Evidence

Stages 0-4 are complete:

- Six sanitized River snapshots freeze preflop, flop, turn, river, multiway,
  and short-stack decisions. Fixture-derived coverage disables the v0 river
  solver for an unsupported action line.
- Historical replay covers 156 unique decisions with zero illegal actions and
  zero operational fallbacks.
- `clients/node` owns the generic process client and has no River imports.
- River API, normalization, legality, journaling, wait, atomic action, and
  runner implementation reside under `platforms/river-club` and
  `apps/river-club-agent`.
- Pure runner-state tests cover wait, act, stale recovery, leaving, table
  dissolution, explicit resume, stop-loss, CLI error recovery, and bounded
  decision timing.
- `bin/` contains thin launchers into compiled TypeScript; a source-policy
  gate rejects JavaScript implementation in TypeScript-owned directories,
  and clean builds prevent stale `dist/` artifacts.
- CMake enforces `bigshark_poker`, `bigshark_solver`, `bigshark_policy`,
  `bigshark_service`, and `bigshark_v0_protocol`; the host composition root is
  under `apps/engine-host`.
- yyjson is private to the v0 protocol target. OpenSpiel and HiGHS are private
  to the solver target.
- The Protobuf v1 IDL, Buf dependency lock, exact toolchain versions,
  preset-local C++ bindings, TypeScript bindings, compatibility baseline, and
  bidirectional full-field conformance vectors are implemented.
- The Linux GCC 12 no-HiGHS lane validates the portable DCFR fallback and ELF
  dependency set in a digest-pinned Debian container.
- Debug, Release, ASan/UBSan, Node, documentation, RFC, golden fixture, and
  replay checks pass.

Stage 4 is complete. The v1 IDL, generated bindings, compatibility baseline,
and cross-language vectors are not connected to the production host. Stage 5
is the next implementation stage.

## Stage 0: Freeze the Behavioral Baseline

Owners:

- `platforms/river-club` future owner
- current `bin/` integration
- replay tooling

Changes:

- Select representative River snapshots for preflop, flop, turn, river,
  multiway, short-stack, stale-state, and unsupported-river-line cases.
- Store sanitized provider snapshots under a River-owned fixture directory.
- Record current v0 normalized contexts.
- Record current engine responses and deterministic seeds.
- Add fixture tests for River snapshot to v0 context.
- Add replay assertions for action, amount, source, and legality.

Required evidence:

- Frozen fixtures contain no credentials or hidden opponent cards.
- Repeated fixture runs are deterministic.
- Historical replay remains at zero illegal actions and zero unexpected
  fallbacks.

Rollback:

- Tests and fixtures are additive and remain useful if later stages roll back.

Completion gate:

- Every migration-relevant River state has at least one golden fixture.

## Stage 1: Extract the Node Engine Process Client

Owners:

- `clients/node`
- compatibility export in `bin/cpp-engine.mjs`

Changes:

- Create a pure v0 engine process client.
- Move process startup, warmup, NDJSON framing, FIFO correlation, timeout, and
  process-exit handling out of `bin/cpp-engine.mjs`.
- Keep River normalization in its existing location temporarily.
- Preserve the current `decide()` and `buildContext()` export behavior through
  a compatibility module.

Required evidence:

- Unit tests cover framing, partial reads, multiple responses, timeout,
  process exit, malformed output, and late responses.
- Golden River contexts and decisions remain byte-for-byte or
  semantically equivalent.
- Replay remains unchanged.

Rollback:

- Restore the original process-client implementation behind the same exports.

Completion gate:

- The engine client has no River Club imports or room-field knowledge.

## Stage 2: Extract the River Club Adapter

Owners:

- `platforms/river-club`
- `apps/river-club-agent`
- compatibility launchers in `bin/`

Changes:

- Move River API, normalization, action-history reconstruction, legality
  checks, atomic action execution, journaling, and session lifecycle into the
  River-owned module.
- Keep existing `bin/bigshark.mjs`, `bin/play.mjs`, `bin/shoot.mjs`, and wait
  commands as thin compatibility launchers.
- Keep the v0 engine context during this stage.
- Preserve `.runtime` and `sessions/*.jsonl` behavior.

Required evidence:

- Golden snapshot-to-context tests pass.
- State-machine tests cover wait, act, stale, leave, table dissolution,
  explicit resume, and stop-loss.
- Existing CLI commands retain arguments, exit codes, and output shape.
- Replay remains unchanged.

Rollback:

- Compatibility launchers switch back to the former implementation paths.

Completion gate:

- No reusable River implementation remains in `bin/`.
- The adapter contains no poker strategy.

## Stage 3: Split the C++ Engine Boundaries

Owners:

- `engine/poker`
- `engine/solver`
- `engine/policy`
- `engine/service`
- `apps/engine-host`

Changes:

- Introduce `bigshark_poker`, `bigshark_solver`, `bigshark_policy`, and
  `bigshark_service` CMake targets.
- Move card, evaluator, range, and equity code into the poker target.
- Move river and multi-street solver code into the solver target.
- Move preflop, postflop, strategy routing, and deterministic action sampling
  into the policy target.
- Move request semantic orchestration and domain errors into the service
  target.
- Keep yyjson parsing in the v0 host boundary.
- Move the executable composition root under `apps/engine-host`.

Required evidence:

- Each library compiles with only declared public dependencies.
- Poker and policy headers contain no yyjson, Protobuf, OpenSpiel, HiGHS, Node,
  or platform types.
- Existing C++ tests pass without strategy changes.
- Debug, release, ASan, and replay gates pass.

Rollback:

- Restore the former CMake target wiring while retaining moved tests where
  practical.

Completion gate:

- Dependency rules from RFC 0001 are enforced by CMake target visibility.

## Stage 4: Land the Protobuf Toolchain and IDL

Owners:

- `proto/bigshark/engine/v1`
- protocol generation configuration

Changes:

- Pin Buf and Protobuf toolchain versions.
- Add `buf.yaml`, `buf.gen.yaml`, and `engine.proto`.
- Implement every accepted message and semantic decision from RFC 0002.
- Generate C++ and Node bindings under build or package output.
- Add lint, format, generation, and breaking-change gates.
- Add binary and Protobuf JSON golden vectors.

Required evidence:

- `buf lint` passes.
- `buf format --diff --exit-code` passes.
- Generated C++ and Node code compiles.
- Cross-language round trips preserve every field.
- Unknown fields and additive enum values follow the accepted compatibility
  policy.

Rollback:

- Remove the unconnected generated targets and IDL without affecting v0.

Completion gate:

- `.proto` is the only authoritative v1 protocol definition.

## Stage 5: Implement Protobuf Domain Mapping and Host Mode

Owners:

- engine protocol mapper
- engine service
- engine host

Changes:

- Map generated Protobuf messages to pure service-domain requests.
- Implement semantic validation.
- Map domain strategies and errors back to Protobuf.
- Add length-delimited framing with a 1 MiB limit.
- Add capability requests.
- Run Protobuf mode beside the v0 JSON host.
- Keep protocol output on standard output and diagnostics on standard error.

Required evidence:

- Malformed, truncated, oversized, and unknown payload tests pass.
- Semantic validation covers cards, players, pots, histories, legal actions,
  feature support, and probability normalization.
- Fuzz targets cover envelope decode and domain mapping.
- v0 and v1 produce equivalent domain requests and selected actions on frozen
  fixtures.

Rollback:

- Disable Protobuf host mode and continue using v0.

Completion gate:

- Protobuf can serve all frozen River fixtures without changing production.

## Stage 6: Add the Protobuf Node Client

Owners:

- `clients/node`

Changes:

- Generate or package ESM-compatible Node bindings with bigint support.
- Add length-delimited frame encoding and decoding.
- Add request-ID correlation, capability negotiation, timeout handling, and
  process restart.
- Keep the v0 client selectable for rollback.

Required evidence:

- `uint64` values remain exact through Node and C++ round trips.
- Framing tests cover partial, coalesced, truncated, oversized, and malformed
  frames.
- Concurrent request correlation is correct even if the first host remains
  sequential.

Rollback:

- Select the v0 process client.

Completion gate:

- Node can call Protobuf host mode independently of River Club.

## Stage 7: Migrate River Club to Protobuf

Owners:

- `platforms/river-club`
- `apps/river-club-agent`

Changes:

- Convert River snapshots into accepted Protobuf requests.
- Convert engine strategy responses into River legal actions.
- Preserve adapter-private snapshot tokens and atomic action submission.
- Run v0 and v1 differential decisions over frozen and historical snapshots.
- Enable Protobuf first in replay, then dry runs, then live operation.

Required evidence:

- Golden request mapping passes.
- v0/v1 domain and selected-action parity passes or approved differences are
  documented as strategy changes.
- Replay reports zero illegal actions and zero unexpected fallbacks.
- Live dry runs preserve stale-state, timeout, leave, and resume behavior.

Rollback:

- Select v0 client and host without changing River commands or session logs.

Completion gate:

- River Club production uses Protobuf with an exercised v0 rollback path.

## Stage 8: Remove v0 and Normalize Packaging

Owners:

- engine host
- Node client
- `bin/`
- build and release tooling

Changes:

- Remove v0 JSON request parsing and compatibility code after an explicit
  checkpoint.
- Move tracked implementation out of `bin/`.
- Publish executables and launchers through install or package output.
- Preserve approved stable command entry points.
- Update current architecture, protocol, and integration documentation.

Required evidence:

- No v0 callers remain.
- Release packaging contains all required launchers and binaries.
- Clean checkout build and test pass.
- Replay and River operational validation pass.

Rollback:

- This is the final destructive stage. Tag the last v0-capable release and
  retain its documented deployment artifact.

Completion gate:

- RFC 0002 protocol acceptance criteria are fully implemented.

## Stage 9: Validate with a Second Platform

Owners:

- `platforms/<second-platform>`
- adapter conformance tests

Changes:

- Implement one real second platform adapter.
- Reuse the Protobuf engine client and engine without strategy changes.
- Compare semantically equivalent states across adapters.
- Extract shared runner behavior only where both adapters prove equivalence.

Required evidence:

- No platform branch enters engine poker, solver, policy, or service code.
- Cross-adapter conformance fixtures pass.
- Platform-specific lifecycle behavior remains adapter-owned.

Rollback:

- Remove the second adapter without affecting River Club or the engine.

Completion gate:

- RFC 0001 acceptance criteria are fully implemented.

## Global Verification Matrix

Every implementation stage runs:

```bash
npm run check
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
node bin/replay.mjs
```

Stages that modify native memory, framing, generated native code, or solver
boundaries also run:

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Protocol stages additionally run Buf lint, formatting, breaking checks,
generation, and cross-language conformance tests.

## Status Transitions

- When Stage 0 implementation begins, change both RFCs to `Implementing`.
- RFC 0001 becomes `Implemented` only after Stage 9.
- RFC 0002 becomes `Implemented` only after Stage 8.
- Any material deviation from an accepted design requires a superseding RFC
  before implementation continues.
