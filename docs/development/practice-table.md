# Offline Practice Table

Status: Current

The offline practice table lets a human play no-limit Texas hold'em hands
locally against engine-served opponents. It is a GTO training tool: there is
no server on the network, no credential file, and no money. Every hand
restarts every seat at the configured buy-in, and the running result is
printed in big blinds.

## The opponent tier

There is exactly one opponent tier: every bot action is decided by a local
`bigshark-engine --serve-proto` process through the framed Protobuf
protocol, using whatever resident blueprints and flop libraries are
configured. The table itself constructs NO opponent strategy; it creates
all bot seats empty, and the CLI injects one engine-served policy per
non-human seat before the first hand.

There is no random or heuristic ("easy"/"medium") opponent. Those legacy
tiers were removed deliberately: a practice surface that is not the
equilibrium tier has no place in a GTO trainer. Two consequences:

- If the engine process cannot be started or reached, the CLI aborts with
  an error instead of falling back to a weaker opponent. Start the engine
  (or fix `--engine-path`) and retry.
- Table shapes for which no equilibrium artifact exists are not playable
  against a fake opponent; the engine serves its labeled fallback
  receipts on shapes outside the configured libraries.

## Running it

The interactive CLI is `bigshark-practice` (built from
`engine/benchmarks/practice_simulator.cpp`):

```bash
# Six-seat table against engine-served opponents, 100 BB stacks.
./build/release/engine/bigshark-practice --seats 6

# Heads-up against the engine tier.
./build/release/engine/bigshark-practice --seats 2

# With a resident blueprint and flop library.
./build/release/engine/bigshark-practice --seats 6 \
  --resident-root artifacts/blueprint=abc123... \
  --flop-library artifacts/flop-library-6way-fullrange
```

At your turn pick an action from the numbered menu (or type `f`/`x`/`c`,
`b TARGET`, `r TARGET`); `q` ends the session.

The CLI accepts these flags:

| Flag | Default | Description |
| --- | --- | --- |
| `--seats N` | `6` | Table size including the human, `2` through `10`. |
| `--stack-bb N` | `100` | Starting stack in big blinds for every seat. |
| `--hands N` | `0` (until quit) | Stop after N hands. |
| `--seed S` | `0` (fixed default) | SplitMix64 deck seed. |
| `--engine-path PATH` | `bin/bigshark-engine` | Path to the engine binary. |
| `--resident-root PATH=SHA256` | none | Repeatable resident root spec. |
| `--flop-library DIR` | none | Flop library directory. |
| `--solve-budget-ms N` | `10000` | Per-decision solve budget (RFC 0041 Amendment 1 10-second p95 envelope). |
| `--engine-timeout-ms N` | `30000` | Round-trip timeout. |
| `--lazy` | off | Enable lazy flop class loading. |

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

### The phase-dispatch seam (RFC 0029)

The street-by-street dispatch is a free function, not a private loop:
`run_practice_hand(GameState, const PracticeHand&, const HumanAgent&,
bs::SplitMix64&, PracticeObserver*)` returns a `PracticeHandResult` holding the
per-seat utility, the folded flag, the revealed board prefix, and the settled
terminal state. `play_hand` is a thin wrapper that deals the cards, calls it,
and updates session P/L and the button.

The split exists because `play_hand` builds its own `GameState` from a
file-local `make_def`, so nothing outside the class could ever hand the loop a
state it did not deal itself. A test that needs a `Phase::Frontier` leaf — which
no dealt hand produces, since `PracticeConfig` has no terminal-depth field and
`make_def` never sets `terminal` — can now construct one and drive the dispatch
directly. `PracticeHand` carries the hole cards, the five runout cards, the
human seat, the hand id, one bot policy per seat, and the table the human agent
renders from.

A frontier leaf throws `std::runtime_error`: the table has no evaluator to
consult and no rules settlement applies. The `Frontier` arm of the dispatch
carries no `default:` label, and `-Werror=switch` is enabled on
`bigshark_practice`, so a future `poker::Phase` enumerator fails the build
rather than spinning through a loop with no arm for it.

The core is built as the `bigshark_practice` static library linking only
`bigshark_behavior` (the policy interface) and poker; it contains NO bot
strategy. The engine client is a separate `bigshark_practice_engine` static
library that links `bigshark_practice` (PUBLIC) plus `bigshark_engine_protocol`
and `bigshark_protocol` (PRIVATE), so the offline core never links the
protocol or service. The configure-time offline guard lists
`bigshark_practice` among the forbidden prefixes, so it cannot be linked
into the decision service, either wire protocol, or the engine host, and
the interactive binary is a leaf that never links those targets.

### Engine-served opponents

The CLI spawns one `bigshark-engine --serve-proto` child process and injects an `EngineServedPolicy` for every bot seat. The policy
converts the current `GameState` and `HandLog` into an engine `DecisionRequest`,
sends it through the ULEB128-framed protocol, and converts the engine
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
- `engine/src/practice/engine_served_policy.cpp` - `GameState` -> engine
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
