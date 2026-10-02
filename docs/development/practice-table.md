# Offline Practice Table

Status: Current

The offline practice table lets a human play no-limit Texas hold'em hands
locally against deterministic bots. It is a training tool: there is no server,
no network call, no credential file, and no money. Every hand restarts every
seat at the configured buy-in, and the running result is printed in big
blinds.

## What the bots are

The bot roster is deliberately labelled by what it does:

| Difficulty | Behavior |
| --- | --- |
| `easy` | Uniform random over the declared finite action menu (fold/check/call, minimum raise, five declared pot fractions, and the cap). Loose and unpredictable, not strategic. |
| `medium` | The pinned preflop charts plus the postflop Monte-Carlo-equity heuristic — standard ABC poker. The engine records this policy as `Guarantee::Approximate`; it is **not** a solved equilibrium and is never labelled "GTO". |
| `engine` | Forwards every bot decision to a separate `bigshark-engine --serve-proto` subprocess over the v1 framed protobuf protocol. With resident roots configured the bot plays the resident blueprint → terminal-only resolver pipeline (RFC 0009 W4d); without roots it receives the engine's declared operational fallback (check, else call, else fold — never the heuristic, per the W3 demotion). On any transport or engine error the adapter falls back to check/call/fold locally, classified and counted in the shutdown report. The label is descriptive; it is **not** "GTO". Heads-up only in this stage. |

The `easy` and `medium` rosters are self-contained offline policies. The `engine`
tier is a process boundary, not a new strategy: it reuses the same resident
pipeline the live runner uses, so its strength is exactly the strength of the
loaded artifact library. A trained equilibrium policy is not bundled offline:
the stage-6 coarse candidate artifacts are measurement-only and have not been
promoted (their measured confidence intervals overlap the baseline; see the
RFC 0008 stage-6 plan notes).

## Running it

```bash
cmake --build --preset release --target bigshark-practice
./build/release/engine/bigshark-practice \
  --seats 6 --difficulty medium --stack-bb 100 --hands 0 --seed 0
```

- `--seats N`: table size including the human, `2` through `10`. Two seats use
  the heads-up blind layout (button posts the small blind); three seats and up
  post the small and big blinds clockwise of the button.
- `--difficulty easy|medium|engine`: selects the bot roster above.
- `--stack-bb N`: starting stack in big blinds for every seat, every hand
  (default 100).
- `--hands N`: stop after N hands; `0` (the default) plays until `q`.
- `--seed S`: SplitMix64 deck seed. A fixed nonzero seed reproduces the same
  deal and bot-action sequence; `0` (the default) selects a fixed default
  seed.

### Engine tier flags

`--difficulty engine` is heads-up only (`--seats 2`); the resident pipeline's
gates are heads-up-first. It spawns a `bigshark-engine --serve-proto`
subprocess and forwards every bot decision to it.

- `--engine-path PATH`: path to the engine binary (default
  `bin/bigshark-engine`).
- `--resident-root PATH=SHA256`: repeatable; an immutable resident artifact
  root passed through to the engine verbatim. The SHA-256 is mandatory. With
  no roots the engine serves its declared operational fallback (check, else
  call, else fold — never the heuristic).
- `--solve-budget-ms N`: per-decision solve budget in milliseconds (default
  1000). The practice loop is synchronous, so this bounds how long the human
  waits for a bot action.
- `--engine-timeout-ms N`: round-trip timeout in milliseconds (default 30000).
  On timeout the client kills the engine process and falls back; the next
  decision lazily respawns it.

At startup the CLI prints the negotiated protocol minor, engine build version,
whether the resident pipeline is advertised, and the solve budget — or a
clear warning that the engine is unreachable and every bot decision will fall
back. At shutdown it prints a latency and fallback report:

```text
Engine tier: 12 decisions, 10 served, 2 fallbacks (0 timeouts, 1 engine errors,
1 protocol errors, 1 restarts); latency avg/min/max = 845/720/1100 us over 10
served decisions.
```

The human is always logical seat 0; the button rotates one seat clockwise each
hand, so the human plays every position over an orbit.

At a decision the menu shows the seat layout, stacks and street commitments,
the pot, the board, the exact legal set, and the declared sizing choices with
numbers. Inputs are:

- a menu number, or
- `f` / `x` / `c` for fold / check / call,
- `b TOTAL` or `r TOTAL` for an aggressive action at an exact street target
  total (the verb the rules require is selected automatically),
- `q` to leave.

Illegal input is rejected and re-prompted; nothing is ever clamped or
auto-submitted.

## Engine core and boundary

The reusable core is `bs::stage6::PracticeTable`
(`engine/include/bs/stage6/practice_table.hpp`,
`engine/src/stage6/practice_table.cpp`): it shuffles a real 52-card deck,
posts blinds, drives the unified `GameState` street by street, samples each bot
through its `BehaviorPolicy`, settles folds and showdowns with the exact L1
rules, asserts zero-sum utility, rotates the button, and keeps the cumulative
P/L. The human arrives through a `HumanAgent` callback and a
`PracticeObserver` receives start, decision, board, and settlement events; the
interactive CLI (`engine/benchmarks/practice_simulator.cpp`) is a thin view,
which keeps the whole loop unit-testable as `stage6_practice` without a
terminal.

Information exposure is enforced by the core, not the view: before the
showdown the human receives only their own hole cards and the board prefix
that has actually been dealt; bot hole cards and unrevealed runout cards are
not reachable through the API. A showdown reveals only the cards of players who
reached it live; a folded hand reveals nobody's cards.

The core is built as the `bigshark_practice` static library linking only
`bigshark_stage6_eval` (which provides the bot policies) and poker. The
configure-time offline guard lists `bigshark_practice` among the forbidden
prefixes, so it cannot be linked into the decision service, either wire
protocol, or the engine host, and the interactive binary is a leaf that never
links those targets.

### Engine client boundary

The `engine` difficulty tier adds the `bigshark_engine_client` static library
(`engine/include/bs/engine_client/engine_client.hpp`,
`engine/src/engine_client/`). It implements the v1 framed protobuf client:
ULEB128 frame codec, process spawn/kill, minor-2 handshake (with minor-0
fallback), `GameState` → `DecisionRequest` builder, and `DecisionResponse` →
`poker::Action` mapper. It links only `bigshark_protocol` (generated
protobuf), `bigshark_poker`, and the `bigshark_behavior` interface — it must
not link `bigshark_v1_protocol` (which would drag in the service, policy,
solver, and resident layers). The guard enforces this by construction:
`bigshark_engine_client` is a forbidden prefix for live targets, and two
directional closure checks verify that `bigshark_practice` does not link it
and that it does not link the service, protocol, policy, solver, or resident
libraries.

The `EngineServedPolicy` is a `BehaviorPolicy` adapter: `distribution()`
spawns the engine lazily (or respawns after a crash), builds a decision
request from the live `GameState` and `HandLog`, sends it over the pipe, and
maps the response. On any failure — timeout, end-of-stream, malformed frame,
engine error, or illegal returned action — it falls back to check/call/fold
and records the failure in a classified stats counter. The single-element
distribution makes `PracticeTable::sample_bot` a deterministic no-op.

The practice core itself knows nothing about the engine client:
`PracticeTable::set_bot(seat, unique_ptr<BehaviorPolicy>)` is the seam the CLI
uses to inject the `EngineServedPolicy`. The core's `make_practice_bot`
returns `nullptr` for the `Engine` tier, and `play_hand` throws if an Engine
bot seat was not filled via `set_bot`.

## Adapter verb convention

The offline `GameState -> Ctx` adapter advertises the aggressive legal action
with the live server vocabulary, pinned from session journals: postflop,
opening the betting is `bet` and wagering over an existing bet is `raise`;
preflop, the big blind's option after a limp is still `raise` even though the
unified rules type that exact action `Bet` (nothing owed).
`map_deployed_decision` accepts exactly the advertised token — preflop only
`raise` (mapped to the state's Bet type at the BB option), postflop an exact
bet/Bet or raise/Raise match — and throws on any other token/state pairing.
