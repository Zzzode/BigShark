---
rfc: "0002"
subject: "Protobuf Engine Protocol"
status: "Implementing"
authors: "BigShark maintainers"
created: "2026-09-13"
updated: "2026-09-13"
owners: "protocol, engine service, engine host, engine clients, platform adapters"
supersedes: ""
superseded-by: ""
---

# RFC 0002: Protobuf Engine Protocol

## Summary

Adopt Protocol Buffers as the single source of truth for communication between
platform adapters and the BigShark engine host. Use one versioned Protobuf
package, Buf for linting and compatibility checks, generated C++ and Node
types, and length-delimited binary messages over standard input and output.

The protocol returns a complete action strategy plus an optional
deterministically sampled action. It separates invalid or unsupported requests
from strategic folds and keeps platform snapshot tokens outside the engine.

## Motivation

The current production host accepts unversioned JSON and parses it directly
inside `engine/src/decision.cpp`. The current JavaScript client builds that
JSON from River Club state.

This contract has several limitations:

- missing fields silently receive defaults;
- malformed input can become a fold-shaped response;
- serialization types and policy code are coupled;
- no generated cross-language types exist;
- no stable versioning or compatibility policy exists;
- compact range-tracker action strings leak internal representation;
- one selected action hides the mixed strategy expected from a GTO engine;
- the protocol cannot negotiate solver capabilities;
- large integers require an explicit Node representation policy.

A short-lived JSON Schema prototype explored the domain fields and semantic
invariants. It was never connected to production and was removed before this
RFC entered review so the repository would not retain two candidate sources
of truth.

## Goals

- Define one authoritative, versioned engine protocol.
- Generate type-safe C++ and Node bindings.
- Preserve additive forward and backward compatibility within v1.
- Express normalized NLHE state without provider-specific fields.
- Define exact chip, pot, commitment, and target-total semantics.
- Return full mixed strategies, selected actions, EV data, and solver
  provenance.
- Represent invalid and unsupported requests as explicit errors.
- Support capability negotiation before decisions are submitted.
- Keep transport replaceable while standardizing local process framing.
- Make protocol linting, breaking-change checks, and conformance tests native
  project gates.

## Non-Goals

- Defining provider authentication, room discovery, polling, or retries.
- Standardizing platform snapshot tokens.
- Adopting gRPC in the first implementation.
- Defining storage for large precomputed strategy blueprints.
- Supporting arbitrary poker variants in v1.
- Making side-pot or tournament ICM strategy production-ready.
- Migrating River Club in the same change that lands the IDL.
- Preserving the experimental JSON Schema as an independent contract.

## Current State and Evidence

### Production v0

`bin/bigshark-engine` reads one JSON object or one NDJSON line and returns:

```json
{
  "action": "call",
  "amount": 0,
  "reason": "call eq61",
  "equity": 0.612,
  "mdf": 0.7
}
```

`engine/src/decision.cpp` owns yyjson parsing and policy execution.
`bin/cpp-engine.mjs` owns both River state normalization and process
supervision.

### JSON protocol prototype

The removed Draft 2020-12 prototype proved that field-level validation must be
paired with poker-semantic validation for cards, players, pots, actions, and
legal ranges. RFC 0002 preserves that lesson while assigning the authoritative
contract to Protobuf.

### Runtime characteristics

- Requests are small relative to solver working sets.
- River solving dominates serialization cost.
- The Node adapter and C++ engine run on the same machine.
- Human-readable JSON remains valuable for logs and debugging.
- A future remote engine service is possible but not required now.

These facts favor ecosystem maturity and generated types over zero-copy
serialization.

## Design Principles

1. **One source of truth.** `.proto` files define the wire contract. JSON
   Schema, documentation tables, and generated code must not become parallel
   manually edited contracts.
2. **Domain types remain independent.** Generated Protobuf messages terminate
   at the engine host and client boundaries.
3. **Semantic precision beats transport cleverness.** Amount, pot, action,
   all-in, and error semantics are explicit.
4. **Strategy is richer than one action.** The engine exposes the probability
   distribution it used.
5. **Platform freshness stays private.** Revisions, ETags, optimistic-lock
   tokens, and provider request IDs remain adapter-owned.
6. **Local transport first.** Standard input and output remain the first
   transport; remote RPC can be added without changing domain messages.
7. **Compatibility is automated.** Buf lint and breaking checks guard every
   protocol change.
8. **Large strategy artifacts are separate.** RPC messages do not define
   blueprint storage.

## Proposed Design

### Source layout

```text
proto/
  buf.yaml
  buf.gen.yaml
  bigshark/engine/v1/
    engine.proto

engine/
  src/protocol/
    request_mapper.cpp
    response_mapper.cpp
    semantic_validator.cpp

clients/node/
  generated/
  engine-process-client.mjs
```

Start with one `engine.proto`. Split it only when ownership or generation
boundaries require separate files.

Generated C++ and Node code is written under build or package-output
directories. Generated code is not the source of truth and is not manually
edited.

### Protobuf package

The file begins with:

```protobuf
syntax = "proto3";

package bigshark.engine.v1;
```

The package name carries the major protocol version. Minor evolution is
additive inside the package and advertised through capabilities.

### Transport envelope

Every frame contains an `Envelope`:

```protobuf
message Envelope {
  uint32 protocol_minor = 1;
  string request_id = 2;

  oneof payload {
    GetCapabilitiesRequest get_capabilities_request = 10;
    GetCapabilitiesResponse get_capabilities_response = 11;
    DecisionRequest decision_request = 12;
    DecisionResponse decision_response = 13;
  }
}
```

Rules:

- `request_id` is generated by the caller and echoed by the response.
- The v1 host rejects unsupported minor-version requirements explicitly.
- Exactly one payload is present.
- Standard output contains frames only.
- Human-readable diagnostics go to standard error.

### Local framing

The initial transport is length-delimited Protobuf over standard input and
standard output:

```text
varint encoded frame length
serialized Envelope bytes
```

Properties:

- maximum frame size: 1 MiB;
- incomplete frames at end of stream are protocol errors;
- responses may be correlated by request ID;
- the initial host may process sequentially;
- clients must not rely on response ordering once concurrent execution is
  introduced;
- process exit fails every outstanding request;
- logs never share standard output with frames.

No gRPC dependency is introduced in the first implementation. A later gRPC or
Unix-domain-socket host may reuse the same messages.

### Exact amount representation

All monetary values use unsigned 64-bit integers in atomic units:

```protobuf
message AmountUnit {
  string name = 1;
  uint32 decimal_places = 2;
}
```

Examples:

- chips with no fraction: `name = "chip"`, `decimal_places = 0`;
- cent-denominated chips: `decimal_places = 2`.

Displayed amount is `atomic_units / 10^decimal_places`.

JavaScript generated bindings must expose `uint64` values as `bigint`. Number
conversion is forbidden unless the value has first been checked against
`Number.MAX_SAFE_INTEGER`.

Amount definitions:

- `stack`: chips behind;
- `street_committed`: chips already committed on the current street;
- `pot_total`: all chips in pots before the requested hero action;
- `to_call`: additional chips required from hero;
- `target_total`: total hero commitment on the current street after a bet or
  raise;
- legal target ranges are inclusive.

### Game definition

`GameDefinition` contains:

- variant, initially `NLHE`;
- betting structure, initially `NO_LIMIT`;
- cash or tournament game type;
- table capacity;
- amount unit;
- small blind and big blind;
- ante and button ante;
- optional rake policy;
- optional straddle policy.

Rake policy contains:

- basis points;
- cap in atomic units;
- no-flop-no-drop behavior.

Unsupported game rules produce `UNSUPPORTED_FEATURE`, never silent
approximation.

### Cards

Cards use enums instead of strings:

```protobuf
message Card {
  Rank rank = 1;
  Suit suit = 2;
}
```

Every enum has an `UNSPECIFIED = 0` member. Semantic validation rejects
unspecified cards, duplicate cards, and overlap between hero cards and board.

### Seats and players

`HandState` carries:

- `hand_id`;
- monotonically increasing `decision_index` within the hand;
- current street;
- button seat;
- hero player ID;
- repeated player states;
- hero hole cards;
- board;
- pot model;
- forced contributions;
- voluntary action history;
- legal actions.

Each player has:

- opaque player ID;
- seat index;
- stack behind;
- current-street commitment;
- active, folded, or all-in status.

Position labels are derived by the engine from ordered occupied seats and the
button. Adapters must not send redundant authoritative position strings.

### Forced contributions

Blinds, antes, dead blinds, and straddles are represented separately from
voluntary action history:

```protobuf
message ForcedContribution {
  string player_id = 1;
  ForcedContributionType type = 2;
  uint64 amount = 3;
}
```

This preserves full preflop economics without mixing forced posts with player
decisions.

### Structured action history

`ActionEvent` contains:

- sequence number;
- street;
- actor player ID;
- canonical action;
- incremental amount when chips move;
- resulting street target total when chips move;
- pot before the action;
- stack after the action;
- all-in flag.

Action history contains no:

- localized text;
- provider event IDs;
- provider revisions;
- compact solver line codes;
- display names.

The engine service derives any internal range-tracker representation.

### Pots

`PotState` contains:

- total pot;
- main pot;
- repeated side pots with amount and eligible player IDs.

Capabilities advertise side-pot strategy support. The current engine reports
it as unsupported and returns `UNSUPPORTED_FEATURE` for a decision that
requires side-pot-aware utility.

### Legal actions

The request contains repeated `LegalAction` messages:

```protobuf
message LegalAction {
  ActionType type = 1;
  optional uint64 min_target_total = 2;
  optional uint64 max_target_total = 3;
  bool all_in_only = 4;
}
```

Rules:

- fold, check, and call have no target range;
- bet and raise require an inclusive target range;
- bet and raise are mutually exclusive at one decision;
- `to_call` is zero when check or bet is legal;
- legal actions describe platform legality, not the solver action abstraction.

### Policy request

`DecisionOptions` contains:

- strategy profile;
- solve time budget;
- deterministic `uint64` seed;
- whether a sampled action is requested;
- whether the full strategy is requested;
- optional solver-mode preference.

The service may reject an unavailable forced solver mode. Automatic mode may
select a supported backend.

### Strategy response

A successful `DecisionResponse` contains:

```protobuf
message Strategy {
  repeated ActionPolicy actions = 1;
  optional SelectedAction selected_action = 2;
  SolverMetadata solver = 3;
}
```

Each `ActionPolicy` contains:

- action type;
- optional target total;
- all-in flag;
- probability;
- optional expected value in atomic units.

Probabilities must be finite, non-negative, and sum to one within a documented
tolerance. Heuristic policies may return a degenerate distribution with one
action at probability one.

`SelectedAction` is present only when requested. It must be one of the policy
actions and is sampled from the supplied deterministic seed.

`SolverMetadata` contains:

- source: preflop chart, postflop heuristic, river LP, or river DCFR;
- solve time;
- optional equity;
- optional minimum defense frequency;
- optional exploitability;
- cache-hit state;
- compact stable reason code;
- optional human-readable diagnostic reason.

### Errors

`DecisionResponse` uses a `oneof`:

```protobuf
oneof result {
  Strategy strategy = 1;
  EngineError error = 2;
}
```

Error codes include:

- invalid request;
- unsupported protocol;
- unsupported game;
- unsupported feature;
- no decision;
- deadline exceeded;
- resource exhausted;
- internal error.

Errors carry a safe message, retryability, and structured field violations.
The engine never converts an error into fold. The platform adapter owns the
final legal fallback.

### Capabilities

`GetCapabilitiesResponse` advertises:

- protocol minor versions;
- engine build version;
- supported game variants and betting structures;
- supported player counts;
- supported streets;
- legal action vocabulary;
- amount semantics;
- solver modes;
- strategy profiles;
- exact LP availability;
- DCFR availability;
- multi-street status;
- side-pot support;
- rake support;
- tournament ICM support;
- maximum request size and solve time.

Adapters query capabilities once per engine process and fail closed on
unsupported required features.

### Validation

Field-level constraints use Protovalidate annotations where they are portable
across generated C++ and Node code.

The engine service performs semantic validation for:

- card uniqueness;
- board length by street;
- player, seat, and button consistency;
- hero status;
- action actor existence;
- nondecreasing street and sequence order;
- pot and side-pot consistency;
- amount and stack consistency;
- legal action coherence;
- target range ordering;
- requested features versus capabilities;
- strategy probability normalization.

Adapters also validate the response against the latest authoritative platform
snapshot before acting.

### Buf workflow

The repository pins Buf configuration:

- `buf lint` enforces style;
- `buf format --diff --exit-code` enforces formatting;
- `buf breaking --against` prevents incompatible changes to accepted v1;
- `buf generate` produces C++ and Node bindings.

Field numbers and enum values are never reused. Removed fields and names are
reserved.

### Generated code

The repository tracks:

- `.proto` source;
- Buf configuration and dependency lock;
- handwritten mappers and semantic validators;
- conformance fixtures.

Generated C++ and Node code is produced under build or package-output
directories and is not manually edited. Release packaging may include
generated artifacts.

### JSON mapping

Protobuf JSON mapping may be used for:

- debug output;
- readable conformance fixtures;
- session inspection;
- migration differentials.

It is derived from the Protobuf messages and is not an independently authored
contract. The removed JSON Schema prototype is not restored.

### Strategy artifacts

Large precomputed blueprints, regret tables, and bucket data do not use this
RPC protocol by default. Their storage requires a separate RFC covering:

- random access;
- memory mapping;
- compression;
- checksums;
- model versioning;
- compatibility;
- partial loading.

Cap'n Proto, FlatBuffers, or a custom chunked format may be evaluated there.

## Dependency Rules

Permitted:

```text
engine host -> generated protobuf -> protocol mapper -> engine service
Node engine client -> generated protobuf
platform adapter -> Node engine client
```

Forbidden:

- poker domain, solver, or policy code importing generated Protobuf headers;
- generated types crossing into solver-private APIs;
- adapters constructing solver-internal compact action lines;
- provider credentials or snapshot tokens entering protocol messages;
- handwritten JSON Schema duplicating the Protobuf contract;
- logs written to framed standard output.

## Compatibility and Migration

### Version rules

- Package `bigshark.engine.v1` defines major version 1.
- Additive optional fields and enum values are allowed within v1.
- Existing field meaning, type, and number are immutable.
- Removed fields and enum numbers are reserved.
- Receivers tolerate unknown fields.
- Senders use capabilities before relying on an additive feature.
- Breaking changes create package `bigshark.engine.v2`.

### Migration stages

1. Add `.proto`, Buf configuration, generated bindings, and conformance tests
   without changing production.
2. Add Protobuf domain mappers and semantic validation to C++.
3. Add a Protobuf engine-host mode beside v0 JSON.
4. Add framed Protobuf support to the extracted Node engine client.
5. Convert frozen River snapshots to v1 and compare domain requests and
   decisions with v0.
6. Switch River Club to Protobuf behind the existing commands.
7. Remove v0 after replay parity, live validation, and an explicit removal
   checkpoint.

No production stage changes River CLI behavior or platform action semantics.

## Security and Operational Impact

- The host rejects frames larger than 1 MiB before allocation.
- All solve budgets are bounded.
- Protocol output is isolated from diagnostic logs.
- Credentials and provider snapshot tokens never cross the boundary.
- Unknown enum values fail semantic validation when required for a decision.
- Invalid cards, amounts, histories, or capabilities fail closed.
- Error messages exclude hidden cards beyond the caller-provided hero state.
- Correlation IDs allow tracing without using player display names.
- Binary inputs require fuzzing because malformed frames reach native code.

## Alternatives Considered

### JSON plus JSON Schema

Rejected as the authoritative protocol. It is readable and useful for
prototyping, but it lacks generated C++ domain boundaries, compact framing,
standard unknown-field evolution, and mature cross-language breaking checks
equivalent to Buf.

JSON remains a derived debug representation.

### Cap'n Proto

Rejected for engine RPC v1. Zero-copy access is attractive, but request sizes
are small and solver cost dominates serialization. Protobuf has stronger
C++, Node, Python, and Rust adoption; broader tooling; Buf compatibility
checks; and easier future gRPC integration.

Cap'n Proto remains a candidate for large memory-mapped strategy artifacts,
which have different requirements.

### FlatBuffers

Rejected for RPC v1. It provides efficient random access but a weaker service
and compatibility workflow for this use case. It may also be reconsidered for
large read-only artifacts.

### gRPC immediately

Rejected for the first migration. It adds HTTP/2, server lifecycle, ports, and
additional native dependencies before a remote service is required.
Length-delimited standard I/O preserves the current deployment model.

### Custom binary structs

Rejected. They would require custom evolution, language bindings, framing,
validation, and debugging tools.

### Protobuf types as core domain types

Rejected. It would couple policy and solver code to serialization, make domain
tests depend on generated code, and obstruct future in-process callers.

## Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Protobuf adds build complexity | Slower onboarding and CI | Pin Buf and Protobuf versions; document one generation command |
| Node `uint64` precision loss | Incorrect chip amounts | Generate bigint bindings and prohibit unchecked Number conversion |
| Schema allows semantically invalid poker state | Wrong decisions | Central semantic validator plus conformance fixtures and fuzzing |
| v0 and v1 decisions diverge | Strategy regression | Golden River differential tests before switching |
| Mixed-strategy response increases complexity | Adapter misuse | Keep selected action explicit and validate it against the distribution |
| Unknown enum handling is inconsistent | Silent feature mismatch | Capability negotiation and reject unspecified required values |
| Generated code enters source ownership | Review noise | Generate under build/package output and never edit manually |
| Protocol grows into artifact storage | Oversized unstable messages | Require a separate strategy-artifact RFC |

## Verification Plan

- `buf lint` and `buf format --diff --exit-code`.
- Buf breaking-change check against the accepted v1 baseline.
- Generated C++ and Node compilation.
- Binary round trips for every message.
- Cross-language C++ to Node and Node to C++ golden vectors.
- Truncated, oversized, malformed-varint, and unknown-message framing tests.
- Protovalidate field-constraint tests.
- Semantic validator tests for cards, seats, pots, histories, amounts, legal
  actions, and capabilities.
- Fuzzing of envelope decode and domain mapping.
- v0/v1 differential decisions over frozen River snapshots.
- Existing debug, release, sanitizer, and replay suites.
- Benchmark serialization separately from policy and solver time.

## Rollout Plan

1. Land the accepted `.proto` and validation toolchain.
2. Generate bindings and compile them without linking production.
3. Add domain mappers and semantic tests.
4. Add an opt-in Protobuf host mode.
5. Add an opt-in Node Protobuf client.
6. Run offline River fixture differentials.
7. Enable Protobuf in replay.
8. Enable Protobuf in River dry runs.
9. Switch the live River runner while retaining v0 rollback.
10. Remove v0 only after explicit review of parity evidence.

Every stage is a separate logical change.

## Rollback Plan

Until stage 10, retain the v0 JSON host and client. A runtime switch selects v0
without changing platform behavior or strategy code. Generated Protobuf code
and conformance tests may remain even if live traffic rolls back. No persisted
session log migration is required.

## Open Questions

Stage 4 resolved the initial toolchain decisions:

- Buf CLI 1.73.0 and Protobuf 36.1 define the supported toolchain.
- Protobuf-ES 2.13.0 generates strict ESM TypeScript with `bigint` for
  64-bit integers.
- The first host remains sequential while retaining request IDs.
- gRPC service declarations are deferred until a remote transport has a
  concrete consumer.

## Acceptance Criteria

- Protobuf is approved as the sole authoritative engine protocol IDL.
- Buf is approved for linting, generation, and breaking checks.
- Length-delimited standard I/O is approved as the initial transport.
- Exact integer amount semantics and Node bigint handling are approved.
- Structured state, action history, mixed strategy, error, and capability
  models are approved.
- Generated types are confined to host and client boundaries.
- The decision to keep JSON only as a derived debug representation is
  approved.
- The v0 compatibility and rollback plan is approved.
- No protocol implementation starts before this RFC records explicit
  approval.

## Implementation Evidence

Stage 4 is complete:

- `proto/bigshark/engine/v1/engine.proto` is the single v1 source of truth.
- `proto/buf.yaml`, `proto/buf.lock`, and `proto/buf.gen.yaml` pin lint,
  dependency, and generation behavior.
- npm generation writes developer-facing bindings under `build/generated`;
  every CMake preset writes C++ bindings under its own build directory.
- Protobuf 36.1 is checksum-pinned for compiler download and CMake runtime
  builds. Buf 1.73.0 and Protobuf-ES 2.13.0 are exact npm dependencies.
- `proto/baseline/v1.binpb` is the accepted breaking-change baseline.
- C++ and TypeScript consume shared binary and ProtoJSON golden vectors for
  every envelope payload, response result, nested message, and field.
- TypeScript-generated vectors are consumed by C++, and a C++-generated
  capability vector is consumed by TypeScript. The tests verify `uint64`
  bigint precision, signed expected value, oneof selection, unknown fields,
  and additive enum values.
- Buf lint, format, breaking, generation, C++ compilation, and TypeScript
  compilation are CMake and CTest gates.
- The v1 messages remain disconnected from production; Stage 5 owns mapping,
  semantic validation, framing, and host mode.

## Decision

Approved by: BigShark project maintainer

Decision date: 2026-09-13

Approved as written. Protocol Buffers is the sole authoritative engine
protocol IDL, Buf owns protocol linting and compatibility checks, and
length-delimited standard input and output is the initial transport.
Implementation must retain the v0 rollback path until the parity and removal
gates in this RFC pass.
