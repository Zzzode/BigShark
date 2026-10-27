---
rfc: "0032"
subject: "Preserve the Full Board Through the Decision Pipeline"
status: "Implementing"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-21"
owners: "engine resident, River adapter, engine protocol"
supersedes: ""
superseded-by: ""
---

# RFC 0032: Preserve the Full Board Through the Decision Pipeline

> **Errata (2026-10-27, [RFC 0040](0040-retire-river-club-agent-api.md)).** The `River adapter` owner, the platform test evidence in the deleted `platforms/river-club/tests/w1-resident-wiring.test.ts`, and the journal-replay verification gate are voided by the retirement of the River Club Agent API v2.1 integration. The engine-resident board-preservation deliverables and this RFC's status are unchanged.

## Summary

Every postflop decision in the live pipeline is re-rooted to a three-card flop
board, discarding the turn and river cards. The resident path cannot hold a
four- or five-card root, the board-identity helpers compare only the first three
cards, and the River adapter builds a `board_size = 3` game from a snapshot that
may carry five. This RFC makes the pipeline carry the board it was given, and
pins the current wrong behavior with a test before changing it.

It also has to repair one thing on the way, or the change makes production
worse: the uniform subgame paths a turn decision falls through to set an empty
`artifact_sha256`, RFC 0025 specifies that deliberately, and the response mapper
rejects it as `ERROR_CODE_INTERNAL`. Board preservation alone would therefore
convert a wrong-but-working turn decision into a mechanical check/fold. The
digest contract is fixed first, as its own stage.

It does not add a turn/river strategy, tree kind, evaluator, or format change.

## Motivation

A turn spot is decided as if it were a flop spot. That is a correctness defect
independent of which turn/river strategy eventually ships, and it is the
prerequisite for every later turn/river proposal.

The consequence is concrete. The adapter collapses a turn snapshot to a
flop-street-start `GameState` with decision-time chips
(`engine/src/protocol/resident_mapper.cpp:504-532`), so the broader pipeline
never sees `board[3]`. The resident path refuses a non-flop root when it builds
a game (`engine/src/resident/resident_policy.cpp:110-113`), so no resident
record can be rooted past the flop; the board-identity helpers compare only
`board[0..2]`
(`engine/src/resident/resident_policy.cpp:53-87,753-761`,
`engine/src/resolver/counterfactual_reach.cpp:26-39`), and the public-reach
blocking loop stops at three cards
(`engine/src/resident/public_reach.cpp:140`). Fixing only the adapter would
therefore not feed "the real board" into an unchanged pipeline: it would make
turn decisions stop matching resident records and fall through to
`resolve_uniform` — which is exactly the path whose response the mapper
currently rejects (see "What a turn decision becomes"). The board fix and the
digest fix are therefore a pair, not a change and an optional extra.

## Goals

- **G1**: A board with four or five cards survives the adapter, the resident
  identity checks, and the reach model without truncation, and the resulting
  decision is **usable**: it returns a strategy the adapter accepts, not an
  internal error.
- **G2**: The current truncating behavior is pinned by a test before the change,
  so the fix is demonstrably a behavior change and not a refactor.
- **G3**: Every four- or five-card decision is attributable in the results log
  to the origin that produced it — a served strategy from a named source, or the
  operational fallback under a distinct label — and never an internal error
  flattened into an ordinary `safe-fallback` indistinguishable from a transport
  outage.

## Non-Goals

- **No turn/river strategy, tree kind, or evaluator.** `TerminalDepth::Turn`,
  `TurnDeal`, `RiverDeal`, and any river evaluator are out of scope. RFC 0011
  Phase 3 defines virtual deal leaves and explicitly defers itself until
  Phase 1/2 measurements show the on-demand path is too slow; that gate has not
  been met, and `TerminalDepth` selects where a game terminates, not where it
  roots.
- **No `RiverFrontierEvaluator`.** `EquityFrontierEvaluator` already handles
  3-, 4-, and 5-card boards: a 5-card board is direct showdown settlement, a
  4-card board enumerates the remaining river card
  (`engine/src/gto/equity_frontier.cpp:191-215`), and it has an independent
  brute-force oracle test including a 4-card side-pot case
  (`engine/tests/test_equity_frontier.cpp:505`). A second evaluator would be a
  duplicate implementation.
- **No BSFLIB02 version bump.** No four- or five-card class is written by this
  RFC, so the format needs no new discriminator. A bump would change the
  artifact SHA-256, which is mixed into the resolver seed
  (`apps/engine-host/main.cpp:390-417`) and the resolver cache key
  (`engine/src/resolver/resolver.cpp:52-90`), silently changing every resolver
  candidate for every spot.
- **No turn/river class library.** Not addressed here at all.
- **No subgame quality claim.** The uniform-prior solve is used as-is. This RFC
  does not claim its output beats today's flop-rooted decision; it claims the
  decision is no longer made for the wrong game and the outcome is attributable.

## Current State and Evidence

### Facts (verified in this repository)

- **The adapter re-roots to the flop.**
  `engine/src/protocol/resident_mapper.cpp:504-532` builds
  `flopDef.board_size = 3` and `flopDef.board = {cursor.board()[0..2]}` for any
  postflop decision, discarding `board[3]` and `board[4]`.
- **The rebuild does not construct for a whole class of hands, and that is
  why the corruption has gone unnoticed.** It copies **gross** `contributed`
  (`:527`) while taking the **net** `cursor.pot()` (`:523`), and
  `poker::validate` reconciles the two: heads-up rooted boards require equal
  contributions when not mid-street and that their sum equal the pot
  (`engine/src/poker/game_definition.cpp:135-139`); 3+ seats require the pot to
  equal the declared contributions (`:199-208`). So `GameState flopState(flopDef)`
  at `:530` throws for a heads-up postflop node with unequal contributions
  (facing a bet) and for any refunded hand, and the `catch (const
  std::exception&)` at `:536-542` turns that into `ReplayMiss::ReplayMismatch`
  (or a rethrow at two seats). Those hands never reach a decision at all. An
  earlier draft claimed the opposite twice — that the rebuild "carries the same
  pot, stacks, and contributions as the flop root, so `same_root` matches" — and
  that claim is only true for the subpopulation that constructs: equal
  contributions, no refund.
- **The truncation also corrupts an information key.** On a turn check-through
  line the mapper's flop definition does match the flop root, so `same_root`
  matches and `replay_public_path` processes turn events against a three-card
  span. The
  prefix-board builder computes `prefix_board_size = 4` for a turn event and
  reads `state.board()[3]` (`engine/src/resident/resident_policy.cpp:861-872`).
  `state.board()` has size 3, so this reads past the span's extent. It is a bug
  but **not a memory-safety bug, and no sanitizer will catch it**: the span is
  built over a zero-initialized `std::array<int,5> board_`
  (`engine/include/bs/game_definition.hpp:229,303`), so index 3 lies inside the
  same object and returns 0 — card id 0 is a real card, the two of spades.
  Out-of-range `span::operator[]` is only asserted when
  `_GLIBCXX_ASSERTIONS` is defined, which this build does not set (the `asan`
  preset adds `-fsanitize=address,undefined` and nothing else). An earlier draft
  called this undefined behavior and made ASan the mitigation; that was wrong,
  and the practical consequence is that only a **behavioural** assertion can
  detect the site. The result is a corrupted information key, not merely a
  re-rooted game.
- **No resident record can be rooted past the flop.**
  `engine/src/resident/resident_policy.cpp:110-113` throws for
  `game.board_size != 3 && != 0`, naming RFC 0009 D4 as the reason it is not
  yet generalized. Three record paths reach it — the v2 artifact load
  (`:355,621,672`), the probe path (`:544`), and the pre-RFC-0009 legacy load
  via `solver::to_unified_game` (`:549`, which hardcodes `board_size = 3` at
  `engine/src/solver/seat_policy.cpp:75`) — and the packed-library path assigns
  `board_size = 3` directly (`:499-500`). No record can be rooted past the flop.
  `poker::validate` itself already accepts a turn or river root
  (`engine/src/poker/game_definition.cpp:129-130`), so the restriction is the
  resident layer's, not the rule engine's.
- **Board identity compares three cards.** `same_root` loops `i < 3` in
  `engine/src/resident/resident_policy.cpp:53-87` and in
  `engine/src/resolver/counterfactual_reach.cpp:26-39`.
  `canonical_query_board` canonicalizes only `board[0..2]`
  (`resident_policy.cpp:753-761`).
- **The reach model blocks three cards.**
  `engine/src/resident/public_reach.cpp:140` runs
  `for (i < game.def.board_size && i < 3)`, so a fourth and fifth card remain
  in the pool and are dealt as if unseen.
- **Further truncation sites survive these fixes.**
  The prefix-board builder hardcodes `3 + prefix_street`
  (`resident_policy.cpp:861-872`); board assembly assumes the flop class
  (`:1087-1093`, `:1191-1198`). These are enumerated in AC1 rather than
  discovered during implementation.
- **The packed format stores a three-card board.** Sections write
  `canonical_board` as three `u8`
  (`engine/src/resident/mmap_library.cpp:479-481`), `board_key` packs three
  cards (`:246-249`), and `class_id_for_board` takes `array<int,3>` (`:1407`),
  with the reader returning the same (`:1422`). This RFC does not change the
  format; it is why a four- or five-card root cannot yet be *served* from a
  library.
- **`TerminalDepth` is persisted in three surfaces**: BSFLIB02 header byte 19
  with 0/1 semantics (`engine/src/resident/mmap_library.cpp:646,1150-1173`);
  the strategy-artifact SQLite `CHECK (terminal_depth IN (0,1))`
  (`engine/src/artifacts/strategy_artifact.cpp:722,734,854,2002-2003`); and
  `SeatPolicy.terminal_depth`. None is widened here.
- **`resolve_flop` hardcodes cap 1** (`engine/src/resolver/subgame_resolver.cpp:504-505`);
  only `resolve_uniform` (`:442`) and `reconstruct_action_abstraction`
  (`:112-117`) omit the cap. RFC 0030 owns the cap.

### Assumptions (to be verified)

- Serving cost. The exact n-seat reach model
  (`engine/src/resident/public_reach.cpp`) is used by the resident
  blueprint-belief path only; `resolve_uniform` and `resolve_flop` do not use it
  (`engine/src/resolver/subgame_resolver.cpp:340-720`). `Resolver::resolve` does
  build a reach model, but a different one — `detail::ReachModel` in
  `engine/src/resolver/counterfactual_reach.cpp:62-151`, initialized at
  `engine/src/resolver/resolver.cpp:139` — so the claim about the subgame paths
  holds and the sentence no longer implies `resolve` is reach-free. A prior
  draft claimed the recursion makes any turn/river serving path non-terminating
  at five or six seats, citing figures that appear nowhere in the repository.
  Those figures are withdrawn. Any latency claim must be measured as part of
  this RFC's Stage 3 and recorded with its configuration.
- The five-card suit-canonicalization semantics are unspecified: what the
  canonical representative of a four- or five-card board is, and whether the
  newly dealt card participates in the relabel. This decides whether any later
  class addressing is consistent and must be settled before it is needed.

## Design Principles

- **Pin the bug before fixing it.** A correctness change with no failing
  baseline test is indistinguishable from a refactor.
- **Never silently decide a different game.** Where a four- or five-card root
  cannot be served from a resident record, say so in the results log rather than
  dress the fallback up as an ordinary decision.
- **Fix only what is broken.** No speculative interfaces, no evaluator that
  already exists, no format change with no writer.

## Proposed Design

### Pipeline

The adapter stops clamping; the identity helpers start comparing the whole
board; `build_v2_game` keeps refusing a shape no artifact can yet hold:

- `resident_mapper.cpp:504-532` returns the **replay cursor's runtime with a
  decision-node identity def attached**, not the cursor wholesale and not the
  current rebuild. Both simpler options are wrong, and the reasons pin the
  design.

  *Why not the cursor wholesale.* `GameState::def_` is a copy of the
  constructor's def and is never updated: `after_card` writes only
  `next.board_`, `next.street_`, and `next.players_`
  (`engine/src/poker/game_definition.cpp:727-732`) and `after_action` writes
  only `next.players_`; no statement in the file assigns `def_` after
  `:294`. So `cursor.def()` remains the **hand-start preflop root**
  (`board_size = 0`, `preflop = true`, blinds-only pot, starting stacks) while
  `cursor.board()` correctly holds the real four-card board. Returning it
  directly breaks every identity consumer at every street, not just turn:
  `resolve_root` compares `same_root(record.game.def, state.def(), ...)`
  (`engine/src/resident/resident_policy.cpp:781-782,791-792`) and
  `same_root`'s preflop branch returns false for `0 != 3` (`:60-62`), so a flop
  query stops matching its own record; `counterfactual_reach.cpp:80` returns
  `CoverageMiss`; `resolver.cpp:57-66` mixes root values into `cache_key`;
  `resolver.cpp:123` and `certifier.cpp:120` read `def().pot` as the root pot;
  and `publicSeed` mixes `def.board`, `def.stacks`, `def.contributions`, and
  `def.pot` (`apps/engine-host/main.cpp:403-407`), so every resolver candidate
  for every spot would change.

  *Why not the current rebuild.* It re-derives pending and actor from three
  copied fields, which cannot express "this seat already acted this street"
  (`engine/include/bs/game_definition.hpp:75-164`; `pending` exists only on the
  runtime `GamePlayer`, `:209`).

  *The design.* The two views genuinely differ and each consumer gets the one it
  needs:

  | View | Contents | Consumers |
  | --- | --- | --- |
  | Runtime (`board()`, `players()`, `pot()`, `actor()`) | the replay cursor: real board of any length, true `pending`, correct actor, live `street_committed`, net-contributed refunds | `hero_decision`, `resolve_uniform`, `resolve_flop`, the adapter's legality check |
  | Identity (`def()`) | `stacks`, `contributions`, `folded`, `pot`, `button`, `big_blind`, `preflop = false`, `terminal` — with **net** contributions — plus `board`/`board_size` from `cursor.board()` and, for a mid-street root, `street_committed`/`raise_rights`/`last_full_raise` | `same_root`, `cache_key`, `root_pot`, the certifier, `publicSeed` |

  **The identity def must be a valid `GameDef`, and that forces net
  contributions.** Two constraints decide every field:

  1. `GameState`'s contract is `validate(def)` first
     (`engine/include/bs/game_definition.hpp:218`, enforced at
     `engine/src/poker/game_definition.cpp:295`), and a seam that skipped it
     would publish a def five identity consumers then hash and certify against.
     So AC1 requires `poker::validate(identity_def)` to pass.
  2. `def().pot` must equal `pot()` — otherwise `root_pot`
     (`engine/src/resolver/resolver.cpp:123`), the certifier
     (`engine/src/resolver/certifier.cpp:120`), `cache_key` (`:57-66`), and
     `publicSeed` (`apps/engine-host/main.cpp:403-407`) read a different pot than
     the state reports.

  `pot()` sums `net_contributed()` (`game_definition.cpp:440-444`), and
  `validate` reconciles contributions against the pot, so the def's
  contributions must be **net** and its pot their sum. The current rebuild's
  gross/net mix is why it throws; copying it unchanged would reproduce the
  throw.

  **Net contributions alone are not sufficient, and an earlier revision of this
  paragraph claimed they were.** The two-seat branch has a *second* rule:
  `if (!mid_street) require(def.contributions[0] == def.contributions[1])`
  (`engine/src/poker/game_definition.cpp:135-137`), where `mid_street` is
  derived from `street_committed > 0` (`:95-98`) — not from the contribution
  values. So a heads-up node facing a bet still fails `validate` with net
  contributions, because its contributions are unequal and its
  `street_committed` is zero. The same holds for the short-all-in shape.

  The identity def therefore carries `street_committed`, `raise_rights`, and
  `last_full_raise` from the cursor when the street has a wager — by exactly the
  rule `build_subgame_def_from_node` already uses and is already tested with
  (`engine/src/resolver/subgame_resolver.cpp:357-369`): copy
  `players()[p].street_committed` and `raise_rights` per seat, and set
  `last_full_raise` to the live value only when some `street_committed` is
  nonzero, leaving the `0` "street start" sentinel otherwise. This is **not** the
  work the earlier draft did on the runtime — the runtime never needed these
  fields, because the cursor already holds the correct `pending` set and actor.
  The identity def needs them only to be a *valid* `GameDef`, and the values are
  a straight copy from the cursor, not a re-derivation.

  With this, both `validate` branches are satisfied: at a street-start root
  (every `street_committed` zero) contributions are equal by construction, and
  at a mid-street root the equality rule is relaxed and the mid-street branch
  requires `last_full_raise >= big_blind` (`:230-232`) and at least one seat
  still facing the wager (`:233-238`) — both true for a node the cursor already
  validated as actionable.

  This is a **deliberate scope statement, not an implementation detail**:
  heads-up postflop hands facing a bet, and every refunded hand, begin
  constructing for the first time. They are not covered by "no
  resident-served response changed", because none exists today to compare
  against — they were rejected before any decision. AC5 states the
  subpopulation it applies to.

  A refunded hand is **equal after the refund**, so it validates as an ordinary
  street-start root. This is not an assumption: `close_street()` calls
  `refund_unmatched()` (`engine/src/poker/game_definition.cpp:577-578`), so by
  the time a street is closed the unmatched excess has already been returned and
  the net values have converged. The rebuilder therefore never sees a
  street-start root with unequal net contributions, and if one ever appears the
  seam's `validate` rejects it — the correct outcome for an inconsistent ledger,
  not a gap to paper over.

  Precisely: `validate` rejects a root that has **both** unequal contributions
  **and** no seat facing a wager. Nonzero `street_committed` alone does not force
  rejection — equal net contributions with mid-street fields still validate as a
  street-start root — so the rule to remember is the pair, not "a wager implies
  mid-street". Every unequal-contribution root the reducer produces has a live
  wager, and those are exactly the ones the mid-street fields make valid.

  **The short-all-in cell is named rather than left implicit.** A heads-up node
  where the short seat has *no chips left* and the wager is already settled has
  unequal `street_committed` values with no seat facing a wager, so
  `validate`'s mid-street branch rejects it (`:233-238` requires
  `facing_wager >= 1`). This is not a case the reducer produces — an unmatched
  excess is refunded at street close (`:577-578`), and a seat that shoves is
  answered or the street does not close — so the state is inconsistent by
  construction. If it appears, the seam's `validate` throws and the request is a
  `ReplayMiss`, which is correct. AC1's fixture set does not include it, and the
  AC says so instead of implying universal coverage.

  For the hands that *do* construct today — a street-start root with equal
  contributions and no refund — contributions already equal their net values and
  every `street_committed` is zero, so the identity def is numerically identical
  to today's rebuild and every existing `same_root` outcome is preserved; only
  the board comparison moves. For a mid-street root the identity def now carries
  nonzero `street_committed`, which the old path left at zero — but every record
  is flop-rooted and street-start, and `same_root` does not compare
  `street_committed` at all (`engine/src/resident/resident_policy.cpp:53-87`), so
  no matching outcome changes on that axis either. A turn query differs
  only in `board_size`, which is the property the whole design rests on and
  which AC1 tests directly.

  Fields the rebuild never sets — `ante`, `variant`, `blinds_posted` — keep
  their `GameDef` defaults (`engine/include/bs/game_definition.hpp:83`, `:115`,
  `:162`), which `validate` accepts for a rooted postflop def
  (`engine/src/poker/game_definition.cpp:151-181`). `last_full_raise`, `street_committed`, and
  `raise_rights` stay zero, matching a street-start root; `same_game_def`
  compares them (`:275`, `:285`) but no River path calls that predicate, so the
  omission is latent rather than live — noted under Risks.

  Mechanically this needs a way to attach an identity def to a cursor copy —
  a constructor or member taking `(runtime state, identity def)` — because
  `def_` is private and the plain constructor re-derives `pending` from
  `street_committed`. That seam is part of the same commit as the board change,
  not a follow-up: landing the cursor without it disables the resident path at
  every street.

  The cursor already *is* the decision node: `reconstructHandState` replays
  every transition event through `cursor = cursor.after_card(...)` and
  `cursor = cursor.after_action(...)` (`:351`, `:392`), then validates that the
  cursor's street, board size, actor, and pot match the declared snapshot
  (`:407-413`). It holds the real board of any length, the true pending set, the
  correct actor, the live `street_committed`/`raise_rights`/`last_full_raise`,
  and the net-contributed refunds. The flop-rooted `GameDef` is built by
  discarding all of that and re-deriving it from `stacks`, `contributions`, and
  `folded` alone (`:525-529`).

  The alternative — keep the rebuild and copy three more fields onto it — was
  the previous draft, and it does not work. `GameDef` has no field for "this
  seat has already acted on the current street" (`engine/include/bs/game_definition.hpp:75-164`;
  `pending` exists only on the runtime `GamePlayer`, `:209`). When a turn is
  checked to the hero, every `street_committed` is zero, so the constructor
  takes its street-start branch, marks every live seat pending, and picks the
  seat clockwise of the button (`engine/src/poker/game_definition.cpp:395-397,415-422`)
  — which is the hero's opponent when the hero acted first. `hero_decision`
  keys off `*state.actor()` (`engine/src/resident/resident_policy.cpp:1082,1094`)
  and `resolve_uniform` off `*node.actor()`
  (`engine/src/resolver/subgame_resolver.cpp:425`), so the decision would be
  computed for the wrong seat on exactly the check-through line this RFC exists
  for. Returning the cursor fixes that by construction rather than by adding a
  field that `GameDef` does not have.
- The rebuild also mis-states the pot when a refund happened: it copies gross
  `contributed` (`:527`) while `poker::validate` reconciles the pot against
  contributions (`engine/src/poker/game_definition.cpp:135-139`, `:199-208`) and
  `GameState::pot()` uses `net_contributed()`
  (`engine/include/bs/game_definition.hpp:210`). A heads-up turn with gross
  `{30,20}` and pot 45 throws `unmatched contributions at closed-street root`.
  With net contributions this divergence disappears for both views: `pot()` is
  the sum of `net_contributed()`, and the identity def's `pot` is the same sum,
  so `def().pot == pot()` holds by construction. An earlier draft said the
  identity def keeps today's `pot` while AC1 asserted equality with `pot()` —
  those two cannot both be true for a refunded hand. Net contributions are the
  only choice that satisfies `validate` and keeps the four identity consumers
  (`root_pot`, the certifier, `cache_key`, `publicSeed`) reading the same pot
  the state reports.
- **The preflop branch is unchanged, and that is a requirement, not an
  observation.** It returns the hand-start `rootState` built from `rootDef`
  (`:494-503`), which is what the preflop blueprint's identity expects and why
  `same_root` matches it. Returning the cursor there would publish a state whose
  `pending` and `actor` reflect the replayed actions instead of the hand start.
  The postflop branch is the only one that changes.
- `out.state` is a `std::optional<poker::GameState>`
  (`engine/src/protocol/mappers.hpp:65-70`) and the cursor is a local in the
  replay (`:337`); `out.hero_actor` (`:159`, validated against the cursor at
  `:411`) is the actor source, consistent with what the attached runtime
  carries.
- `same_root` in both places compares **`board_size`, then all `board_size`
  cards**. The size equality alone is not enough: two four-card states that
  differ only in `board[3]` both have `board_size == 4`, so a three-card loop
  still calls them equal. Both halves are required — the equality stops a
  three-card record matching a four-card query, and the widened loop stops two
  four-card states matching each other. Neither half is redundant with the
  other: a three-card `GameDef` leaves `board[3]` at its zero default
  (`engine/include/bs/game_definition.hpp:303`), card id 0 is a real card, and
  the widened loop alone would therefore match a flop record against a turn
  query whose turn card happened to be card 0. The `canonical_b` overload keeps its
  three-card storage, which is sound because a card-abstracted record is always
  a three-card flop class; the size equality is checked before that storage is
  read, so the pointer is only dereferenced for a matching size, and
  `canonical_query_board` now declines to supply one for a longer board.
- `canonical_query_board` **stays three-card and is narrowed to say so**: it
  returns `nullptr` unless `query_def.board_size == 3`, instead of accepting any
  `board_size >= 3` and canonicalizing the first three cards of a longer board
  (`engine/src/resident/resident_policy.cpp:753-761`). Generalizing it to a span
  is explicitly *not* proposed: only three-card records exist, so a five-card
  canonicalization would be unexercised outside a synthetic test, and its
  semantics are unresolved (Open Question 1). The narrowing makes the
  three-card contract local to the function rather than a property the caller
  must preserve by checking sizes first.
- `build_v2_game` is **unchanged**: it already throws for a four- or five-card
  root (`engine/src/resident/resident_policy.cpp:110-113`), naming the board
  size. Because this RFC adds no turn/river library, that condition is never
  satisfiable, so the refusal stays where it is and gains a pinning test. The
  query-time outcome is *labelling*, not a new refusal — see "What a turn
  decision becomes" below.
- The resolver consequence is intended, not incidental. Every resident record
  is flop-rooted, so once `same_root` compares sizes a four- or five-card query
  matches none of them: `resolver_source` returns `nullptr`
  (`engine/src/resident/resident_policy.cpp:1234-1241`) and `resolvingDecision`
  takes its no-blueprint branch, which tries `resolve_uniform` and then
  `resolve_flop` (`apps/engine-host/main.cpp:187-230`). That is the correct
  reading of the identity — one flop root cannot serve a node rooted one street
  later — and it is the only route a turn or river decision now takes.
- The reach-model loop blocks `board_size` cards, not three.
- The prefix-board builder and board assembly gain the same board-size
  parameter and a test each.

**Which of these are live, and which are latent — and what that costs the test
plan.** Four of the sites above cannot fire in production after this change, and
an earlier draft both miscounted them and asked for tests that cannot exist:

- The span read at `resident_policy.cpp:861-872` is closed by the `same_root`
  size check, not by editing that function. A turn query no longer matches any
  record, so `replay_public_path` is never entered with a turn event against a
  three-card state.
- The reach loop, the prefix builder, and both board-assembly blocks sit in
  **anonymous namespaces** (`resident_policy.cpp:742-975`; `read_action_probabilities`
  is file-local) and are reachable only through `public_belief`/`hero_decision`
  (`engine/include/bs/resident_policy.hpp:284,292`) *after* `resolve_root`
  matches — which after the change a 4/5-card query cannot do, and `build()` is
  the only constructor (`:270-272`). There is no seam, so a test cannot "reach"
  them and copying the logic into a test would not exercise the site.
- Sites the earlier draft's list missed, found by the stage-1 grep rather than
  by reading: the second three-card loop in `ReachModel::initialize`
  (`engine/src/resident/public_reach.cpp:164`), the lazy flop-library record path
  that assigns `board_size = 3` at `resident_policy.cpp:574-576`, and — the one
  that matters — **`cache_key` mixes only `def.board[0..2]`
  (`engine/src/resolver/resolver.cpp:57-59`)** while `publicSeed` mixes the same
  three (`apps/engine-host/main.cpp:403-407`). Both are identity-def readers, so
  under the two-view design they must hash all `board_size` cards or two distinct
  roots will share a cache entry and a resolver seed. The grep found the first;
  the same reading applies to `publicSeed`, which already mixes `def.board`
  three times by design and must follow the identity def's size. The record enumeration was also short: the
  eager legacy paths use `solver::to_unified_game` at `:628` and `:679`, which
  hardcodes three cards (`engine/src/solver/seat_policy.cpp:75`), in addition to
  `:355`, `:544`, `:549`, `:621`, and `:672`.

Two honest consequences, both reflected in AC1 rather than papered over:

1. These are **latent** fixes: correctness-by-construction for the day a
   four- or five-card record exists. They are included because leaving them
   three-card would make the first such record silently wrong.
2. Verifying them **requires a new seam, and this RFC says so unconditionally**.
   An earlier draft presented an integration test as the chosen route with a
   seam as fallback; that was wrong. `Record` is forward-declared and `records_`
   is private (`engine/include/bs/resident_policy.hpp:313,335`), `build()` is the
   only constructor (`:270-272`), and a four-card def passed through it hits
   `build_v2_game` and lands as `LoadFailed`, never advertised. There is no way
   to place a synthetic four-card record without widening the interface.

   The seam is therefore part of the change: a construction entry point that
   accepts a pre-built game without a board-size refusal, marked with its
   single caller and its lifetime in the header comment. It is **not** a
   compatibility shim — it exists so the sites can be tested, and the
   no-legacy rule forbids leaving it in place once a turn-rooted record makes it
   unnecessary. AC1 states this as a required part of the commit.

### Behavior change and its gate

The adapter change is measured, not assumed, and it is **unconditional**: one
code path exists and the adapter always preserves the board the room reported.
There is no per-seat-count switch between an old and a new behaviour — that
would be exactly the dual implementation the repository forbids, and it would
violate G3 at the seat counts it kept on the old path. A seat count that misses
the criteria below is **reported as a failing capability**; it has no old path
to fall back to.

The replay therefore measures **what the change does**, not whether to enable
it. It answers two questions: how often a turn decision is labelled
`turn_root_unserved` rather than served from a resident record, and whether
every seat count meets the budget.

**The corpus does not exist yet.** `sessions/` is empty and gitignored
(`.gitignore:2`), so the >= 200-snapshot corpus requires **live play under
separate authorization**. This RFC cannot reach `Implemented` before that, and
says so rather than implying the data is available.

**Configuration.** The replay reuses `collectReplaySnapshots`
(`tools/replay/journal.ts:22`), which already dedupes by
`(room, handId, revision, street)`. It must run with **the same resident roots
and flop library as production**. An earlier draft said the harness configures
neither; that is half wrong. Roots are indeed unset
(`tools/replay/main.ts:8-40`), but libraries are auto-detected:
`resolveFlopLibraries` falls back to `autoDetect()`
(`platforms/river-club/src/engine.ts:242-245`), which scans
`artifacts/flop-library*/manifest.json` (`:158-168`), and
`artifacts/flop-library-6way-fullrange/manifest.json` exists. The harness
therefore does load a resident library today and does not load the configured
roots production uses. The replay must set both explicitly, and its `timeoutMs`
must match the criteria below.

**Decision source** is derived from the response's `guaranteeLevel` and reason
string, not inferred.

**Acceptance evidence must survive review.** The raw JSONL is gitignored, so
Stage 3 commits either a redacted subset or the corpus's SHA-256 plus the exact
extraction command into `docs/plans/rfc0032-replay.md`.

**Criteria**, per seat count over the stratified corpus. The thresholds are
derived from the **production** engine budget, which caps every live decision at
`maximumMs = 2_000` (`apps/river-club-agent/runner-state.ts:87`) and passes that
as `solveTimeBudgetMs` (`apps/river-club-agent/main.ts:411-434`). A replay
configured to allow longer would measure a system that does not ship:

| Criterion | Threshold |
| --- | --- |
| p95 decision latency | <= 1,600 ms (80% of the cap) |
| p99 decision latency | <= 1,950 ms (97.5% of the cap) |
| decisions at or above the 2,000 ms cap | 0 |
| `turn_root_unserved` count | counted and reported per seat count |

The 80%/97.5% headroom is the point: a decision that reaches the cap has already
spent its entire budget and is one scheduling hiccup from a fallback, so the
criteria require the work to finish with room to spare rather than at the
ceiling. Raising `maximumMs` is not proposed here — it would change every
decision at every street and needs its own gate.

A snapshot is one **actionable postflop state** (`room.next === "act"`)
recorded in a session journal. "At least two tables" is identified by distinct
`room.id` (falling back to `room.name`).

**The corpus carries two strata, and the flop stratum is not optional.** An
earlier draft defined a snapshot as `board_size >= 4`, which is turn-only, and
then ran stage 1's flop before/after count "on the same corpus". A regression
that stopped a *flop* query matching its resident record would not have appeared
in a single measurement, and AC5's "served from a resident record is
behavior-identical" would have been satisfied vacuously — nothing would be
served from a resident record. The corpus is therefore stratified:

| Stratum | Requirement | What it can detect |
| --- | --- | --- |
| Flop (`board_size == 3`) | >= 20 per seat count, 2..6 | loss of resident matching, `publicSeed`/`cache_key` drift, stage 1's error-to-strategy change |
| Turn/river (`board_size >= 4`) | >= 20 per seat count, 2..6 | the RFC's subject |

Both strata come from the same two-table requirement. The flop stratum is also
guarded in-tree by the existing fixture at
`platforms/river-club/tests/w1-resident-wiring.test.ts:505-543`, which asserts a
blueprint-served flop decision and fails if the resident path stops matching —
that test runs on every `npm run check`, so the regression is caught even
without the corpus.

### What a turn decision becomes, and why the current path breaks

Two facts constrain this section. An earlier draft got the first right and
missed the second, which invalidated its mechanism.

**A resolver miss does not produce an error on the production path.** The
adapter defaults to `SolverMode.AUTOMATIC` (`platforms/river-club/src/mapper.ts:996`),
and in AUTOMATIC the envelope falls through a miss to
`mapOperationalFallbackResponse` (`engine/src/protocol/envelope.cpp:356-377`) —
a *strategy* response. Only `SOLVER_MODE_RESOLVING` maps a miss to an error code
(`:343-352`). Other error codes remain reachable for other reasons — validation,
replay, protocol — but none is produced by a miss.

**The path a turn decision takes instead is already broken, and this RFC lands
traffic on it.** With every record flop-rooted, a four- or five-card query
matches nothing, `resolver_source` returns `nullptr`
(`engine/src/resident/resident_policy.cpp:1238-1239`), and `resolvingDecision`
enters its no-blueprint branch, which tries `resolve_uniform` and then
`resolve_flop` (`apps/engine-host/main.cpp:187-230`). When either **solves**, it
sets `result.row.artifact_sha256 = {}` (`:202`, `:222`) — RFC 0025 specifies
exactly that, "no artifact" (`docs/rfcs/0025-off-tree-decision-handling.md:345`).

The response mapper rejects it. `mapResolvedResponse` routes any non-certified
outcome to `mapExpandedResponseImpl` (`engine/src/protocol/response_mapper.cpp:317-326`),
which throws `MappingError(ERROR_CODE_INTERNAL, "resident answer lacks an
artifact digest")` when the digest is not 64 characters (`:191-193`). The
envelope converts the throw into an error response (`envelope.cpp:404-406`), the
adapter throws (`platforms/river-club/src/mapper.ts:1080-1083`), and `decide()`
converts it to `safeFallback` (`engine.ts:437-453`) whose reason is
`safe-fallback:check|call|fold` (`mapper.ts:157-168`).

So in the case the RFC previously called the new normal — a turn spot the
subgame resolver *successfully solves* — the client receives an internal error
and plays a mechanical check/call/fold. It also means a label emitted only at
the operational-fallback boundary would fire on the **failure** branch and be
absent on the **success** branch, which is the opposite of useful.

This is a pre-existing defect on a path the production adapter rarely reaches,
because the flop clamp currently keeps every query on the blueprint path. This
RFC is what makes it the normal path, so fixing it is in scope: a change that
only preserves the board would convert a wrong-but-working decision into an
internal error.

### The digest contract, and how this RFC repairs it

`artifact_sha256` is `optional string` with pattern `^[a-f0-9]{64}$`
(`proto/bigshark/engine/v2/engine.proto:609`). RFC 0025 chose `""` because a
uniform-prior subgame has no artifact; the mapper requires 64 characters because
it treats the digest as mandatory for any strategy response. Both are
defensible; together they are a contradiction.

The repair keeps RFC 0025's semantics and makes the **mapper** honour the
field's optionality:

- The digest is emitted only when a real artifact backs the row. For a
  subgame-solved row it stays unset, matching `:345`.
- `mapExpandedResponseImpl` accepts an absent digest and rejects only a present
  one that is not 64 hex characters. The guarantee level and the
  `subgame-resolve:approximate` diagnostic
  (`engine/include/bs/resolver.hpp:108`) already distinguish the source, so the
  digest need not carry that meaning.
- The adapter already treats the digest as optional
  (`platforms/river-club/src/mapper.ts:1063`, `:1089` spreads it only when
  defined), so no client change is required.
- The metadata for these rows also misreports the source:
  `mapExpandedResponseImpl` sets `reason_code = "blueprint"` and
  `cache_hit = true` for any `SOLVER_SOURCE_BLUEPRINT` row
  (`engine/src/protocol/response_mapper.cpp:246-247`), while RFC 0025 specifies
  `"resolving"`, `SOLVER_SOURCE_RESOLVING`, and `cache_hit = false` for a
  subgame row (`docs/rfcs/0025-off-tree-decision-handling.md:340-344`). Stage 1
  corrects this too, because AC4's source accounting reads those fields and
  would otherwise classify every subgame row as a blueprint row. The adapter's
  reason precedence (`platforms/river-club/src/mapper.ts:1049-1051`) already
  prefers `diagnostic_reason`, so the label reaches the log either way — but
  reporting a cache hit that did not happen is a false signal in its own right.
- Losing the digest is not a silent quality regression: the response carries
  `guarantee_level` and the diagnostic token, and both reach the results log.

**The emit side must clear the field, not set it empty.** `mapExpandedResponseImpl`
currently writes `metadata->set_artifact_sha256(...)` unconditionally
(`engine/src/protocol/response_mapper.cpp:248`); for an empty digest that is
*presence with a zero-length value*, which the RFC's own rule would still
reject. It must call `clear_artifact_sha256()` when the row has no artifact, and
AC3 must assert `has_artifact_sha256() == false`, not an empty string.

**The relaxation must be scoped, not global.** `mapExpandedResponseImpl` serves
resident rows too (`:311-326`), and `mapResolvedResponse` labels an Approximate
outcome `SOLVER_SOURCE_BLUEPRINT` (`:324`), so the source cannot distinguish
them. Relaxing the digest check globally would also accept a resident row that
lost its digest, where the 64-character check is the only assertion. The
relaxation is therefore keyed on the row having no artifact — which only the
subgame branches produce — and every resident row still requires 64 characters.

**This is a live flop change, and an earlier draft said the opposite.**
`resolvingDecision`'s no-blueprint branch is reached whenever a root is
advertised but the query matches none (`apps/engine-host/main.cpp:179-181`, whose
own comment names "a seat count without a production flop library"), and
`resolve_flop` is eligible exactly at `board.size() == 3`
(`engine/src/resolver/subgame_resolver.cpp:469-472`) — which is what every
postflop query is clamped to today. So today's empty-digest path is a **flop**
path, and stage 1 converts those rows from `ERROR_CODE_INTERNAL` into an
`approximate` strategy.

That is very likely an improvement, but it is a real behaviour change with no
replay evidence attached, and it must be measured rather than assumed. AC5 and
the rollout stop condition are worded accordingly: *responses served from a
resident record* are byte-identical; the previously-erroring no-resident-match
path now returns a strategy. Stage 1 records its own before/after count on the
same corpus AC4 uses, so the flop change is visible even though it is not the
RFC's subject.

### Attribution: `turn_root_unserved`

With the digest repaired, a turn decision ends in exactly one of two states, and
both are attributable without new machinery:

| Outcome | Signal already on the wire | New work |
| --- | --- | --- |
| Subgame solved | `guaranteeLevel = approximate`, `reason = subgame-resolve:approximate` | none |
| Operational fallback | `guaranteeLevel = operational_fallback` | reason becomes `turn_root_unserved` for a 4/5-card board |

The label is set in `operationalFallbackDecision`
(`engine/src/protocol/envelope.cpp:171-189`) from the request board size, and
`mapOperationalFallbackResponse` writes `decision.reason` into `reason_code` and
`diagnostic_reason` instead of the hardcoded literal
(`engine/src/protocol/response_mapper.cpp:328-371`). A flop or preflop fallback
keeps `operational-fallback`, so the frozen path is byte-identical.

Two details matter for counting, and an earlier draft got both wrong:

- **The runner prefixes the reason.** `validateEngineDecision` returns
  `reason: \`cpp:${decision.reason}\`` (`platforms/river-club/src/mapper.ts:188-192`),
  so the log line reads `cpp:turn_root_unserved`. The counter must match on
  suffix or after stripping the prefix; the existing replay counter matches
  `safe-fallback` by prefix (`tools/replay/main.ts:38`) and would need the same
  care.
- **The decision must still be legal.** Live play must never stall, and the
  runner has no handler for a thrown decision
  (`apps/river-club-agent/main.ts:504-694`: the loop body catches only CLI and
  JSON parse failures, so a throw from `decideWithinBudget` escapes the loop and
  skips the `leave` at `:698`).

"Visible" here means **accounted for in the results log and countable in the
replay**, not surfaced to an operator in real time.

## Dependency Rules

- The River adapter owns the wire-to-`GameDef` conversion and must not decide
  the board size independently of what the room reports.
- The resident path owns `same_root` and the reach blocking loop; neither may
  truncate a board.
- No format, no evaluation, no tree changes. RFC 0029 is not a dependency.

## Compatibility and Migration

- No persisted format changes. No library, checkpoint, or artifact is
  rebuilt, and the 188.4 MB `library.bin` and its RFC 0027 byte-identical
  reproduction are untouched.
- `.sections/build.json` is unchanged.
- The adapter change is behavior-visible for turn and river spots at every
  seat count; there is no per-seat-count switch, as above.
- **A second newly-reachable class exists at the flop, and it is stated here.**
  Once heads-up facing-bet and refunded postflop hands construct, their identity
  def carries unequal net contributions and a larger pot, so they match no
  flop-rooted street-start record and take the same subgame path a turn spot
  does — at the flop. That is correct (the record is genuinely a different
  root), but it is not a turn/river effect, and a reader comparing flop
  behaviour before and after would otherwise find an unexplained difference.
- **The wire changes for subgame-served rows only.** A row that today maps to
  `ERROR_CODE_INTERNAL` maps to an `expanded_strategy` with `artifact_sha256`
  absent. Responses served from a resident record do not move. `buf breaking` is
  unaffected because no `.proto` file changes — `artifact_sha256` stays
  `optional string` (`proto/bigshark/engine/v2/engine.proto:609`) and
  `proto/baseline/v2.binpb` is the buf breaking-change descriptor baseline
  (`package.json:29`,
  `docs/reference/protobuf-engine-protocol.md:77`), not a response fixture, so it
  neither moves nor needs to. Clients that require the digest must treat it as
  optional — the adapter already does (`platforms/river-club/src/mapper.ts:1063`).

## Security and Operational Impact

- No credential or trust-boundary change; the adapter consumes only public
  state.
- Observability: this RFC **adds** debug-build logging of the board size on
  every adapter conversion (`resident_mapper.cpp` has none today), so a
  truncation is visible rather than silent, and a test asserts the line is
  emitted, and the `turn_root_unserved` label records the board size.

## Alternatives Considered

1. **Fix only the adapter.** Rejected as the whole change, because the resident
   path would then silently fall through on a three-card identity match. Its
   replay measurement is retained, but as the record of what the full change
   does — not as a gate deciding whether to enable it.
2. **Add turn/river class libraries.** Out of scope here, economically
   unattested at six seats, and it would supersede RFC 0011's chosen direction
   rather than implement it.
3. **Implement RFC 0011 Phase 3 now.** Rejected: Phase 3 explicitly defers
   itself until Phase 1/2 show the on-demand path is too slow, and nothing here
   establishes that trigger. A separate RFC may take it up.
4. **Add `RiverFrontierEvaluator`.** Rejected: `EquityFrontierEvaluator`
   already covers 3-, 4-, and 5-card boards with an independent oracle test.

## Risks

1. **The adapter change changes live decisions with no quality evidence, and
   this RFC does not claim the result is better.** What it claims is narrower:
   the decision stops being made for the wrong game — a flop board, and on a
   checked-to line the wrong seat, both of which the current checkpointing
   rebuild produces — and the outcome becomes attributable. Whether a
   uniform-prior subgame solve beats the current wrong-but-fast flop decision at
   five and six seats is **unmeasured** and this RFC supplies no evidence for
   it. *Mitigation:* the Stage 3 replay records
   action, source, and latency per seat count, so the difference is visible
   rather than assumed — but a reader should treat the quality question as open,
   not settled by the correctness argument.
2. **Turn and river spots lose their current (wrong but fast) path.** The
   replacement is `resolve_uniform` — turn-rooted, uncapped, no depth limit,
   uniform priors — which at five and six seats is expected to build toward the
   tree cap and then fall back to the operational fallback. *Mitigation:* that
   fallback is labelled
   `turn_root_unserved` and counted, and the replay reports the count per seat
   count so the cost is measured rather than hidden. Whether the new decision is
   better is not claimed here.
3. **More truncation sites exist than are enumerated.**
   *Mitigation:* AC1 lists each known site with its own test, and a
   `board_size >= 4` fixture is run through every entry point named in the
   resident and protocol sources.
4. **The two-view state is a new coupling and can drift.** `def()` and the
   runtime now describe different points in the hand by design, and a consumer
   that reads the wrong one gets a plausible but wrong answer — the failure this
   design exists to prevent arriving from the other direction. The concrete
   failure mode is an **unvalidated identity def**: the seam attaches a def
   without the constructor's `validate(def)` call
   (`engine/include/bs/game_definition.hpp:218`), so a def that `poker::validate`
   would reject is published and five identity consumers hash, compare, and
   certify against it with no error anywhere. *Mitigation:* the seam validates
   the identity def and throws if it fails; AC1 asserts `validate` passes for
   every fixture shape, not only `def().pot == pot()`.

   A second, latent form: `same_game_def` compares `last_full_raise`,
   `street_committed`, and `raise_rights` (`engine/src/poker/game_definition.cpp:275`,
   `:285`). The identity def now carries them for a mid-street root and leaves
   them zero at street start, so the trap is **shape-dependent**: a future caller
   routing `def()` through that predicate would match on one class of hand and
   silently mismatch on another, which is harder to notice than a uniform
   mismatch. No River path calls it today (`same_root` does not compare these
   fields, `engine/src/resident/resident_policy.cpp:53-87`), so it is latent —
   but the shape dependence is the reason to note it.
5. **A four-card state reaches code that assumes a three-card board.**
   `GameState::board()` now yields four or five cards from an adapter-built
   state, and any remaining `i < 3` or `3 + street` arithmetic that this RFC
   missed reads the wrong card. *Mitigation:* AC1's fixture asserts the exact
   four- and five-card span contents at each site, because ASan cannot see this
   class of error — the backing array is five ints and the read stays inside it
   (see the "corrupted information key" fact). A behavioural assertion is the
   only detector; an earlier draft wrongly relied on the sanitizer.

## Verification Plan

- **Baseline pin.** A test reproduces today's truncating behavior — a turn
  snapshot yields a `board_size = 3` game with `board[0..2]` — so the change is
  demonstrably a change.
- **Board preservation.** A four- and a five-card snapshot produce a `GameDef`
  carrying every card, through the adapter and into the resident entry points.
- **Identity.** `same_root` distinguishes two four-card states differing only in
  `board[3]`, two five-card states differing only in `board[4]`, and a
  three-card record from a four-card query. The first two are the only cases the
  current three-card loop gets wrong once the size equality exists, which is why
  they are the ones tested. `canonical_query_board` returns `nullptr` for a
  four- or five-card query and a canonical board for a three-card one.
- **Reach.** The blocking loop excludes every board card, verified by a fixture
  where `board[3]` is otherwise dealable. This is a unit-level fixture; no live
  query reaches the loop after the change.
- **Labelling.** A four-card root served from the operational fallback carries
  the `turn_root_unserved` reason, and `resultsLogEntry` writes it. A native
  test asserts the reason is produced only for a four- or five-card request that
  reaches the fallback; a Node test asserts the runner logs it. No new error
  code is introduced, because AUTOMATIC never returns one for a resolver miss.
- **Replay measurement.** The 200-snapshot corpus replayed with production
  roots and matching `timeoutMs`; action, source, p95, p99, and
  `turn_root_unserved` counts per seat count recorded in
  `docs/plans/rfc0032-replay.md`, with either a redacted subset or the corpus
  SHA-256 and extraction command, since the raw journals are gitignored.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

1. **Digest repair, standalone.** Clear `artifact_sha256` instead of setting it
   empty, and accept an absent digest only for a row with no artifact, keeping
   RFC 0025's semantics (`docs/rfcs/0025-off-tree-decision-handling.md:345`).
   This lands **before** the adapter change. Gate: a subgame-solved response
   maps to a strategy instead of `ERROR_CODE_INTERNAL`; a resident row that lost
   its digest is still rejected; and the change's own before/after count on the
   AC4 corpus is recorded, because **this stage changes a live flop path** —
   today's no-resident-match flop query errors and will now return an
   `approximate` strategy. That is outside this RFC's subject and is measured,
   not assumed.
2. **Baseline pin and site enumeration.** Add the failing-baseline test and
   enumerate every identity/truncation site with its own test. Gate: the
   baseline test fails as expected and the list is complete against a grep.
3. **Board preservation and the two-view state.** Adapter (cursor runtime plus
   a decision-node identity def, in **one commit** — landing the runtime without
   the def disables the resident path at every street), `same_root` (both),
   `canonical_query_board` narrowing, reach loops (both), record paths,
   prefix-board builder, board assembly. Gate: preservation, identity, and
   hero-attribution tests pass; the flop path is unchanged for responses served
   from a resident record.

   **Cost, stated because it is real:** AC1's fixture set is 24 cells
   (2 seat-counts × 3 streets × 2 wager states × 2 refund states), and each cell
   needs a hand-built snapshot — there is no live corpus to draw from. That is
   more mapper fixture work than the repository currently has, and it belongs in
   this stage's estimate rather than being discovered during it.
4. **Labelling.** Add the `turn_root_unserved` reason to the operational
   fallback path and to `resultsLogEntry`. Gate: native and Node tests pass, and
   the label is counted in the replay output.
5. **Replay measurement.** Requires live play; the corpus does not exist yet.
   Run the recorded set with production configuration and record action, the
   four-way source table, and latency per seat count into
   `docs/plans/rfc0032-replay.md`. Gate: the result file exists with its
   evidence (redacted subset or corpus SHA-256 plus extraction command), **and
   the `safe-fallback:*` count is zero**.

Order matters: stage 1 is a prerequisite for stage 3, because landing board
preservation first would route live turn traffic into the digest error and turn
a wrong-but-working decision into a mechanical check/fold.

Stop conditions: any change to a **flop** response served from a resident
record halts the rollout. (Scoped to the flop because a turn check-through line
matches a flop-rooted record today — that is the RFC's own first Fact — so a
turn response *is* resident-served before the change and is not after it. The
unscoped wording would halt on the intended effect.) The stage-1 no-resident-match change is expected and
recorded, not a stop condition — halting on it would halt on the stage itself.
A seat count that misses the latency criteria is **reported as a failing
capability**, not quietly left on the old path — there is no old path to leave
it on.

**The last gate cannot be discharged from the repository.** Step 5 needs a
corpus that does not exist (`sessions/` is empty and gitignored), so this RFC
stays `Implementing` after stage 4 and does not reach `Implemented` until that
run happens under separate authorization. Stating this in the rollout plan
rather than only in AC4 is deliberate: an unbounded gate should be visible to
whoever schedules the work.

## Rollback Plan

- Every stage is a source change with no persisted-format effect. Reverting
  restores the three-card clamp and today's behavior.
- No artifact is migrated in either direction.

## Open Questions

1. **What are the five-card suit-canonicalization semantics?** The canonical
   representative of a four- or five-card board, and whether the newly dealt
   card participates in the relabel. This is unspecified today and decides
   whether any later class addressing is consistent. It does not block this
   RFC, which only preserves cards; it blocks the follow-up that addresses
   classes, and that follow-up is **not a small change**: because
   `canonical_query_board` is narrowed to three cards here, any future
   class-addressed turn or river root needs a second identity scheme alongside
   the flop one, with its own digest and its own comparison rule.
2. **Is a turn-rooted frontier value sufficient with no turn/river library?**
   Untested. It determines whether the follow-up needs RFC 0011 Phase 3 or can
   stop at the addressability fixed here.

## Acceptance Criteria

- **AC1**: The adapter change (postflop state sourced from the replay cursor) and
  both `same_root` changes land in one commit; `same_root` does not compare
  `board_size` on the postflop branch today, so an adapter-only landing could
  match a four-card root against a flop record. Every truncation site is
  enumerated with a test of the stated kind:

  | Site | Test kind |
  | --- | --- |
  | Adapter (`resident_mapper.cpp:504-532`) | direct |
  | `same_root`, both (`resident_policy.cpp:53-87`, `counterfactual_reach.cpp:26-39`) | direct |
  | `build_v2_game` (`:110-113`) | direct |
  | `canonical_query_board` narrowing (`:753-761`) | direct |
  | Reach loop #1 (`public_reach.cpp:140`) | direct |
  | Reach loop #2 (`public_reach.cpp:164`) | direct |
  | Resolver cache key (`resolver.cpp:57-59`) | direct: all `board_size` identity cards |
  | Record paths `:355,544,549,574-576,621,628,672,679` | direct (assert `board_size`) |
  | Postflop runtime source (`:504-532`) | direct: cursor runtime returned |
  | **Decision-node identity def** | direct: see the assertions below |
  | Gross-vs-net contributions (`:527`, refund case) | direct: net in runtime, and in the identity def |
  | Prefix builder (`:861-872`) | integration, synthetic record |
  | Board assembly (`:1087-1093`, `:1191-1198`) | integration, synthetic record |

  The identity row is the one that would otherwise pass while the design is
  broken, so its assertions are enumerated rather than described. The fixture
  set is the cross product of {heads-up, 6-max} × {flop, turn, river} ×
  {street-start, facing a bet} × {with, without a refund}, and for every cell
  `out.state` must satisfy: **`poker::validate(out.state->def())` passes**; `def().board_size ==
  board().size()`; `def().board` equals `board()` element for element;
  `def().preflop == false`; and `def().pot == pot()`.

  Two further assertions hold for **every** cell: it must validate, construct,
  and satisfy `out.state->actor() == out.hero_actor` (already guaranteed by
  `engine/src/protocol/resident_mapper.cpp:411`, asserted here so the runtime is
  pinned). Note what is **not** asserted: `GameState(identity_def).actor()` is
  *not* expected to equal the hero. For a street-start check-through turn the
  identity def is deliberately indistinguishable from a street-start root, so a
  rebuilt state's actor is the seat clockwise of the button — the opponent when
  the hero acted first. That is exactly why the runtime carries the pending set
  and actor separately, and an AC that asserted the def-derived actor would be
  asserting the two-view design does not exist. But the two `same_root`
  properties — a flop query still matching a flop-rooted record, and a turn query
  differing only in `board_size` — apply **only to the street-start column**, and
  an earlier revision wrongly required them of every cell. A facing-a-bet def
  necessarily carries unequal net contributions and a different pot than a
  street-start record, and `same_root` compares both
  (`engine/src/resident/resident_policy.cpp:55`, `:82-85`), so it cannot match.
  The AC's table states which assertion each column carries:

  | Column | `validate` | construct + actor | board eq. | `def().pot == pot()` | flop `same_root` | turn differs only in `board_size` |
  | --- | --- | --- | --- | --- | --- | --- |
  | street-start | required | required | required | required | **required** | **required** |
  | facing a bet | required | required | required | required | not applicable — the root genuinely differs | not applicable |

  The design's load-bearing property is about the street-start root, and the AC
  says so rather than demanding it of a class where it is false by construction.

  The "facing a bet" axis is not decoration: it is the cell that fails without
  the mid-street fields. An earlier revision of this AC listed "equal and
  unequal contributions" without saying which *cause* the inequality, and an
  inequality with no live wager cannot be validated at all — so the axis is the
  wager, and the unequal-but-street-start cell is excluded because
  `close_street()` refunds before the street closes (see the Pipeline section).

  The `validate` assertion is the seam's rule, not a formality. `GameState`'s
  documented contract is "`validate(def)` first"
  (`engine/include/bs/game_definition.hpp:218`), and the seam attaches a def
  without going through the constructor. If the seam skips validation, an
  impossible def is published and five identity consumers hash, compare, and
  certify against it with no error. The rule is therefore: **the seam validates
  the identity def and throws if it fails**, exactly as the constructor would.

  A test that only asserts "the cursor is returned" would be green with `def()`
  left at the preflop root and the resident path dead at every street.

  The two anonymous-namespace rows require a **new seam**, and the RFC says so
  unconditionally: `Record` is forward-declared and `records_` is private
  (`engine/include/bs/resident_policy.hpp:313,335`) and `build()` refuses a
  four-card def at `build_v2_game`, so a synthetic record cannot be placed
  without widening the interface. The seam ships in the same commit, with its
  single caller and lifetime documented, and a test that copies the logic does
  not count. Span contents are asserted explicitly rather than relying on a
  sanitizer, which cannot detect this class (see the facts above).
- **AC2**: A baseline test pins today's truncating behavior and fails before the
  change.
- **AC3**: A four- or five-card decision never arrives as `safe-fallback:*`.
  A subgame-solved row maps to a strategy with its digest absent and
  `guaranteeLevel = approximate`; a fallback row is labelled
  `turn_root_unserved` (logged as `cpp:turn_root_unserved` and matched on
  suffix); no path silently decides a three-card game. Verified natively
  (`mapResolvedResponse` accepts an absent digest, rejects a malformed one) and
  in Node (the adapter surfaces both reasons unchanged).
- **AC4**: The 200-snapshot corpus (>= 20 per seat count, 2..6, >= 2 tables by
  `room.id`) is replayed with production resident roots and library.
  The corpus is the two-stratum set defined above.
  "Before" is a named pre-change revision recorded in
  `docs/plans/rfc0032-replay.md` with its exact command and a preserved binary,
  since Stage 2 lands the change before Stage 3 runs; "after" is the same
  command on the change. The file records per seat count the action, the
  decision source, latency p95/p99, and the `turn_root_unserved` count, against
  the 1,600 ms / 1,950 ms criteria.

  **Source accounting must separate four outcomes, not two.** An earlier draft
  derived the source from `guaranteeLevel` and reason alone, which cannot
  distinguish a turn root that fell back from an engine that broke: before the
  digest repair both arrive as `operational_fallback` + `safe-fallback:*`. After
  the repair they are separable, and the replay must report them separately:

  | Source | Signature |
  | --- | --- |
  | Subgame-solved | `guaranteeLevel = approximate`, reason `subgame-resolve:approximate` |
  | Labelled fallback | `guaranteeLevel = operational_fallback`, reason `cpp:turn_root_unserved` |
  | Ordinary fallback | `guaranteeLevel = operational_fallback`, reason `cpp:operational-fallback` |
  | Transport/engine failure | reason `safe-fallback:*` |

  A non-zero count in the last row is a **failure of this RFC**, with one
  exception noted so an unreachable-cell rejection is not miscounted: a hand the
  provider reports in a shape the local reducer would not produce (an unequal
  ledger with no seat facing a wager) legitimately fails `validate` and lands in
  `safe-fallback:*`. Those are counted and reported separately, with their
  reason, rather than folded into the failure bucket.

  Because the corpus requires live play under separate authorization, this AC is
  not dischargeable from repository evidence alone. That makes `Implemented`
  unbounded, so it is stated as a rollout gate below rather than only here.
- **AC5**: Every **flop** response served from a resident record is
  behavior-identical — no artifact digest changes, no resolver candidate
  changes — and the full Engine Build Contract passes with ASan clean and `check-rfcs` / `check-docs`
  passing. The **no-resident-match** path is *not* identical: stage 1 turns a
  flop query that currently errors into an `approximate` strategy. AC5 asserts
  the resident path byte-identically and requires stage 1's before/after count
  for the other, rather than claiming a no-op it does not perform.

  **This AC cannot be satisfied vacuously, and it does not cover everything.**
  It applies to the subpopulation that constructs a state today, which is
  narrower and more specific than "equal contributions, no refund": **any
  non-refunded 3+-seat shape** (whose contributions may be unequal) plus
  **non-refunded heads-up street-start with equal contributions**. Heads-up
  facing a bet and every refunded hand throw before any decision. Heads-up postflop hands facing a bet and every
  refunded hand are *newly constructed* by this change (see the Pipeline
  section), so no prior response exists for them to be identical to; AC5 must
  not be read as covering them, and AC4's flop stratum is what measures them.
  The claim about responses that *do* come from a resident record requires
  showing they still do. The
  in-tree flop fixture
  (`platforms/river-club/tests/w1-resident-wiring.test.ts:485-515`) asserts a
  blueprint-served flop decision with the fixture's digest and guarantee level,
  and fails if the resident path stops matching; AC5 requires that test to pass
  unchanged. Without it, "no resident-served response changed" would be true
  the moment none were served at all.

## Decision

```text
Author agent: BigShark maintainers
Approved by: independent-approval-agent/opensource-deepseek-v4_1_flash/2026-10-21
Decision date: 2026-10-21
Review outcome: Approved
Reviewed scope: Seventh revision in full, against the cited sources, with
  out-of-tree probes compiled against the repository's own libraries. The
  reviewer built the 24-cell cross product of {heads-up, 6-max} x
  {flop, turn, river} x {street-start, facing a bet} x {refund, no refund},
  validating each identity def with poker::validate, constructing a GameState
  from it, and reading the resulting actor; replayed today's rebuild across five
  classes to establish which hands construct before the change; and replayed a
  check-through turn node to compare cursor.actor() against
  GameState(identity_def).actor().
Review summary: Approved on the seventh round. Every cell of the 24-cell matrix
  validates, constructs, yields an actor, and satisfies def().pot == pot(). The
  design's central claim is verified: the identity def uses net contributions
  plus the cursor's mid-street fields, so both poker::validate clauses hold, and
  a street-start flop query still matches its flop-rooted record while a turn
  query differs only in board_size. Two blocking findings were accepted and
  fixed across the last two rounds: the earlier gross/net mix made several hand
  classes fail construction entirely (head-up facing a bet, any refunded hand),
  and net contributions alone were insufficient because the two-seat equality
  rule is keyed on street_committed rather than on contribution values. The
  reviewer's remaining three findings were wording-only and were applied before
  recording this decision: the actor assertion is now stated as
  out.state->actor() with an explicit note that the def-derived actor is not
  expected to be the hero; AC5 and the stop condition are scoped to flop
  responses, since a turn check-through line is resident-served today and is not
  after the change; and AC5's subpopulation is characterized as any non-refunded
  3+-seat shape plus non-refunded heads-up street-start, matching measurement.
  The RFC does not claim the resulting decisions are better; the quality
  question is recorded as open. One risk is unproven and flagged for stage 2:
  whether a turn check-through line actually hits a resident row today, which the
  baseline pin can record.
Remaining risks: the do-not-modify surfaces are the mid-street fields on the
  identity def and the two-view split itself; same_game_def's shape-dependent
  divergence from same_root is latent and recorded; the Stage 5 corpus gate
  remains unbounded and the RFC cannot reach Implemented without live play under
  separate authorization.
```

### Implementation evidence (stage 2)

Stage 2 changed the adapter, both `same_root` copies, the reach loops, the
prefix-board builder, the board assembly, `cache_key`, and `publicSeed`, in one
commit, as AC1 requires. Evidence: the full release suite, the sanitizer preset,
and a mutation check per repaired site.

**Erratum to this RFC's fixture description.** AC1 and the review record above
describe the fixture set as the full 24-cell cross product. Implementation
measured the same population exhaustively — every reachable postflop action node
over 3..202-chip stacks at 2..6 seats, four stack depths — and found two axes are
not independent:

- a refunded action node is always a street start, because `refund_unmatched`
  returns an excess only to the unique highest contributor, leaving every other
  live seat all in for less, so the refunded seat is the only one with chips and
  the only possible actor;
- heads-up never carries a refund at a decision node, because at two seats the
  all-in flag survives the refund and the board then runs out with no further
  action.

`testDecisionNodeIdentityMatrix` therefore drives the fifteen constructible
cells and names the nine unreachable ones, rather than claiming twenty-four. The
design is unaffected; only the matrix's size was overstated. The AC's
four-column assertion table is what the test implements.

**A defect this stage's own matrix caught.** The first identity def copied
`last_full_raise` whenever it was nonzero. `GameState::last_full_raise()` reports
`big_blind` at a street start while `GameDef`'s sentinel is `0`, so a refunded
street-start node carried an increment that `validate` rejects. The fix applies
the rule `build_subgame_def_from_node` already uses: carry the increment only
when some `street_committed` is nonzero.
