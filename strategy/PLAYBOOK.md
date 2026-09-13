# BigShark Poker Decision Playbook

Status: Current

This is the agent's decision framework, not a rigid script. Review the
checklist before every action and adjust to observed table conditions. Every
decision must use the latest privacy-filtered state.

Scope: no-limit Hold'em cash games near 100 BB, with 6-max as the default.
`rooms` reports both blinds directly. For a 10/20 game, a standard 100 BB
buy-in is 2,000 chips.

## 0. Decision Checklist

1. **Position**: identify hero position and verify blind/button information
   against the current hand events.
2. **Effective stack**: use the smaller stack between hero and the principal
   opponent to determine stack-to-pot ratio (SPR) and commitment risk.
3. **Made hand and draws**: evaluate current strength, clean outs, and whether
   improvements can make the nuts.
4. **Pot odds versus equity**: required equity is
   `call / (pot after calling)`. Estimate equity, then consider implied odds.
5. **Action line**: identify bets, calls, raises, player count, and known
   opponent tendencies.
6. **Legal bounds**: use `room.legal.actions` and the inclusive bet or raise
   range. `--amount` is the target total for the street.
7. **Time**: use `room.timeLeftMs`. Spend time on material decisions and keep
   routine decisions fast.

## 1. Position and Ranges

Think in ranges. Later position supports wider ranges because it provides more
information and control.

### Raise-First-In Ranges

The following ranges are 6-max approximations when action folds to hero:

| Position | Open rate | Reference range |
| --- | --- | --- |
| UTG | ~18% | 22+, A2s+, KTs+, QJs, JTs, A9o+, KQo |
| HJ | ~22% | 22+, A2s+, K9s+, Q9s+, J9s+, T9s, 98s, A9o+, KJo+ |
| CO | ~28% | 22+, A2s+, K7s+, Q8s+, J8s+, T8s+, 98s, 87s, A8o+, K9o+, QJo |
| BTN | ~45% | 22+, all Ax, K5s+, Q7s+, J7s+, T7s+, 97s+, 86s+, 75s+, A2o+, K7o+, Q9o+, J9o+, T9o |
| SB | ~40% | Slightly tighter than BTN; raise to 3 BB plus 1 BB per limper or fold |
| BB versus raise | Price-dependent | Defend wider when closing the action with favorable odds |

- At 9-max, tighten UTG to roughly 12-15% and contract later positions
  accordingly.
- At loose recreational tables, prioritize value and reduce bluffs. Widen
  opens selectively without building large pots with dominated hands.
- Against limpers, isolate strong playable hands at 4 BB plus 1 BB per limper.
  Avoid large limp-behind ranges.

### Facing Opens and 3-Bets

- In position with a strong hand, 3-bet to roughly 3x the open; use 4x out of
  position. Start with TT+/AQ+ for value and A2s-A5s, KQs/KJs, and suited
  connectors as proportional bluffs. Add 99/AJ for value against loose ranges.
- Fold medium offsuit broadways such as KJo or QTo against early-position
  opens when out of position.
- Continue against 3-bets with the top of range. Call small pairs only when
  effective stacks provide at least 20 times the call amount in implied odds.
- Against a 4-bet, contract to KK/AA plus selected AK bluff-catches against
  sufficiently aggressive opponents.

## 2. Pot Odds, SPR, and Sizing

### Rule of Two and Four

- With one card to come, approximate equity as outs times 2%.
- From the flop with two cards to come, approximate equity as outs times 4%.
- A flush draw has 9 outs (~36% across two cards), an open-ended straight draw
  has 8 (~32%), a flush draw with two overcards has 12 or more (~48%), and a
  gutshot has 4 (~16%).
- Call when estimated equity exceeds required equity. Use implied odds only
  when later value is credible, and discount dirty outs.

### SPR Commitment Thresholds

- **SPR <= 2**: overpairs and top pair with a strong kicker can usually commit.
- **SPR 3-6**: two pair or better and strong draws can apply pressure; top-pair
  commitment depends on board texture.
- **SPR >= 9**: keep top-pair pots near one pot. Stacking off normally requires
  a set, straight, flush, or nut-quality combination draw.
- Agent tables can play linearly. At high SPR, account for random two-pair and
  set combinations in loose ranges.

### Sizing Reference

- Value bet 50-75% pot, with larger sizes for nutted hands against weak ranges.
- Continuation-bet dry ace- or king-high boards frequently at 25-40% pot.
- Reduce continuation bets on wet multiway boards.
- With a strong hand on a draw-heavy board, deny correct drawing odds.
- River bluffs require credible blockers. Remove them against calling-heavy
  tables.
- Clamp every target to the nearest legal integer inside `raiseTo`.

## 3. Postflop Lines

- **Nuts or near-nuts**: fast-play for value. Slow-play only when an opponent
  is likely to continue betting, and avoid free cards on wet boards.
- **Marginal made hand**: control the pot and check-call one or two streets.
  Without improvement, prefer showdown or a disciplined river fold.
- **Strong draw**: semi-bluff when fold equity and hand equity both contribute.
  Gutshots usually call rather than raise.
- **Air**: give up without credible fold equity.
- **Multiway pot**: tighten materially, reduce bluffs, and apply direct value
  pressure with two pair or better.
- **All-in decision**: cash games have no ICM. Compare chip equity directly
  with pot odds.

## 4. Opponent Modeling

The API provides actions rather than physical tells. Record:

- preflop participation, open raises, limps, cold calls, and 3-bet frequency;
- continuation betting, check-calling, and showdown hand strength;
- repeated evidence that distinguishes a nit, calling station, aggressive
  regular, or maniac.

Historical protocol statistics are cumulative counters, not percentages:

- VPIP rate is `vpip / hands`;
- PFR rate is `pfr / hands`;
- 3-bet rate is `threeBets / threeBetOpportunities`.

See the historical [state schema](../docs/state-schema.md). Before a useful
sample exists, use the tight-aggressive baseline and avoid unsupported labels.
Update at least one table observation after each reviewed session.

## 5. Bankroll and Mental Discipline

- Use a standard 100 BB buy-in. Consider topping up below 60 BB.
- Leave and review after losing two buy-ins in one session.
- Do not move up in stakes because of a win.
- Select tables by game quality, with loose passive tables preferred.
- Do not retaliate or expand ranges after a bad beat.
- Re-run the decision checklist before every action.

## 6. Post-Session Review

Use `room.events` and hero actions from `sessions/YYYY-MM-DD.jsonl`:

1. For every large losing pot, verify the range, pot-odds, and SPR reasoning.
2. For every questionable fold or call, compare the observed showdown and
   update the opponent model.
3. Review whether marginal hands entered from unsuitable positions and whether
   3-bet and fold ranges stayed coherent.
4. Produce three to five concrete findings.
5. Update this playbook only when repeated evidence supports a rule change.
