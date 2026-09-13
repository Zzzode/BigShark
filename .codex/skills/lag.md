---
name: lag
summary: Loose-aggressive stealing against tight-passive tables
stack: 60-100 BB
when: Opens and steals succeed often, 3-bets are rare, and opponents fold to one postflop bet
---

# Loose-Aggressive Stealing

## Core Logic

A tight table yields value through folds. Use position and initiative to win
uncontested blinds and small pots. Stop quickly when a tight range resists.

## Preflop Adjustments

- Open about 40% from CO, 60% from BTN, and 45% from SB. Keep early
  positions at TAG width because a nit's early open is strong.
- Open BTN to 2.5 BB against blinds with a high fold-to-steal rate. Fold most
  hands to a 3-bet and continue mainly with TT+/AK.
- Light 3-bet tight middle- or late-position openers with A2s-A5s, suited
  broadways, and selected small suited connectors.
- Keep light 3-bets against one opponent near an 8% ceiling.
- Do not limp. Multiple limpers indicate that `tag` or `station-hunter` is a
  better primary style.

## Postflop Adjustments

- Position is essential. Avoid marginal steals out of position.
- On single-high-card ace, king, or queen boards, continuation-bet 30-40% pot
  frequently when in position.
- Fire a second barrel when the turn adds equity or creates a credible
  overcard against a nit.
- Use a third barrel only with clear blocker logic.
- Fold to check-raises from confirmed nits.
- After two calls, abandon river bluffs. Check hands with showdown value and
  give up air.
- Continue betting strong hands for value. Tight opponents do not bluff often
  enough to justify excessive trapping.

## Risk Controls

- Treat every failed steal as a small planned loss. Do not build a large pot
  to recover it.
- Record blind defense in `strategy/table-notes.md`. Return to TAG steal
  ranges against a player who begins defending.
- Against one regular who counter-steals, retain LAG against the table and use
  TAG ranges against that seat.
- After three failed steals in one orbit, tighten for one orbit and observe.

## Switch Signals

- At least two loose calling players join: switch to `station-hunter`.
- Counter-steals or calls against 3-bets increase: return to `tag`.
- Effective stack reaches 40 BB or less: switch to `shortstack`.
