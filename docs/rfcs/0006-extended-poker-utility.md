---
rfc: "0006"
subject: "Extended Poker Utility and Multiway Evaluation"
status: "Implementing"
authors: "BigShark maintainers"
created: "2026-09-14"
updated: "2026-09-14"
owners: "engine poker, engine solver, engine benchmarks, protocol, engine service"
supersedes: ""
superseded-by: ""
---

# RFC 0006: Extended Poker Utility and Multiway Evaluation

## Summary

Extend the canonical poker rules with contribution-based pot settlement,
declared rake, and tournament Independent Chip Model (ICM) utility. Add an
experimental multiway trainer and independent deviation evaluation using the
same rules. These are separately gated features: a correct payout calculator
does not establish a strong policy or authorize production capability claims.

## Motivation

The live engine approximates multiway decisions through equity and heuristics.
Its current contexts cannot reconstruct all side-pot eligibility or complete
tournament equities. Reusing the heads-up zero-sum solver without changing
terminal utilities and evaluation would produce misleading results.

## Goals

- Settle main and side pots with exact integer chip conservation.
- Compute declared capped rake and explicit net chip utility.
- Compute exact ICM payout equity for a bounded remaining tournament field.
- Train and evaluate a declared multiplayer action abstraction experimentally.
- Make unsupported rules and insufficient provider data explicit.
- Define the production protocol gate without changing v1 semantics.

## Non-Goals

- Claiming general convergence of multiplayer CFR to a Nash equilibrium.
- Treating variable rake as a constant-sum two-player game.
- Reusing heads-up resolving safety for multiway or ICM utility.
- Bounties, progressive knockouts, re-entry, insurance, run-it-twice, mixed
  games, or tournaments with unavailable complete chip information.
- Weakening current v1 field semantics or silently fabricating contributions.

## Current State and Evidence

- `engine/include/bs/decision.hpp` has pot total and player count, but lacks
  per-player hand contributions and side-pot eligibility.
- `engine/src/gto/river_game.cc` returns `{u, -u}` and assumes two-player
  zero-sum utility.
- `proto/bigshark/engine/v1/engine.proto` carries rake and side pots but no
  complete tournament field, payout vector, or explicit main-pot eligibility.
  `SidePot.eligible_player_ids` requires at least two entries; a settled layer
  with a single eligible winner cannot be encoded there without losing meaning.
- `docs/design/gto-engine.md` explicitly excludes these utility models.
- RFC 0004 supplies common chip transitions; RFC 0005's safety gate applies
  only to its modeled heads-up zero-sum game.

## Design Principles

1. Chip transitions and payouts are rules, not strategy heuristics.
2. A folded player's contribution stays in pots but they cannot win.
3. No unmatched wager is raked or counted as a contested pot.
4. Every utility has explicit units and a reference point.
5. Multiway quality requires independent deviation tests and adversaries.
6. Missing tournament or contribution data yields unsupported, not a guess.

## Proposed Design

### Domain ownership

Extend the RFC 0004 poker rules directly with a player vector and contribution
ledger. Do not create a second settlement engine or provider-specific pot
calculator in the solver.

`bigshark_poker` owns chip/rake settlement and ICM arithmetic.
`bigshark_solver` owns multiway sampling, policies, and evaluation.
`bigshark_service` checks game/feature support and complete input.
Adapters supply public facts and enforce platform action legality.

Shared changes are necessary because terminal evaluation, replay, and every
solver must agree on chip conservation. Local copies inside a multiway solver
or River adapter would duplicate the central invariant.

### Contribution-based settlement

Track each player's gross hand contribution `G`, cumulative uncalled refunds
`F`, net contribution `N = G - F`, seat, and status. `G` never decreases;
refunds are recorded once in `F`. Current stack includes refunds already
returned. Before terminal awards are credited, hand-start chips equal
`current_stack + G - F`. After awards are credited, subtract those credited
awards from the right-hand side to recover hand-start chips.
After identifying genuinely unmatched excess, sort unique positive net
contribution levels `l1 < ... < lk`, with `l0 = 0`.
For each level, form the layer:

`amount(k) = (lk - l(k-1)) * count(N >= lk)`.

All contributing players fund a layer, including folded players.
Eligible winners are non-folded players whose net contributions reach that
level. A layer with one eligible player is awarded directly; a layer with
multiple eligible players goes to the strongest hand or splits among ties.
A layer with no eligible players indicates invalid history and is rejected.
The rules transition identifies uncalled excess before this construction;
one eligible player alone does not imply the amount is uncalled.

For ties, divide integer chips equally. Allocate remainder chips in clockwise
seat order starting left of the button, restricted to tied winners. The
settlement rule identifier is required; providers with different odd-chip
rules are unsupported until explicitly modeled.

Verify:

`sum(awards) + rake + sum(F) = sum(G)`.

For each player, chip utility is `awards + F - G`.
All chips and intermediate products use checked integer arithmetic.
For example, gross contributions `[100, 60]` with refunds `[40, 0]`
produce net contributions `[60, 60]` and one 120-chip pot. Without rake,
awards plus the 40-chip refund equal the 160 gross chips. Settlement must
not return the 40 chips twice when current stacks already include the refund.

### Rake

The initial supported policy is pot-percentage rake with basis points,
an atomic-unit cap, floor rounding, and an explicit no-flop-no-drop flag.
After refunds, let `P` be the sum of contested/awarded pot layers:

`R = min(cap, floor(P * basis_points / 10000))`.

No-flop-no-drop makes `R = 0` if the hand ended before a flop was dealt.
Cap zero means zero permitted rake; unlimited rake is not inferred.
Reject negative, overflowing, or unsupported policies.

Allocate `R` proportionally across layers using floor division; distribute
remaining rake units by descending fractional remainder, ties from the
lowest contribution layer upward. Award each resulting net layer using
its eligibility set. Validate `sum(chip_utility) = -R`.

Different house practices (per-pot caps, nearest rounding, fixed fees, or
winner-specific deductions) need distinct rule identifiers and fixtures.
Do not advertise rake support for a provider whose policy is unknown.
No rake is allowed in the first tournament ICM profile.

### ICM

ICM estimates prize equity from the complete remaining field's chip counts
and ordered payouts. It does not account for future position, skill, blind
increases, or every tournament incentive.

The first exact implementation supports 2..10 remaining players, an ordered
non-increasing payout vector, and a separate payout amount unit. Require
complete public chip counts for players at other tables and stable IDs.
For positive remaining stacks:

`Pr(player i finishes first) = chips(i) / sum(chips)`.

Recursively remove the first finisher and apply the same rule to subsequent
places. Sum prize-weighted placement probabilities, memoizing subsets.
For positive stacks the equities sum to the remaining prize pool. Equal
stacks with equal eligibility imply equal equities.

At a terminal hand, first settle chips. Process newly busted players before
ICM recursion: fewer starting chips means the lower finish; equal starting
chips split the corresponding prizes equally, with payout-unit remainders
assigned by the declared tournament tie rule. Elimination order and hand-start
stacks are required input. Tournament formats using another rule are
unsupported. The first supported tie rule uses stable player-ID byte order
only when the provider explicitly uses that rule; otherwise tied eliminations
are rejected until its rule is modeled.

Tournament utility is terminal prize equity (including prizes allocated to
newly busted players) minus pre-hand prize equity, in payout atomic units.
Never encode this value in a chip-EV field. Payout rounding occurs only on
actual awarded prizes; expected equities use finite doubles. The numeric
profile limits chips and payout totals to `2^53 - 1`.
Larger fields, unknown remote stacks, bounties, or missing payout data return
unsupported; no unstated Monte Carlo ICM approximation.

### Multiway training and range semantics

Extend the common game to 3..6 players for the first experimental profile.
Retain all player-specific stacks, raise rights, observations, and full
public history. Preflop actor order starts left of the big blind; postflop
starts left of the button, skipping players who cannot act.
Apply the provider-independent full-raise reopening rule; cumulative short
all-ins must be tested for each player's individual raise rights.

Sample joint private deals proportional to the product of supplied range
weights conditioned on mutual card compatibility. Never independently sample
each hand after renormalizing only the last player's pool. Maintain one joint
deal per traversal. The public policy identity includes all ranges and rules.

Use external-sampling regret minimization as an experimental learning method,
with full average-policy updates on small validation games. Any sampled
multiway averaging optimization must match that reference before scaling.
Variable rake and ICM use their own payoff vector, never `{u, -u}`.

The two-player zero-sum convergence theorem does not extend here.
Evaluate unilateral deviations against the fixed opponents' policy profile
on small enumerable games, report each player's gain and their sum
(NashConv), and report numerical/estimation error separately.
Large games additionally use predeclared adversarial policies, held-out seeds,
seat permutations, and confidence intervals. Winning self-play results alone
are not an equilibrium certificate.

### Production contract gate

Implement and test domain utility before adding production wire support.
The first production extension uses a new `bigshark.engine.v2` package because
complete contribution, pot eligibility, tournament state, and utility units
must be required coherently. Do not relax or reinterpret v1 annotations.

Before implementing the v2 IDL, author a follow-up protocol RFC defining exact
field numbers and conformance vectors for:

- complete per-player hand contributions and root-state completeness;
- eligibility on every pot layer, including a single eligible player;
- settlement/rake/tournament rule identifiers;
- full remaining tournament field, payouts, units, and hand-start stacks;
- utility-tagged expected values and quality/experimental feature status;
- feature combinations supported by each solver, not independent booleans
  that imply unsupported combinations;
- v1/v2 host selection, capability negotiation, and independent rollback.

That IDL is deliberately a separate approval gate: provider settlement rules
and complete input availability are not established in the current codebase.
This RFC approves the domain models and experimental evaluation only.
Production rake, side-pot, multiway-solving, and ICM capabilities remain
disabled until the follow-up protocol and adapter gates pass.

## Dependency Rules

Extend `poker <- solver <- policy <- service` without introducing a utility
plugin framework. No generated wire types, SQLite handles, or platform
identifiers in the core. Existing public poker value types remain usable by
the heads-up trainer. Tests for arithmetic belong to poker, not River.

## Compatibility and Migration

Keep the current v1 and v0 behavior unchanged for supported legacy contexts.
Reject unsupported extended features at the service boundary.
RFC 0005 no-rake artifacts remain readable but cannot match an extended
utility game. Utility and settlement identifiers are part of artifact
identity; extended artifacts require a new schema revision where necessary.
Never reuse chip-EV benchmarks to validate prize-EV.

## Security and Operational Impact

Only public contributions and public tournament counts may enter requests.
Unknown opponents' cards are sampled internally for training, never acquired
from a live platform. Cap field sizes, recursion state, player counts, and
training allocations. No live play or external spending is authorized by
these domain changes.

Log selected utility/rule IDs, unsupported reason, arithmetic validation
errors, evaluation method, and cost. Do not log player identities or raw
private cards for model profiling.

## Alternatives Considered

- Subtract a fixed rake from heads-up utility: wrong when the drop depends on
  the terminal pot or whether a flop was dealt.
- Split the whole pot among non-folded players: wrong for unequal all-ins.
- Substitute chip equity for ICM: loses nonlinear prize incentives.
- Advertise multiway GTO after self-play training: lacks a justified guarantee.
- Redefine v1 side pots and EV fields: breaks their established semantics.
- Design a comprehensive tournament framework now: unnecessary for the
  bounded exact model and missing provider evidence.

## Risks

- House-rule differences can invalidate exact arithmetic. Identify each
  rule and fail closed on unknown variants.
- Multiway CFR can fail to converge. Treat quality as measured experimental
  behavior; do not promote on iteration count alone.
- Exact ICM grows exponentially. Enforce the 10-player limit before allocation.
- Complete tournament information may be unavailable. This is a production
  integration blocker, not permission to fabricate or scrape private state.
- New utility rules invalidate old certificates. Scope every guarantee to
  its game and utility identity.

## Verification Plan

Run the full native build contract, sanitizers, and the unchanged historical
replay. Add table-driven payout examples and exhaustive small contribution
grids in CTest, including folded contributors, unequal stacks, ties,
single-eligible layers, uncalled refunds, and overflow.

Test capped/uncapped-by-value rake examples, no-flop-no-drop, proportional
allocation rounding, and exact conservation. Test ICM two-player analytic
results, equal stacks, equal payouts, payout monotonicity, simultaneous bust
handling, and prize-pool conservation within `1e-10` relative tolerance.

For multiway, include a small three-player game with independently enumerated
unilateral deviations and a complete utility vector. Release evaluation must
include fixed seeds 1, 17, and 43, seat permutations, zero private-card overlap,
probability normalization, and repeatability within `1e-12` on the same build.
Report failure to reduce deviation gains as a failed quality gate, not an
implementation success. Before any production promotion, freeze a named
fixture suite and threshold in a reviewed release profile.

## Rollout Plan

1. Contribution ledger, refunds, pot layers, and payout invariants.
2. Declared rake arithmetic and isolated tests.
3. Exact bounded ICM arithmetic and prize-unit tests.
4. Multiway rules, training, and independent deviation evaluation.
5. Provider data/rule audit and follow-up v2 protocol RFC.
6. v2 conformance, adapter replay, supported-profile quality gates, then
   separately approved live promotion.

No arithmetic milestone alone enables production strategy capabilities.

## Rollback Plan

Keep extended utility opt-in and restore the prior no-rake policy profile.
Retain prior artifact generations. A v1 host remains available during any
future v2 rollout. Unavailable required utility returns unsupported instead
of silently applying a legacy approximation.

## Open Questions

- Which provider can supply complete public contributions and tournament
  field data? Production protocol/adapter design waits for that audit.
- What house rules govern simultaneous bust ties and rake allocation?
  The declared experimental rules must not be presumed universal.
- What resource budget and held-out adversaries define production multiway
  quality? Until approved, this remains an offline experimental capability.

## Acceptance Criteria

- Settlement preserves exact chip conservation across all supported cases.
- Rake follows the declared rule and is included in terminal utilities.
- Exact bounded ICM passes analytic and independent enumeration checks.
- Multiway training uses a valid joint deal and passes independent evaluation.
- Quality, supported rules, units, and limitations are documented honestly.
- Production extensions stay disabled until separately specified and verified.
- No change weakens the existing heads-up or legacy protocol gates.

## Decision

Author agent: /root
Approved by: 7f86ec31-e5b6-4753-a478-a48366ac2edd
Decision date: 2026-09-14
Review outcome: Approved
Reviewed scope: Complete domain settlement, declared rake, bounded exact ICM, and experimental multiway proposal; pre-decision SHA-256 befc6afb522be2d1657b44ca21611196b00350a1d22729a9929d98d27f4383cf.
Review summary: Formal independent review confirmed consistent G/F/N accounting, payout reference, separate ICM units, and experimental multiway limits. Provider rules, complete tournament data, nonconvergence, and production release thresholds remain risks. Production v2, provider integration, and live promotion require separate gates.

Design approval only. No extended production capability or completed numeric
validation is implied.
