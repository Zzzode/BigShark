---
rfc: "0008"
subject: "Unified N-Player Engine Architecture"
status: "Proposed"
authors: "BigShark engine agent"
created: "2026-09-19"
updated: "2026-09-19"
owners: "Poker, solver, policy, service, protocol, artifact boundary"
supersedes: ""
superseded-by: ""
---

# RFC 0008: Unified N-Player Engine Architecture

## Summary

Restructure the engine around the one thing it is currently missing: an
**explicit, versioned abstraction layer**. Today the rules exist twice
(`HeadsUpState` and `MultiwayState`), the solver family is five mutually
incompatible entry points, the preflop charts are hard-coded C++ tables, and the
heuristic is indistinguishable from a solver in the decision path. The proposal
introduces one game definition parameterized by seat count, one abstraction
layer over it, one solver interface that sees a tree rather than a player count,
and one explicitly-labeled guarantee level on every decision the engine
returns. It then removes the duplicated and superseded paths in dependency
order, with each removal gated on the replacement being verified.

The target is a 2..10 seat engine whose strength degrades along a declared,
measurable gradient instead of a table-and-heuristic fallback.

## Motivation

The engine cannot grow toward larger tables because the layer that would make
larger tables tractable does not exist. Concretely, measured in this repository:

- **The rules are implemented twice.** `HeadsUpState`
  (`engine/include/bs/heads_up.hpp`, 19 member functions) and `MultiwayState`
  (`engine/include/bs/multiway.hpp`, 22 member functions) share twelve member
  functions that each implements separately: `actor`, `pot`, `can_raise`,
  `legal`, `pay`, `refund_unmatched`, `close_street`, `after_action`,
  `after_card`, `award`, `settle_fold`, and `settle_showdown`.
  RFC 0004's design principle 2 requires "all strategies use the same legal
  transition function"; the repository currently violates it.
- **There is no abstraction layer.** `grep` for a card or action abstraction
  finds only unrelated hits; bucket logic exists inside the experimental
  multistreet validation game, not as a shared component. RFC 0004 defers
  "lossy card buckets" to "a subsequent design and measured abstraction error"
  and that design is this RFC.
- **The solver family has five incompatible interfaces.** `river_gto`
  (`SolveRiver`, a river-specific `RiverNode` enumeration), `cfr_solver`
  (bounded full-tree DCFR), `multistreet_cfr` (experimental validation game),
  `heads_up_solver` (multi-size CFR with an `InformationKey` and a
  `HeadsUpPolicy`), and `multiway_sampler` (joint-deal enumeration). None share
  a policy or tree type.
- **The preflop charts are compiled in and self-described as approximations.**
  `engine/src/poker/charts.cpp` is 98 lines that build a `Charts` value from 22
  literal range-spec strings ("22+ A2s+ KTs+ ...") parsed by `parseRange` into
  `Range169` sets, consumed directly by `preflop()` in
  `engine/src/policy/decision.cpp`. The header itself calls them
  "solver-approximation preflop charts" and scopes them to 6-max 100bb
  (`engine/include/bs/charts.hpp:1-2`), yet the decision that consumes them
  reports nothing about their approximation status - the same defect as the
  heuristic's, an approximation with no label.
- **The heuristic is not distinguishable from a solver.** `decision.cpp` routes
  by street (preflop to charts, river to `river_gto`, everything else to
  heuristics) and returns a `Decision` with no statement of what, if anything,
  is guaranteed about it.
- **The settlement layer stops at six seats.**
  `engine/include/bs/settlement.hpp:67` states "Supports 2..6 players", while
  the platform's own room schema allows `maxPlayers` up to 10
  (`docs/openapi.json:93`). The engine therefore cannot represent the largest
  table it can be asked to play.

The cost of the current shape is not aesthetic. It means every new capability
must be implemented per seat count, every solver has its own correctness story,
and a reader of a decision cannot tell whether it came from an equilibrium
computation or a hand-written rule.

## Goals

- One game definition for 2..10 seats, with one legal-transition implementation
  and one settlement path, such that `HeadsUpState` and `MultiwayState` become
  configurations of it rather than separate types.
- An abstraction layer (card and action) that is explicit, versioned, bounded,
  and carries a **measured** error - a bound where an exact solve exists to
  compare against, and a labeled estimate where none does - so its effect on
  decision quality is reported rather than assumed.
- One solver interface that takes an abstract tree and returns a policy, so a
  new solver is added without touching the rules, the abstraction, or the
  decision path.
- Every decision the engine returns carries an explicit **guarantee level**
  drawn from a closed set, so no approximate source can be mistaken for an
  equilibrium one.
- The superseded paths are removed in dependency order, each removal gated on a
  verified replacement.

## Non-Goals

- Claiming a 10-seat equilibrium. Multi-seat CFR has no convergence guarantee,
  and RFC 0006 states plainly that the two-player zero-sum theorem does not
  extend. This RFC builds the structure that lets measured quality be reported
  at larger seat counts; it does not assert a quality result it has not measured.
- Preserving v0 on the wire beyond the removal stage below. v0 is what stage 7
  deletes, so it cannot remain the default: that stage includes flipping the
  default to v1, and says so rather than leaving it implied. It is the one
  behavior change in this RFC that is not additive, which is why it is gated
  with the removal rather than with the decision service.
- Replacing the heuristic with a solver in states where no solver applies at all.
  The heuristic remains available as a labeled source at every seat count; what
  this RFC removes is its standing as an unnamed policy indistinguishable from an
  equilibrium, not the fallback itself. Where a solver does apply, stage 6
  measures it against the heuristic and reports which one wins.
- Tournament ICM, rake, and utility-model changes; RFC 0006 owns those.
- Live promotion of any new profile. That remains a separate gate.

## Current State and Evidence

Verified at commit `70491a9`:

- Rules split: `engine/include/bs/heads_up.hpp` and
  `engine/include/bs/multiway.hpp`; implementations as cited above. The
  heads-up type additionally carries two root profiles (`preflop` and
  flop-rooted) and the multiway type carries 3..6 seats.
- Settlement: 2..6 seats, with earlier stages having added side pots, rake, and
  bounded ICM (§ Stage 11, Stage 12).
- Solver interfaces: as listed in Motivation, verified by reading each header.
- Decision routing: `engine/src/policy/decision.cpp` selects by street; the
  preflop branch consumes `charts()`, the river branch calls into `gto`, and the
  remainder falls through to heuristic helpers in the same file.
- Protocol: production traffic uses v0 NDJSON
  (`engine/src/protocol/v0_json.cpp`, 132 lines) with a v1 Protobuf contract
  implemented behind it (RFC 0002 Stages 7-8). The roadmap already records v0
  removal as `P8`, gated on "no remaining callers, rollback release artifact".
- Module dependencies (from `engine/CMakeLists.txt`): `solver -> poker`,
  `artifacts -> solver`, `resolver -> solver`, `resident -> artifacts +
  resolver`, `policy -> poker`. No abstraction component exists in this graph.

Assumptions, stated as such: that an action abstraction is sufficient to make
multi-seat trees tractable at some declared quality, and that a card abstraction
with a bounded error can be built over the existing range representation. Both
are to be measured in the stages below rather than assumed; the second is the
known-hard part and is called out as a risk.

## Design Principles

1. **One rules implementation.** Seat count is a parameter of a game definition,
   never a second copy of the transition function.
2. **Abstraction is explicit and versioned.** An abstracted game carries its
   abstraction identity and its measured error alongside it, so a policy can
   never be read as a statement about the unabstracted game.
3. **Solvers face trees, not player counts.** A solver's input is an abstract
   tree plus ranges plus limits; nothing in a solver signature mentions heads-up
   or multiway.
4. **Guarantee levels are a first-class output.** Every decision states what is
   guaranteed about it, from a closed set, and the set is ordered by strength.
5. **A heuristic is a strategy source, not a solver.** It is declared, labeled,
   and measured like any other source.
6. **Removal follows replacement.** A superseded path is deleted only after the
   replacement is verified against it, and the verification is recorded.

## Proposed Design

### Layering

```mermaid
flowchart TD
  Eval[L0 hand evaluation] --> Game[L1 game definition, N seats]
  Game --> Abs[L2 card + action abstraction, versioned, measured]
  Abs --> Tree[L3 abstract betting tree]
  Tree --> Solver[L4 solver interface: tree + ranges + limits -> policy]
  Solver --> Store[L5 immutable policy storage]
  Store --> Decide[L6 decision service: state -> abstract -> lookup -> action + guarantee]
  Decide --> Wire[L7 protocol and adapters]
```

| Layer | Owns | Must not own |
| --- | --- | --- |
| L0 `bigshark_poker` evaluation | Card ids, hand ranking | Anything about betting or strategy |
| L1 game definition | Legal transitions, chip state, settlement, for 2..10 seats | Abstraction choices, solvers, storage |
| L2 abstraction | Card bucketing, action menus, abstraction identity and error | Rules, solving, IO |
| L3 tree construction | Enumerating the abstract betting tree from L1 + L2 | Solving, storage |
| L4 solver | Producing a policy for a tree | Rules, abstraction, persistence, transport |
| L5 storage | Immutable policy artifacts, digests, bounded reading | Strategy semantics |
| L6 decision service | Routing a state to a policy source and reporting a guarantee | Rules, solver internals, transport |
| L7 protocol | Wire contracts | Strategy |

### L1: one game definition

`bs::poker::GameDef` describes a hand: seat count (2..10), button, blinds, antes,
stacks, and the rules variant. It also carries the terminal-depth rule RFC 0007
introduced, `TerminalDepth::River | TerminalDepth::Flop`
(`docs/rfcs/0007-preflop-profile-abstraction.md:197`), because a game that ends
at a completed flop is a different game and not a different tree: the depth
changes which settlement path runs, so it belongs to the definition, not to the
abstraction above it. `bs::poker::GameState` is the transition machine
over it, with the union of the two current types' behavior expressed once.

The two existing profiles become configurations rather than types:

- the heads-up preflop root is `GameDef` with 2 seats and posted blinds;
- the heads-up flop root is `GameDef` with 2 seats and a rooted board;
- the current multiway root is `GameDef` with 3..6 seats.

Compatibility is preserved by keeping `HeadsUpState` and `MultiwayState` as
adapters over `GameState` during the transition, so every existing caller and
every existing test keeps working while the implementation is unified. The
adapters are deleted in the removal stage, not before.

Calling them thin would understate the work, and the differences are where the
real correctness risk sits. Of the twelve shared names, exactly one - `actor` -
has a byte-identical body in both types; the rest differ, and several differ in
poker semantics rather than in shape. Verified against both implementations:

| Shared name | How the two differ today |
| --- | --- |
| `can_raise` | Multiway additionally requires `!all_in`, so a seat that is all-in with raise rights still set is legal under one type and not the other |
| `pay` | Multiway sets `all_in = stack == 0` as a side effect; heads-up tracks all-in state in a separate `all_in_` array it maintains itself |
| `refund_unmatched` | The substantive one. Heads-up compares two commitments and always refunds the excess. Multiway skips folded seats when locating the high commitment, returns without refunding unless exactly one seat holds it, and bounds the refund by the best level any other seat reached, because a folded player's chips stay in the pot as dead money |
| `close_street` | Heads-up clears `pending_`, `raise_rights_`, and `big_blind_option_`; multiway clears pending state only, because it has no option rule to clear and re-establishes raise rights when the next street opens |
| `after_card` | Both reset raise rights when action opens, but heads-up additionally maintains `big_blind_option_` and its own two-seat `all_in_` array, while multiway derives "no betting possible" from `next_actor` returning empty |
| `after_action` | Heads-up threads `big_blind_option_` through a preflop option rule with no multiway analogue at all |
| `Phase::Folded` | The same enum name means the same thing in both - the whole hand ended by folding - but the multiway path reaches it only when `live_players().size() <= 1`, while a seat that folds mid-hand merely sets its `folded` flag and the action continues. A unified state must not let a reader conflate "this seat folded" with "the hand is over", which is the reading the shared name invites |
| `legal` | Different structures (`LegalActions` versus `MultiwayLegal`) |
| `award`, `settle_fold`, `settle_showdown` | Different types (`Settlement` with fixed two-seat arrays versus `ContributionSettlement` with vectors), and the multiway showdown must rank every live hand rather than take a winner index |
| `pot` | Same definition, different arity |
| `actor` | Identical |

Two consequences follow. First, unifying is not forwarding: each row above is a
decision about which behavior is correct, and the multiway behaviors are the
richer ones because they had to answer questions heads-up never faces.
Second, `big_blind_option_` is a rule that exists in only one of the two types
today, so the unified definition has to state whether the option rule is
heads-up-only or general, and the oracle has to pin whichever answer is chosen.
Stage 1 carries those decisions; the exhaustive oracle in the Verification Plan
is what makes "the heads-up path is unaffected" a measured claim rather than an
assertion.

Settlement extends from 6 to 10 seats. The contribution-layer algorithm is
already seat-count agnostic in structure; the change is a bound and its tests.

### L2: abstraction

Two components, both versioned and both carrying their own identity:

- **Action abstraction** maps a legal action set to a declared ordered menu. The
  existing per-street fractional size schedules (RFC 0007) are one
  implementation of it, made explicit.
- **Card abstraction** maps a concrete two-card holding plus board to a bucket.
  The first implementation is deliberately simple and measurable: a strength
  bucket over the existing range representation, with the bucketing function
  itself a declared, versioned parameter.

The hard-coded preflop charts are NOT an abstraction and are not relocated into
one. `engine/src/poker/charts.cpp` holds hand-class sets, which is a precomputed
policy over hand classes, not a map from states to a smaller state space. They
become a declared policy source with its own identity, recorded by the guarantee
mapping below as `approximate`, and they leave the decision path rather than
being re-expressed as an abstraction instance. Conflating the two would hide a
policy inside the layer whose job is to make approximations visible.

`AbstractionId` (name plus version plus parameters plus a digest) is part of a
game's identity, so a policy trained under one abstraction can never be looked
up under another. No abstraction is ever applied silently; a caller that
requests the unabstracted game gets the exact game or an explicit refusal.

**Sequencing, to avoid designing identity twice.** RFC 0007 already defines the
mechanism this must reuse rather than reinvent: game identity is an explicit
declaration carried on the artifact's versioned identity surfaces, compared by
`same_game`, and versioned by a schema-major bump with the coexistence rules
already in force (`docs/rfcs/0007-preflop-profile-abstraction.md:181`,
`:328-341`). `AbstractionId` therefore joins that existing identity as a declared
field in the stage that lands the first checkpoint requiring it, and it stores
no claimed error number: the measured error is an evidence artifact keyed by the
id, produced by the harness below and reported per decision, never a quantity the
artifact asserts about itself.

**Measured error is part of the contract, and it has two definitions because it
has two regimes.** RFC 0004's principle that lossy buckets "require measured
abstraction error" is satisfied by publishing a number, not by asserting
smallness.

- **On enumerable fixtures** (heads-up, small boards, restricted ranges) the
  error is the exact difference in exploitability between the exact solve and
  the abstracted solve, per fixture. This is a real bound and it is the primary
  evidence.
- **At 7..10 seats** no exact solve exists to subtract from, so the exact-versus-
  abstracted definition cannot apply and pretending otherwise would be a fake
  number. There the reported quantity is a **measured deviation estimate**: an
  estimated NashConv against declared reference opponents, computed on a fixed
  seed set, lower-is-better and explicitly not a bound. Every such number is
  labeled as an estimate wherever it appears, including on the artifact and in
  the decision report.

Both definitions are reported on the same fixtures at every seat count where
both are computable, which is what ties the two regimes together rather than
leaving the 7..10 numbers unanchored.

### L3: abstract tree

One builder produces an abstract tree from a `GameDef` and an `AbstractionId`.
The tree node type is shared by every solver. Terminal values come from the L1
`GameDef` and its settlement, so multi-seat payout vectors (RFC 0006) flow
through unchanged.

### L4: solver interface

```
struct SolveRequest { const AbstractTree& tree; ranges; SolveLimits limits; seed; };
struct SolveResult  { Policy policy; Guarantee guarantee; measured cost fields; };
SolveResult solve(const SolveRequest&);
```

Existing solvers become implementations: the river LP and DCFR, the heads-up
CFR, and any future multi-seat trainer. A solver declares which tree shapes it
supports and refuses the rest explicitly rather than approximating.

### L5: storage

The existing artifact and resident machinery (RFC 0005) is retained. Its game
identity gains the `AbstractionId`, and its schema-major coexistence rules (RFC
0007) are reused to carry the widened game definition.

### L6: decision service and guarantee levels

Every decision returns a `Guarantee` from a closed, ordered set. The level is
computed from the policy source and the model that source solved, never from the
caller's hopes, and it is reported on the wire.

| Level | Meaning | What it requires |
| --- | --- | --- |
| `certified_bound` | An independent bound was recomputed and passed for this decision | An RFC 0005 Stage 9-style certifier |
| `exact_solved` | Solved exactly for a declared model that carries NO unmeasured approximation | Either no range cap, or a cap that is itself a declared, measured abstraction |
| `abstract_solved` | Solved for a declared abstract model whose error is measured and published | An `AbstractionId` with a reported error number |
| `approximate` | A non-solve policy, or a solve over a model carrying an unmeasured approximation | Nothing further; this is the honest default |
| `operational_fallback` | A legality-preserving fallback because no strategy source was available | Nothing |

The `exact_solved` definition is deliberately strict, and it is the reason this
table exists. A solve can be exact for its model while the model itself is
unmeasured - a range-capped river solve is the live example - and calling that
`exact_solved` would let a session-journal reader take it for an equilibrium of
the declared game, which is exactly the misreading this section exists to
prevent. Such a solve is `approximate` until its cap is either removed or
elevated into a declared, measured abstraction.

**Every source maps to a level, and the mapping is normative.** The wire
sources are the seven values of `SolverSource`
(`proto/bigshark/engine/v1/engine.proto:97-108`):

| Wire source | Level today | To reach a stronger level |
| --- | --- | --- |
| `PREFLOP_CHART` | `approximate` | Becomes an abstract solve, or a declared policy source, per N5 |
| `POSTFLOP_HEURISTIC` | `approximate` | Becomes an abstract solve for a declared model |
| `RIVER_LP` | `approximate` | `abstract_solved` once the capped-range model is declared with a measured error; `exact_solved` only with the cap removed, which is the path the exact-versus-abstracted measurement runs on the enumerable fixtures. The cap is live and small (`TrackedRangesOptions::cap = 24`, `engine/include/bs/river_gto.hpp:101`), so the bounded-range solve is itself a measurable abstraction instance and the honest default is the weaker level |
| `RIVER_DCFR` | `approximate` | `abstract_solved` once its model and error are declared; a bounded iterative solver has no certificate |
| `MULTISTREET_CFR` | `approximate` | Experimental and offline; no path to a stronger level is claimed |
| `BLUEPRINT` | `approximate` | `abstract_solved` once the blueprint's game carries a measured abstraction error; a benchmark quality metric is not a bound |
| `RESOLVING` | `certified_bound` when certified; `approximate` on the deadline-blueprint fallback | Already the strongest level, as RFC 0005 Stage 9 defines it |

A source that is not listed is a programming error and fails closed to
`operational_fallback` rather than being assigned a level by default.

**Wire disposition.** `SolverMetadata.guarantee` is a validated string enum
constrained to `["modeled_exact_bound", "uncertified", "baseline"]`
(`engine.proto:379-385`), and RFC 0005 fixes that contract for minor 1. The new
levels are therefore ADDITIVE and land under a newly negotiated minor (the
existing negotiation mechanism, RFC 0002 Stage 8) or in the `bigshark.engine.v2`
package that RFC 0006 anticipates - decided at stage 5, with two constraints
that do not move: minor-0 bytes stay frozen, and existing minor-1 clients keep
receiving exactly `modeled_exact_bound | uncertified | baseline` with unchanged
meaning. `certified_bound` maps onto the existing `modeled_exact_bound` string;
`exact_solved` and `abstract_solved` are new values, not a reinterpretation of
`uncertified`.

**Request side.** A caller may declare a minimum acceptable level. This is NOT
the eligibility bit RFC 0005 forbids: it carries no provenance claim and asserts
nothing the stateless host could not already see. Absent, the caller accepts any
level. When the best available level is below the declared floor, the host
returns a typed `GUARANTEE_BELOW_REQUEST` rather than serving a silently weaker
policy. Enforcement is response-side, and the RFC 0005 rule that the host
"cannot reconstruct execution provenance" is unchanged: it reports what it did,
it does not validate what the caller claimed.

This replaces the current street-based routing with source-based routing plus an
explicit preference order.

### L7: protocol

The v1 Protobuf contract gains the guarantee level as a structured field. v0 is
removed in the removal stage below, after the replacement is verified.

## Dependency Rules

- L1 depends only on L0. L2 depends only on L1 types. L3 depends on L1 + L2. L4
  depends on L3. L5 depends on L4's policy type. L6 depends on L5 + L4. L7
  depends on L6.
- No layer may depend on a later layer. In particular the abstraction must not
  know about solvers, and the decision service must not know about a specific
  solver's internals.
- `bigshark_solver` keeps its current dependency on `bigshark_poker` and gains no
  transport or storage dependency.
- The abstraction component is new and is placed in `bigshark_poker`'s dependents,
  not inside it: the rules must not know how they are abstracted.

## Compatibility and Migration

| Surface | Effect |
| --- | --- |
| `HeadsUpState` / `MultiwayState` | Keep working as adapters during the transition; deleted only in the removal stage |
| Existing policies and artifacts | Keep their identity and digests; the game identity gains an abstraction field under RFC 0007's schema-major rules |
| v0 NDJSON | Kept until the removal stage, where its removal is gated on no remaining callers and a rollback artifact |
| Preflop charts | Behavior preserved while they are re-expressed as an abstraction instance; the numeric tables do not change in that step |
| Live decision path | Unchanged in behavior until a source is explicitly promoted; every step is independently revertible |
| Settlement | Extended to 10 seats; 2..6 behavior is unchanged and re-verified against the Stage 11 oracle |

No flag day. Each stage lands behind the existing opt-in mechanisms and is
revertible on its own.

## Resource Envelope

A genuine risk, because abstraction buys tractability with memory and the
current declarations are sized for heads-up. The envelope is declared here so
that a stage cannot pass by quietly raising a limit:

- **What may grow.** The abstract tree's node and information-set counts grow
  with seat count, and the abstraction's bucket count grows with the card
  model's granularity. These are bounded by `TrainingLimits` (`max_nodes`,
  `max_information_sets`, `max_depth`, `max_bytes`) and by the tree builder's
  own refusal, and every stage that raises one records the measured cost that
  forced it.
- **What may not grow silently.** The declared accounting constants - the
  game-copy byte charge and the artifact footprint estimate - are identity
  surfaces whose consumers include the frozen benchmark matrix and the
  allocation exact-fit gate, so any change follows RFC 0007's documented-rebase
  procedure with recorded evidence, not a convenience edit.
- **What must be measured at stage 6.** Resident bytes, solve wall clock, and
  per-decision latency at the largest seat count the stage reports, published
  with the same table as the quality figures. A stage 6 table with no cost column
  is incomplete even if its quality numbers are good.
- **What this RFC does not do.** It does not commit to a training budget, does
  not enable large-scale training (that keeps its own authorization), and does
  not require a latency target it has not measured. Latency targets for live
  service are the separate live-promotion gate's concern.

## Security and Operational Impact

Offline-only for every stage that touches training or abstraction. No
credentials, network access, or live play. The security-relevant boundary is
unchanged: the server remains authoritative, the engine still chooses only from
the supplied legal action set, and the hidden-information rule is unchanged -
an information key still contains only the acting seat's own cards, the board,
and public history.

The operational change is observability: every decision now reports a guarantee
level, which makes a quality regression visible in the session journal instead of
being indistinguishable from an equilibrium decision.

Resource limits are per-layer and declared: abstraction construction, tree size,
solve budget, and lookup budget each have their own bound, and an oversized tree
or an unavailable solver is a typed refusal rather than a degradation.

## Alternatives Considered

- **Extend `MultiwayState` from 6 to 10 seats and keep the current shape.**
  Rejected as the primary approach: it multiplies the duplication (a third
  transition implementation or a second branch in the existing two) and does not
  address why the engine cannot grow, which is the missing abstraction.
- **Adopt a full-game solver design with neural abstractions.** Rejected for this
  repository stage: it requires training compute and a validation story this
  project's bounded local limits do not support, and it would replace a working
  verified solver with an unverifiable one.
- **Keep the five solver interfaces and add a sixth for multi-seat.** Rejected:
  the interfaces are the reason a new capability touches every layer.
- **Delete the heuristic and serve only solved policies.** Rejected as a
  sequencing error: the heuristic is currently the only source that covers most
  live states, so deleting it before a replacement exists makes the engine
  unable to return a legal action, which violates a safety invariant.
- **Keep v0 indefinitely behind an adapter.** Rejected: v0 is transitional debt
  with a recorded removal item, and the adapter would have to carry the widened
  state anyway.

## Risks

| Risk | Mitigation |
| --- | --- |
| The unification changes heads-up or v0 behavior | Adapters first, deletion later; every step re-runs the Stage 1 exhaustive oracle (12,226 nodes), the replay suite (156 decisions / 0 illegal / 0 JS fallbacks), and the frozen matrix |
| The card abstraction's measured error is large enough to make the abstract policy worse than the current heuristic | Measured per validation fixture before it feeds any decision; a level weaker than `approximate` is never promoted |
| Multi-seat CFR does not converge, so a 10-seat solve is not trustworthy | No convergence claim is made; the guarantee level reports `abstract_solved` and the measured quality, and RFC 0006's nonconvergence statement stands |
| Widening settlement to 10 seats breaks the verified 2..6 path | The Stage 11 exhaustive contribution grid is re-run unchanged and extended; 2..6 results must be bit-identical |
| The abstraction becomes a place where quality silently degrades | `AbstractionId` is part of game identity, so no policy is ever looked up under a different abstraction, and the decision reports its level |
| The refactor stalls with two rule implementations alive | Each stage is independently revertible and the adapters keep both alive deliberately; the removal stage is gated on the replacement's verification, not on a calendar |
| The larger-table policy does not beat the current heuristic, which is this RFC's headline claim | Measured at stage 6 before any live path depends on it, reported as a table with the heuristic's own number on the same fixtures and seeds, and recorded as a negative result rather than smoothed over |
| The card abstraction's error is measured only at 2 seats and assumed upward | Both definitions are reported at every seat count where both are computable, and the 7..10 figures are labeled estimates, so the gap is visible instead of implied |
| Widening the rules type breaks a head-up semantic that the shared name hid, such as `can_raise` and `all_in` | Each difference is decided explicitly at stage 1 and the exhaustive oracle proves the head-up path unchanged before anything is deleted |

## Verification Plan

- **Unification equivalence.** For every root configuration the Stage 1 oracle
  enumerates, `GameState` and `HeadsUpState` produce identical transitions,
  chip states, and settlement. The oracle is the independent enumerator and is
  not modified by this work.
- **Settlement extension.** The Stage 11 contribution grid re-run unchanged for
  2..6 seats, bit-identical, plus a 7..10 seat extension with independent chip
  conservation and side-pot checks.
- **Abstraction identity.** A policy trained under `AbstractionId` A cannot be
  looked up under B; the refusal is typed. Round-trip through storage preserves
  the identity.
- **Native v1 suites, not only replay.** Replay exercises the v0 NDJSON path and
  therefore cannot witness a v1 regression. The v1 suites are gates in their own
  right at every stage: `test_v1_frame_stream`, `test_v1_request_mapper`,
  `test_v1_response_mapper`, `test_v1_semantic_validator`, `test_v1_minor1`,
  `test_v1_resolving`, `test_v1_resident_mapper`, and `fuzz_v1_proto`
  (`engine/tests/`).
- **Measured abstraction error.** Exact versus abstracted exploitability on
  enumerable validation games, reported per fixture as a number, including the
  case where the abstraction is the identity (error must be zero).
- **Solver interface conformance.** Every existing solver is reachable through
  `solve`, refuses unsupported tree shapes explicitly, and reproduces its
  previous results bit-for-bit under the identity abstraction.
- **Guarantee-level correctness.** Every decision path reports the level its
  source warrants; a test asserts a weaker level is never reported as a stronger
  one, and that a request for a stronger level than available is typed.
- **Removal gates.** Each removed path has a recorded verification that its
  replacement covers every caller before deletion, and the full `AGENTS.md` gate
  passes at each removal commit.

## Rollout Plan

Each stage is independently verifiable and revertible. Stop on any legality,
conservation, oracle, or interface-conformance failure.

1. **Game definition.** Introduce `GameDef`/`GameState` for 2 seats behind
   adapters, preserving `HeadsUpState` behavior exactly. Gate: the Stage 1
   oracle and the replay suite, unchanged.
2. **Seat-count parameterization.** Extend `GameState` to 3..10 seats behind
   adapters, preserving `MultiwayState` behavior exactly. Gate: the multiway
   test suite plus the settlement grid.
3. **Abstraction layer.** Introduce action and card abstraction with
   `AbstractionId`, an identity implementation, and the measured-error harness.
   Gate: the error is zero for the identity abstraction, and the existing size
   schedules reproduce their current decisions.
4. **Tree and solver interface.** Introduce the shared tree and `solve`, and
   port the existing solvers behind it. Gate: bit-for-bit reproduction of each
   solver's previous results.
5. **Decision service and guarantee levels.** Source-based routing with the
   typed level on every response. Gate: the level-correctness tests and the
   replay suite with unchanged decisions.
6. **Measured larger-table policy.** The headline deliverable: apply the
   abstraction to produce and measure a policy at larger seat counts, reporting
   measured quality and no convergence claim. The gate is a published table,
   because it is the only thing that makes this RFC's central claim falsifiable:

   - **Seat counts.** At least one seat count strictly greater than 6, and at
     least one at 10. A policy that never exceeds 6 seats does not satisfy this
     stage.
   - **Metric.** Per-fixture deviation gain at each seat count: the measured
     deviation estimate of the abstract policy against the declared reference
     opponents, reported alongside the same estimate for the declared heuristic
     on the identical fixtures and seeds. The abstract policy must beat the
     declared heuristic on the aggregate of the declared fixture set, or the
     result is recorded as a negative result and stage 6 is not complete.
   - **Seeds and permutations.** A fixed, published seed list, with every
     reported figure reproducible from the seeds alone. Seat positions are
     permuted so a figure is not an artifact of one assignment of positions.
   - **Reference opponents.** Declared and versioned: at minimum the current
     heuristic, a fixed uniform-random policy, and the previous stage's policy at
     the same seat count for a self-improvement comparison.
   - **No convergence claim.** The report states the estimator, the seeds, and
     the fixture set, and states in the same place that the number is an estimate
     without a convergence guarantee (RFC 0006).

   Gate: the published table, reproducible from its seeds, with the estimate
   labeled as an estimate. Negative results are reported as results and do not
   block the rest of the RFC; they block only the stronger claims the RFC's
   Non-Goals already disclaim.
7. **Removal.** Delete the duplicated rule implementations (via the adapters),
   the hard-coded chart tables (removed from the decision path, per L2), the
   experimental solver paths subsumed by the interface, and v0. The default
   protocol flips to v1 in this stage, as stated in the Non-Goals. Gate: no
   remaining callers, a rollback release artifact, and the full gate green.

## Rollback Plan

Every stage up to 7 is additive behind adapters or opt-in selection, so
reverting the stage restores the previous behavior exactly. Stage 7 is
destructive and is therefore gated on a published rollback release artifact
containing the removed paths; the roadmap already requires this for v0 under
`P8`. No policy artifact is rewritten or reinterpreted at any stage, and no
existing digest moves.

## Open Questions

- Which card abstraction (strength buckets over the existing range, or something
  finer) gives an acceptable measured error at larger seat counts? Decided by
  stage 3's measured error, not by preference.
- At what seat count does the abstracted solve stop producing a policy that beats
  the declared heuristic, measured? Reported in stage 6 against the declared
  reference opponents and seeds; this is the honest answer to "how far up the
  table sizes can this engine actually go", and it is expected to be a
  seat-by-seat curve rather than a single threshold.
- Does the guarantee level belong on the wire as a new v1 field or as a v2
  package? RFC 0006 anticipates a `bigshark.engine.v2` for coherent full-state
  support; decided when stage 5 lands.
- Should the heuristic move into its own target with its own tests rather than
  staying in the decision service? Decided in stage 5.

## Acceptance Criteria

- One legal-transition implementation serves every seat count from 2 to 10, with
  the previous types available only as adapters, and the Stage 1 oracle passes
  unchanged.
- Settlement supports 2..10 seats with the 2..6 results bit-identical to the
  Stage 11 evidence and independent conservation checks at 7..10.
- An `AbstractionId` is part of game identity, no policy is looked up under a
  different abstraction, and the identity abstraction's measured error is zero.
- A card abstraction publishes a measured error per validation fixture, and no
  decision is served from an abstraction whose error is unreported.
- A reproducible stage 6 table reports the abstract policy's measured deviation
  estimate at each tested seat count, including at least one seat count strictly
  greater than 6 and one at 10, against the declared reference opponents on the
  fixed seed list, next to the declared heuristic's estimate on the same fixtures
  and seeds. The figure is labeled an estimate, states its estimator, and makes
  no convergence claim. A stage 6 that produces no such table is not complete,
  and this criterion is met by reporting the result, positive or negative.
- Every solver is reachable through one interface, refuses unsupported shapes
  explicitly, and reproduces its previous results bit-for-bit under the identity
  abstraction.
- Every decision reports a guarantee level from the closed set; a weaker source
  is never reported as a stronger level.
- The superseded paths are removed with recorded replacement evidence, and the
  full gate is green at each removal.

## Decision

Pending independent approval-agent review.
