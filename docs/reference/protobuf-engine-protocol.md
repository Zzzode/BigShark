# Protobuf Engine Protocol

Status: Current

## Scope

`proto/bigshark/engine/v1/engine.proto` is the authoritative v1 engine
protocol definition. It is implemented as a generated-code and compatibility
test target. Production River Club traffic remains on the v0 NDJSON protocol
until RFC 0002 Stages 5-7 add domain mapping, framing, and adapter migration.

## Toolchain

| Component | Pinned version | Ownership |
| --- | --- | --- |
| Buf CLI | 1.73.0 | npm development dependency |
| Protobuf compiler and C++ runtime | 36.1 | Checksum-verified compiler download and CMake FetchContent |
| Protobuf-ES generator and runtime | 2.13.0 | npm dependencies |
| Protovalidate schema | Locked BSR commit | `proto/buf.lock` |

`tools/proto/ensure-toolchain.ts` downloads the platform-specific `protoc`
archive to the ignored `.tools/` directory and verifies its SHA-256 digest.
An interprocess lock serializes first-time bootstrap, and a validated temporary
installation is renamed into place atomically. Supported bootstrap platforms
are macOS and Linux on arm64 and x86-64.

## Contract

The package is `bigshark.engine.v1`. One `Envelope` carries a protocol minor
version, caller-generated request ID, and exactly one payload:

- capability request;
- capability response;
- decision request;
- decision response.

The decision request models:

- game rules and atomic amount units;
- players, seats, stacks, status, hero cards, and board;
- pots and side-pot eligibility;
- forced contributions and structured voluntary action history;
- legal actions with inclusive target-total ranges;
- strategy profile, solve budget, deterministic seed, and solver preference.

The response contains either:

- a mixed strategy, optional selected action, expected values, and solver
  metadata; or
- a structured engine error with field violations and retryability.

All chip amounts use `uint64`. Protobuf-ES exposes them as JavaScript
`bigint`. Expected value uses `sint64` because it may be negative. Provider
credentials, revisions, display names, and localized event text are excluded.

## Generated Outputs

`npm run proto:generate` writes the developer-facing outputs:

```text
build/generated/cpp/
build/generated/ts/
```

Each CMake preset generates its C++ bindings under
`build/<preset>/generated/cpp`, preventing cross-preset clean and compile
races. npm owns the shared TypeScript output, and CMake serializes tests and
targets that regenerate it with an interprocess lock under `build/.locks`.
Generated files are ignored and recreated from the IDL. `bigshark_protocol`
compiles the preset-local C++ bindings and keeps generated types outside
poker, solver, and policy APIs.

## Compatibility

`proto/baseline/v1.binpb` is the accepted v1 descriptor baseline used only for
Buf breaking checks. It is not an alternate schema source. The compatibility
policy is:

- field numbers and enum values are never reused;
- additive fields and enum values are allowed in v1;
- receivers preserve unknown fields;
- unknown enum values remain representable;
- breaking changes require package `bigshark.engine.v2`.

## Verification

```bash
npm run proto:lint
npm run proto:format:check
npm run proto:breaking
npm run proto:generate
npm run proto:typecheck
npm run proto:test
cmake --build --preset release --target protobuf-check
ctest --preset release -R '^protobuf_'
```

The C++ and TypeScript tests consume shared binary and ProtoJSON fixtures for
all four envelope payloads and both decision response branches. The fixtures
cover every nested message and field. They verify exact values above
`Number.MAX_SAFE_INTEGER`, signed expected value, oneof selection,
unknown-field preservation, and additive enum handling. A separate C++
producer writes a capability envelope that the TypeScript test consumes,
covering both cross-language directions.

## Pending Runtime Work

Length-delimited framing, semantic request validation, capability handling,
and service-domain mapping belong to RFC 0002 Stage 5. The generated protocol
is not linked into the production v0 host yet.
