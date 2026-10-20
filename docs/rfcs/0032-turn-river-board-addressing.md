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
- **G3**: Where the pipeline cannot yet serve a four- or five-card root, it
  refuses by name instead of silently deciding a different game.

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
  reads `state.board()[3]` (`engine/src/resident/resident_policy.cpp:861-872`);
  the backing `board_` is a zero-initialized `array<int,5>`, so it substitutes
  **card 0** as the turn card. That is a corrupted key, not merely a re-rooted
  game.
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
- `same_root` in both places compares every present card.
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

The adapter change is measured, not assumed. Before it lands, a recorded set of
turn and river snapshots is replayed through the current pipeline and the new
one, comparing chosen action, decision source, and p95 latency per seat count. A
change of source from resident lookup to `resolve_uniform` is expected and is the
evidence the change is real.

**The replay corpus.** The snapshots come from the local `sessions/` journals,
which `bin/bigshark.mjs` already appends to. The RFC requires at least 200
postflop snapshots with `board_size >= 4`, drawn from sessions across at least
two tables and stratified by seat count (2..6), so every seat count has at least
20. The corpus is recorded as raw JSONL in `sessions/rfc0032-replay/`, and the
summary lives in `docs/plans/rfc0032-replay.md`.

**The deadline is a stated tier, not "the deadline".** `room.timeLeftMs` varies
by table (roughly 11 s to 25 s), so a threshold without a tier is unfalsifiable.
The gate uses the **12 s tier**, the boundary in `CLAUDE.md`'s decision-time
budget between hybrid and engine-led play, and the budget itself is 2.5 s for
"decide directly from range, board, and pot odds". Pass criteria per seat count,
over the stratified corpus:

| Criterion | Threshold |
| --- | --- |
| p95 decision latency | <= 2,500 ms |
| p99 decision latency | <= 4,000 ms |
| snapshots exceeding 11 s | 0 |
| named refusals | counted and reported, not hidden |

A seat count that fails any row keeps the current flop-re-rooted path. The
result file records every row per seat count.

**The gate is a capability boundary, not a switch.** It is not a flag selecting
an old and a new implementation: one code path exists, and the pipeline serves a
four- or five-card root only at the seat counts where the replay proved it can
meet the budget. The boundary is recorded in the capability/status documentation
when implemented, and it is re-measured whenever the decision clock tier or the
supported seat counts change — so it is a measured capability matrix with a
re-measurement trigger, not an indefinite toggle.

RFC 0029 does **not** cover this: its Non-Goals exclude turn/river measurement.
This RFC's own replay corpus is the evidence.

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
- **Refusal.** A four-card root that cannot be served raises a named error, not
  a silent three-card decision.
- **Replay measurement.** The 200-snapshot corpus replayed through both
  pipelines; action, source, p95, p99, and refusal counts per seat count
  recorded in `docs/plans/rfc0032-replay.md` with the raw JSONL beside it.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

1. **Baseline pin and site enumeration.** Add the failing-baseline test and
   enumerate every identity/truncation site with its own test. Gate: the
   baseline test fails as expected and the list is complete against a grep.
2. **Board preservation.** Adapter, `same_root` (both), `canonical_query_board`,
   reach loop, prefix-board builder, board assembly. Gate: preservation and
   identity tests pass; the flop path is unchanged.
3. **Replay measurement and gate.** Run the recorded set, record action/source/
   latency, and enable per seat count where the deadline is met. Gate: G3 and
   the recorded result file.
4. **Refusal.** Replace the silent fall-through with a named refusal where a
   root cannot be served. Gate: the refusal test passes.

Stop conditions: any flop-path behavior change halts the rollout; any seat
count whose replay exceeds the deadline keeps the old path.

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
- **AC4**: The 200-snapshot corpus (>= 20 per seat count, 2..6) is replayed
  before and after, and `docs/plans/rfc0032-replay.md` records per seat count
  the action, decision source, p95, p99, and refusal count against the 12 s
  tier's 2,500 ms / 4,000 ms thresholds.
- **AC5**: The flop path is behavior-identical — no artifact digest changes, no
  resolver candidate changes for flop spots — and the full Engine Build
  Contract passes with ASan clean and `check-rfcs` / `check-docs` passing.

## Decision

Pending independent approval-agent review.
