# Protobuf Engine Protocol

Status: Current

## Scope

`proto/bigshark/engine/v1/engine.proto` is the authoritative v1 engine
protocol definition. RFC 0002 Stage 7 ships an opt-in framed Protobuf host
mode, C++ request/response mappers with a strict hand-written semantic
validator, a framed TypeScript client, a River v1 mapper, and v0/v1 golden
differential parity. RFC 0002 Stage 8 adds the negotiated minor-1 RPC
integration (`ExpandedStrategy`, full resident blueprint distributions up to
32 actions, `SOLVER_MODE_BLUEPRINT`/`SOLVER_SOURCE_BLUEPRINT`, artifact
digest and guarantee metadata) behind an explicit capability handshake and
the offline `--resident-root` host flag. RFC 0008 stage 5 adds the
negotiated minor-2 guarantee ladder: every decision carries its typed level
(`SolverMetadata.guarantee_level`, field 11), callers may demand a minimum
level (`DecisionOptions.minimum_guarantee`, field 8), and an answer below
the floor is refused with error code 9 rather than served. The v0 NDJSON
host, the Node JSON client, and live River Club traffic remain the default;
the framed path and minors 1/2 are selected only explicitly (see [Framed
host](#framed-host), [Negotiated minor 1](#negotiated-minor-1), and
[Negotiated minor 2](#negotiated-minor-2-rfc-0008-stage-5)).

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
service-domain mapping, and the minor-1 resident blueprint distribution path
are implemented (Stages 7 and 8). Live dry-run/canary, v0 removal, the
resolving gadget and certification/eligibility gates, and whole-range
selection remain explicit external gates.

## Negotiated minor 1

Minor 1 is negotiated, never assumed. A client that wants it performs the
handshake before using any minor-1 feature:

1. query capabilities on minor 0 (the warmup call);
2. re-query capabilities with `protocol_minor = 1`;
3. use minor 1 only when the response echoes minor 1 and lists 1 in
   `supported_protocol_minors`; otherwise stay on minor 0.

The host accepts minors 0, 1, and 2: an envelope advertising minor 3 or
higher is answered `UNSUPPORTED_PROTOCOL` echoing minor 0. Minor 0 and minor
1 use the same request schema; only the selectable solver modes and the
decision response oneof differ. RFC 0008 stage 5 adds minor 2 with the typed
guarantee level; see [Negotiated minor 2](#negotiated-minor-2-rfc-0008-stage-5).

### Capability filtering

- A minor-0 capabilities query returns the frozen Stage-7 response byte for
  byte: `supported_protocol_minors = [0]`, build version `v1.0.0`, and exactly
  `SOLVER_MODE_AUTOMATIC` and `SOLVER_MODE_HEURISTIC`, regardless of which
  resident roots are loaded. The host never serves `expanded_strategy`, enum
  6/7, or the new metadata to a minor-0 client.
- A minor-1 capabilities query lists minors `[0, 1]`, build version `v1.1.0`,
  adds `SOLVER_MODE_BLUEPRINT` to `solver_modes` when at least one resident
  root was advertised, and adds `SOLVER_MODE_RESOLVING` (7) only when a live
  resolver plus an advertised terminal-only root is available. Minor 0 never
  emits enum 6/7.

### Selectable modes

| Mode | Minor 0 | Minor 1 |
| --- | --- | --- |
| `AUTOMATIC` (1) | heuristic only | tries a resident blueprint, then the heuristic on any miss; never resolves |
| `HEURISTIC` (2) | heuristic | heuristic (resident never consulted) |
| `BLUEPRINT` (6) | `UNSUPPORTED_FEATURE` | resident blueprint; any coverage miss is a non-retryable `UNSUPPORTED_FEATURE` |
| `RESOLVING` (7) | `UNSUPPORTED_FEATURE` | terminal-only whole-range resolve; see [Resolving](#resolving-rfc-0005-stage-9) |
| `RIVER_LP`/`RIVER_DCFR`/`MULTISTREET_CFR` (3/4/5) | `UNSUPPORTED_FEATURE` | `UNSUPPORTED_FEATURE` |

### ExpandedStrategy

A minor-1 blueprint hit returns `DecisionResponse.expanded_strategy` (field
3), never `Strategy`:

- `actions` carries the full resident row: 1..32 `ActionPolicy` entries,
  verbatim non-negative probabilities summing to one within 1e-12, a target
  total only on bet/raise, and `all_in` derived as the legal max equal to the
  hero's `stack + street_committed`. The minor-0 five-entry cap does not
  apply and a distribution is never truncated to fit it.
- every emitted action is a member of the request legal set by kind and
  exact target. A resident abstract action that is legal in the poker game
  but outside the client's `[min_target_total, max_target_total]` window is
  an `OffTreeAmount` coverage miss; the host never clamps the amount.
- `selected_action` is present only when `include_sampled_action` is set. It
  is chosen by one draw of a domain-separated SplitMix64 over the row (see
  [Sampling](#sampling)) and is independently validated against both the row
  and the request legal window by kind and exact target.
- `solver.source` is `SOLVER_SOURCE_BLUEPRINT` (6), `cache_hit` is true,
  `reason_code` is `blueprint`, `artifact_sha256` is the lowercase hex
  SHA-256 of the artifact bytes, and `guarantee` is `uncertified` for an
  ordinary forced BLUEPRINT lookup. A resolving-deadline baseline row (see
  below) is the only response that pairs source BLUEPRINT with `baseline`.
  `modeled_exact_bound` is emitted exclusively with source RESOLVING (7).

### Resolving (RFC 0005 Stage 9)

Forced `SOLVER_MODE_RESOLVING` on minor 1 runs the bounded terminal-only
resolver. AUTOMATIC never resolves. The host is stateless about eligibility:
the explicit mode is only the adapter's assertion that its per-hand monotone
prefix conditions held (fresh pinned artifact-root snapshot followed by a
matching blueprint execution); no eligibility bit crosses the RPC.

The solve is whole-range and hero-card independent; the actual hero combo
selects only the returned row afterward. Outcomes:

- **Certified** — the independent per-infoset best-response check passed
  (`BR_cand(I) <= b(I) + 1e-9*root_pot` for every positive-mass responder
  infoset). Response `expanded_strategy`, `solver.source =
  SOLVER_SOURCE_RESOLVING` (7), `guarantee = "modeled_exact_bound"`, full
  distribution and the same membership/sampler rules as a blueprint row,
  `artifact_sha256` the pinned digest.
- **Deadline with a validated baseline** — the solve/certification window
  expired (or the candidate failed the bounds) while a complete validated
  blueprint row is available. Response `expanded_strategy`, source
  `SOLVER_SOURCE_BLUEPRINT` (6), `guarantee = "baseline"`, the resident
  blueprint row.
- **Deadline with no baseline** — forced resolving returns a retryable
  `ERROR_CODE_DEADLINE_EXCEEDED`; the host never returns a fold or a
  heuristic.
- **Unsupported** — non-terminal-only node, no advertised root, off-tree
  history/runout/amount, digest mismatch, untrained/zero-reach combo, or an
  unadvertised resolver: a non-retryable `UNSUPPORTED_FEATURE`.

A facing-all-in fold/call node is admitted on the resolving path (the
opponent may be `ALL_IN` while the acting hero retains chips) but is still
rejected by the ordinary BLUEPRINT reconstruction gate. The request's
`solve_time_budget_ms` (1..120000) bounds the solve; a steady-clock deadline
starts at receipt and reserves at least 5 ms / 10% for return processing.
Minor 0 mode 7 is rejected before any resolver call, keeping its bytes
frozen.

A minor-1 heuristic result (the AUTOMATIC miss fallback, or explicit
HEURISTIC) is also returned as an `ExpandedStrategy`, but it stays
degenerate (one probability-1 row), keeps the heuristic's REAL source
(postflop heuristic, preflop chart, river LP/DCFR — never BLUEPRINT), and
carries no `artifact_sha256` and no `guarantee`.

### Coverage misses

Forced `BLUEPRINT` resolves to a non-retryable `UNSUPPORTED_FEATURE` error
(with a machine-readable diagnostic suffix) on every miss: the snapshot is
not postflop heads-up on the no-ante/equal-matched profile, no advertised
root matches (or the pinned digest mismatches), the structured history does
not replay exactly, the node/runout is off the trained tree, the row action
is outside the client window, the hero combo is untrained/board-blocked/zero
reach, or joint belief is empty. The host never returns a fold strategy on a
miss and never invents or clamps a root.

## Negotiated minor 2 (RFC 0008 stage 5)

Minor 2 is the negotiated home of the typed five-level guarantee ladder and
is opted into exactly like minor 1 (the probe cascade is minor 2, then
optionally minor 1, then 0; an old host answering an unsupported probe with
an error leaves the client safely below). It is not a new package version:
`bigshark.engine.v2` remains reserved for coherent full-state support, while
this change is additive enum plus two additive fields in the shape minor 1
established.

### Levels and vocabulary

The ascending ladder is `operational_fallback < approximate <
abstract_solved < exact_solved < certified_bound`. Today:

| Source | Level |
| --- | --- |
| preflop chart, postflop heuristic, river LP (capped live ranges, cap 36), bounded river DCFR, offline multistreet CFR, resident blueprint, resolve-deadline baseline | `approximate` |
| certified whole-range resolve | `certified_bound` |
| (none yet) | `abstract_solved`, `exact_solved` |

Promoting a source to abstract/exact is a later stage's measured job; no
stage-5 source earns those levels, and a request demanding them today is a
typed refusal rather than a downgraded answer.

The level is written to `SolverMetadata.guarantee_level` (field 11) on every
successful minor-2 response — heuristic answers included. The per-minor
vocabularies are disjoint and enforced by construction:

- minor 0 sets neither field;
- minor 1 sets `guarantee` (field 10) to exactly `modeled_exact_bound` /
  `uncertified` / `baseline` and never field 11;
- minor 2 sets `guarantee_level` (field 11) to one of the five tokens and
  never field 10.

`certified_bound` is the minor-2 spelling of minor 1's
`modeled_exact_bound`: the same certified resolver outcome and meaning under
the ladder name. Minor 1's `uncertified` and `baseline` both map to
`approximate`.

Every minor-2 decision returns the `expanded_strategy` oneof. A
consequence of truthful source declaration is that the four preflop chart
folds (`fold pre`, `fold vs open`, `fold vs 3bet`, `fold vs 4bet`) are
reported `SOLVER_SOURCE_PREFLOP_CHART` on minor 2, whereas minor 0/1's
frozen reason-text inference labels them postflop heuristic. The level is
identical (`approximate`) on both minors; only the source tag differs.

The wire source is declared at the policy routing branch, never inferred
from the reason text at minor 2. A future or unrecognized `SolverSource`
fails closed to `operational_fallback` at the boundary; the host decision
path derives its level from the declared source. RFC 0009 W3: a minor-2
AUTOMATIC blueprint miss now ends at the declared operational fallback
(check, else call, else fold, from the supplied legal set only), tagged
`operational_fallback` with an UNSPECIFIED source, so a host-produced
successful minor-2 response CAN carry `operational_fallback` — but only on
that AUTOMATIC-miss path. RFC 0009 W4d: on a minor-2 AUTOMATIC blueprint
miss the host now tries terminal-only resolving (when the spot is eligible
and a resolver root is advertised) before the operational fallback. A
certified resolve serves at `certified_bound` with the RESOLVING source; a
deadline baseline serves at `approximate` with the BLUEPRINT source. Any
resolver miss (not advertised, spot not eligible, deadline exceeded,
unsupported seat count) falls through to the operational fallback. An
explicit HEURISTIC request still runs the sourced chart+heuristic cascade
at `approximate`, and a blueprint hit is `approximate`; neither is ever
`operational_fallback`. The separate live operational-fallback surface is
the River adapter's local decision when the engine binary/transport is
unavailable.

### Request floor and error code 9

`DecisionOptions.minimum_guarantee` (field 8, optional enum) is the
caller-declared floor:

- absent — accept any level;
- present on minor 0/1 (including an explicit `UNSPECIFIED`, ordinal 0) —
  `UNSUPPORTED_FEATURE`;
- present as `UNSPECIFIED` or an out-of-range open-enum value at minor 2 —
  `INVALID_REQUEST`;
- present as one of the five real levels — the host serves the decision only
  when the achieved level meets the floor.

When a complete, serializable answer exists below the floor the host returns
`ERROR_CODE_GUARANTEE_BELOW_REQUEST` (9) with no strategy payload,
non-retryable for an unchanged state. The floor is compared only after
validation, mode lookup, and the answer's completeness checks, so coverage
misses, deadlines, and malformed rows keep their real error codes — a
bad-digest row with a high floor is `INTERNAL`, never code 9. The floor
never alters the served action.

The forced solver-mode matrix is unchanged at minor 2: `RIVER_LP`,
`RIVER_DCFR`, and `MULTISTREET_CFR` remain `UNSUPPORTED_FEATURE`; minor 2
widens nothing about mode selection.

### Capabilities

A minor-2 capabilities query echoes minor 2, lists `supported_protocol_minors
= [0, 1, 2]`, reports engine build version `bigshark-engine-v1.2.0`, and
otherwise has the same solver-mode set and feature bits as the minor-1
advertisement. The minor-0 and minor-1 capability bytes are unchanged.

### River adapter

The TypeScript client learns `0|1|2` negotiation behind
`BIGSHARK_ENGINE_PROTO_MINOR2=1` with the strict probe order 2 → (when minor
1 is also opted in) 1 → 0. The decoded field-11 token is exposed as
`ExecutableDecision.guaranteeLevel`; every local fallback (`safeFallback`
and the pre-legal `no-legal` fold) labels it `operational_fallback`, and the
runner journals it through `.runtime/results.log`. A code-9 refusal is a
typed `V1EngineError` that propagates — it is never replaced by a safe
fallback. The shipped runner never sets a floor itself; request floors are
owned by explicit callers via `EngineConfig.minimumGuaranteeLevel`.

### Resident provisioning

The host owns one immutable resident set, built before serving from
repeatable flags:

```text
bigshark-engine --serve-proto \
  --resident-root /path/to/policy.db=<64 lowercase hex sha256>
```

The SHA-256 is mandatory. Each root is probed, digest-verified, fully
validated, flattened, and advertised only when it fits the shared 256 MiB
resident budget; one bad root never disables another. Per-root results
(status, digest, information-set count, resident bytes, or failure detail)
are reported on standard error only, never on framed stdout. With no flags
the binary is the default host: no resident roots, BLUEPRINT unadvertised,
and the v0/JSON/minor-0 paths unchanged. `bigshark_resident` is linked
PRIVATE to `bigshark_v1_protocol`, so SQLite/OpenSSL and resident symbols do
not cross the public boundary into the host's v0, service, policy, decision,
client, or platform targets.

### Postflop state reconstruction

`v1_resident_mapper.cpp` converts the current `HandState` into a
`bs::poker::HeadsUpState` at the decision node. Player index is occupied
seat order (lower seat first), `root.button` is the button player's index,
and cards map `(Rank-1)*4 + (Suit-1)` to the poker id. The flop root is
`{ordered flop, stacks-behind-at-flop, equal matched contributions, pot,
big blind, button}`: postflop stacks are current stacks plus exactly the
chips paid postflop, and the flop pot is the current pot minus those
payments (or the first postflop event's `pot_before` when the platform
supplies it). Preflop events establish only the root pot and are never
replayed; flop/turn/river voluntary events replay through the exact poker
engine with absolute bet/raise target totals, inserting turn/river cards at
dealing boundaries. The reconstructor fail-closes (`OffTree` /
`RootNotSupported`) on non-postflop, multiway, antes/button-antes/enabled
straddles/rakes, non-blind forced contributions, side pots, a folded seat,
unresolved actors, card collisions, illegal transitions, unmatched
stacks/street commitments/pot/to_call, or an unparseable ledger.
`incremental_amount` is advisory (platforms report either the total payment
or the raise-only increment); the authoritative replay quantity is always
the exact target total. The River adapter fills these fields for every
postflop action under three exactness rules:

1. **Exact-integer text only.** The trailing token of the localized event
   text is trusted only when it is pure digits with no decimal point or
   K/M/B multiplier ("bet to 70", "<name> 980"). A rounded display
   ("bet to 1.2K" for an exact 1225 half-pot bet, "ALL IN 3.6K", "70.5") is
   never sent as an exact target.
2. **Current-street structured recovery.** On the decision street, when the
   text is rounded the exact value is recovered from the live structured
   snapshot (`seat.bet` current-street commitment, an exact integer) — but
   only for the actor's final chip action of the street; a per-(street,
   actor) ledger guard refuses recovery when an earlier same-street action
   by that actor is unaccounted, so a CALL increment is never mis-subtracted.
3. **Past streets are text-only.** Closed flop/turn actions with rounded
   display text cannot be recovered from the current snapshot and stay
   unset, which is the deterministic OffTree miss. Fold/check carry no
   amount.

No amount is ever guessed; unparseable/rounded past-street amounts are the
honest fail-safe miss (an error/heuristic decision, never a wrong node).
Corpus coverage (`sessions/2026-09-11.jsonl`, every postflop chip action in
room snapshots carrying events): 3318/3357 (98.84%) are exact integer
tokens and 39 are K-suffix rounded display (zero plain decimals); those 39
break down by `solver.playersInHand` as `{3:1, 4:3, 5:15, 6:20}` (all
multiway — none heads-up), and on actionable postflop decision snapshots
only 2 are current-street actions and both recover structurally. Preflop
structured CALL recovery seeds the street ledger with the posted blinds
(SB/BB forced contributions) so a blind-posting actor's rounded final call
returns only the new chips (SB limp to 100 with a 10 SB emits 90; a BB
call to 40 with a 20 BB emits 20), matching the C++ preflop street-paid
seed; postflop streets carry no blind seed and are unaffected. The
reconstructor also rejects any ALL_IN player at a postflop decision and
verifies equal matched preflop contributions when the full attributed
preflop ledger is present (an even pot alone is insufficient; 15+25=40
fails and 20+20=40 reconstructs).

### Sampling

The sampler is a shared, domain-separated SplitMix64 (`engine/include/bs/
prng.hpp`): the state starts at `seed XOR 0x425356312d73616d`, a constant
distinct from the trainer's stream. The lookup itself consumes no entropy —
the distribution is identical for every seed — and one draw selects the
sampled bucket: top 53 bits mapped to `[0,1)` then scaled onto the row's
actual recorded prefix sum, first prefix-CDF bucket strictly greater than
the draw, with a clamp to the final positive bucket. This makes every row
passing the 1e-12 probability-sum check deterministically sample-able
(including rows summing to 1 +/- 1e-13) while a zero-probability bucket can
never be selected; on exactly normalized distributions the pinned golden
vectors hold (seed 42 -> bucket 3, seed 0 -> bucket 6, seed 1 -> bucket 4).
`include_sampled_action: false` suppresses the field.

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

## C++ Process Client (RFC 0009 W4e)

The offline practice simulator's `engine` difficulty tier uses a C++ process
client (`bigshark_engine_client`, `engine/src/engine_client/`) that spawns a
`bigshark-engine --serve-proto` subprocess and speaks the framed protocol over
stdin/stdout pipes. It is a leaf-side library: it links only
`bigshark_protocol` (generated protobuf), `bigshark_poker`, and the
`bigshark_behavior` interface. It must not link `bigshark_v1_protocol` (which
would drag in the service, policy, solver, and resident layers); the
configure-time offline guard enforces this by construction.

### Frame codec

The client re-implements the server's canonical ULEB128 codec (mirroring
`engine/src/protocol/v1_frame_stream.cpp`, which it must not link):
`encode_frame` (varint + payload, max 1 MiB), `read_frame` (`poll`-based read
with deadline, EINTR retry, partial-read handling, max 5 prefix bytes,
overlong and zero-length rejection, single allocation), and `write_frame`
(full write with EINTR/partial-write handling). `SIGPIPE` is set to `SIG_IGN`
once at process start so a closed pipe returns `EPIPE` rather than killing the
client.

### Process lifecycle

`EngineProcess` spawns the engine with two `pipe()` calls (stdin
parent→child, stdout child→parent), `fork()` + `dup2()` + `execv()`. The
parent keeps the write end of stdin and the read end of stdout. `round_trip`
writes one envelope frame and reads one response frame with a configurable
timeout (default 30 000 ms). On timeout the client kills the process with
`SIGKILL` (the engine may be wedged in a solve); the next decision lazily
respawns it. `stop()` tries `SIGTERM` first (2 s wait), then `SIGKILL`.

### Handshake

The client sends `GetCapabilities` at protocol minor 2 first. On
`UNSUPPORTED_PROTOCOL` or any failure it retries at minor 0. Both fail →
`start()` returns false and every subsequent decision falls back. A minor-2
success confirms the resident blueprint → terminal-only resolver → labeled
operational fallback chain (RFC 0009 W3/W4d). A minor-0 success degrades to
the heuristic-only `AUTOMATIC` path. The client captures
`engine_build_version`, `solver_modes`, `maximum_solve_time_ms`, and
`maximum_request_bytes` from the capabilities response.

### Decision request builder

`build_decision_request` converts a live `GameState` + hero seat + hole cards
+ `HandLog` into a `pv::DecisionRequest`. The `HandLog` records only
`{seat, action}`; `pot_before`, `stack_after`, and `all_in` are reconstructed
by replaying the deterministic poker machine from the root state through the
logged actions, advancing board cards at dealing boundaries. A cheap
invariant check after replay (pot, street, per-seat stack vs. live state)
throws on mismatch — the policy catches it as a fallback.

The preflop aggressive verb convention is critical: the unified engine types
preflop aggression as `Bet`, but the server vocabulary is `raise`. The client
maps preflop `Bet`/`Raise` → `ACTION_TYPE_RAISE` in both `legal_actions` and
`action_history`, and maps an aggressive response back through
`legal.aggressive->type` (never blindly to `Raise`).

### Response mapper

`map_decision_response` extracts the action from a `DecisionResponse`:
- `error` oneof → fallback with reason `"engine error <code>: <message>"`.
- `strategy` (minor 0) or `expanded_strategy` (minor 1/2): if
  `selected_action` is present use it; otherwise sample from the distribution
  using the engine's documented sampler (SplitMix64 seeded
  `decision_seed XOR 0x425356312d73616d`, top 53 bits → `[0,1)`, first CDF
  bucket strictly greater than the draw, clamp to last positive bucket).
- Type mapping: `FOLD`/`CHECK`/`CALL` direct; `BET`/`RAISE` with
  `target_total` → `Action{legal.aggressive->type, target_total}`. Missing
  `target_total` on aggressive → fallback.
- Defense in depth: `legal.contains(action)` fails → fallback
  `"illegal engine action"`.
- Fallback (single definition): check if legal, else call if legal, else fold.

### EngineServedPolicy

`EngineServedPolicy` is a `BehaviorPolicy` adapter. `distribution()` is
`const`; transport and stats are `mutable` (single-threaded synchronous use
only). It increments the decision counter, ensures the engine process is
started (lazy respawn after crash), builds and sends the request, maps the
response, and records latency (min/max/total in microseconds) on success. On
any failure it increments the matching classified counter (timeout, engine
error, protocol error) and returns the fallback action. The stats surface is
printed at simulator shutdown: decisions, served, fallbacks (timeouts, engine
errors, protocol errors, restarts), and latency avg/min/max.

## C++ Protocol Boundary

The public boundary header `engine/include/bs/v1_protocol.hpp` exposes the
frame codec, the byte-level `handleEnvelope` entry points, and a
protobuf-free `V1HostServices` seam (`blueprintAdvertised` plus a
`blueprintHeroDecision` lookup returning poker-domain rows). It includes no
generated Protobuf and no resident types; the host composition root supplies
the resident-backed implementation and the default entry point uses a shared
no-resident service. The mappers, semantic validator, and reconstructor live
in `engine/src/protocol/v1_*` and are the only translation units (with the
host and the v1 tests) that include generated headers. The dependency chain
is host -> generated protobuf -> v1 mapper -> `bs::decide`; domain, solver,
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
  AUTOMATIC and HEURISTIC as selectable modes (BLUEPRINT/RESOLVING are
  rejected there even when a resident root is loaded); minor 1 additionally
  accepts BLUEPRINT, while RESOLVING remains rejected. The capabilities list
  advertises exactly the selectable modes per minor, while the
  exact-LP/DCFR feature bits still report which internal river backend the
  heuristic engine may select;

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
derived from the heuristic reason prefix on minors 0 and 1 (`gto-cfr` ->
river DCFR, `gto` -> exact river LP, preflop chart reasons -> preflop chart,
otherwise postflop heuristic); at minor 2 the source is the policy-declared
routing branch and the level is read from field 11 instead.

| Condition | ErrorCode | Retryable |
| --- | --- | --- |
| malformed envelope, missing/unspecified fields, poker-semantic violation | `INVALID_REQUEST` | no |
| `protocol_minor > 2` | `UNSUPPORTED_PROTOCOL` | no |
| non-NLHE, limit structure, tournament/ICM | `UNSUPPORTED_GAME` | no |
| fractional units, antes, rake, straddle, side pots, forced multistreet mode, forced resolving mode on minor 0 or without an advertised resolver | `UNSUPPORTED_FEATURE` | no |
| `minimum_guarantee` present on minor 0/1 | `UNSUPPORTED_FEATURE` | no |
| minor-1 forced BLUEPRINT coverage miss (unsupported hand state, no matching root, off-tree history/runout/amount, blocked/untrained/zero-reach combo) | `UNSUPPORTED_FEATURE` | no |
| minor-2 achieved level below the requested minimum (complete answer exists) | `GUARANTEE_BELOW_REQUEST` (9) | no |
| empty engine action | `NO_DECISION` | yes |
| solve budget exhausted | `DEADLINE_EXCEEDED` | yes |
| allocation failure | `RESOURCE_EXHAUSTED` | yes |
| non-member engine action, unmappable kind, unexpected exception | `INTERNAL` | yes |

Errors are never converted into fold-shaped strategies; a genuine strategic
fold is a `Strategy` containing `ACTION_TYPE_FOLD` at probability 1. The
minor-0 capabilities response advertises only `supported_protocol_minors =
[0]`, NLHE no-limit cash, the target-total-inclusive amount semantics, the
tag/lag/station-hunter profiles, actual exact-LP/DCFR support, experimental
multistreet status, unsupported side pots, rake, and ICM, and a 1 MiB /
120 s maximum. Minor-1 capabilities re-list `[0, 1]` and add BLUEPRINT only
with an advertised resident root (see [Negotiated minor
1](#negotiated-minor-1)).

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
client and behavior are unchanged. Minor 1 is an additional explicit opt-in:
the client is constructed with `negotiateMinor1: true` (the built-in River
client does this when `BIGSHARK_ENGINE_PROTO_MINOR1=1`) and exposes the
negotiated result as `negotiatedProtocolMinor` / `minor1Capable`. Minor 2
follows the same pattern with `negotiateMinor2: true` /
`BIGSHARK_ENGINE_PROTO_MINOR2=1`; when both flags are set the probe order is
2 → 1 → 0 and the client settles on the highest minor the host advertises.
A caller that does not opt in never sends above minor 0. The River adapter
forces BLUEPRINT only with `EngineConfig.protoBlueprint` against a minor-1+
client; minor-1 AUTOMATIC otherwise tries the resident and transparently
falls back to the heuristic (frozen RFC 0005 behavior), while minor-2
AUTOMATIC tries the resident, then terminal-only resolving on a blueprint
miss (RFC 0009 W4d), and on a resolver miss ends at the engine's declared
operational fallback (the RFC 0009 W3 demotion — never the heuristic). At
minor 2 the adapter additionally exposes
`ExecutableDecision.guaranteeLevel`, attaches `operational_fallback` to
every locally produced fallback, journals the level to
`.runtime/results.log`, and maps an explicit
`EngineConfig.minimumGuaranteeLevel` to field 8 — a code-9 refusal is a typed
process error, never a played fallback. The River mapper reads whichever
decision oneof is returned (`Strategy` or `ExpandedStrategy`), validating the
sampled action against the FULL reported list (up to 32 entries) before
execution, so an expanded distribution with a non-member sampled action is
an operational error routed to `safeFallback`, never a fold.

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

