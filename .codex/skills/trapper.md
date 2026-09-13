---
name: trapper
summary: Seat-specific counter-strategy for a confirmed maniac
stack: 60-100 BB+
when: One player raises or 3-bets excessively, barrels repeatedly, and shows weak hands
---

# Maniac Counter-Strategy

## Core Logic

A maniac loses most in large pots and at showdown. Enter with a tighter,
stronger range, bluff less, call more, and let the opponent bluff into value.
Do not compete for aggression without a range advantage.

## Preflop Adjustments

- Against the maniac's open, remove marginal 3-bet bluffs. Value 3-bet
  TT+, AQs+, and AK with a normal-to-large size.
- In position, flat selected middle pairs and suited connectors when implied
  odds are strong. Out of position, prefer a value 3-bet or fold.
- Steal normally when the maniac is in the blinds, but avoid small provocative
  sizes that create awkward marginal 4-bet decisions.
- After opening and facing the maniac's 3-bet, value 4-bet QQ+/AK. Continue
  with JJ/TT only when position and stack depth justify it.

## Postflop Adjustments

- With top pair and a strong kicker or better, call continuation bets in
  position at a high frequency and raise later streets for value.
- Fast-play nutted hands on wet boards to deny free cards.
- Treat medium-strength hands as bluff-catchers for one or two streets when
  pot odds allow. Be cautious against large polarized river bets.
- Use no pure bluffs against this seat. Move bluffs to opponents who can fold.
- When a multi-barrel bluff misses, fold cleanly. Bluff-catch only with useful
  blockers or showdown value.
- Tighten further in multiway pots. A passive player's call behind a maniac's
  large action often represents a very strong range.

## Tempo and Discipline

- Accept long folding stretches. The strategy earns through one or two large
  value pots rather than constant contest.
- Do not widen defense after repeated blind steals.
- Record every relevant showdown in `strategy/table-notes.md`.
- Disable this branch immediately when evidence shows an aggressive regular
  with a strong range rather than a true maniac.

## Switch Signals

- The maniac leaves or materially reduces aggression: remove this branch.
- The opponent switches to short-stack jams: compare direct all-in equity with
  pot odds instead of slow-playing.
- Hero effective stack reaches 40 BB or less: switch to `shortstack`.
