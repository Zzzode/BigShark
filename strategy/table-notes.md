# Table and Opponent Notes

Status: Current

Protocol v2 does not expose player statistics or long-term history. Durable
opponent information must come from our own reviewed observations.

Record a profile only after at least three similar actions or a confirming
showdown. Mark weaker evidence as `pending validation`. Update this file after
leaving a table and read it before joining.

## Player Profiles

<!-- Template:
### Name (open ID suffix)
- First observed: date / table / blinds
- Type: station-limp / station-chase / nit / maniac / reg-TAG / reg-LAG / unknown
- Evidence: preflop tendency, postflop lines, and shown hands
- Counter-strategy: skill plus concrete exploit
-->

No confirmed player profiles.

## Table History

<!-- Template:
### Date - table name (blinds, player count)
- Baseline: loose-passive / tight-passive / mixed / aggressive
- Primary skill:
- Findings:
- Profit and loss:
-->

### 2026-09-11 - Short protocol v1 observation, pending v2 validation

- "Friends Gathering" 25/50 and "Jeju Game" 10/20 showed high VPIP
  (44-84%), low PFR (1-25%), and frequent limped pots. This suggested a
  station-limp environment, but the sample contained only one observed hand
  and no direct play.
- Platform decision clocks were 10-20 seconds. Protocol v2 now exposes
  `timeLeftMs` in each snapshot, so decisions must remain fast.

### 2026-09-11 - First engine session: Riverside Gathering 10/20

Result: 78 decisions, +580 chips, +29 BB.

- The table was a typical loose-passive game with frequent limps and passive
  calls. Effective stacks were usually 70-100 BB.
- Profit came from a tight preflop range, two premium-pair 4-bet jams
  including hand 489502, and a 540-chip river value bet with a set in hand
  202286. Fast-playing strong value was effective.
- Engine defects found and corrected:
  1. Hand 855209 semi-bluffed a weak flush draw in a five-way pot with `4c2h`.
     Multiway semi-bluffs are now limited to nut-quality draws.
  2. Hand 460647 called a raise with a real `Th5h` flush draw, then called 60
     on a missed river because equity was incorrectly doubled to 9%. River
     draw equity is now zero, weak draws receive a 0.55 discount, and only nut
     draws or two pair and better continue against a re-raise.
  3. Players joining or leaving during the model window advanced the revision
     and caused stale actions. The runner now refreshes immediately before
     submission.
  4. Repeated decisions for one state differed because of `Math.random`.
     Mixed decisions now use a deterministic hand ID and revision seed.
  5. The runner joined a new table after dissolution without authorization.
     It now leaves and waits for an explicit `resume`.
- Preflop participation remained tight: 14 of 78 decisions, or about 18%.
  Future sessions may test wider CO and BTN isolation ranges at passive limp
  tables.

### 2026-09-11 - Second session: Friends Gathering 25/50

Result: 50 decisions, +7,750 chips, +155 BB.

- This was a high-variance small sample. Most profit came from two hands:
  `QhJh` flopped three jacks and jammed a low-SPR three-spade turn for value,
  and a straight made a value raise.
- The station-limp pattern continued. Players limped and called frequently,
  including large bets with weak made hands.
- The engine had zero execution failures. The process was healthy, but the
  sample is too small to widen value or bluff frequencies from results alone.
