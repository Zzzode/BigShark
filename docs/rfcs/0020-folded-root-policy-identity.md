---
rfc: "0020"
subject: "Folded-Root Policy Identity"
status: "Implemented"
authors: "Codex /root"
created: "2026-10-14"
updated: "2026-10-14"
owners: "engine resident, engine resolver, engine tests"
supersedes: ""
superseded-by: ""
---

# RFC 0020: Folded-Root Policy Identity

## Summary

Include every dealt seat's root folded flag in resident and terminal-resolver
blueprint-root matching. Fix this correctness prerequisite before opening
flop roots reconstructed from six-seat preflop folds. Preserve all existing
correct matches, artifact bytes and protocol versions. The complete six-max
GTO feature remains In Progress.

## Motivation

A folded contributor funds the pot but cannot act or win. Identical board,
chips, button and pot therefore do not identify the same game if root folded
flags differ. The current two lookup predicates omit those flags. Six-seat
cross-street reconstruction must not select a policy for a different live set.
Refusing a mismatched artifact is necessary correctness, not GTO coverage.

## Goals

- Reject resident public-belief, hero-decision and resolver-source lookup
  against a root with any different per-seat folded flag.
- Reject a directly supplied resolver blueprint with mismatched root folded
  flags before querying rows, enumerating deals or building candidate values.
- Preserve exact existing matches and folds occurring after the root through
  ordinary root-relative history; do not compare current-node folds to root
  folds.
- Add native regressions, including six dealt seats, that identify this
  specific early rejection rather than an incidental missing policy row.

## Non-Goals

- Complete six-max GTO, new strategy promotion, live play or large training.
- Changes to GameDef layout, game transitions, policy keys, artifact formats,
  Protobuf fields, solver guarantees or range propagation.
- Full canonicalization of the older six-field matching predicates. Their
  deliberate terminal-depth and flop-card normalization remains unchanged.
- Publishing folded-root artifacts, full cross-street reconstruction, refunds,
  side-pot wire support or provider observation accumulation. Those remain
  mandatory delivery tasks, not exclusions from the whole user feature.

## Current State and Evidence

- `engine/src/resident/resident_policy.cpp::same_root` compares seat count,
  pot, big blind, button, flop/preflop shape, stacks and contributions, but
  not `GameDef::folded`. It gates pinned/unpinned resident selection.
- `engine/src/resolver/counterfactual_reach.cpp::same_root` has the same
  omission before `build_model` replays the blueprint path. A caller can
  provide a BlueprintSource directly, bypassing resident selection.
- `GameState` seeds root folds from `GameDef::folded`. This affects actor
  rotation, the live-player set and terminal awards. The central
  `poker::same_game_def` already includes folded flags.
- Existing persisted SQLite/mmap roots do not carry root folded flags;
  decoded roots default to false. This change does not add that capability.
- RFC 0011's subgame definitions retain earlier folds. A future correctly
  reconstructed flop root must preserve those flags and all dealt indices,
  not erase them or compact six dealt players into two live players.
- Full-history reconstruction still needs separate public-data and refund
  gates: before awards, hand-start chips are `current + gross - refunds`.
  Complete trustworthy history and known starting stacks can derive refunds;
  v1 lacks the coherent completeness/ledger/eligibility contract required by
  RFC 0006. This identity fix does not bypass its v2 production gate.

Overlapping contracts: RFCs 0005, 0006, 0008, 0009, 0011 and 0019.

## Design Principles

- Root identity describes the game at the policy root, not the latest node.
- Keep all dealt seats, including folded contributors and card blockers.
- Correct matching takes priority over increasing apparent coverage.
- Fix the two existing owner-local predicates, without a new shared layer.

## Proposed Design

In each predicate's existing per-seat comparison, require
`a.folded[p] == b.folded[p]` alongside stack and contribution equality.
Keep the current canonical-flop handling and intentional terminal normalization.
Do not replace the predicates wholesale with `same_game_def`: that would
change unrelated root-affinity semantics beyond this narrowly proven defect.

The resident returns its existing root-mismatch miss; the terminal resolver
returns its existing CoverageMiss and root-mismatch diagnostic. No new error,
configuration or diagnostic vocabulary is needed. Compare the definitions
(`state.def()` and `blueprint.game().def`), never live `players().folded`.
Folds explained by history therefore continue matching their original root.
When both root bitmaps are equal, this check alone must not reject the query.
Other coverage, validation and resource checks still apply.

### Shared-change gate

Owners: resident selection and resolver reach-model admission. Local closure
in the mapper is insufficient: it cannot govern direct native callers or a
direct BlueprintSource; a resident-only fix leaves the resolver bypass open.
Conversely, rejecting every folded node in the mapper would hide the defect
and reject valid folds after an unchanged root. Two one-condition changes
are the smallest viable shared correction. Existing callers, row keys, modes,
numeric units and persisted formats are unchanged. The only removed behavior
is matching a demonstrably different game. Required verification includes
both direct native boundaries, equal-root positive controls and existing
post-root-fold regressions. No data migration is needed.

## Dependency Rules

Keep resident and resolver dependent on existing poker domain records.
No dependency between the two modules is added. No storage or generated
wire type enters poker. Native tests use existing fixture/source patterns.

## Compatibility and Migration

All existing properly matched root flags remain equal, normally all false.
Root-relative action histories and their information keys remain byte-for-byte
unchanged. Existing artifacts remain immutable and readable. A mismatched
root now produces the established coverage miss, not a silently substituted
policy. This is a correctness restriction, not a new wire capability.
Truly folded-root publication still requires separate format approval.

## Security and Operational Impact

The check uses public status already in the domain and at most ten booleans.
It introduces no network, credentials, private-card reads or live mutations.
Existing coverage-miss reporting remains authoritative. No latency speedup or
new quality guarantee is claimed.

## Alternatives Considered

- Change only resident: direct resolver sources still bypass identity safety.
- Change only protocol: native callers bypass it and after-root folds differ
  from folded-at-root games.
- Erase folds or compact seats: changes eligibility, blockers and positions.
- Replace every identity predicate: broader compatibility and normalization
  changes are unnecessary to close this specific defect.
- Introduce a generic identity service: two bounded local comparisons suffice.
- Rewrite artifacts now: unnecessary for rejecting a mismatch safely.

## Risks

Comparing current folds instead of root folds would reject legitimate history;
pin this distinction with a positive control. A test that merely lacks rows
could pass before the fix; assert the typed early root mismatch and no source
row query where possible. Mechanical six-seat fixtures prove admission only,
not policy quality or normal-stack online performance.

## Verification Plan

Use existing `test_resident_policy.cpp` and `test_resolver.cpp` native tests.
Exercise pinned/unpinned resident entry points and a direct resolver source;
hold board, button, stacks, contributions, count and pot constant while
changing a non-acting seat's root folded flag. Include six dealt seats.
Retain matching-root and genuine post-root-fold success regressions. Avoid
new full six-seat training simply to test root admission; existing tiny rows
and native seam sources establish the early boundary directly.

Run root release configure, format, build, full CTest, multistreet benchmark,
Node/protocol checks, replay and docs/RFC checks. Build through root ASan and
run affected native tests without smaller fixtures; disclose unrelated full
sanitizer suites still pending and do not reuse an older binary as new evidence.

## Rollout Plan

1. Obtain independent approval of this exact correction.
2. Add regressions which fail for the missing-flag comparison.
3. Fix both owner-local predicates and update current matching documentation.
4. Verify focused and broader gates, review independently, then continue the
   cross-street ledger task. Complete six-max remains In Progress.

## Rollback Plan

No persisted data changes. Retain previous immutable strategy generations.
If a deployment needs the prior binary, keep new folded-root serving disabled;
reverting the check alone reintroduces the known mismatch and is not a safe
way to increase strategy coverage. No deployment is performed by this RFC.

## Open Questions

None for this bounded identity correction. Complete public-state data,
refund/side-pot support, qualified ranges and six-max quality remain open
in the delivery register and are not resolved here.

## Acceptance Criteria

- Different root folded flags produce an early typed resident/resolver miss,
  including a six-seat native regression.
- Equal root flags retain existing behavior; an action-history fold does not
  become a root mismatch.
- No new format, public field, strategy, allocation policy or dependency.
- Native/project verification and independent implementation review recorded.
- Whole-feature status stays In Progress and no mock quality claim is made.

## Decision

Author agent: Codex /root
Approved by: /root/sixmax_runtime_audit
Decision date: 2026-10-14
Review outcome: Approved
Reviewed scope: All 18 sections, RFCs 0005/0006/0008/0009/0011/0019,
resident root selection, direct resolver admission and native fixture/source
patterns. Pre-decision proposal SHA-256:
d594814f42ab4843e2f28ecb83dc73ca94f69dc47b8164db2193f0d48a4b9531.
Review summary: No blocking findings. Two owner-local root folded comparisons
are the smallest correction and protect direct BlueprintSource callers.
Preserve terminal/canonical-flop normalization and compare root definitions,
not current folds. Six-seat 100 BB hand-written native rows can establish
mechanical positive/negative controls without training. Require typed early
misses and real after-root-fold hits; same nonzero native root flags reaching
Ineligible prove identity only. Persisted folded roots, full public ledger,
v2/refunds/side pots and complete six-max quality remain separate gates.
Current matching documentation must not imply a fully canonical predicate.
Design approval only; implementation verification and review remain required.

## Implementation Plan

1. Native test owner adds six-seat 100 BB mechanical admission regressions
   in `test_resident_policy.cpp` and `test_resolver.cpp`, using the existing
   assembler/source patterns. Run the added failure cases against the old
   predicates first. A wrong-root hit or a later, non-root miss demonstrates
   the defect; do not mistake an unavailable row for a passing regression.
2. Resident/resolver owner adds one root folded comparison in each existing
   predicate, with no other identity fields or behavior change.
3. Run the same tests again: typed early rejection must replace the old result,
   while existing root/history-fold positive cases retain their behavior.
4. Update current normalized-root matching documentation and run project
   gates. Record actual sanitizer evidence separately from older pending runs.
5. Independent source/evidence review before marking Implemented. Complete
   six-max remains In Progress throughout.

## Implementation Evidence

Both new native regression entry points failed against the old predicates:
resident failed the typed root-miss assertion after real positive row hits;
resolver failed the expected early CoverageMiss assertion. The production
change adds only per-seat root folded equality in the two existing predicates.
Source checkpoint by `/root/sixmax_runtime_audit`: Approved, no material
blocker; this is not final implementation acceptance.

Stable RFC 0020 release configure/format/build and full CTest passed 89/89
in 354.83 seconds. The complete native resolver also passed directly.
Multistreet's three gates passed with repeat delta zero; npm check passed
132 Node tests and 25 protocol tests; direct proto check passed. Historical
replay found zero usable snapshots, while six fixed synthetic cases had zero
illegal actions or JavaScript fallbacks. Neither is six-max quality evidence.
Documentation/RFC and diff checks passed.

The earlier full ASan run is a prior build and is not RFC 0020 evidence; it
was stopped after recorded failures, without counting unfinished tests.
New-source sanitizer verification and final review are still outstanding;
keep this RFC Implementing. The earlier suite also hit the original template
watchdog and a stage6 estimator fixture ResourceLimit, which must not be
silently treated as successful qualification.

Subsequent stable-source release verification, including RFC 0021 and the
separate native hand-ledger baseline, passed 91/91 in 346.80 seconds.
The complete current native resolver passed true Debug ASan/UBSan in
86.53 seconds. Full current-suite sanitizer evidence, including the resident
target, remains pending; no final implementation acceptance is recorded.

On 2026-10-14 a four-agent independent audit (RFC 0019, RFC 0020, RFC 0021,
plus build wiring and docs) found this RFC conformant with no blocker or
major findings, confirming both root-matching predicates reject mismatched
folded flags before any row query, deal enumeration or candidate-value
construction. Current-source gates: release format/build passed; full
`ctest --preset release` passed 91/91 in 363.84 seconds, including the
`resident` and `resolver` targets; `npm run check` passed 156/156 Node
tests; direct `npm run proto:check` passed 25/25; `node bin/replay.mjs`
exited 0; `benchmark-multistreet` passed all three quality gates. The true
Debug ASan/UBSan build compiled and linked the current source. The full
ASan CTest suite was stopped at the user's request after roughly 85 minutes
with `heads_up_solver_sampled` and `resident_policy` still running; no
sanitizer error was reported by any completed test. The full-suite
sanitizer gate, including the resident target under ASan, remains open and
this RFC stays Implementing.
