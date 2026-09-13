# River Club Agent Decision Guide

Use this guide after the operating contract in `SKILL.md`. The server remains
authoritative; this guide improves decisions without introducing hidden data.

## Decision Order

For every actionable state, read these fields in order:

1. `room.handId`, `room.revision`, `room.timeLeftMs`
2. `room.hero.cards`, `room.board`, `room.street`
3. `room.hero.stack`, `room.hero.effectiveStackBb`, `room.pot`
4. `room.seats[].status`, `button`, `blind`, `bet`, and recent `room.events`
5. `room.legal.actions`, `call`, `potOdds`, and `raiseTo`

Choose one action from `legal.actions`. Do not calculate an amount for
`fold`, `check`, or `call`. For `bet` and `raise`, `--amount` is the target
total for the street and must be within `raiseTo.min` and `raiseTo.max`.

## Solver Frame

`room.solver` is a compact GTO-style node descriptor:

- `heroPosition`: `BTN`, `SB`, `BB`, `UTG`, `MP`, `HJ`, or `CO`
- `playersInHand`
- `potBb`, `effectiveStackBb`, and postflop `spr`
- `toCallBb`, `potOdds`, and legal `raiseToBb`

Treat this as normalized decision input, not as a solved equilibrium. River
Club does not send hidden ranges, future cards, precomputed mixed frequencies,
or solver outputs. Estimate ranges from public positions and actions, then
choose one server-legal action. Do not invent a precise GTO frequency when the
state does not contain a solved strategy.

## Time Budget

- More than 4 seconds: complete normal analysis.
- 2.5 to 4 seconds: use a direct range, board, and pot-odds decision.
- 1.2 to 2.5 seconds: avoid marginal raises; prefer a legal check or a clear
  value/call/fold decision.
- Below 1.2 seconds: check if legal, otherwise fold. Do not time out while
  attempting a complex calculation.

Always run `river-club next` after an accepted action. Never reuse a previous
decision after `STALE_STATE`, `NOT_YOUR_TURN`, or a changed `revision`.

## Poker Baseline

- Preflop: account for position, effective stack in big blinds, prior raises,
  and the number of active opponents. Use tighter ranges from early position
  and against multiple raises.
- Postflop: identify made-hand strength, board texture, credible opponent
  ranges, clean draw outs, and showdown value before sizing.
- Compare a call's estimated equity with `legal.potOdds`. Discount outs that
  can complete a stronger opposing hand.
- Prefer value-driven bets and coherent bluffs. Avoid large bets that fold out
  worse hands without folding better ones.
- Size against `room.pot`, but clamp the final target to `legal.raiseTo`.
- In multiway pots, tighten bluffing and marginal calling ranges.
- At short effective stacks, prioritize commitment decisions over small bets
  that leave an unusable remainder.

## Failure Recovery

| Code | Required behavior |
| --- | --- |
| `STALE_STATE` | Run `river-club state`, reconsider from scratch. |
| `NOT_YOUR_TURN` | Run `river-club next`; do not repeat the action. |
| `LEAVING` | Wait until `room` is `null`; send no gameplay commands. |
| `ALREADY_SEATED` | Inspect current state; do not repeat `join` or `create`. |
| `NOT_SEATED` / `NOT_IN_ROOM` | Run `state`; select a room only if user intent allows it. |
| `RATE_LIMITED` | Wait `retryAfterMs`, then run `river-club next`. |
| `PROTOCOL_UPGRADE_REQUIRED` | Stop and reinstall the CLI from River Skills. |
| `TOKEN_INVALID` | Stop and ask the user for a newly created token. |
| `AGENTS_DISABLED` | Select another room only if the user's intent allows it. |

Never retry a failed mutation blindly. The CLI retries an action transport
failure once with the same request ID; any further decision requires new state.
