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
| `engine` | A local `bigshark-engine --serve-proto` process decides every bot action through the v2 framed Protobuf protocol. The engine loads resident blueprints and flop libraries when configured; without them it serves the labeled operational fallback. A descriptive label, never "GTO". |

The `easy` and `medium` rosters are self-contained offline policies. A trained
equilibrium policy is not bundled offline: the stage-6 coarse candidate
artifacts are measurement-only and have not been promoted (their measured
confidence intervals overlap the baseline; see the RFC 0008 stage-6 plan
notes).

## Running it

The interactive CLI is `bigshark-practice` (built from
`engine/benchmarks/practice_simulator.cpp`):

```bash
# Six-seat table, engine-served bots, 100 BB stacks.
./build/release/engine/bigshark-practice --seats 6 --difficulty engine

# Heads-up against the Medium baseline bot.
./build/release/engine/bigshark-practice --seats 2 --difficulty medium

# With a resident blueprint and flop library.
./build/release/engine/bigshark-practice --seats 6 --difficulty engine \
  --resident-root artifacts/blueprint=abc123... \
  --flop-library artifacts/flop-library-6way
```

At your turn pick an action from the numbered menu (or type `f`/`x`/`c`,
`b TARGET`, `r TARGET`); `q` ends the session.

The CLI accepts these flags:

| Flag | Default | Description |
| --- | --- | --- |
| `--seats N` | `6` | Table size including the human, `2` through `10`. |
| `--difficulty easy\|medium\|engine` | `medium` | Bot roster. |
| `--stack-bb N` | `100` | Starting stack in big blinds for every seat. |
| `--hands N` | `0` (until quit) | Stop after N hands. |
| `--seed S` | `0` (fixed default) | SplitMix64 deck seed. |
| `--engine-path PATH` | `bin/bigshark-engine` | Path to the engine binary (engine tier). |
| `--resident-root PATH=SHA256` | none | Repeatable resident root spec (engine tier). |
| `--flop-library DIR` | none | Flop library directory (engine tier). |
| `--solve-budget-ms N` | `1000` | Per-decision solve budget (engine tier). |
| `--engine-timeout-ms N` | `30000` | Round-trip timeout (engine tier). |
| `--lazy` | off | Enable lazy flop class loading (engine tier). |

## Engine core and boundary

The reusable core is `bs::stage6::PracticeTable`
(`engine/include/bs/stage6/practice_table.hpp`,
`engine/src/stage6/practice_table.cpp`): it shuffles a real 52-card deck,
posts blinds, drives the unified `GameState` street by street, samples each bot
through its `BehaviorPolicy`, settles folds and showdowns with the exact L1
rules, asserts zero-sum utility, rotates the button, and keeps the cumulative
P/L. The human arrives through a `HumanAgent` callback and a
`PracticeObserver` receives start, decision, board, and settlement events.
The `stage6_practice` native test exercises the core without a terminal.

Information exposure is enforced by the core, not the view: before the
showdown the human receives only their own hole cards and the board prefix
that has actually been dealt; bot hole cards and unrevealed runout cards are
not reachable through the API. A showdown reveals only the cards of players who
reached it live; a folded hand reveals nobody's cards.

The core is built as the `bigshark_practice` static library linking only
`bigshark_stage6_eval` (which provides the bot policies) and poker. The
engine tier is a separate `bigshark_practice_engine` static library that
links `bigshark_practice` (PUBLIC) plus `bigshark_engine_protocol` and
`bigshark_protocol` (PRIVATE), so the offline core never links the protocol
or service. The configure-time offline guard lists `bigshark_practice` among
the forbidden prefixes, so it cannot be linked into the decision service,
either wire protocol, or the engine host, and the interactive binary is a
leaf that never links those targets.

### Engine tier

The `Engine` difficulty tier spawns one `bigshark-engine --serve-proto` child
process and injects an `EngineServedPolicy` for every bot seat. The policy
converts the current `GameState` and `HandLog` into a v2 `DecisionRequest`,
sends it through the ULEB128-framed protocol, and converts the v2
`DecisionResponse` into a `PolicyAction` distribution. One `EngineProcess`
instance is shared across all bot seats so every decision comes from the same
loaded artifact library and resolver state.

The engine tier is implemented in:

- `engine/include/bs/practice/engine_process.hpp` — process lifecycle,
  capabilities handshake, request/response correlation.
- `engine/include/bs/practice/engine_served_policy.hpp` — `BehaviorPolicy`
  implementation that delegates to the engine host.
- `engine/src/practice/engine_process.cpp` — fork/exec, pipe I/O, ULEB128
  framing, poll-based timeout.
- `engine/src/practice/engine_served_policy.cpp` — `GameState` → v2
  `DecisionRequest` conversion (G/F ledger, pot layers, transition path with
  board cards and call amounts, forced contributions, legal actions,
  provenance).

The first decision of each hand (preflop, no prior actions) has an empty
transition path. The engine's validator rejects `FULL` coverage with an empty
path, so the policy uses `MID_HAND_ATTACH` coverage and falls back to
check/call/fold for that one decision. Subsequent decisions use `FULL`
coverage with the complete transition path.

The `stage6_practice` native test covers the engine tier with a real engine
binary: `test_engine_process_handshake` verifies start/stop and the
capabilities handshake, and `test_six_seat_engine_tier` runs five six-seat
hands with five engine-served bot seats and verifies zero-sum utility and
that at least one decision was served by the engine.

## Adapter verb convention

The offline `GameState -> Ctx` adapter advertises the aggressive legal action
with the live server vocabulary, pinned from session journals: postflop,
opening the betting is `bet` and wagering over an existing bet is `raise`;
preflop, the big blind's option after a limp is still `raise` even though the
unified rules type that exact action `Bet` (nothing owed).
`map_deployed_decision` accepts exactly the advertised token — preflop only
`raise` (mapped to the state's Bet type at the BB option), postflop an exact
bet/Bet or raise/Raise match — and throws on any other token/state pairing.
