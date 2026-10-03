# Engine Process Protocol

Status: Current

## Purpose

`bin/bigshark-engine` exposes the C++ decision core through JSON. The protocol
contains normalized poker state and has no transport, authentication, or room
management fields.

`engine/src/protocol/v0_json.cpp` owns JSON mapping and yyjson. It maps to
protocol-neutral domain types before `bigshark_service` invokes policy.

The v0 protocol is an internal, unversioned JSON contract. Its accepted
typed replacement is defined by
[RFC 0002](../rfcs/0002-protobuf-engine-protocol.md). The v1 IDL and generated
bindings are documented in
[Protobuf Engine Protocol](protobuf-engine-protocol.md). The River Club
runner defaults to v1 (with roots); v0 remains as the fallback path.

## Transport

One-shot mode reads one JSON document from standard input and writes one JSON
decision to standard output:

```bash
printf '%s\n' '{"street":"preflop","hole":["As","Kh"],"legal":{"actions":["fold","raise"],"raiseTo":{"min":40,"max":2000}}}' \
  | ./bin/bigshark-engine
```

Server mode uses NDJSON. Every non-empty input line produces one output line,
in FIFO order:

```bash
./bin/bigshark-engine --serve
```

The process writes no protocol framing beyond the newline delimiter.

## Request

Example:

```json
{
  "handId": "hand-42",
  "revision": 7,
  "seed": 0,
  "style": "tag",
  "street": "turn",
  "blinds": [10, 20],
  "position": "BTN",
  "playersInHand": 2,
  "effectiveStackBb": 84.5,
  "hole": ["As", "Kh"],
  "board": ["Qs", "Jh", "2c", "7d"],
  "pot": 280,
  "raises": 1,
  "openerPosition": "HJ",
  "heroWasRaiser": true,
  "heroPreflopAggressor": true,
  "limpers": 0,
  "opponentPcts": [0.55],
  "riverGtoOn": false,
  "riverLine": "",
  "flopLine": "Ob,Hc",
  "turnLine": "",
  "riverBetFrac": 0.75,
  "riverRaiseFrac": 1.0,
  "legal": {
    "actions": ["fold", "call", "raise"],
    "call": 120,
    "potOdds": 0.3,
    "raiseTo": {
      "min": 360,
      "max": 1690
    }
  }
}
```

## Request Fields

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `handId` | string | empty | Stable hand identifier used in deterministic seeding |
| `revision` | integer | `0` | Decision revision used in deterministic seeding |
| `seed` | unsigned integer | derived | Explicit deterministic random seed |
| `style` | string | `tag` | `tag`, `station-hunter`, or `lag` |
| `street` | string | `preflop` | `preflop`, `flop`, `turn`, or `river` |
| `blinds` | two integers | `[10,20]` | Small blind and big blind in chip units |
| `position` | string | `BTN` | Normalized poker position |
| `playersInHand` | integer | `2` | Players that have not folded |
| `effectiveStackBb` | number | `100` | Effective stack in big blinds |
| `hole` | string array | empty | Two cards in rank-suit notation |
| `board` | string array | empty | Public cards in rank-suit notation |
| `pot` | integer | `0` | Current pot in chip units |
| `legal.actions` | string array | empty | Legal canonical action names |
| `legal.call` | integer | `0` | Chips required to call |
| `legal.potOdds` | number | `0` | Required equity for a call |
| `legal.raiseTo` | object | absent | Inclusive target-total range |
| `raises` | integer | `0` | Preflop raises before the hero decision |
| `openerPosition` | string | empty | Normalized first-raiser position |
| `heroWasRaiser` | boolean | `false` | Hero made the latest relevant raise |
| `heroPreflopAggressor` | boolean | `false` | Hero owns preflop initiative |
| `limpers` | integer | `0` | Preflop callers before the hero decision |
| `opponentPcts` | number array | empty | Per-opponent preflop strength gates |
| `riverGtoOn` | boolean | `false` | Enable the modeled heads-up river solver |
| `riverLine` | string | empty | Current river public action code |
| `flopLine` | string | empty | Actor-tagged flop actions |
| `turnLine` | string | empty | Actor-tagged turn actions |
| `riverBetFrac` | number | `0.75` | River bet fraction used by the abstract game |
| `riverRaiseFrac` | number | `1.0` | River raise fraction used by the abstract game |

Card strings use rank `2-9`, `T`, `J`, `Q`, `K`, or `A`, followed by suit
`s`, `h`, `d`, or `c`.

Actor-tagged action lines use:

- actor `H` for hero and `O` for opponent;
- action `x` for check, `c` for call, `b` for bet, `r` for raise, and `f` for
  fold;
- commas between actions, for example `Ob,Hc`.

## Response

Example:

```json
{
  "action": "call",
  "amount": 0,
  "reason": "call eq61",
  "equity": 0.612,
  "mdf": 0.7
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `action` | string | `fold`, `check`, `call`, `bet`, or `raise` |
| `amount` | integer | Target total for `bet` or `raise`; otherwise `0` |
| `reason` | string | Compact policy or solver explanation |
| `equity` | number | Optional estimated showdown equity |
| `mdf` | number | Optional minimum defense frequency |

## Amount Semantics

All values are integer chips. A `bet` or `raise` amount is the total amount
committed on the current street after the action. It is not the incremental
amount added by the action.

The caller must provide a legal interval. The engine clamps generated sizes to
that interval, and the platform adapter must validate the response again
before submission.

## Determinism

Mixed decisions use `seed` when it is non-zero. Otherwise the engine derives a
seed from `handId` and `revision`. A platform adapter that lacks those concepts
must provide a stable decision identifier or explicit seed.

## Current Error Semantics

The parser applies defaults to missing values. Syntactically invalid JSON
becomes an empty default context and produces a fold-shaped `no hole cards`
response. An exception produces a fold-shaped `parse-error` response.

This is safe for the River Club runner, which performs a legality check and
owns the final operational fallback. It is insufficient as a public
multi-platform contract because errors and strategic folds are not strongly
distinguished. RFC 0002 requires explicit protocol versions, request IDs,
validation errors, mixed strategies, and capability negotiation.

## Compatibility

This document describes the v0 fallback implementation and does not declare a
stable public API. New adapters target the accepted RFC 0002 contract. The
River Club runner defaults to v1 (with roots); v0 remains as the fallback
path for backward compatibility.
