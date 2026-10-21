# Protobuf Engine Protocol

Status: Current

## Scope

`proto/bigshark/engine/v2/engine.proto` is the authoritative engine protocol
definition (RFC 0022). It is a coherent full-state protocol with a G/F
contribution ledger, phase-tagged pot layers, observation provenance, and
capability tuples. The engine-host accepts `protocol_version` field 1 == 3
on every frame. RFC 0022 is the authoritative design reference.

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

The package is `bigshark.engine.v2`. One `Envelope` carries a protocol
version (3), caller-generated request ID, and exactly one payload:

- capability request;
- capability response;
- decision request;
- decision response.

The decision request models:

- game rules, rule profile, and atomic amount units;
- players, seats, stacks, G/F contribution ledger, status, hero cards, and board;
- phase-tagged pot layers with side-pot eligibility;
- TransitionEvent path (ActionEvent and BoardDeal);
- legal actions with inclusive target-total ranges;
- strategy profile, solve budget, deterministic seed, solver preference, and
  utility profile.

The response contains either:

- a mixed strategy, optional selected action, expected values, and solver
  metadata; or
- a structured engine error with field violations and retryability.

`SolverMetadata.artifact_sha256` is `optional string` with a `^[a-f0-9]{64}$`
pattern, and its **presence** is meaningful. A row served from a resident
artifact reports the digest. A row with no artifact behind it — a uniform-prior
subgame solve — reports the field **absent** and carries the
`subgame-resolve:approximate` diagnostic with `cache_hit = false` and reason
code `resolving`; this follows RFC 0025, which specifies an empty digest for
that path. The field is never present with a zero-length value, because the
pattern forbids it and a present-but-malformed digest is rejected as an internal
error.

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

`proto/baseline/v2.binpb` is the accepted descriptor baseline used only for
Buf breaking checks. It is not an alternate schema source. The compatibility
policy is:

- field numbers and enum values are never reused;
- additive fields and enum values are allowed;
- receivers preserve unknown fields;
- unknown enum values remain representable;
- breaking changes require a new package version.

## Verification

```bash
npm run proto:lint
npm run proto:format:check
npm run proto:breaking
npm run proto:generate
npm run proto:typecheck
cmake --build --preset release --target protobuf-check
ctest --preset release -R '^protobuf_|^conformance$'
```

The C++ conformance tests consume executable test vectors that cover every
nested message and field. They verify exact values above
`Number.MAX_SAFE_INTEGER`, signed expected value, oneof selection,
unknown-field preservation, and additive enum handling.

## Framed Host

The published `bin/bigshark-engine` binary supports framed invocation:

| Invocation | Transport |
| --- | --- |
| `bigshark-engine --serve-proto` | persistent framed coprocess, one frame per decision (default for the River Club runner) |

The framed host is sequential. Each frame is a canonical ULEB128 length
prefix followed by a serialized `Envelope`. Standard output carries frames
only; all diagnostics go to standard error. The host echoes the caller
`request_id` on every response. A framing-level failure (oversize declared
length, malformed or overlong varint, incomplete frame at end of stream)
closes the connection with exit status 2; a parseable but invalid envelope is
answered with a `DecisionResponse.error`.

The host answers every parseable frame with an error envelope rather than
exiting when possible. `FieldViolation` lists are capped at 32 entries (the
last entry marks omitted violations), guaranteeing every error response fits
inside one 1 MiB frame so a request with hundreds of thousands of bad entries
cannot tear down a persistent coprocess.

### Frame limits

- Maximum frame size is 1,048,576 bytes (1 MiB), matching
  `maximum_request_bytes` in capabilities.
- The length prefix is at most five bytes; a sixth continuation byte, a
  non-canonical terminating zero group, or a zero length is rejected.
- The reader decodes the prefix into a five-byte header buffer and compares
  the running length against the 1 MiB limit before allocating the single
  payload buffer, so an oversize declaration never allocates the payload.

## Protocol Details

### Version discriminator

The envelope uses `protocol_version = 3` (field 1, uint32 varint at wire
tag 0x08). The engine-host peeks field 1 without a full protobuf parse
(`peekProtocolVersion`). The handler rejects any `protocol_version != 3`
with `UNSUPPORTED_PROTOCOL`.

### G/F contribution ledger

Every `PlayerState` carries:

- `starting_stack` — chips at hand start;
- `stack` — current chips behind;
- `gross_contributed` (G) — total chips put in pot this hand, never
  decreases (includes refunded amounts);
- `refunded` (F) — cumulative uncalled returns this hand.

Invariant: `starting_stack = stack + gross_contributed - refunded`.
The native `GamePlayer` already tracks G (`contributed`) and F
(`refunded`); the resident mapper validates wire G/F against the
native replay after reconstructing the hand state.

### Phase-tagged pot layers

`PotLayer` carries a `status` tag:

- `PENDING` — provisional layer during the action phase; a sole-contributor
  PENDING layer is valid (an open bet with no callers yet);
- `FINALIZED` — at settlement; requires at least two contributors (a
  sole-contributor positive layer is a refund, not a pot).

`PotState` carries a `phase` (`ACTION`, `SHOWDOWN`, `SETTLED`) and
`pot_total` (must equal the sum of layer amounts). The handler accepts
ACTION-phase decision requests; SHOWDOWN and SETTLED phases are validated
but rejected as `UNSUPPORTED_FEATURE` for decisions (settlement reporting
is a future extension).

### Observation provenance

`ObservationProvenance` on every `TransitionEvent` records:

- `source` — who observed the event (closed-set enum);
- `coverage` — whether the observation is direct or inferred;
- `controller_epoch` — non-empty epoch identifier;
- `observed_event_count` — must equal the transition path length.

### Capability tuples

`GetCapabilitiesResponse` emits `CapabilityTuple` messages. Each tuple
declares a complete supported combination:

- `quality_scope` (e.g. `"hu-r1"`);
- `min_dealt_players`, `max_dealt_players`;
- `min_live_players`, `max_live_players`;
- `supported_streets`;
- `max_stack_bb`;
- `side_pots`;
- `rule_profile` (settlement, reopening, runout, dealt seat count);
- `utility_profile`;
- `required_coverage`;
- `solver_modes`;
- `max_guarantee`.

The initial advertisement is `hu-r1` (2 dealt, 2 live, all streets, no
side pots). `sixmax-r1` (6 dealt, 2–6 live, side pots) is defined but not
emitted until T2–T5 complete.

### No mid-street rebase

The protocol replays from the true preflop root through the `TransitionEvent`
path to the actionable node in one continuous replay. Refunds are retained
in the native state throughout.

### Error codes

| Code | Name | Meaning |
| --- | --- | --- |
| 10 | `HISTORY_INCOMPLETE` | Transition path missing or has gaps |
| 11 | `REPLAY_MISMATCH` | Replayed state does not match declared snapshot |
| 12 | `INCOMPATIBLE_ROOT` | No strategy root matches the game definition |
| 13 | `OFF_TREE_AMOUNT` | Legal action amount is outside the policy |
| 14 | `RULES_UNCONFIRMED` | Rule profile has not been confirmed |
| 15 | `UNKNOWN_UTILITY` | Requested utility profile is not supported |

### Sampler domain

The sampler uses SplitMix64 with domain separator
`0x425356322d73616d` ("BSV2-sam").

## C++ Protocol Boundary

The public boundary is `engine/include/bs/protocol.hpp`, a protobuf-free
header that declares `handleEnvelope`, `peekProtocolVersion`, `EnvelopeResult`,
`BlueprintMiss`, `BlueprintRow`, `BlueprintResult`, `ResolveOutcome`,
`ResolveResult`, `HostServices`, and `ReplayMiss`. Generated protobuf types
are confined to `engine/src/protocol/` translation units.

### Semantic validation

The hand-written semantic validator (`engine/src/protocol/semantic_validator.cpp`)
validates every field of the `DecisionRequest`: game definition, rule profile,
G/F ledger invariant, pot layer math, transition path provenance, legal action
bounds, and decision options. It returns `ERROR_CODE_UNSPECIFIED` when the
request is valid, otherwise the first violation's error code.

### Resident mapper

The resident mapper (`engine/src/protocol/resident_mapper.cpp`) reconstructs
a native `GameState` from the wire `HandState` by replaying the
`TransitionEvent` path from the true hand-start root. It validates the G/F
contribution ledger and pot state against the native replay at the actionable
node.

### Request mapper

The request mapper (`engine/src/protocol/request_mapper.cpp`) maps the
`DecisionRequest` into the heuristic `bs::Ctx` consumed by `bs::decideSourced`.
It extracts the public action history from the transition path and builds the
same Ctx knobs the mapper produces.

### Response mapper

The response mapper (`engine/src/protocol/response_mapper.cpp`) maps native
decision and lookup results into wire responses: heuristic decisions, resident
blueprint hits, resolved terminal-only rows, and errors. It also builds the
capability advertisement with `CapabilityTuple` messages.

### Implementation files

| File | Purpose |
| --- | --- |
| `engine/include/bs/protocol.hpp` | Protobuf-free public boundary |
| `engine/src/protocol/envelope.cpp` | Envelope dispatch, version check, solver-mode routing |
| `engine/src/protocol/semantic_validator.cpp` | Hand-written semantic validation |
| `engine/src/protocol/resident_mapper.cpp` | Wire `HandState` → native `GameState` reconstruction |
| `engine/src/protocol/request_mapper.cpp` | Wire `DecisionRequest` → heuristic `bs::Ctx` mapping |
| `engine/src/protocol/response_mapper.cpp` | Native → wire response mappers, `errorResponse`, `buildCapabilities` |
| `engine/src/protocol/mappers.hpp` | Internal shared header (includes generated protobuf) |
| `engine/src/protocol/frame_stream.cpp` | ULEB128 frame codec, `FrameReader`, `writeFrame` |
| `engine/tests/test_conformance.cpp` | RFC 0022 conformance vectors and envelope tests |

## TypeScript Client and River Adapter

The TypeScript client (`clients/node/proto-engine-process-client.ts`)
implements the framed engine process lifecycle:

- ULEB128 frame codec (`encodeVarint`, `MAX_FRAME_BYTES`);
- Capability-tuple handshake at startup (sends `GetCapabilitiesRequest`,
  verifies `supported_protocol_versions` includes `3`, stores
  `CapabilityTuple[]`);
- Every envelope carries `protocolVersion = 3`;
- Request/response correlation by `requestId`;
- Process spawn, warmup, restart, and stop.

The River adapter mapper (`platforms/river-club/src/mapper.ts`) converts a
`RiverRoom` snapshot into a `DecisionRequest`:

- **G/F ledger**: seeded with blind amounts, then replayed through structured
  actions. Refunds are detected at closed-street boundaries by comparing
  per-street commitments; the current (open) street's refund is applied by
  the engine at close. The invariant `startingStack = stack + G - F` is
  validated before sending.
- **Pot layers**: built from `room.pots` when present, otherwise from net
  contributions (`G - F`). All layers are `PENDING` during the action phase.
- **TransitionEvent path**: `ActionEvent` and `BoardDeal` events with
  `potBefore` and `stackAfter` tracked incrementally from blinds.
- **Response mapping**: `executeFromStrategy` and `validateEngineDecision`
  process the `Strategy`/`ExpandedStrategy` response.

### Conformance vectors

Three executable test vectors (from RFC 0022) are pinned in
`engine/tests/test_conformance.cpp`:

1. **Ordinary open bet** — 6-max, flop decision, hero seat 2, to_call=8.
   Validates G/F ledger, PENDING pot layers, legal action bounds, and
   heuristic decision through the envelope handler.
2. **Capped call with refund** — replay verification. Validates that a
   post-street-closure state with a refund produces `REPLAY_MISMATCH`
   (the actionable node is past the decision point).
3. **Side pot + pending wager** — 6-max, turn decision, hero seat 5,
   to_call=20. Validates side-pot layer math, multi-contributor PENDING
   layers, and heuristic decision.
