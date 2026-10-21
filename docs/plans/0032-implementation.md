# RFC 0032 Implementation Plan

Status: Current

Execution state: RFC 0032 is Accepted (independent approval record in the RFC's
Decision section, 2026-10-21). This plan maps its five stages to tasks, evidence,
and gates. Stages 0 and 1 are independently valuable and change no persisted
format. Stages 2-4 cannot reach `Implemented` from repository evidence: they
require a live-play corpus that does not exist (`sessions/` is empty and
gitignored), so the RFC stops at `Implementing` until that run happens under
separate authorization.

## Why stage 0 comes first and alone

Stage 0 repairs a defect that exists **today**, before any board work: a subgame
row carries `artifact_sha256 = {}` (RFC 0025 specifies exactly that), the
response mapper rejects anything that is not 64 characters, and the adapter
turns the resulting `ERROR_CODE_INTERNAL` into `safe-fallback`. A turn spot the
subgame resolver *successfully solves* is therefore played as a mechanical
check/call/fold.

Landing the board change first would route more traffic into that defect. Stage 0
is therefore a prerequisite for stage 2, not a cleanup.

Stage 0 also changes a live **flop** path — the no-resident-match branch is
reached at `board_size == 3` today — so it is measured on its own rather than
folded into the board work.

## Stage 0 — Digest contract repair

Status: not started.

### Tasks

1. **Emit absence, not emptiness.**
   `engine/src/protocol/response_mapper.cpp:248` calls
   `set_artifact_sha256(std::string(row.artifact_sha256))` unconditionally. For an
   empty digest that is *presence with a zero-length value*, which the field's
   own pattern (`^[a-f0-9]{64}$`,
   `proto/bigshark/engine/v2/engine.proto:609`) forbids. Change it to
   `clear_artifact_sha256()` when `row.artifact_sha256` is empty and set it
   otherwise.

2. **Scope the acceptance to rows with no artifact.**
   `mapExpandedResponseImpl` serves resident rows as well as subgame rows, and
   `mapResolvedResponse` labels an Approximate outcome
   `SOLVER_SOURCE_BLUEPRINT` (`:317-326`), so the source cannot distinguish
   them. The 64-character check at `:191-193` must therefore stay for every row
   that *has* a digest. The relaxation is: absent is legal, present-and-malformed
   is not.

3. **Correct the metadata for subgame rows.** These rows currently report
   `reason_code = "blueprint"` and `cache_hit = true` (`:246-247`) while RFC 0025
   specifies `"resolving"`, `SOLVER_SOURCE_RESOLVING`, and `cache_hit = false`
   (`docs/rfcs/0025-off-tree-decision-handling.md:340-344`). Fix the source token,
   the reason code, and the cache flag together, because stage 4's accounting
   reads them.

### Gates

- A subgame-solved response (empty digest, `subgame-resolve:approximate` maps to
  an `expanded_strategy` with `has_artifact_sha256() == false` and
  `guarantee_level = approximate`) instead of `ERROR_CODE_INTERNAL`.
- A resident row with a malformed digest still fails with
  `ERROR_CODE_INTERNAL`; the existing invariant
  `engine/tests/test_conformance.cpp:532` ("artifact sha256 present for
  blueprint") still holds, because it is conditioned on
  `SOLVER_SOURCE_BLUEPRINT` and the repaired subgame rows no longer claim that
  source.
- `platforms/river-club/tests/w1-resident-wiring.test.ts:369-399` passes
  unchanged: the adapter already treats the digest as optional
  (`mapper.ts:1063`, `:1089`), so a present digest still surfaces and an absent
  one still yields `undefined`.
- A before/after count on the AC4 corpus for the no-resident-match flop path,
  recorded in `docs/plans/rfc0032-replay.md`, because this stage is a real
  behaviour change outside the RFC's subject.

### Rollback

One commit touching `response_mapper.cpp` and its tests. No persisted format, no
artifact, no client change.

## Stage 1 — Baseline pin and site enumeration

Status: not started.

### Tasks

1. Add the failing-baseline test: a turn snapshot currently yields a
   `board_size = 3` game. It must fail before stage 2.
2. While the baseline fixture is being built, record whether a turn
   check-through line actually **hits** a resident row today. This is the one
   step the accepted RFC leaves unproven, and the baseline pin is the cheapest
   place to settle it. The answer belongs in the RFC's Implemented record.
3. Enumerate every identity/truncation site against a grep, per AC1's table.

### Gates

The baseline test fails as expected; the enumeration matches a grep over
`board_size = 3`, `i < 3`, and `3 + prefix_street` across `engine/src/resident`
and `engine/src/protocol`.

## Stage 2 — Board preservation and the two-view state

Status: not started.

This is the largest stage and it must land as **one commit**. Landing the runtime
without the identity def disables the resident path at every street.

### Tasks

1. **Adapter.** `resident_mapper.cpp:504-532` stops rebuilding a postflop
   `GameDef` and returns the replay cursor's runtime with a decision-node
   identity def attached:
   - runtime = the cursor (real board of any length, true `pending`, correct
     actor, live `street_committed`, net refunds);
   - identity = `stacks`, `contributions` (**net**), `folded`, `pot`, `button`,
     `big_blind`, `preflop = false`, `terminal`, with `board`/`board_size` from
     `cursor.board()` and, for a mid-street root, `street_committed`,
     `raise_rights`, `last_full_raise` copied by the rule
     `build_subgame_def_from_node` already uses
     (`engine/src/resolver/subgame_resolver.cpp:357-369`).
2. **The seam.** `GameState::def_` is private and the plain constructor
   re-derives `pending` from `street_committed`, so a construction entry point
   taking `(runtime state, identity def)` is required. It **validates** the
   identity def and throws if it fails, exactly as the constructor would; the
   seam is not permitted to bypass the class's documented contract. Document the
   seam's single caller and lifetime in the header.
3. **The preflop branch is unchanged.** It keeps returning the hand-start
   `rootState` from `rootDef` (`:494-503`); returning the cursor there would
   publish replayed `pending`/`actor` where the blueprint expects the hand start.
4. **`same_root`, both copies.** Compare `board_size` then all `board_size`
   cards (`engine/src/resident/resident_policy.cpp:53-87`,
   `engine/src/resolver/counterfactual_reach.cpp:26-39`). Neither half is
   redundant: card id 0 is a real card, so a widened loop alone would match a
   flop record against a turn query whose turn card is card 0.
5. **Narrow `canonical_query_board`** to return `nullptr` unless
   `board_size == 3` (`resident_policy.cpp:753-761`).
6. **Reach loops (both), prefix-board builder, board assembly.** Assert span
   contents explicitly; ASan cannot detect this class, because the span is over
   a zero-initialized `std::array<int,5>` and `span::operator[]` is only asserted
   under `_GLIBCXX_ASSERTIONS`, which no preset sets.
7. **The test seam for the two file-local sites.** `Record` is forward-declared
   and `records_` is private (`engine/include/bs/resident_policy.hpp:313,335`),
   and `build()` refuses a four-card def at `build_v2_game`, so a synthetic
   record cannot be placed without widening the interface. The seam ships in this
   commit with its lifetime documented; it is not a compatibility shim.

### Gates

The 24-cell fixture matrix from AC1 — {heads-up, 6-max} × {flop, turn, river} ×
{street-start, facing a bet} × {refund, no refund} — with every cell validating,
constructing, satisfying `out.state->actor() == out.hero_actor`, agreeing on the
board, and satisfying `def().pot == pot()`; the two `same_root` properties on the
street-start column only. Plus: no resident-served **flop** response changes
(AC5), and `platforms/river-club/tests/w1-resident-wiring.test.ts:485-515` passes
unchanged.

### Rollback

A single revert restores the three-card clamp. No artifact is migrated in either
direction.

## Stage 3 — Labelling

Status: not started.

### Tasks

1. `operationalFallbackDecision` (`engine/src/protocol/envelope.cpp:171-189`)
   sets `reason = turn_root_unserved` when the request board has four or five
   cards.
2. `mapOperationalFallbackResponse` forwards `decision.reason` instead of the
   hardcoded literal, leaving flop and preflop fallbacks byte-identical.
3. The replay counter matches on **suffix**, because the adapter prefixes the
   reason with `cpp:` (`platforms/river-club/src/mapper.ts:188-192`).

### Gates

Native and Node tests pass; the label is counted in the replay output.

## Stage 4 — Replay measurement (cannot be discharged from the repository)

Status: blocked on live play under separate authorization.

### Tasks

1. Produce the two-stratum corpus: >= 20 snapshots per seat count 2..6 per
   stratum (flop `board_size == 3`, turn/river `>= 4`), >= 2 tables by `room.id`.
2. Replay with **production resident roots and flop library** — the harness
   auto-detects libraries (`platforms/river-club/src/engine.ts:242-245`) but sets
   no roots, so both must be configured — and with `timeoutMs` matching the
   criteria.
3. Record per seat count: action, the four-way source table, latency p95/p99,
   and the `turn_root_unserved` count.

### Gates

p95 <= 1,600 ms, p99 <= 1,950 ms, zero decisions at the 2,000 ms cap,
`turn_root_unserved` counted per seat count, and **zero `safe-fallback:*`**
except provider-reported shapes the local reducer cannot produce, which are
counted separately with their reason.

Evidence must survive review: the corpus SHA-256 plus its extraction command, or
a redacted subset, in `docs/plans/rfc0032-replay.md`.

## Cross-cutting

- **The 24-cell fixture set is fixture work, not test work.** Each cell needs a
  hand-built snapshot and there is no live corpus to draw from. It belongs in
  stage 2's estimate.
- **No Legacy Code.** The seam from stage 2 is deleted once a turn-rooted record
  makes it unnecessary; nothing in this plan is a flag selecting old and new
  behaviour.
- **Documentation updates land with their stage**: `docs/integrations/river-club.md`
  for the adapter behaviour, `docs/design/gto-engine.md` for the two-view state,
  and `docs/plans/rfc0032-replay.md` for evidence.

## Verification commands per stage

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target benchmark-multistreet
npm run check
npm run proto:check
node bin/replay.mjs
node bin/check-rfcs.mjs
node bin/check-docs.mjs
```

ASan (`cmake --preset asan`) is required for stage 2, which touches spans,
lifetimes, and a new construction seam.
