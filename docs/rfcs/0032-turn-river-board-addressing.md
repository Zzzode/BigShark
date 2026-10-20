---
rfc: "0032"
subject: "Preserve the Full Board Through the Decision Pipeline"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-21"
owners: "engine resident, River adapter, engine protocol"
supersedes: ""
superseded-by: ""
---

# RFC 0032: Preserve the Full Board Through the Decision Pipeline

## Summary

Every postflop decision in the live pipeline is re-rooted to a three-card flop
board, discarding the turn and river cards. The resident path cannot hold a
four- or five-card root, the board-identity helpers compare only the first three
cards, and the River adapter builds a `board_size = 3` game from a snapshot that
may carry five. This RFC makes the pipeline carry the board it
was given, and pins the current wrong behavior with a test before changing it.
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
(`engine/src/resident/resident_policy.cpp:53-87,753-760`,
`engine/src/resolver/counterfactual_reach.cpp:26-40`), and the public-reach
blocking loop stops at three cards
(`engine/src/resident/public_reach.cpp:140`). Fixing only the adapter would
therefore not feed "the real board" into an unchanged pipeline: it would make
turn decisions stop matching resident records and fall through to
`resolve_uniform`, changing the decision source and its latency.

## Goals

- **G1**: A board with four or five cards survives the adapter, the resident
  identity checks, and the reach model without truncation.
- **G2**: The current truncating behavior is pinned by a test before the change,
  so the fix is demonstrably a behavior change and not a refactor.
- **G3**: Where the pipeline cannot serve a four- or five-card root, the
  outcome is a distinct, counted label — not a silent decision of a different
  game and not an operational fallback indistinguishable from one caused by an
  unrelated miss. It is not an error code: the adapter's solver mode never
  yields one for a resolver miss (see "What a turn decision becomes").

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

## Current State and Evidence

### Facts (verified in this repository)

- **The adapter re-roots to the flop.**
  `engine/src/protocol/resident_mapper.cpp:504-532` builds
  `flopDef.board_size = 3` and `flopDef.board = {cursor.board()[0..2]}` for any
  postflop decision, discarding `board[3]` and `board[4]`.
- **The truncation also corrupts an information key.** For a turn check-through
  line the mapper's flop definition carries the same pot, stacks, and
  contributions as the flop root, so `same_root` matches and
  `replay_public_path` processes turn events against a three-card span. The
  prefix-board builder computes `prefix_board_size = 4` for a turn event and
  reads `state.board()[3]` (`engine/src/resident/resident_policy.cpp:861-872`).
  `state.board()` has size 3, so this is an **out-of-bounds span read —
  undefined behavior** — which happens to yield card 0 only because the backing
  `board_` is a zero-initialized `array<int,5>`
  (`engine/include/bs/game_definition.hpp:303`). The result is a corrupted
  information key, not merely a re-rooted game, and it is an ASan case as well
  as a behavioural one.
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
  `engine/src/resolver/counterfactual_reach.cpp:26-40`.
  `canonical_query_board` canonicalizes only `board[0..2]`
  (`resident_policy.cpp:753-760`).
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

- Serving cost. The exact n-seat reach model is used by the resident
  blueprint-belief path only; `resolve_uniform`, `resolve_flop`, and `resolve`
  do not use it (`engine/src/resolver/subgame_resolver.cpp:340-720`). A prior draft
  of this RFC claimed the recursion makes any turn/river serving path
  non-terminating at five or six seats, citing figures that appear nowhere in
  the repository. Those figures are withdrawn. Any latency claim must be
  measured as part of this RFC's Stage 3 and recorded with its configuration.
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

- `resident_mapper.cpp:504-532` builds the postflop `GameDef` from the actual
  board length and cards instead of clamping to three.
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
  (`engine/src/resident/resident_policy.cpp:750-760`). Generalizing it to a span
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

**Which of these are live, and which are latent.** Three of the sites above
cannot fire in production after this change, and saying otherwise would repeat
the refusal mistake in a different place:

- The immediate out-of-bounds read at `resident_policy.cpp:861-872` is closed by
  the `same_root` size check, not by editing that function. A turn query no
  longer matches any record, so `replay_public_path` is never entered with a
  turn event against a three-card state. The prefix-board and reach-loop edits
  are correctness-by-construction for the day a four- or five-card record
  exists, and today they are exercised only by synthetic fixtures. They are
  included because leaving them three-card would make the first such record
  silently wrong, not because the change depends on them.
- Consequently the reach loop and prefix builder are tested with a hand-built
  four- or five-card `GameDef`/`UnifiedGame`, not through `build_v2_game`, which
  still refuses that shape. AC1 says so rather than implying production
  reachability.

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
and flop library as production** — the existing harness calls
`decide(room, {timeoutMs: 10_000})` with no roots configured
(`tools/replay/main.ts:8-40`, `platforms/river-club/src/engine.ts:372-408`), so
it neither exercises resident lookup nor measures anything meaningful today. The
`timeoutMs` must match the criteria below.

**Decision source** is derived from the response's `guaranteeLevel` and reason
string, not inferred.

**Acceptance evidence must survive review.** The raw JSONL is gitignored, so
Stage 3 commits either a redacted subset or the corpus's SHA-256 plus the exact
extraction command into `docs/plans/rfc0032-replay.md`.

**Criteria**, per seat count over the stratified corpus. The thresholds are
derived from the **production** engine budget, which caps every live decision at
`maximumMs = 2_000` (`apps/river-club-agent/runner-state.ts:87`) and passes that
as `solveTimeBudgetMs` (`apps/river-club-agent/main.ts:412-435`). A replay
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

A snapshot is one **actionable postflop state** (`room.next === "act"` with
`board_size >= 4`) recorded in a session journal. "At least two tables" is
identified by distinct `room.id` (falling back to `room.name`).

### What a turn decision becomes, and how it is labelled

Two facts constrain this section, and an earlier draft got both wrong.

**A resolver miss does not produce an error on the production path.** The
adapter defaults to `SolverMode.AUTOMATIC` (`platforms/river-club/src/mapper.ts:996`),
and in AUTOMATIC the envelope falls through a miss to
`mapOperationalFallbackResponse` (`engine/src/protocol/envelope.cpp:356-377`) —
a *strategy* response labelled operational fallback. Only
`SOLVER_MODE_RESOLVING` maps a miss to an error code (`:343-352`). So a named
error code cannot be the mechanism. Other error codes remain reachable for other
reasons — validation, replay, protocol — but none is produced by a miss.

**There is no board shape that is unservable.** With no resident match a turn
root reaches `resolve_uniform`, which accepts four- and five-card boards and
2..10 seats (`engine/src/resolver/subgame_resolver.cpp:394-410`); whether it
*returns* a solution depends on the budget and the tree cap, not on the board.
So a refusal predicate keyed on the board would have nothing to test, and any
predicate keyed on seat count would re-introduce the per-seat-count switch this
RFC removed. Unservability is a runtime outcome, and a runtime outcome is what
the label records.

The mechanism is therefore **labelling, not refusal**, and it needs no new
state to decide when to apply it: by the time the operational fallback is
reached, the blueprint has already missed and the resolver has already failed,
so the only remaining question is the board size, which the request carries and
the validator already accepts at four or five cards (`HandState.board`,
`repeated Card` with `max_items = 5`, `proto/bigshark/engine/v2/engine.proto:511`;
"board must have at most 5 cards", `engine/src/protocol/semantic_validator.cpp:723-724`).

- The decision still returns a legal action — live play must never stall, and
  the runner has no handler for a thrown decision
  (`apps/river-club-agent/main.ts:504-694`: the loop body catches only CLI and
  JSON parse failures, so a throw from `decideWithinBudget` escapes the loop and
  skips the `leave` at `:698`), which would surrender the hand to the auto-fold
  timer.
- `operationalFallbackDecision` (`engine/src/protocol/envelope.cpp:171-189`)
  sets `decision.reason` to `turn_root_unserved` when the request board has four
  or five cards, and leaves it `operational-fallback` otherwise.
- `mapOperationalFallbackResponse`
  (`engine/src/protocol/response_mapper.cpp:328-370`) writes `decision.reason`
  into `reason_code` and `diagnostic_reason` instead of the hardcoded
  `"operational-fallback"` literal. The flop and preflop strings are unchanged,
  so AC5's byte-identity holds; only a four- or five-card fallback changes.
- The adapter already surfaces the label without modification: `executeFromStrategy`
  sets `reason` from `diagnosticReason || reasonCode`
  (`platforms/river-club/src/mapper.ts:1049-1053`), and the runner logs it in
  both `resultsLogEntry` and the `acted` note
  (`apps/river-club-agent/main.ts:654-660`). The street, and therefore the board
  size, is in the same entry.
- Nothing is added to the frozen fallback chain for any other root: a flop or
  preflop operational fallback keeps its current reason string.

"Visible" here means **accounted for in the results log and countable in the
replay**, not surfaced to an operator in real time and not an error code.

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

1. **The adapter change changes live decisions with no quality evidence.**
   *Mitigation:* the Stage 3 replay records action, source, and latency per seat
   count. The change is correct even where the resulting decisions are not yet
   better, and the record makes the difference visible rather than assumed.
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
4. **A four-card state reaches code that assumes a three-card board.**
   `GameState::board()` now yields four or five cards from an adapter-built
   state, and any remaining `i < 3` or `3 + street` arithmetic that this RFC
   missed reads the wrong card. *Mitigation:* AC1's fixture is run under ASan,
   so an out-of-bounds span read fails the build rather than silently reading a
   zero-initialized card — the same failure mode the "corrupted information key"
   fact documents from the pre-change tree.

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

1. **Baseline pin and site enumeration.** Add the failing-baseline test and
   enumerate every identity/truncation site with its own test. Gate: the
   baseline test fails as expected and the list is complete against a grep.
2. **Board preservation.** Adapter, `same_root` (both), `canonical_query_board`
   narrowing, reach loop, prefix-board builder, board assembly. Gate:
   preservation and identity tests pass; the flop path is unchanged.
3. **Labelling.** Add the `turn_root_unserved` reason to the operational
   fallback path and to `resultsLogEntry`. Gate: native and Node tests pass, and
   the label is counted in the replay output.
4. **Replay measurement.** Requires live play; the corpus does not exist yet.
   Run the recorded set with production configuration and record action, source,
   `turn_root_unserved` count, and latency per seat count into
   `docs/plans/rfc0032-replay.md`. Gate: the result file exists with its
   evidence (redacted subset or corpus SHA-256 plus extraction command).

Stop conditions: any flop-path behavior change halts the rollout. A seat count
that misses the latency criteria is **reported as a failing capability**, not
quietly left on the old path — there is no old path to leave it on.

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
   classes.
2. **Is a turn-rooted frontier value sufficient with no turn/river library?**
   Untested. It determines whether the follow-up needs RFC 0011 Phase 3 or can
   stop at the addressability fixed here.

## Acceptance Criteria

- **AC1**: The adapter change and both `same_root` changes land in one commit;
  `same_root` does not compare `board_size` on the postflop branch today, so an
  adapter-only landing could match a four-card root against a flop record.
  Every truncation site is enumerated and has its own test: the adapter
  (`resident_mapper.cpp:504-532`), both `same_root` functions, the reach
  blocking loop, `build_v2_game`, `canonical_query_board`, the prefix-board
  builder (`resident_policy.cpp:861-872`), and board assembly
  (`:1087-1093,1191-1198`). A `board_size >= 4` fixture reaches each, under
  ASan. The reach loop, prefix builder, and board assembly are driven by a
  hand-built four- or five-card game rather than through `build_v2_game`, which
  still refuses that shape; they are latent fixes, as the Pipeline section
  states.
- **AC2**: A baseline test pins today's truncating behavior and fails before the
  change.
- **AC3**: A four- or five-card root that no resident record serves is labelled
  `turn_root_unserved` and counted; no path silently decides a three-card game.
- **AC4**: The 200-snapshot corpus (>= 20 per seat count, 2..6, >= 2 tables by
  `room.id`) is replayed with production resident roots and library.
  "Before" is a named pre-change revision recorded in
  `docs/plans/rfc0032-replay.md` with its exact command and a preserved binary,
  since Stage 2 lands the change before Stage 3 runs; "after" is the same
  command on the change. The file records per seat count the action, decision
  source (from `guaranteeLevel` and reason), latency p95/p99, and the
  `turn_root_unserved` count, against the 1,600 ms / 1,950 ms criteria. Because
  the corpus requires live play under separate authorization, this AC is not
  dischargeable from repository evidence alone and the RFC cannot reach
  `Implemented` before that run.
- **AC5**: The flop path is behavior-identical — no artifact digest changes, no
  resolver candidate changes for flop spots — and the full Engine Build
  Contract passes with ASan clean and `check-rfcs` / `check-docs` passing.

## Decision

Pending independent approval-agent review.
