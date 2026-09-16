# Protobuf Engine Protocol

Status: Current

## Scope

`proto/bigshark/engine/v1/engine.proto` is the authoritative v1 engine
protocol definition. RFC 0002 Stage 7 ships an opt-in framed Protobuf host
mode, C++ request/response mappers with a strict hand-written semantic
validator, a framed TypeScript client, a River v1 mapper, and v0/v1 golden
differential parity. The v0 NDJSON host, the Node JSON client, and live River
Club traffic remain the default; the framed path is selected only explicitly
(see [Framed host](#framed-host)).

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
and service-domain mapping are implemented as the RFC 0002 Stage 7 minor-0
path below. Live dry-run/canary, v0 removal, and minor-1 full mixed-strategy
distributions remain explicit external gates.

## Framed Host

The published `bin/bigshark-engine` binary keeps its v0 invocation unchanged
and adds two explicit flags:

| Invocation | Transport |
| --- | --- |
| `bigshark-engine` | v0 one-shot JSON on stdin/stdout (default) |
| `bigshark-engine --serve` | v0 persistent NDJSON coprocess (default) |
| `bigshark-engine --proto` | one framed `Envelope` in, one framed `Envelope` out |
| `bigshark-engine --serve-proto` | persistent framed coprocess, one frame per decision |

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

## C++ Protocol Boundary

The public boundary header `engine/include/bs/v1_protocol.hpp` exposes only
the frame codec and a byte-level `handleEnvelope` entry point; it includes no
generated Protobuf types. The mappers and semantic validator live in
`engine/src/protocol/v1_*` and are the only translation units (with the host
and the v1 tests) that include generated headers. The dependency chain is
host -> generated protobuf -> v1 mapper -> `bs::decide`; domain, solver,
policy, and service headers never see generated messages.

### Semantic validation

The protovalidate C++ runtime is not linked; `v1_semantic_validator.cpp`
hand-writes every protovalidate annotation plus the RFC 0002 poker
invariants:

- unspecified enums on fields required for a decision (variant, structure,
  game type, street, player status, forced contribution type, card rank and
  suit, action type, solver mode, strategy profile membership);
- RFC 3629 UTF-8 on every proto3 string field (`request_id`, ids, names,
  profile, and side-pot eligibility): the C++ runtime parses invalid UTF-8
  leniently in debug builds while Protobuf-ES rejects it, so the validator
  enforces it explicitly to keep both decoders consistent and fail closed;
- proto3 open-enum closed sets: wire enums are open, so every
  request-received enum is range-checked before any indexing, mapping, or
  decision use — ActionType FOLD..RAISE (indexed into a fixed five-slot legal
  array), Rank TWO..ACE, Suit SPADES..CLUBS, decision/history streets
  PREFLOP..RIVER (SHOWDOWN and unknown values rejected), PlayerStatus
  ACTIVE/FOLDED/ALL_IN, ForcedContributionType SMALL_BLIND..STRADDLE,
  GameVariant, BettingStructure, GameType, and SolverMode. The request
  mappers independently throw on an unknown enum instead of silently emitting
  an empty street, a one-character card, or a dropped action. Response-only
  enums (SolverSource, FeatureSupport, AmountSemantics, ErrorCode) are
  produced internally from constants and never read from a request;
- exactly two unique hole cards; board uniqueness and no hero/board overlap;
  board length matching the current street (0/3/4/5);
- 2..10 players with unique ids and seats, button referencing a seat, hero a
  seated active player; seats within table capacity;
- pot coherence: non-negative profile amounts, every chip field that reaches
  the integer heuristic (pot total, main pot, side pot amounts, blinds,
  stacks, street commitments, forced amounts, history targets, to_call, legal
  ranges) must fit a signed int; main pot not over total, with no side pots
  total must equal main pot, and side pots sum to total minus main with at
  least two unique eligible seated players;
- bet/raise target maximum must not exceed the hero's available chips
  (`stack + street_committed`), so an all-in boundary is legal but a
  larger platform range is rejected;
- the big-blind option is the single structured exception where check and
  raise coexist with `to_call == 0`: the hero must have posted exactly the
  big blind this preflop street, no voluntary preflop aggressive action (bet
  *or* raise) may exist yet, and the legal set is fold/check/raise with an
  absolute target. Every other no-bet spot stays check/bet only;
- derived chip sums: individual fields are checked for signed-int fit, and
  the `pot_total + to_call` combination used by pot odds, MDF, and bet
  geometry is validated in `uint64` before mapping and rejected with
  `INVALID_REQUEST` when it cannot fit the integer heuristic (each add is
  itself below 2^54 and cannot overflow `uint64`). The shared policy computes
  these ratios in widened `double`/`int64` arithmetic, so no signed overflow
  UB exists on either path;
- forced contributions with positive profile amounts and existing actors;
- action history with existing actors, strictly increasing sequences,
  nondecreasing streets not beyond the current street, and chip fields only
  on actions that move chips;
- legal action coherence: fold/check/call carry no target range; bet/raise
  require an inclusive well-ordered positive range; bet xor raise; check legal
  exactly when `to_call == 0`; call legal when chips are owed;
- all chip fields inside the 2^53-1 integer profile, with the fields consumed
  by the integer heuristic additionally required to fit in `int`;
- feature negotiation: fractional amount units, antes, button antes, rake,
  straddles, tournament/ICM games, forced RIVER_LP/RIVER_DCFR/MULTISTREET_CFR
  solver modes, and any side pot return `UNSUPPORTED_FEATURE`/
  `UNSUPPORTED_GAME` rather than approximations. Minor 0 accepts only
  AUTOMATIC and HEURISTIC as selectable modes; the capabilities list
  advertises exactly those two, while the exact-LP/DCFR feature bits still
  report which internal river backend the heuristic engine may select;

### v1 to heuristic Ctx reconstruction

`v1_request_mapper.cpp` maps a validated `DecisionRequest` to the existing
`bs::Ctx`:

- blinds, absolute pot, and absolute inclusive bet/raise targets map to the
  integer `sb`, `bb`, `pot`, and `legal.raiseMin/raiseMax`;
- `position` is derived by rotating occupied seats from the button (heads-up:
  button is `BTN`, the other seat `BB`; otherwise `BTN`, `SB`, `BB`, then
  `UTG`/`HJ`/`CO` counting back from the button);
- `effectiveStackBb`: the request may carry
  `options.preflop_effective_stack_bb`, an additive minor-0 hint (tag 7)
  holding the caller/platform preflop effective depth in big blinds. The
  heuristic uses it verbatim when present (validated finite, positive, and
  within a 10,000 bb bound). The River adapter always sends it with v0's exact
  precedence (`solver.effectiveStackBb ?? hero.effectiveStackBb ?? 100`),
  giving exact parity. When a generic v1 client omits it, the host falls back
  to a structural approximation: heads-up
  `min(stack + street_committed, single live opponent total) / bb`, multiway
  `stack / bb`. That fallback is a physical estimate (corpus analysis showed
  the server value is not reconstructable exactly: 83/162 hero-depth and
  35/162 min-total matches), so the full input space requires the hint for
  exact short-stack jam parity; observed corpora have zero jam-gate flips
  under the fallback.
- `potOdds` reconstructs the platform value exactly: River rounds
  `to_call / (pot + to_call)` to four decimal places (frozen values 0.3061,
  0.08, 0.0313); the mapper applies the same four-decimal rounding so
  threshold decisions cannot diverge from the platform value;
- cards become the exact v0 tokens (`23456789TJQKA` rank then `shdc` suit);
- preflop `raises`, `limpers`, and `openerPosition` are counted from
  structured preflop action history only; postflop these knobs stay zero;
- the river solver line knobs replay the v0 normalizer's token-round
  grouping over the structured history (folds end round transitions, calls
  and two passive checks close a round) and rebuild `riverLine`,
  `flopLine`, `turnLine`, and the rounded facing-bet fraction. Heads-up
  five-card river spots with complete fold-free rounds enable the solver;
- the v1 seed is a full `uint64`; the v0 signed cast exists only on the v0
  path.

### Response and error mapping

Minor-0 returns a degenerate `Strategy`: one `ActionPolicy` at probability 1
for the chosen action, with an exact target total for bets/raises. The
`SelectedAction` is present only when requested and equals the policy action;
the mapper independently verifies it against the requested legal set by kind
and exact target. `-1` equity/MDF sentinels stay unset. Solver provenance is
derived from the heuristic reason prefix (`gto-cfr` -> river DCFR, `gto` ->
exact river LP, preflop chart reasons -> preflop chart, otherwise postflop
heuristic).

| Condition | ErrorCode | Retryable |
| --- | --- | --- |
| malformed envelope, missing/unspecified fields, poker-semantic violation | `INVALID_REQUEST` | no |
| `protocol_minor != 0` | `UNSUPPORTED_PROTOCOL` | no |
| non-NLHE, limit structure, tournament/ICM | `UNSUPPORTED_GAME` | no |
| fractional units, antes, rake, straddle, side pots, forced multistreet mode | `UNSUPPORTED_FEATURE` | no |
| empty engine action | `NO_DECISION` | yes |
| solve budget exhausted | `DEADLINE_EXCEEDED` | yes |
| allocation failure | `RESOURCE_EXHAUSTED` | yes |
| non-member engine action, unmappable kind, unexpected exception | `INTERNAL` | yes |

Errors are never converted into fold-shaped strategies; a genuine strategic
fold is a `Strategy` containing `ACTION_TYPE_FOLD` at probability 1. The
capabilities response advertises only `supported_protocol_minors = [0]`
(minor 1 full distributions are a later stage), NLHE no-limit cash, the
target-total-inclusive amount semantics, the tag/lag/station-hunter profiles,
actual exact-LP/DCFR support, experimental multistreet status, unsupported
side pots, rake, and ICM, and a 1 MiB / 120 s maximum.

## TypeScript Framed Client and River Adapter

`clients/node/proto-engine-process-client.ts` is the framed client: Buffer
ULEB128 codec with the length cap checked before allocation, `fromBinary`/
`toBinary` envelopes, all `uint64` values kept as `bigint` end to end,
request correlation via the echoed `request_id` map (never FIFO), stderr
isolated from framed stdout, and the same warmup, timeout, exit, and hard
restart semantics as the NDJSON client. The River adapter
`platforms/river-club/src/v1-mapper.ts` builds the `DecisionRequest` from a
`RiverRoom` (chip unit 0, enum cards, ordered seats, forced blinds,
round-replayed structured history, main pot plus every River side pot with
its eligible seats, absolute inclusive targets bounded by hero chips) and
maps `Strategy`/`EngineError` into the existing `ExecutableDecision` shape.
Actor display names are resolved by full display-name prefix with hero
precedence and longest-name tie-break. The preflop *opener* is an intentional
minor-0 parity shim: the v0 normalizer derives the opener from the first
whitespace token with exact seat-name equality, so a multi-word opener
("Sir Lancelot" -> "Sir") matches no seat and yields no opener (C++ defaults
to HJ). The TS adapter replicates that quirk only for the first preflop
raise, sending an empty `actor_player_id` (the wire marker the C++ mapper
treats as an unresolved opener: raise counts still accrue, but no opener
position or last-raiser is attributed). General actor and hero attribution
uses correct full-name matching. The shim lives only in the TS adapter and is
deleted with the v0 path at RFC 0002 v0-removal. A hand carrying side pots is
answered `UNSUPPORTED_FEATURE` (the adapter populates every River side pot) and
routes to the operational fallback. Number-to-bigint
conversion happens only after integer validation, bigint-to-number only inside
the `Number.MAX_SAFE_INTEGER` profile; out-of-range values fail closed. Engine
and transport errors surface the code and route to the existing operational
`safeFallback`, never to a strategic fold.

The framed path is selected only by `BIGSHARK_ENGINE_PROTO=1` or an explicit
`EngineConfig.proto`; with the env unset and no config flag, the v0 JSON
client and behavior are unchanged.

## Golden Differential

`platforms/river-club/tests/v0-v1-differential.test.ts` runs all six frozen
`v0-golden.json` snapshots through the real engine binary on both the v0 JSON
path and the framed v1 path from the identical River state, asserting equal
action kind and exact target total plus selected-action membership. Beyond the
six frozen hands it adds adversarial dual-transport parity rooms that
exercise branches the snapshots do not: a short heads-up effective-stack jam
driven by the additive hint, a cross-gate 12bb hint (structural 13bb), and a
four-way multi-word opener that must inherit v0's HJ default. Cases with no v0
counterpart (side-pot fail-closed, BB-option validity, transport failures) are
labeled v1-only rejection/validation tests and are not counted as parity.
`test_v1_request_mapper` independently maps hand-built v1 requests and
compares `Ctx` fields and engine decisions for each representable fixture,
including hint-present, hint-absent fallback, and invalid-hint rejection.

