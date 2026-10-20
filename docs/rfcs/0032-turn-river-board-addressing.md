---
rfc: "0032"
subject: "Preserve the Full Board Through the Decision Pipeline"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-20"
owners: "engine resident, River adapter, engine protocol"
supersedes: ""
superseded-by: ""
---

# RFC 0032: Preserve the Full Board Through the Decision Pipeline

## Summary

Every postflop decision in the live pipeline is re-rooted to a three-card flop
board, discarding the turn and river cards. The resident path refuses a
four- or five-card root outright, the board-identity helpers compare only the
first three cards, and the River adapter builds a `board_size = 3` game from a
snapshot that may carry five. This RFC makes the pipeline carry the board it
was given, and pins the current wrong behavior with a test before changing it.
It does not add a turn/river strategy, tree kind, evaluator, or format change.

## Motivation

A turn spot is decided as if it were a flop spot. That is a correctness defect
independent of which turn/river strategy eventually ships, and it is the
prerequisite for every later turn/river proposal.

The consequence is concrete. The adapter collapses a turn snapshot to a
flop-street-start `GameState` with decision-time chips
(`engine/src/protocol/resident_mapper.cpp:504-532`), so the broader pipeline
never sees `board[3]`. The resident path then refuses a non-flop root
(`engine/src/resident/resident_policy.cpp:110-113`), the board-identity
helpers compare only `board[0..2]`
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
- **G3**: Where the pipeline cannot yet serve a four- or five-card root, the
  refusal is a named, counted outcome carrying the board size — not a silent
  decision of a different game and not a `safeFallback` labelled as an ordinary
  operational fallback.

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
- **The resident path refuses a non-flop root.**
  `engine/src/resident/resident_policy.cpp:110-113` throws for
  `game.board_size != 3 && != 0`, naming RFC 0009 D4 as the reason it is not
  yet generalized.
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
  cannot be served, refuse by name.
- **Fix only what is broken.** No speculative interfaces, no evaluator that
  already exists, no format change with no writer.

## Proposed Design

### Pipeline

The adapter, `same_root`, `canonical_query_board`, `build_v2_game`, and the
reach-model blocking loop carry `board_size` through unchanged:

- `resident_mapper.cpp:504-532` builds the postflop `GameDef` from the actual
  board length and cards instead of clamping to three.
- `same_root` in both places compares **`board_size` explicitly as well as
  every card**. Without the size equality a three-card record still matches a
  four-card query on the first three cards — the exact failure AC1 warns about —
  so the requirement is part of the design, not a residual risk.
- `canonical_query_board` generalizes to a board span; the flop branch is
  unchanged.
- `build_v2_game` is **unchanged**: it already throws for a four- or five-card
  root (`engine/src/resident/resident_policy.cpp:110-113`), naming the board
  size. Because this RFC adds no turn/river library, that condition is never
  satisfiable, so the refusal stays where it is and gains a pinning test. The
  query-time named refusal is raised one layer up, in the host's resolver
  dispatch (`SubgameResolveStatus::Ineligible` with a message naming the board
  size), so a turn decision that reaches it fails visibly instead of deciding a
  different game.
- The reach-model loop blocks `board_size` cards, not three.
- The prefix-board builder and board assembly gain the same board-size
  parameter and a test each.

### Behavior change and its gate

The adapter change is measured, not assumed, and it is **unconditional**: one
code path exists, the adapter always preserves the board the room reported, and
a seat count that cannot yet be served gets the named refusal described below.
There is no per-seat-count switch between an old and a new behaviour — that
would be exactly the dual implementation the repository forbids, and it would
also violate G3 by silently deciding a different game at the seat counts it kept
on the old path.

The replay therefore measures **what the change does**, not whether to enable
it. It answers two questions: how often a turn decision reaches the named
refusal rather than a served root, and whether the seat counts that do resolve
meet the budget.

**The corpus does not exist yet.** `sessions/` is empty and gitignored
(`.gitignore:2`), so the >= 200-snapshot corpus requires **live play under
separate authorization**. This RFC cannot reach `Implemented` before that, and
says so rather than implying the data is available.

**Configuration.** The replay reuses `collectReplaySnapshots`
(`tools/replay/main.ts`), which already dedupes by
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

**Criteria**, per seat count over the stratified corpus:

| Criterion | Threshold |
| --- | --- |
| p95 decision latency | <= 2,500 ms |
| p99 decision latency | <= 4,000 ms |
| snapshots exceeding 11 s | 0 |
| named refusals | counted, with the reason string matched |

The 2,500 ms figure is the **lower bound of `CLAUDE.md`'s 2.5-4 s band**
(`CLAUDE.md:110-111`), which is the "decide directly from range, board, and pot
odds" guidance. The 4,000 ms p99 allowance lets one per cent of decisions use
the full band; a run whose p95 sits at the bound but whose p99 routinely exceeds
it is reported as failing, not as passing.

A snapshot is one **actionable postflop state** (`room.next === "act"` with
`board_size >= 4`) recorded in a session journal. "At least two tables" is
identified by distinct `room.id` (falling back to `room.name`).

### The named refusal

This is a contract, not a log line. Today a turn decision that misses the
resident root reaches `resolve_uniform`, and any engine error that is not a
`GUARANTEE_BELOW_REQUEST` refusal is swallowed into `safeFallback`
(`platforms/river-club/src/engine.ts:437-453`) — a check/call/fold labelled
`operational_fallback`. That is a strategic action, not a visible failure, so
"fails visibly" is not true until this RFC makes it true.

The contract:

1. **The host** returns the **existing** `ERROR_CODE_UNSUPPORTED_GAME`
   (`proto/bigshark/engine/v2/engine.proto:127`) whose message carries the
   board size, from the same dispatch site that currently maps
   `SubgameResolveStatus::Ineligible` to `ResolveOutcome::Unsupported`
   (`apps/engine-host/main.cpp:361-363`). No new error code and no protocol
   change: the enum already means "this game shape is not served".
2. **The wire** carries it: `EngineError` already has a `message` field
   (`proto/bigshark/engine/v2/engine.proto:655-665`), but `mapMissToError` returns a code and
   the response uses the canned `messageFor(code)`
   (`envelope.cpp:191-208,334-336`). The host sends the board size in the
   message.
3. **The adapter** stops discarding `EngineError.message`
   (`platforms/river-club/src/mapper.ts:1081-1085`) and surfaces it.
4. **`decide()`** treats `ERROR_CODE_UNSUPPORTED_GAME` like
   `GUARANTEE_BELOW_REQUEST`: it propagates rather than becoming
   `safeFallback`. The caller still receives a **legal action** — the tooling
   path must act, it cannot stall — but that action is labelled
   `unsupported_board_size` and counted, and the reason string names the board
   size. "Visible" here means *accounted for*, not *surfaced to an operator in
   real time*.

Tests in both the native suite (host dispatch) and the Node suite (`decide()`
propagation and labelling) cover this.

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
- The adapter change is behavior-visible for turn and river spots and is gated
  per seat count as above.

## Security and Operational Impact

- No credential or trust-boundary change; the adapter consumes only public
  state.
- Observability: this RFC **adds** debug-build logging of the board size on
  every adapter conversion (`resident_mapper.cpp` has none today), so a
  truncation is visible rather than silent, and a test asserts the line is
  emitted. A refusal names the board size.

## Alternatives Considered

1. **Fix only the adapter.** Rejected as the whole change, because the resident
   path would then refuse or silently fall through; but its replay measurement
   is retained as the gate for the larger change.
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
   count, and the change is gated on that record. The change is correct even
   where the resulting decisions are not yet better.
2. **Turn and river spots lose their current (wrong but fast) path.**
   *Mitigation:* named refusal instead of silent fall-through; the gate keeps
   the old path for seat counts where the new one misses the deadline.
3. **More truncation sites exist than are enumerated.**
   *Mitigation:* AC1 lists each known site with its own test, and a
   `board_size >= 4` fixture is run through every entry point named in the
   resident and protocol sources.

## Verification Plan

- **Baseline pin.** A test reproduces today's truncating behavior — a turn
  snapshot yields a `board_size = 3` game with `board[0..2]` — so the change is
  demonstrably a change.
- **Board preservation.** A four- and a five-card snapshot produce a `GameDef`
  carrying every card, through the adapter and into the resident entry points.
- **Identity.** `same_root` distinguishes states differing only in `board[3]` or
  `board[4]`; `canonical_query_board` does not drop them.
- **Reach.** The blocking loop excludes every board card, verified by a fixture
  where `board[3]` is otherwise dealable.
- **Refusal.** A four-card root that cannot be served produces
  `ERROR_CODE_UNSUPPORTED_GAME` carrying the board size, propagates past
  `safeFallback` swallowing in `decide()`, and is labelled and counted; a
  native test covers the host dispatch and a Node test covers the propagation and
  the label.
- **Replay measurement.** The 200-snapshot corpus replayed through both
  pipelines with production roots and matching `timeoutMs`; action, source,
  p95, p99, and refusal counts per seat count recorded in
  `docs/plans/rfc0032-replay.md`, with either a redacted subset or the corpus
  SHA-256 and extraction command, since the raw journals are gitignored.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

1. **Baseline pin and site enumeration.** Add the failing-baseline test and
   enumerate every identity/truncation site with its own test. Gate: the
   baseline test fails as expected and the list is complete against a grep.
2. **Board preservation.** Adapter, `same_root` (both), `canonical_query_board`,
   reach loop, prefix-board builder, board assembly. Gate: preservation and
   identity tests pass; the flop path is unchanged.
3. **Replay measurement.** Requires live play; the corpus does not exist yet.
   Run the recorded set with production configuration and record action, source,
   refusal count, and latency per seat count into `docs/plans/rfc0032-replay.md`.
   Gate: the result file exists with its evidence (redacted subset or corpus
   SHA-256 plus extraction command).
4. **Named refusal.** Implement the `ERROR_CODE_UNSUPPORTED_GAME` contract end to end
   and route it past `safeFallback`. Gate: native and Node tests pass and the
   refusal is counted in the replay output.

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
  (`:1087-1093,1191-1198`). A `board_size >= 4` fixture reaches each.
- **AC2**: A baseline test pins today's truncating behavior and fails before the
  change.
- **AC3**: A four- or five-card root that cannot be served raises a named error;
  no path silently decides a three-card game.
- **AC4**: The 200-snapshot corpus (>= 20 per seat count, 2..6, >= 2 tables by
  `room.id`) is replayed before and after with production resident roots and
  library, and `docs/plans/rfc0032-replay.md` records per seat count the action,
  decision source (from `guaranteeLevel` and reason), policy latency p95/p99,
  and the named-refusal count with its reason string, against the 2,500 ms /
  4,000 ms criteria. Because the corpus requires live play under separate
  authorization, this AC is not dischargeable from repository evidence alone and
  the RFC cannot reach `Implemented` before that run.
- **AC5**: The flop path is behavior-identical — no artifact digest changes, no
  resolver candidate changes for flop spots — and the full Engine Build
  Contract passes with ASan clean and `check-rfcs` / `check-docs` passing.

## Decision

Pending independent approval-agent review.
