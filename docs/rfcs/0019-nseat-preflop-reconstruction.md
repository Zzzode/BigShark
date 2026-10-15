---
rfc: "0019"
subject: "N-Seat Preflop Reconstruction"
status: "Implementing"
authors: "Codex /root"
created: "2026-10-14"
updated: "2026-10-14"
owners: "engine protocol, engine tests"
supersedes: ""
superseded-by: ""
---

# RFC 0019: N-Seat Preflop Reconstruction

## Summary

Remove the heads-up-only preflop reconstruction barrier for negotiated v1
minor 2 requests by replaying the existing unified game for 2..10 dealt seats.
Keep the complete six-max GTO feature In Progress until its whole-hand,
coverage, quality, resource, and production acceptance gates pass. This RFC
implements one internal prerequisite, not a partially released GTO feature.

## Motivation

The user requires complete GTO operation on six-player tables, not a collection
of partial capability claims. At the implementation baseline, every six-seat
preflop blueprint request was refused before lookup. Training an artifact alone
cannot fix this barrier.
The owning protocol module already has exact preflop replay, but it constructs
two-seat arrays and a two-seat game. Generalize that implementation rather than
creating another strategy, rules machine, or platform-side approximation.

## Goals

- Reconstruct validated minor-2 preflop decisions for 2..10 dealt seats using
  `GameState`, including six-player 100 BB fixtures, all button positions,
  sparse physical seats, preflop folds, raises, calls, and the big-blind option.
- Preserve dealt-seat indices, root identity, public history, exact chip
  movements, and remaining raise rights. Compare all reconstructed player
  statuses against the snapshot for the newly supported multiway profile.
- Preserve minor-0/1 behavior and existing valid heads-up behavior. Reach the
  existing host lookup on a valid six-seat minor-2 request; do not claim that a
  test host row establishes policy strength or available production artifacts.
- Reject malformed histories, blind locations, amounts, and snapshot mismatch
  without returning a fabricated state or changing unsupported utility rules.

## Non-Goals

- Six-max GTO completion, production artifact promotion, quality certification,
  large training campaigns, live play, credentials, external spending, commits.
- New IDL, fields, protocol minor, storage format, solver, training algorithm,
  range abstraction, resource budget, or platform state inference.
- Side-pot support, unequal-starting-stack flop-terminal games, rake, ante,
  straddle, postflop dead-money reconstruction, off-tree policy translation,
  missing action-conditioned ranges, or online deadline redesign. Those remain
  mandatory six-max delivery work, not exclusions from the user feature.

## Current State and Evidence

The following evidence describes the audit before implementation.

- `engine/src/protocol/v1_resident_mapper.cpp` rejects preflop player counts
  other than two and hardcodes root count, blinds, commitments, and stacks to
  two. Postflop reconstruction already maps players by occupied-seat order.
- `engine/src/protocol/v1_envelope.cpp` shares reconstruction between minor-1
  and minor-2 lookups. Its call sites must pass the negotiated minor so only
  minor 2 gains the new multiway admission.
- The IDL/semantic validator accepts 2..10 players; `GameDef` and its exported
  blind helpers already implement multiway preflop order. Flop-terminal games
  still require equal hand-start stacks. RFC 0010 explicitly deferred live
  N-way preflop reconstruction, which this RFC now supplies.
- The resident layer already accepts seat-generic preflop artifacts and uses
  exact root identity. No six-seat artifact was found in the local manifests.
  Existing heads-up or three-seat artifacts cannot be relabeled as six-seat.
- Six-max gaps remain: v1 side-pot refusal, postflop matched-root assumptions,
  absent six-seat production policies/quality gates, and unverified practical
  online costs. None is resolved by this reconstruction change.
- Read-only baseline: native mapper/validator/minor2/resolving tests passed
  4/4; River differential tests passed 17/17. Their side-pot fallback case is
  refusal evidence, not a supported GTO feature.

Overlapping contracts reviewed: RFCs 0002, 0006, 0007, 0008, 0009, and 0010.

## Design Principles

- One rules engine, one preflop replay implementation.
- Table capacity, dealt seats, and live players are distinct quantities.
- Compatibility is version-gated; validation precedes serving.
- Correct reconstruction is necessary but insufficient for GTO qualification.
- No gate is weakened to claim completion of the complete six-max feature.

## Proposed Design

### Version and seat mapping

Add the negotiated minor to the private `reconstructPreflop` interface, with
the legacy value as its default for existing direct callers. Pass the actual
minor from every blueprint/resolving lookup in the envelope. Minor 1 retains
the two-seat profile. Minor 2 admits 2..10 seats, derived from the structured
dealt-player list, never from live count or physical table capacity.

Sort by physical seat, map opaque player IDs to stable internal indices, and
locate the button in that order. Validate duplicate seats/IDs and hero presence.
Retain every dealt player even after a fold. Do not compact a six-seat hand to
heads-up when four players fold. No new generic mapping abstraction is needed.

### Exact ledger and replay

Replace two-element temporary arrays with existing seat-capacity arrays or
bounded vectors and fill `GameDef` arrays for the actual count. Derive each
hand-start stack from its current chips plus exact preflop payments. Use the
native blind helpers/constructor to validate one correctly placed small blind
and one correctly placed big blind. Do not silently correct a swapped blind.
The root keeps `TerminalDepth::Flop` and its equal-starting-stack invariant;
unsupported unequal roots continue to miss, not enter single-pot evaluation.

Replay all observed preflop events in actor order through `GameState`. Preserve
the current target-total convention and Bet/Raise conversion based on amount
owed, including the big-blind option. A fold in the snapshot must be explained
by replay for the new multiway path. All-in players remain indexed, but an
all-in/folded hero cannot be an actionable node. Reject root construction or
replay failure with a deterministic coverage miss and clear partial output.

At success, compare street, phase/actor, pot, stack and street commitment for
every player, amount owed, and multiway folded/all-in status. Retain native
raise-right transitions; test them against the independent expected replay.
No missing contribution is inferred from `pot / player_count`.

### Shared-change gate

Owner: `bigshark_v1_protocol`. Shared changes: its private mapper interface and
envelope lookup helper arguments. The root cause is here; platform filtering,
fake two-seat roots, or retraining cannot repair it locally. Existing callers
are the envelope and native mapper tests. Public IDL, host service signatures,
artifacts, and platform APIs stay unchanged. New acceptance is confined to
minor 2; minor-1 rejection and all existing supported byte fixtures remain
pinned. Rollback restores these source changes without data migration.

## Dependency Rules

Protocol maps generated types to existing poker types. Poker remains unaware
of Protobuf, solver, artifacts, or River. The mapper calls existing rules; it
does not implement another blind, legality, settlement, or strategy core.
Tests use native domain replay and the existing protocol test infrastructure.

## Compatibility and Migration

No IDL/baseline, field interpretation, capability vocabulary, or persisted
format changes. Minor-0/1 behavior is unchanged, including a rejected multiway
preflop request on minor 1. Minor 2 adds a replayable profile, but an artifact
miss retains the existing typed refusal/fallback and guarantee-floor semantics.
Existing artifacts stay immutable, readable, and unmodified. A six-seat row
returned by test services is explicitly engineering evidence, not promotion.

## Security and Operational Impact

Only public player state/actions and hero's own cards enter reconstruction.
Keep semantic validation, numeric bounds, forced-contribution validation,
opaque actor attribution, legality checks, and the official platform boundary.
No network, credentials, real-table mutation, hidden-card acquisition, or new
allocation/resource envelope. At most ten seats are replayed.

## Alternatives Considered

- Change only the player-count gate: leaves fixed arrays and wrong blind order.
- Compact folded seats or project a six-seat pot into a two-seat artifact:
  changes positions, history, blocker and contribution identity.
- Add a separate six-max mapper: duplicates the same preflop rules and ledger.
- Relax side-pot/equal-stack gates here: changes utility contracts before their
  separately reviewed coherent ledger/estimation design exists.
- Apply new admission to minor 1: violates the frozen compatibility boundary.

## Risks

Sparse-seat/button mapping and blind placement can corrupt action order;
cover every button position and reversed input order. Missing histories can
fabricate folds/status; require replay/status agreement. Call-payment and
raise-total disagreements can cancel in pot sums; use per-seat snapshot checks
and native replay. A valid request still needs a matching policy; do not count
safe fallback as six-max GTO completion. Same-build tests do not establish
strategy quality or six-seat online affordability.

## Verification Plan

Native regressions at 100 BB with six dealt seats, all button positions,
sparse/shuffled seats, sequential folds/calls/raises, big-blind option, zero
and nonzero amount owed, and retained status/raise rights. Negative cases:
wrong actor, missing fold, swapped/duplicate blinds, wrong pot/commitment/status,
invalid cards, and unchanged unsupported side-pot/unequal/ante profiles.
Envelope tests must exercise minor-2 BLUEPRINT and AUTOMATIC host lookup and
prove minor-1 never gains multiway lookup. Fake-host results must be labeled
as routing tests, not six-player GTO or a real policy-library validation.

Run root release configure, format, full build/CTest, benchmark-multistreet,
npm check, direct proto check, replay, docs/RFC checks. Run full root Clang
ASan/UBSan CTest for untrusted protocol input changes; report timeouts honestly
and complete affected native binaries without smaller fixtures. No full-scale
training is required for this internal prerequisite.

## Rollout Plan

1. Independently approve this precise mapper change before implementation.
2. Add native regression coverage, then change the mapper/envelope locally.
3. Run native/project gates and independent implementation review.
4. Update current capability boundaries and the complete six-max delivery plan.
   The complete user feature remains In Progress regardless of this RFC status.

## Rollback Plan

Restore the mapper/envelope and corresponding new tests to the prior revision.
Retain existing supported legacy tests and immutable artifacts. No credentials,
training data, live state, or published file is rewritten by this change.

## Open Questions

None for this reconstruction prerequisite. Full six-max utility, strategy
quality, training affordability, and off-tree handling are unresolved delivery
tasks and cannot be closed by this RFC or its mock routing tests.

## Acceptance Criteria

- Six-seat minor-2 preflop nodes replay exactly with complete dealt identity,
  public actions, chips, statuses, and correct button/blind/actor mapping.
- Invalid multiway snapshots fail closed with no usable partial state.
- Minor-1 admission/bytes and supported heads-up fixtures remain unchanged.
- Valid minor-2 multiway requests reach existing host lookup; missing policies
  and unsupported utility still refuse or return honestly labeled fallback.
- Full project/sanitizer checks and independent final approval are recorded.
- No GTO completion, policy-quality, live, or full-training claim is made;
  all remaining six-max blockers remain in the delivery register.

## Decision

Author agent: Codex /root
Approved by: /root/sixmax_runtime_audit
Decision date: 2026-10-14
Review outcome: Approved
Reviewed scope: All 18 sections, RFCs 0002/0006/0007/0008/0009/0010,
mapper/header/envelope, native blind/constructor rules, and mapper/minor1/minor2
fixtures. Pre-decision proposal SHA-256:
6e39a9b9efff5c3891fea098a350b4aec60acdea67794a5dd916dc4a0fe7220f.
Review summary: No blocking findings. Negotiated minor must reach every shared
lookup while minor-0/1 remains frozen; the same GameState and ledger/replay
preserve dealt identity and statuses without a new strategy abstraction.
Side-pot and equal-start-stack utility gates remain intact. The complete
six-max feature stays In Progress. Non-blocking implementation precisions:
test ten-seat and out-of-range boundaries, verify blind type and seat rather
than aggregated amounts only, include root construction in the fail-closed
exception boundary, and confine new status comparisons to the newly admitted
multiway profile. Final implementation approval requires actual native and
full project/sanitizer evidence. This is design approval only.

## Implementation Evidence

Changed files are confined to the following owners:

- Protocol: `engine/src/protocol/v1_resident_mapper.cpp`,
  `engine/src/protocol/v1_resident_mapper.hpp`,
  `engine/src/protocol/v1_envelope.cpp`.
- Native verification: `engine/tests/test_v1_preflop_nseat.cpp`,
  `proto/CMakeLists.txt`, `engine/tests/test_abstraction.cpp`,
  `engine/cmake/stage6_offline_guard.cmake`.
- Documentation: this RFC, `docs/plans/sixmax-gto-delivery.md`,
  `docs/plans/sixmax-gto-qualification.md`, `docs/README.md`,
  `docs/rfcs/README.md`, `docs/design/gto-engine.md`,
  `docs/reference/protobuf-engine-protocol.md`,
  `docs/development/build-and-test.md`.

The mapper/header/envelope change and native
`engine/tests/test_v1_preflop_nseat.cpp` implement the approved scope. The
complete six-max feature remains In Progress; routing fixtures use mock
approximate rows and establish no published policy quality.

Independent source review initially requested changes for one P1 finding:
two wrong CALL payment amounts could cancel in the final per-seat ledger.
The mapper now compares each action's declared payment with the native gross
contribution delta, before accepting subsequent actions. A native regression
uses 19/81 instead of the legal 20/80 while preserving the final totals and
omitting optional intermediate ledgers. It must fail closed. The reviewer
subsequently approved that source checkpoint with no remaining blocker.

Two minimal verification prerequisites were independently reviewed and
approved: equivalent brace initialization in two existing abstraction test
assertions for Clang 14, and an offline-guard dependency on the existing
`protobuf-generate` target so a fresh preset cannot race generated headers.
Neither changes strategy, persisted formats, test fixtures or sanitizer flags.

Final source/document review by `/root/sixmax_runtime_audit` on 2026-10-14:
Approved, with no remaining material source blocker. Final implementation
acceptance is not granted while required sanitizer evidence remains incomplete.
Public wire fields/formats stay unchanged; minor-2 reconstruction admission
is extended. This is not a claim that every public behavior is unchanged.

Verified project evidence:

- Root release configure, format and full build passed. Final stable-source
  `ctest --preset release --parallel 4 --output-on-failure` passed 89/89
  in 361.79 seconds.
- `benchmark-multistreet` passed all three quality gates with repeat delta zero.
- `npm run check` passed, including 132/132 Node tests and 25/25 protocol tests.
  Direct `npm run proto:check` also passed.
- `node bin/replay.mjs` found zero usable historical snapshots: no historical
  coverage. `--synthetic` processed six fixed engineering snapshots, with zero
  illegal actions and zero JavaScript fallbacks; this is not six-max quality.
- Documentation checks passed for 434 text files, RFC checks for 19 RFCs,
  and `git diff --check` passed.
- Full Clang 14 Debug ASan/UBSan build passed with assertions, `-g`, the same
  existing HiGHS prefix, unchanged fixtures and unchanged sanitizer settings.
  Direct native `test_v1_resident_mapper`, `test_v1_minor1`, `test_v1_minor2`,
  `test_v1_resolving` and `test_v1_preflop_nseat` all passed under that build
  with the preset's halt-on-error sanitizer environment.

The earlier full 87-test ASan suite was stopped after recorded failures and
while remaining tests were incomplete. Its `postflop_templates` test
hit the existing 180-second watchdog. The unchanged complete native binary
`./build/asan/engine/test_postflop_templates` subsequently completed all six
independent chance-oracle fixtures and all six structural buckets with exit 0;
the six structural bucket times summed to 399.55 seconds. No fixture, build
flag or training limit was reduced. This closes the same-binary sanitizer
check, but does not turn the timed-out CTest result into a passing full suite.
Its stage6 best-response estimator reference-training fixture also returned
ResourceLimit under the original limits. No sanitizer error was reported by
those failures, but neither is a pass. Unfinished tests are not passes.
Subsequent source changes require current-build sanitizer evidence; this old
run cannot be relabeled as validation of RFCs 0020/0021.
Keep this RFC Implementing until all required evidence and the final
independent implementation decision are recorded.

Later stable-source release verification with RFCs 0020/0021, the local
sanitizer test-clock correction and the independently reviewed native ledger
baseline passed 91/91 in 346.80 seconds. The current six native protocol tests
passed true Debug ASan/UBSan. The unchanged reference training workload now
completes 8/8 iterations and all confirmation/evaluation assertions under its
approved sanitizer-only wall allowance; see RFC 0021's concrete evidence.
The full current sanitizer suite and final independent implementation
acceptance remain open. These later tests add no six-seat policy quality claim.

On 2026-10-14 a four-agent independent audit (RFC 0019, RFC 0020, RFC 0021,
plus build wiring and docs) found this RFC conformant with no blocker or major
findings. The same pass added two minor hardening items beyond the approved
text: the preflop payment accumulator now enforces the same
`kMaxHeadsUpChips` per-add cap as the postflop path (fail-closed for
malformed input only; valid chip ranges are unaffected), and
`test_v1_preflop_nseat` adds a positive replay with all per-event
`pot_before`/`stack_after` ledgers omitted. Current-source gates: release
format/build passed; full `ctest --preset release` passed 91/91 in 363.84
seconds; `npm run check` passed 156/156 Node tests; direct `npm run
proto:check` passed 25/25; `node bin/replay.mjs` exited 0 with zero usable
historical snapshots; `benchmark-multistreet` passed all three quality gates.
The true Debug ASan/UBSan build compiled and linked the current source. The
full ASan CTest suite was stopped at the user's request after roughly 85
minutes with `heads_up_solver_sampled` and `resident_policy` still running;
no sanitizer error was reported by any completed test. The full-suite
sanitizer gate remains open and this RFC stays Implementing.
