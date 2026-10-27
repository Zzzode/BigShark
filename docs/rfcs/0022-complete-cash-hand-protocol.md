---
rfc: "0022"
subject: "Complete Cash-Hand Protocol"
status: "Superseded"
authors: "Codex /root"
created: "2026-10-14"
updated: "2026-10-27"
owners: "protocol, engine host, engine clients, River adapter, River runner, poker, service"
supersedes: ""
superseded-by: "0040"
---

# RFC 0022: Complete Cash-Hand Protocol

## Summary

Draft the coherent cash-hand follow-up required by RFC 0006 and the complete
six-max delivery register. A new `bigshark.engine.v2` contract must carry
hand-start facts, the complete public transition path, gross contributions,
refunds, every pot layer's eligibility, and explicit rules and utility
identity. The River runner must observe official responses before discarding
or classifying them. Native replay remains the only owner of betting rules.

This is an incomplete design, not approval to implement v2 or claim six-max
GTO. Exact field numbers, conformance vectors, rule-profile migration,
capability combinations, and the provider completeness proof remain draft
work. RFCs 0019..0021 remain internal prerequisites. The user feature remains
In Progress, including unequal stacks, all-ins, side pots and all streets.

## Motivation

The v1 state cannot faithfully express the complete contribution ledger or
single-eligible pot layers. RFC 0021 can reconstruct a declared, replayable
no-refund history; it cannot make the legacy River token attribution reliable
or invent a provider history that was not observed. Treating the snapshot's
pot as equal contributions, reducing the dealt-player count after folds, or
rounding a legal target into a policy row corrupts the game being solved.

The smallest correct boundary is a coherent versioned input to the existing
native rules and service, with a separate public-observation adapter. Adding
optional v1 fields while removing its guards would permit old incomplete
requests to enter the new utility model and break established semantics.

## Goals

- Express every dealt seat and its hand-start stack throughout the hand.
- Preserve the actual button, forced payments, action order, exact payments,
  returns, public cards, statuses and individual raise rights.
- Validate current stacks and every pot layer against replay, including
  folded dead money, unequal all-ins and single-eligible layers.
- Distinguish observed-response coverage, action-history completeness,
  successful physical replay and qualified strategy provenance.
- Scope every solver capability and quality claim to one complete game,
  rules, utility, range and artifact identity.
- Preserve all existing v0/v1 callers and immutable artifacts; make v2 an
  explicit opt-in until its complete qualification gates pass.

## Non-Goals

- Live joining, action submission, private endpoints, credentials, spending,
  large training, publication or replacement of existing artifacts.
- A TypeScript poker rules machine, opponent-card inference or synthetic
  opponent actions inserted into an official event list.
- Accepting an incomplete history because final chip totals happen to match.
- Calling legal operational fallback, uniform-prior resolving or an
  approximate policy complete six-max GTO.
- Tournament/ICM delivery, bounties or run-it-twice. RFC 0006's tournament
  contract requirements remain open; this draft does not waive them.
- Artifact schema changes or shared-rule migrations without an exact design
  and independent approval of their compatibility surface.

## Current State and Evidence

### Protocol and utility

`proto/bigshark/engine/v1/engine.proto` gives `PlayerState` a current stack and
street commitment, but no hand-start stack or G/F ledger. `PotState` carries a
scalar main pot; `SidePot` requires at least two eligible IDs. The semantic
validator refuses side pots. RFC 0006 requires a separate coherent v2 gate.
Neither deleting the refusal nor changing the meaning of v1's scalar main pot
is authorized by this draft.

`engine/include/bs/settlement.hpp` already declares exact gross/refund/net
accounting and pot-layer settlement, with clockwise odd chips and a declared
percentage-floor rake. This arithmetic does not establish that River uses
those rules or that a trainer/service supports the entire rule combination.

Native `GameState` retains its original `GameDef`, per-seat contributed and
refunded chips, pending actions and raise rights. Full hand-start replay can
retain these without a mid-street rebase. RFC 0021 deliberately rebases to a
flop root for existing resident lookup and refuses refunds; that restricted
path is not the future v2 ledger carrier.

The independent native `sixmax_hand_ledger` regression now executes eighteen
complete hand-start-to-river cash hands (three literal scenarios times all
six buttons) under the existing offline rules. It verifies exact per-node
G/F/status/actor/chip state, refund revival, folded contributors,
single-eligible side layers and tie odd chips without a rebase or manual
refund credit. Focused release and true Debug ASan/UBSan runs passed. This
establishes native ledger baselines only; no provider, v2 or strategy quality
claim follows.

`game_definition_settlement.cpp` currently hardcodes zero rake and the
clockwise-left-of-button rule for native terminal awards. A nonzero-rake
wire field cannot make that terminal utility rake-aware. Supporting such a
rule requires one coherent native definition/terminal/identity change, not
protocol-side deductions after an already computed strategy.

### Native rules are a declared offline profile

The original dealt count selects the native profile for the entire hand.
Changing six dealt seats to two live seats changes its rules, not just its
storage size. The two-seat profile retains an all-in flag after an uncalled
refund; the 3+ profile derives it again from chips behind. The 3+ profile also
opens a later street's check for a sole actionable seat, even when all other
live seats are all-in. `game_definition.cpp` explicitly states that cumulative
short all-ins do not reopen raise rights.

These are verified implementation facts, not confirmed River rules. If the
provider differs, its correct rules belong in the shared native rules owner
with explicit identity and compatibility, not in an adapter that inserts
checks, silently changes all-in flags or rewrites the button to force an
actor. Rule corrections also affect old solver/artifact meanings and require
a separate, independently approved migration decision.

Native button identity is an occupied-player index, whereas contribution
settlement can represent gaps and an unoccupied physical button. Mapping a
real occupied button is possible; assigning a different player's button to
simulate a dead button is not. Exact dealt membership, waiting/leaving status
and dead-button rules require an explicit provider profile and native support
where the existing representation is insufficient.

As a comparable published rule set, Poker TDA's 2026 Version 1.1 rule 49
allows cumulative short all-ins to reopen an already-acted player's betting
when that player faces a full raise; the minimum increment remains the last
full valid bet/raise. This differs from the current native never-cumulative
rule. It is a tournament reference, not evidence that River follows it.
See the [official TDA rules](https://www.pokertda.com/view-poker-tda-rules/).
For a concrete rule-equivalence test, A bets 100, B jams 150, C jams 200,
D calls 200 with chips remaining and the other seats fold. At A's next turn,
the accumulated increase is 100, although neither individual increment was
100. Preserve the legacy result and separately qualify any intended provider
result under an explicit rule identity; do not silently change old artifacts.

### Official-response observations

`apps/river-club-agent/main.ts` runs one controller using the official
journaling CLI. Initial state, join, every next response, refresh, action
acknowledgement, stale recovery, leave and leave-drain responses all contain
potential observations. Several are discarded before `decide(room)`.
Heartbeat references are not full room snapshots. Engine warmup can create an
unobserved interval while seated; another controller is not an acceptable
repair.

The existing `sessions/` journal preserves full CLI output on a best-effort
basis, swallowing write failures. A file's existence therefore does not prove
that an interval or all public actions was observed. Pending digests retain
only the last eight event texts and cannot restore a complete hand ledger.

`platforms/river-club/src/v1-mapper.ts` groups action text using heads-up token
counts, freezes round advancement after a fold and resolves display names.
That frozen compatibility path is not a six-seat historical authority.
`RiverEvent` has kind/text, not authoritative actor, sequence, street or exact
payment fields. Local official documentation says older events are omitted.
No live/private endpoint has been inspected. This is a documented-data gap,
not proof that the provider can never supply sufficient public information.

On 2026-10-14, a read-only attempt to fetch the advertised official public
`https://riverclub.booocai.com/agents/STRATEGY.md` returned HTTP 404; the web
reader also could not access it. This does not establish absent server
functionality or permission to guess alternative/private endpoints. No player
state, live table, token or authenticated request was used. The installed
official references remain the available baseline, and authoritative current
rules/history documentation is still an unresolved qualification dependency.

## Design Principles

1. The server supplies facts; native rules explain transitions; the service
   checks support; strategies never manufacture any of these inputs.
2. Keep six dealt identities even when only two players remain live.
3. Current chips include returned wagers exactly once. G never decreases.
4. Replayability does not certify observational completeness or equilibrium.
5. Observability must precede branch decisions and preserve uncertainty.
6. Use existing framing, generation, native tests and module boundaries.
7. Refuse unknown rules and incomplete histories rather than substitute a
   different supported game behind a successful GTO response.

## Proposed Design

### Owning modules and shared-change gate

| Owner | Responsibility | Boundary not to cross |
| --- | --- | --- |
| River runner | Observe every official command result, submit intents and acknowledgements; bind lifecycle and restart boundaries | No rule inference or second controller |
| River adapter | Normalize exact public facts and provenance, retain ambiguous/unobserved intervals | No poker state machine or guessed actor |
| Generic Node client | Versioned framing, correlation, bounded transport and deadlines | No River types or event grammar |
| Protocol | Validate v2 shape, bounds, completeness evidence and replay consistency; map to native types | No duplicated payout/raise rules |
| Native poker | All action/card/refund transitions and contribution settlement | No wire/generated/platform types |
| Service and strategy owners | Match rules/utility/ranges/artifacts and qualified combinations | No quality upgrade based on ledger validity |

Local closure is insufficient because every terminal utility, policy root,
history reconstruction and caller must agree on the contribution semantics.
Considered local alternatives are v1 optional-field repair, adapter-only
accounting and a mid-street root substituted from the current snapshot; all
lose a core invariant or established contract. The smallest shared change is
a coherent new version invoking the existing native core, not a utility plugin
framework. Exact core/artifact changes are not yet approved by this draft.

### Public-observation intake

Observe results in the existing controller before any early return,
heartbeat/wait/leave classification, changed-hand skip or strategy call.
Include initial state, idempotent join, next, refresh, action results, stale
retry, leave and drain. An own submission is an intent until a server result
and consistent subsequent observation confirm it. Errors and parser failures
remain evidence of uncertain intervals, not accepted actions.

Keep hand identity scoped to room and hand, immutable dealt-seat identity and
explicit controller/attach boundaries. A local intake ordinal is not the
provider's action sequence. A higher room revision alone cannot prove that
one or several poker actions occurred, because non-action updates can also
advance revisions. Repeated event texts are not globally unique actions.

Recovery may consume the existing official-response journal only after
validating its ordered coverage, hand binding and any write/parse/truncation
gaps. Mid-hand attach starts incomplete unless an authoritative complete
public hand stream or a uniquely verified public transition path reconstructs
the missing prefix. Never use hidden state to resolve ambiguity. Losing the
prefix at warmup/restart is visible incompleteness, not a reason to invent it.

The final design must prove how observed public transitions become uniquely
attributed actions, or explicitly establish the provider-data requirement.
Merely archiving all snapshots is not acceptance of this stage.

### Complete cash-hand input

The following are required semantic groups, not approved IDL field numbers:

- Declared rules, cash utility and atomic chip units; exact blinds, ante/rake
  and settlement/reopening/runout identities, with no assumed provider match.
- Original dealt seats and stable hand-local IDs, actual button, starting
  stacks and complete forced payments. Folded seats remain in the vector.
- An ordered public transition path from hand start to this decision, with
  explicit street/card boundaries, acting IDs, action kind, exact incremental
  payment and target total where applicable. Zero has explicit presence.
- Per-seat current stack, street commitment, status, G and F, with exact
  reconciliation after native replay. Refund observations verify a native
  return; they must never apply that return a second time.
- Phase-tagged current contribution layers, their boundary, amount and
  eligible dealt IDs, including the main layer and single-eligible layers.
  Distinguish a pending unmatched wager from a finalized pot. Compare current
  layers with native replay at the actionable node; terminal settlement
  prerequisites apply only when no further response or refund is pending.
- Exact current public board, hero's own cards and server legal actions.
  Preserve capped call payment versus wager amount owed as distinct values.
- Completeness and observation provenance bound to the same room-independent
  engine hand identity and public path. An asserted completeness bit is not a
  proof and cannot bypass structural replay or adapter coverage validation.
- Resource budget and requested guarantee floor bound to the qualified
  strategy profile, not an implicit effective-stack replacement.

Before awards, per-seat starting chips equal `current_stack + G - F`.
Net hand contribution is `N = G - F`. At settlement,
`sum(awards) + rake + sum(F) = sum(G)`; cash chip utility is
`awards + F - G`. Award-credited terminal snapshots require an explicitly
different phase/award ledger and are not actionable decision requests.
Arithmetic must remain checked inside the exact numeric profile.

#### Pending wagers are not terminal layers

A normal bet awaiting another player's call/fold can have a temporary upper
contribution layer funded by only the bettor. It is part of current
`sum(G - F)` and the server's actionable pot; it is neither a completed refund
nor an invalid terminal payout. Never invoke terminal settlement validation
to reject that valid action node, and never reduce the snapshot's pot by a
refund that native transition has not yet executed.

The final wire design must explicitly distinguish current/provisional
contribution layers and completed settlement layers. During action, derive
and compare any declared layer facts at the exact native node, preserving
pending responses and current commitments. When the native street/hand
transition executes an uncalled return, verify the G/F/stack delta exactly
once. Only finalized settlement rejects a sole-contributor positive layer.
A finalized layer funded by multiple players but with one eligible winner is
a real pot, not a refund. The native poker owner must own the phase-aware
layer invariant; the adapter cannot supply an alternative payout calculator.

Conformance must include an ordinary open bet before a response, a capped
call that triggers a refund on street closure, and an actionable node with
an existing side pot plus a new pending wager. The full field/phase enum and
shared pure layer projection are not yet specified or approved; this Draft
does not authorize their implementation.

Replay from the true hand-start root. Preserve native rule and dealt-count
identity after every fold; do not change the button, switch to a heads-up
profile or discard refunds at a synthetic mid-street root. A strategy's
separately qualified continuation-root derivation must preserve the complete
game semantics and history conditioning; it is not a protocol shortcut.

### Negotiation, capabilities and utility-tagged responses

Use an explicit v2 host selection and generated package. Retain the existing
bounded length-delimited framing and request correlation. Do not autodetect
v2 by trying to parse a v1 frame or silently downgrade a complete-hand request
to incomplete v1. Defaults stay v0/v1 until separate promotion.

Capabilities must list complete supported tuples, not independent booleans
that imply an unimplemented cross-product. Each tuple includes dealt counts,
live counts, streets, stack/side-pot/rule/utility profile, required provenance,
solver mode, guarantee and resource/quality scope. Correctly parsed state
with no qualified six-seat strategy remains unsupported at the decision
boundary. No training or quality result can be relabeled for a different
tuple.

Responses tag utility and units. Cash-chip EV must not become tournament
prize EV. Confidence intervals on learned deviation gains are not an upper
certificate on the optimal best response. Preserve the approved guarantee
ladder and refuse a requested floor that the actual evidence does not earn.

### Failure semantics

Reject malformed bounds/IDs/chips/cards as invalid input. Distinguish missing
or ambiguous history, unavailable rules, replay mismatch, incompatible policy
root, off-tree action, unknown utility and resource/deadline failure. None may
be converted into a successful GTO distribution. A separately labeled legal
operational fallback may keep the controller safe, but does not satisfy the
six-max feature's coverage gate.

### V2 IDL draft (2026-10-16)

The exact v2 wire format is now drafted at
`proto/bigshark/engine/v2/engine.proto`. It passes `buf lint` and `buf build`
without errors. Key design decisions:

**G/F ledger on PlayerState.** `PlayerState` carries `starting_stack`,
`gross_contributed` (G, never decreases) and `refunded` (F, cumulative
uncalled returns). The invariant `starting_stack = stack + G - F` (before
awards) is validated by the host after replaying the transition path.
Seat index (0..9) is the stable hand-local identity; `display_name` is
optional and never used for attribution.

**Phase-tagged pot layers.** `PotState` has a `phase` enum
(`ACTION`/`SHOWDOWN`/`SETTLED`). `PotLayer` has a `status` enum
(`PENDING`/`FINALIZED`), `contribution_level`, `amount`, `eligible_seats`
(1+ items; single-eligible is valid for finalized layers), and optional
`rake`/`winner_seats` for finalized layers only. A sole-contributor PENDING
layer is valid (an open bet); a sole-contributor FINALIZED layer is invalid.
The host must never invoke terminal settlement validation on an ACTION-phase
pot.

**Rule identity.** `RuleProfile` carries `settlement_rule`
(`CLOCKWISE_LEFT_OF_BUTTON`), `reopening_rule` (`NEVER_CUMULATIVE` or
`TDA_2026`), `runout_rule` (`SINGLE`), and `dealt_seat_count` (fixed for the
hand, selects the native heads-up vs multiway profile). `GameDefinition`
requires `rule_profile` in v2.

**Transition path with explicit card boundaries.** `TransitionEvent` is a
oneof of `ActionEvent` (player action) or `BoardDeal` (card deal), making
street/card boundaries explicit. `ActionEvent` uses `actor_seat` (uint32)
instead of v1's `actor_player_id` (string). `incremental_amount` and
`target_total` are `optional` for explicit zero presence.

**Observation provenance.** `ObservationProvenance` carries `source`
(`OFFICIAL_RESPONSE`/`JOURNAL_RECOVERY`/`SUPPLIED`), `coverage`
(`FULL`/`MID_HAND_ATTACH`/`GAP`), `controller_epoch` (opaque identifier),
and `observed_event_count`. An asserted completeness bit is not a proof;
the host validates by replaying the transition path.

**Capability tuples.** `CapabilityTuple` replaces v1's independent booleans.
Each tuple is a complete supported combination: dealt/live player ranges,
streets, max stack, side-pot support, rule profile, utility profile, required
coverage, solver mode, max guarantee, and quality scope.
`GetCapabilitiesResponse` lists tuples, not booleans.

**Utility-tagged responses.** `UtilityProfile` (`CASH_CHIP`/`TOURNAMENT_ICM`)
is on both `DecisionOptions` (requested) and `SolverMetadata` (responded).
Cash-chip EV must not become tournament prize EV.

**New error codes.** `HISTORY_INCOMPLETE` (10), `REPLAY_MISMATCH` (11),
`INCOMPATIBLE_ROOT` (12), `OFF_TREE_AMOUNT` (13), `RULES_UNCONFIRMED` (14),
`UNKNOWN_UTILITY` (15). Each produces a typed error, never a silent fallback.

**Envelope.** `protocol_version` (uint32) replaces v1's `protocol_minor`.
V2 is selected explicitly; no autodetection or silent downgrade.

Resolved open questions: exact field numbers, closed enums, presence/bounds
semantics, phase-tagged layer wire format, provenance wire format, capability
tuple structure, utility tagging, executable conformance vectors, and
version-selection arguments (field 1 = 3 for v2, 0–2 reserved for v0/v1).
Still open: provider rule confirmation and the RFC 0006 tournament-field
gate.

### Native-owned layer projection (2026-10-16)

The native poker module owns all layer computation. The wire `PotLayer`
messages are validated against the native replayed state, never used as the
source of truth for settlement.

**ACTION phase.** The host replays the transition path through `GameState`
and derives provisional contribution levels from each seat's `contributed`
(G) value. It validates:

- `pot_total` equals `sum(G - F)` across all seats (the replayed pot).
- Each PENDING layer's `amount` and `contribution_level` are consistent with
  the replayed per-seat contributions (the layer boundary does not exceed
  any live seat's G).
- A PENDING layer may have a single contributor (an open bet awaiting
  response); this is valid and must not trigger settlement validation.

The host does NOT call `settle_contributions` during ACTION. PENDING layers
are provisional and may change as more chips enter.

**SETTLED phase.** The host calls `settle_contributions` with the replayed
`SettlementInput` (per-seat G/F/stack, button, odd-chip rule, rake). It
validates each FINALIZED layer against the native `PotLayer`:

- `contribution_level` matches exactly.
- `amount` matches exactly.
- `eligible_seats` matches the native `eligible` set (mapped from player
  indices to seat indices).
- `rake` matches the native `rake` for that layer.
- `winner_seats` matches the native `winners` set, including clockwise
  ordering for tied layers.

A sole-contributor FINALIZED layer is invalid: it would be a refund, not a
pot. A sole-eligible FINALIZED layer among multiple contributors is valid (a
single-eligible pot, not a refund).

**Seat index mapping.** Wire `eligible_seats` and `winner_seats` are
hand-local seat indices (0..9). Native `PotLayer.contributors`, `eligible`,
and `winners` are indices into `SettlementInput.players`. The host maps
seat indices to player indices via the `PlayerState.seat` field, which is
the stable hand-local identity.

### Observation provenance behavioral semantics (2026-10-16)

The host's behavior depends on the declared `ObservationCoverage`:

- **FULL**: The host replays the complete transition path from hand start
  through native `GameState`. It validates G/F/stack/pot/board at every
  step. If replay matches the declared current state, proceed to strategy
  lookup/solve. If any step mismatches, return `REPLAY_MISMATCH`. The
  `observed_event_count` must match the replayed path length.

- **MID_HAND_ATTACH**: The host cannot replay from hand start. It requires
  the caller to supply a verified root state (G/F/stack/pot at the attach
  point) and replays from that root. If the root state is absent or
  incomplete, return `HISTORY_INCOMPLETE`. The root state must include
  per-seat G/F, pot layers, and board at the attach point. A later totals
  reconciliation does not cure the missing prefix.

- **GAP**: The host cannot replay through the gap. Return
  `HISTORY_INCOMPLETE`. A higher room revision or matching final totals do
  not prove that the missed actions were observed.

- **JOURNAL_RECOVERY source**: Before using a recovered journal, the host
  validates its ordered coverage, hand binding, and any write/parse/
  truncation gaps. If any gap is found, treat the coverage as GAP. The
  journal's best-effort write semantics mean that a file's existence does
  not prove complete observation.

**Controller epochs.** The `controller_epoch` is an opaque identifier that
changes when the controller restarts or reattaches. The host uses it to
distinguish continuous observation (same epoch, FULL coverage) from
recovered history (different epoch or JOURNAL_RECOVERY source). The host
does not infer completeness from epoch continuity alone; it validates the
replay.

**Restart behavior.** On controller restart, the new controller starts a
new epoch. If it attaches mid-hand, the coverage is MID_HAND_ATTACH unless
an authoritative complete public hand stream is available. The host must
not reuse the previous epoch's observation as if it were continuous.

### Executable conformance vectors (2026-10-16)

This section defines the three required cash conformance vectors and the
version-selection mechanism. Each vector pins literal input, intermediate
G/F and expected pot layers so that a native replay and a cross-language
adapter replay must agree on every value.

**Version selection.** The client selects v2 by setting
`Envelope.protocol_version = 3`. Values 0–2 are reserved for v0/v1
(v1's `protocol_minor` negotiates 0, 1, and 2 on the same field 1).
Using 3 avoids a discriminator collision with v1 minor 2, which is a
live River-adapter contract. The engine-host reads field 1 before
parsing the payload and dispatches: 0–2 to the frozen v1 handler, 3 to
the v2 handler. There is no autodetection: the host never tries to parse
a v1 frame as v2 or vice versa. A v2 request to a v1-only host returns
`ERROR_CODE_UNSUPPORTED_PROTOCOL` — the v1 envelope handler rejects
`protocol_minor > 2` with this typed error before parsing the payload
(`v1_envelope.cpp` lines 518–526). A v1 request to a v2-capable host is
served by the frozen v1 handler. Defaults stay v0/v1 until separate
promotion.

**Capability tuples.** The v2 `GetCapabilitiesResponse` lists complete
supported tuples, not independent booleans. The initial v2 implementation
advertises:

| Tuple | Dealt | Live | Streets | Max BB | Side pots | Rule profile | Utility | Coverage | Solver | Guarantee |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `hu-r1` | 2 | 2 | all | 100 | no | native offline | `CASH_CHIP` | `FULL` | `MULTISTREET_CFR` | `EXACT_SOLVED` |
| `sixmax-r1` | 6 | 2–6 | all | 100 | yes | native offline | `CASH_CHIP` | `FULL` | `RESOLVING` | `ABSTRACT_SOLVED` |

The `sixmax-r1` tuple is advertised only after T2–T5 qualify the actual
strategy. A correctly parsed state with no qualified tuple returns
`ERROR_CODE_INCOMPATIBLE_ROOT`, not a fallback decision.

The host must enforce cross-field constraints that `buf.validate` cannot
express: `min_dealt_players ≤ max_dealt_players` and
`min_live_players ≤ max_live_players` on every `CapabilityTuple`. A
caller that declares an unimplemented rule profile (e.g.
`REOPENING_RULE_TDA_2026`, which has no native implementation) receives
`ERROR_CODE_RULES_UNCONFIRMED` or `ERROR_CODE_UNSUPPORTED_FEATURE`
depending on whether the profile is unknown or known-but-unimplemented.

**Vector conventions.** All amounts are in chips. Blinds are 1/2.
`starting_stack = stack + G − F` must hold for every seat at every
checkpoint (before awards). `pot_total = ΣG − ΣF` must hold at every
checkpoint. Pot layers are ordered by `contribution_level` ascending.
During ACTION all layers are `PENDING`; a `PENDING` layer may have a
single eligible contributor (an open bet). A sole-contributor layer that
survives street closure is refunded (F increases) and removed from the
pot.

#### Vector 1 — ordinary open bet before a response

6-max NLHE cash, blinds 1/2, 100 BB (200 chips) stacks. Button seat 0.
Hero seat 2 (BB).

Forced contributions: seat 1 posts SB 1, seat 2 posts BB 2.

Transition path:

| Seq | Street | Actor | Action | Target total | Pot before | Stack after |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | PREFLOP | 3 | FOLD | — | 3 | 200 |
| 2 | PREFLOP | 4 | FOLD | — | 3 | 200 |
| 3 | PREFLOP | 5 | FOLD | — | 3 | 200 |
| 4 | PREFLOP | 0 | RAISE | 6 | 3 | 194 |
| 5 | PREFLOP | 1 | FOLD | — | 9 | 199 |
| 6 | PREFLOP | 2 | CALL | 6 | 9 | 194 |
| 7 | FLOP | — | BOARD_DEAL [As Kd 7c] | — | 13 | — |
| 8 | FLOP | 2 | CHECK | — | 13 | 194 |
| 9 | FLOP | 0 | BET | 8 | 13 | 186 |

Decision point: seat 2, street FLOP, `to_call = 8`.

G/F ledger:

| Seat | Starting | G | F | Stack | Status |
| --- | --- | --- | --- | --- | --- |
| 0 | 200 | 14 | 0 | 186 | ACTIVE |
| 1 | 200 | 1 | 0 | 199 | FOLDED |
| 2 | 200 | 6 | 0 | 194 | ACTIVE |
| 3 | 200 | 0 | 0 | 200 | FOLDED |
| 4 | 200 | 0 | 0 | 200 | FOLDED |
| 5 | 200 | 0 | 0 | 200 | FOLDED |

Pot = ΣG − ΣF = 21. Layers (ACTION, PENDING):

| Layer | Contribution level | Amount | Eligible |
| --- | --- | --- | --- |
| 1 | 1 | 3 | [0, 2] |
| 2 | 6 | 10 | [0, 2] |
| 3 | 14 | 8 | [0] |

Layer 3 is a single-eligible `PENDING` layer: seat 0's open bet awaiting
response. Legal actions: FOLD, CALL(8), RAISE(min 16, max 194).
Hero hole cards: Qh Qd.

#### Vector 2 — capped call that triggers a refund on street closure

6-max NLHE cash, blinds 1/2, unequal stacks. Button seat 0.
Hero seat 3 (UTG, the bettor who receives the refund).

| Seat | Starting | Role |
| --- | --- | --- |
| 0 | 200 | BTN |
| 1 | 200 | SB |
| 2 | 200 | BB |
| 3 | 200 | UTG |
| 4 | 30 | MP (short) |
| 5 | 200 | CO |

Transition path:

| Seq | Street | Actor | Action | Target total | All-in | Pot before | Stack after |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | PREFLOP | 3 | RAISE | 6 | no | 3 | 194 |
| 2 | PREFLOP | 4 | CALL | 6 | no | 9 | 24 |
| 3 | PREFLOP | 5 | FOLD | — | — | 15 | 200 |
| 4 | PREFLOP | 0 | FOLD | — | — | 15 | 200 |
| 5 | PREFLOP | 1 | FOLD | — | — | 15 | 199 |
| 6 | PREFLOP | 2 | CALL | 6 | no | 15 | 194 |
| 7 | FLOP | — | BOARD_DEAL [Js Jd 4c] | — | — | 19 | — |
| 8 | FLOP | 2 | CHECK | — | — | 19 | 194 |
| 9 | FLOP | 3 | BET | 30 | no | 19 | 164 |
| 10 | FLOP | 4 | CALL | 24 | yes | 49 | 0 |
| 11 | FLOP | 2 | FOLD | — | — | 73 | 194 |

Seq 10 is the capped call: seat 4 calls for 24 (all-in) but the bet is
30. After seq 11 the street closes. Seat 3's bet of 30 was called by
seat 4 for 24 (short by 6) and seat 2 folded. The bet is not fully
called. The sole-contributor layer (level 36, amount 6, eligible [3]) is
refunded to seat 3: F increases by 6.

G/F ledger after refund:

| Seat | Starting | G | F | Stack | Status |
| --- | --- | --- | --- | --- | --- |
| 0 | 200 | 0 | 0 | 200 | FOLDED |
| 1 | 200 | 1 | 0 | 199 | FOLDED |
| 2 | 200 | 6 | 0 | 194 | FOLDED |
| 3 | 200 | 36 | 6 | 170 | ACTIVE |
| 4 | 30 | 30 | 0 | 0 | ALL_IN |
| 5 | 200 | 0 | 0 | 200 | FOLDED |

Invariant checks: seat 3: 200 = 170 + 36 − 6 ✓. Seat 4: 30 = 0 + 30 − 0
✓. Pot = ΣG − ΣF = 73 − 6 = 67.

Layers after refund (ACTION, PENDING — hand runs out board):

| Layer | Contribution level | Amount | Eligible |
| --- | --- | --- | --- |
| 1 | 1 | 4 | [3, 4] |
| 2 | 6 | 15 | [3, 4] |
| 3 | 30 | 48 | [3, 4] |

The refunded sole-contributor layer (level 36, amount 6) is absent from
the pot. This is a replay/settlement verification vector: the engine
replays the transition path, detects the sole-contributor layer at street
closure, applies the refund, and verifies the G/F ledger and remaining
layers. No `DecisionRequest` is emitted because the hand is heads-up with
one player all-in; the native `sixmax_hand_ledger` test exercises this
path through complete legal hand-start paths.

#### Vector 3 — actionable node with an existing side pot plus a new pending wager

Same game and stacks as vector 2. Hero seat 5 (CO).

Transition path (preflop and flop differ from vector 2: seat 5 calls
preflop, seat 2 folds on the flop):

| Seq | Street | Actor | Action | Target total | All-in | Pot before | Stack after |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | PREFLOP | 3 | RAISE | 6 | no | 3 | 194 |
| 2 | PREFLOP | 4 | CALL | 6 | no | 9 | 24 |
| 3 | PREFLOP | 5 | CALL | 6 | no | 15 | 194 |
| 4 | PREFLOP | 0 | FOLD | — | — | 21 | 200 |
| 5 | PREFLOP | 1 | FOLD | — | — | 21 | 199 |
| 6 | PREFLOP | 2 | CALL | 6 | no | 21 | 194 |
| 7 | FLOP | — | BOARD_DEAL [As Kd 7c] | — | — | 25 | — |
| 8 | FLOP | 2 | CHECK | — | — | 25 | 194 |
| 9 | FLOP | 3 | BET | 30 | no | 25 | 164 |
| 10 | FLOP | 4 | CALL | 24 | yes | 55 | 0 |
| 11 | FLOP | 5 | CALL | 30 | no | 79 | 164 |
| 12 | FLOP | 2 | FOLD | — | — | 109 | 194 |
| 13 | TURN | — | BOARD_DEAL [2h] | — | — | 109 | — |
| 14 | TURN | 3 | BET | 20 | no | 109 | 144 |

Seq 10 is a capped call (seat 4 all-in for 24, bet is 30). Seat 5 calls
the full 30 at seq 11, so the bet is fully called: no refund. The capped
call creates a side pot: (30 − 24) × 2 = 12, eligible [3, 5].

Decision point: seat 5, street TURN, `to_call = 20`.

G/F ledger:

| Seat | Starting | G | F | Stack | Status |
| --- | --- | --- | --- | --- | --- |
| 0 | 200 | 0 | 0 | 200 | FOLDED |
| 1 | 200 | 1 | 0 | 199 | FOLDED |
| 2 | 200 | 6 | 0 | 194 | FOLDED |
| 3 | 200 | 56 | 0 | 144 | ACTIVE |
| 4 | 30 | 30 | 0 | 0 | ALL_IN |
| 5 | 200 | 36 | 0 | 164 | ACTIVE |

Invariant checks: seat 3: 200 = 144 + 56 − 0 ✓. Seat 5: 200 = 164 + 36
− 0 ✓. Pot = ΣG − ΣF = 129.

Layers (ACTION, PENDING):

| Layer | Contribution level | Amount | Eligible | Note |
| --- | --- | --- | --- | --- |
| 1 | 1 | 5 | [3, 4, 5] | main pot |
| 2 | 6 | 20 | [3, 4, 5] | main pot |
| 3 | 30 | 72 | [3, 4, 5] | main pot |
| 4 | 36 | 12 | [3, 5] | side pot (seat 4 excluded) |
| 5 | 56 | 20 | [3] | new pending wager |

Layer 4 is the existing side pot from the flop capped call. Layer 5 is
the new pending wager: seat 3's turn bet, single-eligible, awaiting seat
5's response. Legal actions: FOLD, CALL(20), RAISE(min 40, max 164).
Hero hole cards: Qh Qd.

All three vectors are ACTION-phase. During implementation, add a
SETTLED-phase wire vector that exercises FINALIZED layers with rake and
winner_seats, so the cross-language conformance suite covers the
settlement path in addition to the action path. The native
`settle_contributions` function is already tested by
`sixmax_hand_ledger`; the SETTLED vector extends that coverage to the
v2 wire format.

## Dependency Rules

Keep `poker <- solver <- policy/resident <- service` and
`runner -> River adapter -> generic engine client`. Generated v2 messages
remain within protocol/client boundaries. Protocol can invoke poker replay;
poker cannot depend on wire types or River text. The adapter owns provider
fact normalization; the generic client must not learn the event grammar.

The runner keeps one controller and its existing official CLI. No provider
web automation, private state or undocumented HTTP client is introduced.

## Compatibility and Migration

Freeze v0, every v1 minor and accepted v1 fixture. Generate v2 separately with
its own descriptor baseline and cross-language conformance vectors. Default
launch behavior and existing runtime action override contract remain intact.
Existing artifacts are not rewritten or reinterpreted as side-pot-safe,
folded-root or revised-rule artifacts.

Any necessary native rule correction needs a separately reviewed profile
identity/migration design; old artifacts and certificates cannot silently
inherit it. Whether this belongs in a dedicated RFC or a fully specified
revision of this Draft is an open decision. No existing accepted RFC's design
is edited in place.

## Security and Operational Impact

Only official public responses and the hero's own cards may enter this path.
No credentials enter engine requests or new diagnostic records. Public
player names are not stable engine IDs; identity normalization is scoped to a
hand and must remain unambiguous. Journals stay local with restrictive modes.
Enforce maximum frame, history, snapshot and allocation sizes before work.

Receipt-to-action latency includes observation parsing, reconstruction,
lookup/construction/solve and final submission. New observation work must not
create a second polling controller or an unbounded synchronous disk path.
Large campaigns and live promotion remain separate authority/resource gates.

## Alternatives Considered

- Extend v1's fields and remove refusal: violates its established meanings
  and admits incomplete legacy requests.
- Mirror poker rules in TypeScript: duplicates actor, refund, reopening and
  pot invariants; violates the one-native-core boundary.
- Archive only actionable snapshots: misses public transitions before and
  after the hero's turn and cannot prove a full hand path.
- Treat the existing journal as an unconditional proof: its writes are
  best-effort and do not establish every intervening action was observed.
- Rebase from equal pot shares or force the current actor with a new button:
  changes contributions, position and odd-chip identity.
- Defer all input work until a large six-seat training run: expends resources
  before proving the trained game matches the user's actual table.

## Risks

- Official public data may not uniquely reconstruct an unseen prefix.
  Demonstrate the actual data requirement rather than invent facts.
- Native offline rules may differ from the provider. Preserve identity and
  investigate before implementing a production rule profile.
- A complete physical ledger can still have wrong ranges or unqualified
  quality; require independent strategy and feature-wide qualification.
- Whole-hand replay and observation history can consume decision time.
  Bound resources and measure the entire path under six-seat normal stacks.
- New required semantics have a wide compatibility surface. Separate version
  selection, fixture baselines and rollback keep old callers isolated.

## Verification Plan

Use project-native C++/Node/protocol tests and shared conformance vectors.
Cover all six buttons, sparse/shuffled IDs, folded/dealt retention, 2..6 live
players, unequal stacks, capped calls, short all-ins, raise-right histories,
refunds across streets, folded dead money, odd chips and single-eligible pot
layers. Pin literal input, intermediate G/F and final native state, not only
a self-generated snapshot accepted by its own producer.

Candidate literal ledger vectors include:

- Six seats with `G=[100,60,0,0,0,0]`, `F=[40,0,0,0,0,0]` and the other four
  already folded: net contested pot 120, seat 0 winning awards 120 and cash
  utilities `[60,-60,0,0,0,0]`. Current stack already includes the 40 refund.
- `G=[140,100,60,20,20,0]`, `F=[40,0,0,0,0,0]`, with seats 1/3/4/5 folded:
  net layers 100/120/80, eligibility `{0,2}` / `{0,2}` / `{0}`. If seat 2 is
  stronger, awards are `[80,0,220,0,0,0]`; the last 80 is a single-eligible
  pot funded by two players, not an additional refund.
- A tied layer with folded dead money and odd chips, rotating the actual
  physical button: awards depend on clockwise winner order, not input vector
  ordering or a resolver's synthetic button.

These are arithmetic expectations, not executed protocol vectors. The native
`sixmax_hand_ledger` test now exercises these scenarios through complete legal
hand-start paths and genuine forced payments, including the original six
dealt seats. V2 cross-language conformance, provider equivalence and complete
strategy/feature evidence remain unverified.

Test observation intake at every controller branch, startup/warmup gaps,
mid-hand attach, duplicate/reordered event text, display-name ambiguity,
heartbeat references, changing hands, failed/uncertain acknowledgements,
stale recovery and truncated/missing journal recovery. A missing earlier
prefix cannot turn complete merely because later totals reconcile.

Add invalid numeric/card/ID/profile cases, failed replay scratch cleanup,
unknown-version/unknown-enum/unknown-field vectors and no-silent-downgrade
tests. Run root release and true Debug ASan/UBSan full suites, Node checks,
protocol checks, multistreet benchmarks, replay, docs/RFC and diff checks.
Engineering mocks are not six-seat policy quality or live provider evidence.

## Rollout Plan

1. Complete the draft's exact wire and public-observation semantics and rules
   audit; independently approve this RFC before any implementation.
2. Implement native conformance/replay and observation tests at the owning
   boundaries; keep incomplete requests and unqualified combinations refused.
3. Qualify actual rule/utility identity and strategy integration through T2..T6
   and the independent quality plan; do not close the user feature early.
4. Complete six-seat practice and end-to-end acceptance, immutable release
   selection and rollback tests. Live promotion requires separate authority.

## Rollback Plan

Restore explicit v0/v1 client and host selection; leave original descriptors,
golden fixtures and artifacts untouched. Discard an untrusted observation
epoch rather than reuse it under a different rule or hand identity. Any
future persisted observation format must have its own atomicity, recovery and
rollback specification before it is written.

## Open Questions

- ~~Exact v2 field numbers, required/presence semantics, closed enums and
  annotated numeric/length limits are not finalized.~~ **Resolved 2026-10-16:**
  the v2 IDL is drafted at `proto/bigshark/engine/v2/engine.proto` and passes
  `buf lint` and `buf build`. See the [V2 IDL draft](#v2-idl-draft-2026-10-16)
  section.
- ~~Native-owned layer projection: how the host maps wire PotLayer to native
  PotLayer.~~ **Resolved 2026-10-16:** see the [Native-owned layer
  projection](#native-owned-layer-projection-2026-10-16) section.
- ~~Controller epochs, authoritative attribution/coverage, gaps and restart
  behavior.~~ **Resolved 2026-10-16:** see the [Observation provenance
  behavioral semantics](#observation-provenance-behavioral-semantics-2026-10-16)
  section.
- ~~Define executable cash conformance vectors and the exact version-selection
  arguments and capability tuples before review for implementation.~~
  **Resolved 2026-10-16:** see the [Executable conformance
  vectors](#executable-conformance-vectors-2026-10-16) section. Version
  selection is `Envelope.protocol_version = 3` (0–2 reserved for v0/v1
  to avoid the v1 minor-2 discriminator collision) with no autodetection;
  the initial capability tuples are `hu-r1` and `sixmax-r1`.
- ~~Establish which official public fields/transitions can prove full-history
  completeness and which cases require additional authoritative public data.~~
  **Static audit completed 2026-10-24** (the provider public-field audit
  document was deleted with the integration by
  [RFC 0040](0040-retire-river-club-agent-api.md); this bullet stays as
  dated evidence).
  The revision spine, decision legality, current-state fields, and the
  old-events-are-omitted guarantee are settled; `room.pots`, `winners`, the
  event-kind catalog, rake/ante/refund/runout rules, and event-window
  retention are undocumented or unsettled and are the remaining provider
  data gate.
- Confirm River settlement/rake/ante/reopening/runout rules from authoritative
  public sources; current local documentation does not settle these questions.
- Decide and approve any native rule/profile migration without invalidating
  legacy artifacts. Do not disguise differences in the protocol mapper.
- Resolve RFC 0006's tournament-field gate: cash-only v2 versus a complete
  extended-utility wire family. The current River integration exposes chip
  tables, not a complete tournament field. The user's six-player goal did
  not explicitly name tournament scope; record required provider variants in
  T0 rather than use this cash-hand draft to declare the full feature covered
  or silently expand into tournament delivery.
- Finalize artifact identity and continuation-root compatibility requirements
  with T2; existing roots do not establish the required six-seat coverage.

## Acceptance Criteria

- Exact field/enum numbers, presence/bounds, conformance vectors, capability
  tuples and version/rollback rules receive independent approval.
- Public observations yield a validated full hand path or a typed, visible
  incomplete result; no guessed actors, rules or missing actions.
- Native replay preserves all dealt seats, button, G/F, eligibility, current
  status and individual action rights without a changed-game rebase.
- All required six-player cash utility/rule combinations are qualified with
  unchanged legacy callers and immutable artifact semantics.
- Same-source full native/sanitizer/Node/protocol verification passes and
  independent source/evidence review approves the implementation.
- Complete six-max promotion additionally satisfies every T0..T8 feature
  gate; wire/replay acceptance alone is never feature completion.

## Decision

Author agent: Codex /root (Claude session)
Approved by: independent approval agent a2537065e953cd9ce
Decision date: 2026-10-16
Review outcome: Approved
Reviewed scope: Full RFC text, v2 proto IDL, v1 proto compatibility, native
settlement and game-definition types, live-callers and dispatch evidence,
conformance vector arithmetic (independently verified by script), RFC
process and review standard.
Review summary: First review returned Changes Requested with one blocking
finding (Envelope field-1 discriminator collision with v1 minor 2, a live
River-adapter contract). Fixed by reserving 0–2 for v0/v1 and selecting v2
with `protocol_version = 3`; the v1 host's typed `UNSUPPORTED_PROTOCOL`
path for minor > 2 was verified at `v1_envelope.cpp:518-526`. All seven
non-blocking observations resolved (PotLayer comment, index entries, buf
lint/build, CapabilityTuple min≤max host validation, TDA_2026 typed-error
path, SETTLED-phase wire vector commitment, untracked proto acknowledged).
All four P1/P2 Initial Design Checkpoint findings are addressed at the
wire/behavioral/vector level; residuals are external dependencies
(provider completeness proof, provider rule confirmation, RFC 0006
tournament gate, T2 artifact identity). Conformance vector arithmetic
independently verified: all three vectors pass. Remaining risks: provider
data and rules are external (design fails closed); whole-hand replay
decision-time cost needs benchmark measurement; `sixmax-r1` is gated on
T2–T5, so wire approval is not feature completion.

## Initial Design Checkpoint

Independent read-only review by `/root/sixmax_runtime_audit` on 2026-10-14:
direction reasonable; retain Draft, no RFC acceptance or implementation
approval. Material findings before Proposed:

- P1, pending wager versus terminal layer: this draft now explicitly forbids
  premature settlement/refund and requires phase-tagged layer semantics.
  Exact field/enums, a native-owned projection and executable vectors remain
  open; do not claim this finding fully closed.
- P1, provenance trust boundary: a stateless host cannot prove a client
  omitted no public history merely because supplied actions replay. Define
  controller epochs, authoritative attribution/coverage, gaps and restart
  behavior; no speculative proof framework. Still open.
- P1, provider/native/artifact rule identity: freeze authoritative provider
  rules, native differential coverage and identity migration before changing
  existing profiles or accepting old certificates. Still open.
- P2, cash/ICM and capability combinations: resolve RFC 0006's coherent wire
  gate and the target table variants without silently expanding tournament
  scope or closing the full feature from a cash-only draft. Still open.

The reviewer confirmed the official TDA comparative citation independently;
it is not a River rule declaration. No v2 IDL, host/client, provider observer,
native rule migration or artifact change has been implemented from this draft.

**Progress update (2026-10-16):**

- P1, pending wager versus terminal layer: the v2 IDL now specifies exact
  field/enums (`PotLayer.status`, `PotState.phase`), the native-owned
  layer projection (see [Native-owned layer
  projection](#native-owned-layer-projection-2026-10-16)), and three
  executable conformance vectors covering an open bet, a capped-call
  refund and a side-pot-plus-pending-wager node (see [Executable
  conformance vectors](#executable-conformance-vectors-2026-10-16)).
  This finding is addressed at the wire, behavioral and vector level.
- P1, provenance trust boundary: the v2 IDL now specifies
  `ObservationProvenance` with source/coverage/epoch, and the behavioral
  semantics define host behavior for each coverage value (see [Observation
  provenance behavioral
  semantics](#observation-provenance-behavioral-semantics-2026-10-16)).
  This finding is addressed at the wire and behavioral level; provider
  data completeness proof remains open.
- P1, provider/native/artifact rule identity: the v2 IDL now specifies
  `RuleProfile` with settlement/reopening/runout identity. Provider rule
  confirmation and native differential coverage remain open.
- P2, cash/ICM and capability combinations: the v2 IDL now specifies
  `UtilityProfile` and `CapabilityTuple`. The RFC 0006 tournament-field
  gate remains open.
