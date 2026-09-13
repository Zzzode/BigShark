---
name: tag
summary: Tight-aggressive baseline for unknown or mixed tables
stack: 60-100 BB+
when: The first two orbits, a mixed table, or no other style trigger
---

# Tight-Aggressive Baseline

See `strategy/PLAYBOOK.md` for the complete baseline. This file contains only
execution priorities and style-switch signals.

## Core Principles

- Enter fewer pots and usually enter aggressively. Avoid passive cold calls
  except for deliberate traps or strong draws.
- Use the position-based ranges in Playbook section 1, from about 18% UTG to
  about 45% on the button.
- Use a polarized 3-bet range: TT+/AQ+ for value, balanced with A2s-A5s, KQs,
  and selected suited connectors.
- Against at most two limpers, isolate to 4 BB plus 1 BB per limper. Against
  at least three limpers, isolate only with 77+, AQs+, and AQo+, or fold.

## Adjustments

- Classify the table from `room.events` during waiting periods. Track limps,
  aggressive seats, and weak showdowns.
- Reassess the primary style every two orbits.
- Apply seat-specific branches within one orbit, especially `trapper` against
  a confirmed maniac.
- Switch to `shortstack` when effective stack is at or below 40 BB.

## Postflop

- Use frequent small continuation bets, 25-40% pot, on dry ace- or king-high
  boards.
- Check more often on wet multiway boards.
- In heads-up pots, target a baseline continuation-bet frequency near 55-65%.
- Continue pressure in 3-bet pots, but stop river bluffs without coherent
  blocker logic.
- Control the pot with weak top pair or middle pair and check-call one or two
  streets. Without improvement, take showdown or fold the river.

## Switch Signals

| Signal | Change |
| --- | --- |
| At least two players limp often, call down, rarely raise preflop, and show weak pairs | `station-hunter` |
| The table folds frequently to opens and steals, with little 3-betting | `lag` |
| One player raises and barrels excessively, then shows weak hands | Keep `tag`; add `trapper` against that seat |
| Hero effective stack is at or below 40 BB | `shortstack` |
