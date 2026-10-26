# Provider Public-Field Audit — River Club v2.1

Status: Current
Created: 2026-10-24
Scope: T1b/T2 provider gate (RFC 0022 remainder). This audit establishes which
facts required for full cash-hand reconstruction are settled by the
authoritative public contract, which observable fields are undocumented, and
which additional authoritative data must be supplied before a six-seat
side-pot end-to-end claim is possible.

## Authoritative sources

Only the materials shipped by River Club are authoritative:

| Source | What it settles |
| --- | --- |
| [`docs/SKILL.md`](../SKILL.md) | The v2.1 operating contract: decision loop, state machine, field reading order, action legality, stale semantics, event retention. |
| [`docs/openapi.json`](../openapi.json) | OpenAPI 3.1: endpoint shapes, the closed `Action` enum, request requirements, long-poll parameters. The `room` schema is deliberately opaque (`oneOf object|null` with prose only), so it settles no inner field beyond existence. |
| [`docs/upstream-STRATEGY.md`](../upstream-STRATEGY.md) | The official decision guide; names the decision-input fields and their meanings. |

The server is authoritative. Payload fields that do not appear in these three
documents are observed behavior, not a contract, and RFC 0022's gate forbids
certifying reconstruction semantics against them.

## Field matrix

Verdicts:

- **A — Settled**: the authoritative contract names the field and its
  semantics.
- **B — Observable but undocumented**: present in provider payloads and our
  parser (`platforms/river-club/src/types.ts`), absent from the authoritative
  documents.
- **C — Not settled**: no authoritative source states the required semantics.

### Decision fields (current snapshot)

| Field | Verdict | Evidence |
| --- | --- | --- |
| `room.handId`, `room.revision` | A | `ActionRequest` requires both and every mutation is revision-bound (`openapi.json:40-50`); stale snapshots are rejected, never replayed against a newer game (`SKILL.md:114-117`). |
| `room.legal.actions` | A | Actions come from the closed enum `fold/check/call/bet/raise` (`openapi.json:36-39`) and only a listed action is legal (`SKILL.md:110-111`). |
| `room.legal.call`, `legal.potOdds`, `legal.raiseTo.{min,max}` | A | Named in the output contract and decision contract (`SKILL.md:112-113`, `SKILL.md:134`); bet/raise is an inclusive target-total range (`SKILL.md:105`). |
| `room.timeLeftMs` | A | Named decision field (`SKILL.md:135`). |
| `room.next` (`act`/`wait`/`wait-exit`) | A | State machine table (`SKILL.md:69-77`). |
| `room.mode` (`leaving`) | A | Read-only leaving semantics (`SKILL.md:56`, `SKILL.md:120`). Other mode values are B. |

### Current-state fields used for reconstruction

| Field | Verdict | Evidence |
| --- | --- | --- |
| `room.hero.stack`, `hero.effectiveStack(Bb)`, `room.pot` | A | Official decision input order names stack and pot (`STRATEGY.md:12`; `SKILL.md:133`). Current values only; no history. |
| `room.seats[].status`, `button`, `blind`, `bet` | A | Named as decision inputs (`STRATEGY.md:13`). The closed **status vocabulary** (`active/all-in/folded/leaving/waiting`) is not enumerated in any authoritative document (C for the enum's completeness), though fold/leave behavior is described operationally. |
| `room.board` | A | Current board; card visibility is explicitly limited to hero cards and legitimately revealed cards (`SKILL.md:55`, `SKILL.md:136`). |
| `room.seats[].cards` | A (visibility only) | Hero cards and revealed cards only (`SKILL.md:136`). Mucked holdings are never available. |
| `room.blinds`, `room.dealer`, `room.actor`, `hero.position`, `seats[].position` | B | Parsed and used (`types.ts:91-107`); unnamed by the authoritative documents. `smallBlind` is settled for room creation (`openapi.json:87-99`), but the per-hand blind attribution labels are not. |
| `room.winners[]` (`seat`, `amount`, `description`) | B/C | `room.winners` is referenced operationally (`SKILL.md:39`) but its shape is undocumented; `description` is prose. Per-pot attribution, ties, and gross-versus-net meaning are C. |

### History and partition fields

| Field | Verdict | Evidence |
| --- | --- | --- |
| `room.events[].kind/text` | B | The operating loop says to read `events` (`SKILL.md:55`) and the strategy guide calls them "recent events" (`STRATEGY.md:13`), but the OpenAPI room schema is opaque prose and no event-kind catalog exists. The five action kinds match the closed `Action` enum; other observed kinds (`deal`, `join`, `leave` in synthetic fixtures) have no authoritative enumeration. |
| Event retention | A (incompleteness is settled) | "The state response intentionally omits … old events" (`SKILL.md:138`). The window length, ordering guarantee, and turnover semantics are C. |
| `room.pots[].size`, `pots[].eligible[]` | **B — undocumented** | Absent from `SKILL.md` and `openapi.json`; parsed at `types.ts:101,313-314,405-409` and consumed by the mapper at `mapper.ts:586-604`. |
| Pot-layer ordering, eligibility timing, pending/finalized semantics | C | Not in any authoritative source. |
| Rake | C | No rake field or rule is documented anywhere. |
| Antes and dead money | C | No ante appears in `CreateRoomRequest` (`openapi.json:87-99`), but no authoritative statement settles cash ante/dead-money policy. |
| Uncalled-chip refund, all-in runout, bet reopening, settlement order | C | RFC 0022 already records that local documentation does not settle these questions (RFC 0022, Open Questions). |

## Full-history completeness

The decision state is a compact view, not a replay feed. From the authoritative
contract:

1. `GET /state` returns the compact current state; `after` plus the 0-6000 ms
   timeout provide revision-ordered long polling (`openapi.json:103-117`), and
   unchanged heartbeats carry only id/revision (`SKILL.md:138`).
2. Old events are intentionally omitted (`SKILL.md:138`); nothing documents a
   replay or backfill endpoint.
3. Mutations are bound to revisions and stale submissions are rejected rather
   than rebased (`SKILL.md:114-117`), so revision numbers are a sound
   continuity spine.

Consequently a complete action history is provable **only** by observing
contiguously: an observation at hand revision 0 with no prior actions, then
gap-free, alignable overlapping event windows, plus board growth seen between
snapshots. The observation ledger implements exactly this and fails closed to
`MID_HAND_ATTACH` on mid-hand attach, restart, or zero-overlap window turnover
(`platforms/river-club/src/observation.ts:114-216`). Two residual limits are
structural, not implementation defects:

- `{kind, text}` events are not globally unique, so seat attribution relies on
  ordered alignment; any provider-side reordering or window jump longer than
  the window can only be detected as a gap, never reconstructed.
- `deal` and other non-action transitions have no authoritative kind catalog;
  board deals are inferred from board-length growth, which cannot distinguish
  "not observed" from "not dealt" across a gap (the ledger counts only growth
  it actually saw).

## Side-pot gate

The engine side is built and strict: the semantic validator checks layer
ordering, positive amounts, layer-sum conservation against `pot_total`,
phase-aware PENDING/FINALIZED status, rake and winner presence on FINALIZED
layers, eligible-seat references, and winner-subset eligibility
(`engine/src/protocol/semantic_validator.cpp:349-424`). The adapter builds
`PotLayer` messages from either the provider partition (`room.pots`) or
reconstructed net contributions (`mapper.ts:573-629`).

The gate is nevertheless closed:

1. `room.pots` is an undocumented field (verdict B). Its `eligible` semantics
   (contention at the layer, exclusions after fold, tie membership), ordering,
   and the timing at which layers appear are not contractually guaranteed.
2. No authoritative source confirms how uncalled chips, rake, or dead money
   enter `pots[].size` (verdict C); the mapper's min-net-contribution level
   heuristic is unverifiable against provider rules.
3. Settlement cannot be checked end to end: `winners[]` has no documented
   per-layer mapping and no rake figure, so FINALIZED layers cannot be
   populated from authoritative inputs.
4. The host still advertises the heads-up tuple only —
   `min_live_players = max_live_players = 2`, `side_pots = false`
   (`engine/src/protocol/response_mapper.cpp:445-462`); the planned
   `sixmax-r1` tuple is not emitted.

Flipping `side_pots` to true or emitting `sixmax-r1` before items 1-3 are
resolved would certify an undocumented contract and is explicitly out of
scope.

## Required authoritative data (the gate)

T1b/T2 unblocks only when the provider supplies, through official channels:

1. A documented `pots`/pot-layer schema: per-layer amount, eligible-seat set,
   ordering, phase timing, and treatment of uncalled chips, dead money, and
   rake.
2. A settlement schema or event: per-layer winner membership, split/tie
   amounts, and rake taken, sufficient to populate FINALIZED layers and check
   conservation end to end.
3. A closed event-kind catalog (actions, deals, showdown, settlement,
   join/leave), ordering guarantees, and the event-window retention bound.
4. Written cash rules: rake, antes, all-in runout, action reopening, and
   settlement order.
5. Authoritative real hand vectors, captured through the official CLI, that
   cover (a) a two-layer side pot plus a pending wager, (b) a folded all-in
   seat excluded from a layer, (c) a split pot, and (d) rake at settlement.

Until then the single-pot, zero-rake path is the only certifiable cash shape:
it reconstructs from structured A-verdict fields plus the fail-closed
observation ledger, and it matches the advertised `hu-r1` tuple.

## Evidence status

This audit uses only local authoritative documents and the repository's own
code; no live provider data exists locally (`sessions/` is empty and
`.runtime/results.log` has no decision rows), so no end-to-end side-pot
verification is claimed. Synthetic room fixtures
(`tools/replay/fixtures/synthetic-rooms.json`) exercise our parser and mapper
but establish engineering behavior, not provider semantics.
