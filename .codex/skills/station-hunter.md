---
name: station-hunter
summary: Value extraction against loose-passive calling stations
stack: 60-100 BB+
when: At least two players limp often, rarely raise preflop, call multiple streets with weak hands, and show marginal pairs
---

# Calling-Station Exploitation

Historical River Club samples showed many active tables with VPIP between
44% and 84% and PFR between 1% and 25%. Treat this as a prior, then verify the
current table from public actions and showdowns.

## Core Logic

Calling stations continue with marginal holdings. Remove pure bluffs, widen
the value range, and take more value streets. Avoid entering large pots with
dominated trash.

## Preflop Adjustments

- Tighten the weakest TAG opens and increase the share of hands that can take
  three streets of value.
- On BTN or CO against two passive limpers, isolate wider with any pair, Ax,
  KQ/KJ, and QJs+. Use 5-6 BB plus 2 BB per limper.
- Build 3-bets mainly from 99+ and AQ+. Reduce bluffs because these opponents
  call 3-bets too often.
- Avoid large cold-call ranges. Fold dominated offsuit broadways against an
  early open with several callers behind.

## Postflop Adjustments

- Remove pure bluffs. Check and fold air.
- Value-bet top pair at 45-60% pot across up to three streets when the runout
  remains favorable.
- Value-bet second pair with a strong kicker for up to two streets on dry
  boards.
- With two pair or better, use 70-80% pot on draw-heavy boards. Fast-play
  strong hands because weak hands and draws will call.
- Semi-bluff strong flush draws when equity and some fold equity both exist.
  Call with gutshots when implied odds justify it.
- Bet when hand strength is above the middle of the opponent's wide range.
- In multiway limped pots, bet sets, two pair, and nut-quality draws directly.
  Reassess top pair after building one pot.

## Seat-Specific Branches

- Record each station by seat and name in `strategy/table-notes.md`.
- Against a weak-pair caller, remove bluffs and widen value by another tier.
- Against a draw chaser, charge incorrect odds while draws are incomplete and
  give up missed bluffs after the draw completes.
- When a regular remains to act behind a station, tighten the isolation range
  unless the size can also pressure the regular.

## Prohibited Lines

- Do not river blocker-bluff air without showdown value.
- Do not check-raise medium-strength hands merely to apply pressure.
- Do not 3-bet bluff a confirmed calling station.

## Switch Signals

- Calling stations leave and regulars become the majority: return to `tag`.
- A maniac changes the table dynamic: add `trapper` against that player.
- Effective stack reaches 40 BB or less: switch to `shortstack`.
