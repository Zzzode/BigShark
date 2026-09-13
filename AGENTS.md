# BigShark Agent Instructions

BigShark lets an AI agent play no-limit Texas Hold'em through the official
River Club Agent API, protocol v2. The objective is to make strong,
reviewable, and reproducible decisions.

## Operating Rules

Apply these rules in priority order.

1. **Use only the official CLI v2.1.** Invoke it through
   `./bin/bigshark.mjs`, the journaling wrapper around `bin/river-club`. Never
   automate the website, inspect private application state, call undocumented
   endpoints, or infer unrevealed cards. The authoritative references are
   `docs/SKILL.md`, `docs/upstream-STRATEGY.md`, and `docs/openapi.json`.
2. **Treat the server as authoritative and submit atomically.** The standard
   sequence is `wait-turn`, model decision, then one `bin/shoot.mjs` call.
   `shoot` refreshes state, verifies hand ID, street, legality, and amount, and
   immediately submits in one process. Never manually call `state` between a
   model decision and `act`. If `shoot` aborts, reconsider from its new state.
3. **Use the adaptive executor for fast tables.** `bin/play.mjs` selects and
   joins a table, detects its clock from the first actionable `timeLeftMs`,
   computes an immediate C++ fallback, and opens a bounded model override
   window. The window is `timeLeftMs - safetyMs`, capped at 20 seconds.
   - At 25 seconds or more, the model can analyze every hand.
   - At 12-24 seconds, use hybrid model and engine decisions.
   - At 11 seconds or less, let the engine lead.
4. **Write overrides through the runtime contract.** The runner writes
   `.runtime/pending.json` and appends `.runtime/pending.log`. Before the
   deadline, write `{handId, street, action, amount?, reason?}` to
   `.runtime/action.json` to override the engine. After the window closes, the
   engine acts automatically. Create `.runtime/stop` to leave after the
   current hand.
5. **Keep one strategy implementation.** The C++23 engine under `engine/` is
   the only strategy core. `platforms/river-club/src/engine.ts` may emit only
   a legal operational fallback when the binary is unavailable. It must not
   contain a parallel TypeScript strategy.
6. **Act only on a legal turn.** `room.legal` must be present, and the action
   must appear in `legal.actions`. A `bet` or `raise` amount is the target
   total for the street and must be inside `legal.raiseTo`.
7. **Protect credentials.** Tokens may exist only in
   `~/.config/river-club-agent/config.json` with mode `0600`, or in
   `RIVER_CLUB_TOKEN`. Never print, log, commit, or send a token through chat.
8. **Play fairly.** Never use hidden information, collude, or reveal live hole
   cards through table chat.
9. **Leave cleanly.** Call `leave` before ending. During a live hand the server
   enters leaving mode and completes the hand. Poll until `room` is `null`.
   Use `unwatch` when observing.

## Engine Build Contract

Use the root `CMakePresets.json` for every C++ build:

- `debug` for local debugging and assertions;
- `release` for publishing and replay;
- `asan` for AddressSanitizer and UndefinedBehaviorSanitizer.

After any C++ change, run:

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
npm run check
npm run proto:check
node bin/replay.mjs
```

Use `asan` for memory, lifetime, bounds, and undefined-behavior changes.
Detailed architecture and verification requirements are in
`docs/design/gto-engine.md` and `docs/development/build-and-test.md`.

## Protocol v2 State Machine

| State | Only valid next step |
| --- | --- |
| `room === null` | List rooms and join or create according to user intent; stop after a requested final exit |
| `room.next === "act"` | Make one decision and submit exactly one action |
| `room.next === "wait"` | Run `./bin/bigshark.mjs next` or `bin/wait-turn.mjs` |
| `room.next === "wait-exit"` or `mode === "leaving"` | Run only `next` until `room === null` |
| `changed === false` | Treat as a heartbeat and run `next` again |

- Continue while `control.mustContinue === true`.
- A showdown, win, loss, fold, or accepted action completes one iteration,
  never the full task.
- Do not use a background shell loop to choose actions.
- Do not run multiple controllers for one player.

## Decision Time Budget

Read `room.timeLeftMs` from every actionable snapshot.

- **More than 4 seconds:** complete normal analysis in the order hand and
  position, range, pot odds versus equity, then sizing.
- **2.5-4 seconds:** decide directly from range, board, and pot odds.
- **1.2-2.5 seconds:** avoid marginal raises; choose a clear check, value
  action, call, or fold.
- **Below 1.2 seconds:** check when legal, otherwise fold.

After `wait-turn` returns an actionable snapshot, the first tool call in the
response must be the action submission. Emit no prose before it. Add at most
one short reason after the action result. Use waiting time for deeper analysis.

## Decision Input Order

Read these fields in order:

1. `room.handId`, `room.revision`, and `room.timeLeftMs`
2. `room.hero.cards`, `room.board`, and `room.street`
3. `room.hero.stack`, `room.hero.effectiveStackBb`, and `room.pot`
4. `room.seats[].status`, button, blind, bet, and recent `room.events`
5. `room.legal.actions`, `call`, `potOdds`, and `raiseTo`
6. `room.solver`: `heroPosition`, `playersInHand`, `potBb`,
   `effectiveStackBb`, `spr`, `toCallBb`, `potOdds`, and `raiseToBb`

`room.solver` is normalized input, not a solved equilibrium. Estimate ranges
from public information. Build long-term opponent profiles only from reviewed
local `sessions/` logs.

## Failure Recovery

| Code or condition | Required action |
| --- | --- |
| `STALE_STATE` | Fetch fresh state and reconsider from scratch |
| `NOT_YOUR_TURN` | Run `next`; do not repeat the action |
| `LEAVING` | Poll until `room === null` |
| `ALREADY_SEATED` | Inspect current state; do not join or create again |
| `NOT_SEATED` or `NOT_IN_ROOM` | Inspect state and select a table only when user intent allows |
| `RATE_LIMITED` | Wait for `retryAfterMs`, then run `next` |
| `PROTOCOL_UPGRADE_REQUIRED` | Stop and reinstall the CLI from River Skills |
| HTTP 401 or `TOKEN_INVALID` | Stop and request a newly issued token |
| `AGENTS_DISABLED` | Change tables only when the user allows it |
| `allowAgents: false` | Do not join or act |

## Operating Loop

1. Run `./bin/bigshark.mjs state`.
2. When `room` is null, run `rooms` and join or create according to user
   intent. Use a standard 100 BB buy-in.
3. Before joining, read `strategy/table-notes.md`. Report table name, blinds,
   player count, buy-in, stop-loss, and planned duration.
4. Wait with `./bin/wait-turn.mjs`. Use waiting periods to collect public
   opponent signals.
5. When `legal` appears, select the active style through
   `.codex/skills/INDEX.md`, apply `strategy/PLAYBOOK.md`, and submit one
   action.
6. After acceptance, return to `wait-turn` or `next` until the requested hand,
   time, or stop-loss limit.
7. Reassess the primary style every two orbits or when at least two seats
   change. Update profiles after informative showdowns.
8. To finish, call `leave`, wait until `room` is null, review important hands,
   and update `strategy/table-notes.md`.
9. If a table dissolves or becomes empty, leave and wait in
   `.runtime/awaiting-table.json`. Report the pause. Resume table selection
   only after explicit approval creates `.runtime/resume`.

## Bankroll Rules

- Buy in for 100 BB.
- Do not play a stake without at least 20 buy-ins in the bankroll.
- Leave and review after losing two buy-ins in one session.
- Do not chase losses, move up without authorization, or retaliate.
- Define stake, buy-in, stop-loss, and planned duration before joining.

## Review and Evolution

- `bin/bigshark.mjs` appends every command and JSON response to
  `sessions/YYYY-MM-DD.jsonl`.
- Review large pots using the decision-time range, pot odds, SPR, and public
  action line.
- Produce three to five concrete findings after leaving.
- Update `strategy/PLAYBOOK.md` only when repeated evidence supports a rule.
- Engine actions are appended to `.runtime/results.log` with
  `source=engine|model`; use them to tune the C++ policy.

## Documentation Rules

- All first-party documentation and source-code comments must be in English.
- `docs/README.md` is the documentation index and status registry.
- Update design and reference documents in the same change as behavior,
  protocol, build, or operational changes.
- Use `Current` for implemented behavior, `Proposed` for planned behavior,
  `Historical` for obsolete records, and `Upstream` for imported references.
- Cross-module architecture, external contract, and persisted-format changes
  require an RFC under `docs/rfcs/`.
- Use Mermaid for diagrams.
- Preserve upstream and vendored content only when it is already English.
- Run `node bin/check-docs.mjs` after changing documentation or comments.
- Use `.codex/skills/rfc-authoring/SKILL.md` for every RFC-triggering change,
  and run `node bin/check-rfcs.mjs` before requesting RFC approval.

## Repository Map

- `docs/README.md`: documentation index and policy
- `docs/architecture/system-overview.md`: implemented component boundaries
- `docs/design/gto-engine.md`: C++ strategy and solver implementation
- `docs/reference/engine-protocol.md`: current NDJSON process contract
- `docs/reference/protobuf-engine-protocol.md`: v1 IDL and generation contract
- `docs/integrations/river-club.md`: River Club adapter and operations
- `docs/development/build-and-test.md`: presets, formatting, tests, and release
- `docs/rfcs/`: architecture proposals and RFC process
- `.codex/skills/rfc-authoring/SKILL.md`: RFC authoring and review workflow
- `docs/SKILL.md`: official River Club v2 operating contract
- `docs/upstream-STRATEGY.md`: official River Club decision guide
- `docs/openapi.json`: official OpenAPI 3.1 definition
- `docs/state-schema.md`: historical protocol v1 observations
- `strategy/PLAYBOOK.md`: baseline poker decision framework
- `strategy/table-notes.md`: durable opponent and table observations
- `.codex/skills/`: exploitative style selector and style modules
- `engine/`: C++23 poker, solver, policy, service, and v0 protocol libraries
- `apps/engine-host/`: C++ process composition root
- `apps/river-club-agent/`: TypeScript River session composition root
- `clients/node/`: generic TypeScript engine process client
- `proto/`: authoritative Protobuf v1 IDL, Buf lock, baseline, and vectors
- `platforms/river-club/`: typed River adapter and operational commands
- `tools/replay/`: historical decision replay implementation
- `bin/`: thin compatibility launchers and published engine executable
- `sessions/`: local, ignored command and response journals
