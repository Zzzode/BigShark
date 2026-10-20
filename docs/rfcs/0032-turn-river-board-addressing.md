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
- **The truncation also corrupts an information key.** For a turn check-through
  line the mapper's flop definition carries the same pot, stacks, and
  contributions as the flop root, so `same_root` matches and
  `replay_public_path` processes turn events against a three-card span. The
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

- `resident_mapper.cpp:504-532` builds the postflop `GameDef` from the actual
  board length and cards instead of clamping to three, **and carries the
  mid-street root fields**. This second half is not optional and an earlier
  draft omitted it. The def currently copies `stacks`, `contributions`, and
  `folded` (`:525-529`) and never sets `street_committed`, `raise_rights`, or
  `last_full_raise` — a grep over the file finds those three names only as reads
  of the replay cursor (`:378-379`, `:423`). `poker::validate` then rejects the
  shape in the two-seat profile: a rooted board requires either equal
  contributions (`engine/src/poker/game_definition.cpp:135-137`) or a mid-street
  def, and a street-start def must carry `last_full_raise == 0` (`:239-241`).
  A heads-up turn spot where the seats have not contributed equally — the
  ordinary case when facing a bet — throws `unmatched contributions at
  closed-street root`. Carrying the three fields from `cursor` (which already
  has them, `:378-379`) makes it a valid mid-street root, and the constructor
  seeds `pending` and the actor from them (`:392-422`).
- Carrying `street_committed` also fixes hero attribution, which the board
  change alone does not. The returned state is built from `cursor`'s stacks and
  contributions (`:526-528`) and its actor is then the first seat clockwise of
  the button that can act (`engine/src/poker/game_definition.cpp:415-422`) — not
  necessarily the seat facing the wager. `hero_decision` keys off
  `*state.actor()` (`engine/src/resident/resident_policy.cpp:1082,1094`), so
  without the mid-street fields a turn decision can be answered for the wrong
  seat. This is a pre-existing defect that the flop clamp hides: at a flop
  street-start root the seat clockwise of the button *is* the actor by the
  rules. Preserving the board without preserving `street_committed` would
  surface it on every turn spot.
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
- Two sites the earlier draft's list missed: the second three-card loop in
  `ReachModel::initialize` (`engine/src/resident/public_reach.cpp:164`) and the
  lazy flop-library record path that assigns `board_size = 3` at
  `resident_policy.cpp:574-576`. The record enumeration was also short: the
  eager legacy paths use `solver::to_unified_game` at `:628` and `:679`, which
  hardcodes three cards (`engine/src/solver/seat_policy.cpp:75`), in addition to
  `:355`, `:544`, `:549`, `:621`, and `:672`.

Two honest consequences, both reflected in AC1 rather than papered over:

1. These are **latent** fixes: correctness-by-construction for the day a
   four- or five-card record exists. They are included because leaving them
   three-card would make the first such record silently wrong.
2. They are verified by **`[,)`-input unit tests of the same logic**, which
   requires either exposing a test seam or accepting an integration test that
   places a synthetic four-card record into a `ResidentPolicySet`. This RFC
   chooses the latter and states it: the test builds a set directly, bypassing
   `build_v2_game`, and drives `public_belief`/`hero_decision`. If that proves
   impossible without widening an interface, the site gets a named seam in the
   same commit and this paragraph is updated — it is not silently skipped.

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

A snapshot is one **actionable postflop state** (`room.next === "act"` with
`board_size >= 4`) recorded in a session journal. "At least two tables" is
identified by distinct `room.id` (falling back to `room.name`).

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
- Losing the digest is not a silent quality regression: the response carries
  `guarantee_level` and the diagnostic token, and both reach the results log.

This changes the wire for subgame-served rows from an error to a strategy. It
does not touch any flop or preflop response: those rows carry a real digest
today and keep it.

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
- **The wire changes for subgame-served rows only.** A row that today maps to
  `ERROR_CODE_INTERNAL` maps to an `expanded_strategy` with `artifact_sha256`
  unset. No flop or preflop response moves, and `proto/baseline/v2.binpb` (which
  contains only the guarantee token) is unaffected. Clients that require the
  digest field must treat it as optional — the adapter already does
  (`platforms/river-club/src/mapper.ts:1063`).

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
   the decision stops being made for the wrong game (a flop board and, without
   the mid-street fields, possibly the wrong seat), and the outcome becomes
   attributable. Whether a uniform-prior subgame solve beats the current
   wrong-but-fast flop decision at five and six seats is **unmeasured** and this
   RFC supplies no evidence for it. *Mitigation:* the Stage 3 replay records
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
4. **A four-card state reaches code that assumes a three-card board.**
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

1. **Digest repair, standalone.** Make `artifact_sha256` optional end to end in
   the mapper, keeping RFC 0025's empty-digest semantics
   (`docs/rfcs/0025-off-tree-decision-handling.md:345`). This lands **before**
   the adapter change and is independently valuable: it fixes a turn spot the
   subgame resolver already solves today. Gate: a subgame-solved response maps
   to a strategy instead of `ERROR_CODE_INTERNAL`; every flop and preflop
   response keeps its digest byte-identically.
2. **Baseline pin and site enumeration.** Add the failing-baseline test and
   enumerate every identity/truncation site with its own test. Gate: the
   baseline test fails as expected and the list is complete against a grep.
3. **Board and mid-street preservation.** Adapter (board **and**
   `street_committed`/`raise_rights`/`last_full_raise`), `same_root` (both),
   `canonical_query_board` narrowing, reach loops (both), record paths,
   prefix-board builder, board assembly. Gate: preservation, identity, and
   hero-attribution tests pass; the flop path is unchanged.
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

Stop conditions: any flop-path behavior change halts the rollout. A seat count
that misses the latency criteria is **reported as a failing capability**, not
quietly left on the old path — there is no old path to leave it on.

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

- **AC1**: The adapter change (board **and** the three mid-street fields) and
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
  | Record paths `:355,544,549,574-576,621,628,672,679` | direct (assert `board_size`) |
  | Prefix builder (`:861-872`) | integration, synthetic record |
  | Board assembly (`:1087-1093`, `:1191-1198`) | integration, synthetic record |

  The last two rows are file-local in an anonymous namespace and unreachable
  through `resolve_root` after the change; they are driven by placing a
  synthetic four-card record into a `ResidentPolicySet` and calling
  `public_belief`/`hero_decision`. If that requires a new seam, the seam lands
  in the same commit and this table is updated — a test that copies the logic
  does not count. Span contents are asserted explicitly rather than relying on
  a sanitizer, which cannot detect this class (see the facts above).
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

  A non-zero count in the last row is a **failure of this RFC**, not a result.

  Because the corpus requires live play under separate authorization, this AC is
  not dischargeable from repository evidence alone. That makes `Implemented`
  unbounded, so it is stated as a rollout gate below rather than only here.
- **AC5**: The flop path is behavior-identical — no artifact digest changes, no
  resolver candidate changes for flop spots — and the full Engine Build
  Contract passes with ASan clean and `check-rfcs` / `check-docs` passing.

## Decision

Pending independent approval-agent review.
