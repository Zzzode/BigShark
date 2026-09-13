# Observed River Club State Schema

Status: Historical

Observed on: 2026-09-11

Source: a real response from `observe 91674bb4`. This document records
protocol v1 behavior. Protocol v2 is authoritative for current operation.

## Top-Level `state`

- `me`: authenticated player. `id` is the Lark open ID; `wallet` is the chip
  balance; `xp` is experience; `stats` contains cumulative counters rather
  than percentages.
- `rooms[]`: public table summaries. Important fields include
  `smallBlind`, `bigBlind`, `players`, `maxPlayers`, `allowAgents`,
  `agentPlayers`, `passwordProtected`, `turnSeconds`, `reconnectSeconds`,
  `practice`, and `status`.
- `room`: the seated or observed table view, otherwise `null`.
- `serverTime`: Unix epoch time in milliseconds.

## `room`

| Field | Meaning |
| --- | --- |
| `dealer` | Seat index of the button at room level; the next two occupied seats are SB and BB |
| `actor` | Seat index of the player to act, or `null` |
| `street` | `preflop`, `flop`, `turn`, `river`, or `showdown` |
| `handId` | Current hand identifier required for actions |
| `revision` | State revision used for long polling and optimistic locking |
| `deadline` | Action deadline in epoch milliseconds, or `0` outside an action |
| `callAmount` | Reference total required to call |
| `pot` | Total pot; `pots[]` contains side-pot size and eligible seat indices |
| `board[]` | Public cards as `{rank, suit}` objects |
| `legal` | Legal actions and amount ranges; present only for the acting player |
| `winners[]` | Settlement entries with seat, amount, and description |
| `events[]` | Public action events with `id`, `kind`, `text`, and `at` |
| `messages[]` | Table chat with user ID, name, text, and timestamp |
| `spectating` | Whether the authenticated user is observing |
| `canReveal` | Whether the player may reveal cards after settlement |

Event `kind` values include `fold`, `check`, `call`, `bet`, `raise`, `join`,
and `leave`. The v1 `text` value was a localized full action description and
could include an amount.

## `seats[]`

The array has fixed length and uses `null` for an empty seat. Occupied seats
contain:

- `index`, `stack`, current-street `bet`, and `cards[]`;
- `inHand`, `folded`, `allIn`, `connected`, and `leaving`;
- `agentOnline`, indicating control through the Agent CLI;
- `action`, a localized recent-action label;
- `revealed`, indicating whether cards are public;
- `player.stats`, containing raw cumulative counters.

Derived statistics:

- VPIP percentage is approximately `vpip / hands`.
- PFR percentage is approximately `pfr / hands`.
- 3-bet percentage is `threeBets / threeBetOpportunities`.
- Other counters include `flops`, `showdowns`, `showdownWins`, `aggressive`,
  `calls`, `profit`, and `biggestPot`.

## Decision Data Paths

1. Position: start from `room.dealer` as BTN and advance through occupied seat
   indices; the next two seats are SB and BB.
2. Turn ownership: `room.actor` equals the hero seat index and `room.legal` is
   non-null.
3. Time remaining: `room.deadline - serverTime`.
4. Pot odds: combine `pot` or the relevant side pot with `legal.call`, hero
   street commitment, and stack.
5. Opponent model: derive rates from cumulative counters and combine them with
   the current `events[]` action line.
