---
rfc: "0021"
subject: "N-Seat Cross-Street Reconstruction"
status: "Implementing"
authors: "Codex /root"
created: "2026-10-14"
updated: "2026-10-14"
owners: "engine protocol, engine tests"
supersedes: ""
superseded-by: ""
---

# RFC 0021: N-Seat Cross-Street Reconstruction

## Summary

Generalize the existing exact preflop replay to reconstruct negotiated minor-2
multiway postflop requests that supply a replayable hand-start-to-current
history. Rebase once at the true flop boundary, preserving contributions,
folded seats and the original button, then expose only root-relative postflop
history to existing services. This closes a protocol reconstruction defect,
not provider history completeness, action-conditioned ranges or six-max GTO.
The complete user feature remains In Progress.

## Motivation

The current postflop mapper distributes the flop pot equally across all dealt
seats. A six-seat hand with preflop folds has unequal dead-money contributions
even without all-ins, and its pot need not divide by six. Discarding preflop
folds changes eligibility and action order. Native GameState already knows
the exact transitions; another rules engine or a guessed root is unnecessary.

## Goals

- Reconstruct 3..10 dealt seats from supplied exact blind/action observations
  through flop, turn and river, with explicit six-seat 100 BB fixtures in all
  positions and sparse/shuffled physical seats.
- Derive the true flop pot, per-seat net contributions, stacks and root fold
  bitmap through native replay, without `pot / seat_count`.
- Validate each observed action's actual payment, actor/street order and
  optional ledgers, then final cards, pot, chips, status, actor and amount owed.
- Preserve service root-relative keys and legacy minor-0/1 and heads-up paths.
- Fail closed for missing/unreplayable histories, refunds and unsupported
  existing utility profiles, without silently returning to a guessed root.

## Non-Goals

- Complete six-max GTO, policy publication, quality thresholds, live operation.
- New IDL, wire fields, protocol version, persisted format or strategy engine.
- Proving that caller-supplied events are an authoritative complete provider
  stream. River observation accumulation and completeness still need approval.
- Refund/side-pot wire support, ante/rake/straddle, new house rules, or bypassing
  RFC 0006's coherent v2 gate. These remain mandatory whole-feature tasks.
- Preflop-to-flop equilibrium range propagation or folded-root publication.
  Flop-root service history is not whole-hand range evidence.

## Current State and Evidence

- RFC 0019's `reconstructPreflop` uses the native game, sorted dealt seats,
  exact action replay, individual payments and snapshot status checks.
- `reconstructPostflopImpl` in `v1_resident_mapper.cpp` accounts for preflop
  payments but does not replay preflop folds. It assumes equal flop-root
  contributions and rejects a non-divisible pot.
- GameDef can represent per-seat rooted-board contributions and root folds.
  A River-terminal hand can be replayed from blind posting through all streets
  without the equal-stack restriction of a flop-terminal frontier game.
- Services match `state.def()` and consume root-relative PublicAction history.
  A preflop-rooted cursor and its whole-hand history cannot be passed directly
  to a flop-root artifact. RFC 0020 separately protects root folded identity.
- Current v1 has no hand-start stack/refund/completeness or main-pot eligibility
  contract. With no refunds, starting stack is current stack plus all supplied
  payments. With refunds it is `current + gross - refunds`; guessing the last
  term from pot totals would be incorrect.
- The River mapper's current token grouping is heads-up-based and receives
  recent events. Its data cannot be assumed complete merely because this
  native reconstruction accepts a well-formed supplied history.

Overlapping contracts: RFCs 0002, 0005, 0006, 0007, 0008, 0009, 0011, 0019
and 0020. Their published strategy/utility/format gates remain intact.

## Design Principles

- Reuse one exact hand-start replay implementation and one poker rules engine.
- Deal count, live count, physical seats and root-relative history are distinct.
- State correctness does not establish observation provenance or strategy quality.
- Admit a newly replayable profile; do not change frozen legacy behavior.

## Proposed Design

### Dispatch and compatibility

Pass the negotiated minor to both private postflop reconstruction entry points,
defaulting to 1 for existing direct callers. The envelope already carries the
actual minor. At minor 2, a 3..10-seat postflop request containing any preflop
voluntary event selects strict hand-start replay. On failure it must not retry
the legacy equal-contribution reconstruction. Minor 0/1, two-seat requests and
existing postflop-only histories retain their prior path. That older path is
compatibility behavior, not evidence of full-history six-max qualification.

### Shared exact replay

Extract the existing preflop implementation into a protocol-private hand-start
replay helper; retain reconstructPreflop as its wrapper. This is generalization
of the existing implementation, not a second N-seat mapper. Preflop still uses
the Flop terminal/equal-start-stack profile, while strict postflop replay uses
River terminal. Keep legacy heads-up constructor/failure behavior unchanged.

Plan all actions in ascending supplied street order with street payments
reset only at a real street boundary. Sum forced blinds and exact voluntary
payments per seat with checked numeric bounds, deriving initial stacks under
the no-refund assumption. Validate exactly one correctly placed native small
and big blind, opaque actor mapping, cards, statuses and current profile gates.
Replay actual transitions and observed public cards through GameState. A call
payment is required and must equal the native gross contribution delta; a bet
or raise uses its absolute target total and the native amount-owed convention,
including the big-blind option. Supplied aggressive incremental amounts remain
advisory under the existing target-total contract. Optional nonzero pot_before
and stack_after are cross-checked. Native refunds at any replay step reject
this profile; never spread an aggregate discrepancy across seats.

### True flop boundary and service history

After the third observed flop card and before the first flop action, construct
a River-terminal flop GameDef from the native cursor: original dealt count,
seat mapping and button; stacks behind; net_contributed per seat; folded
bitmap; exact native pot and ordered flop; no current-street commitments,
posted blinds or antes. Instantiate the normal GameState from that definition,
then replay the remaining supplied postflop actions/cards.

Only postflop actions enter the service history from this new root. The original
request retains its supplied preflop actions; they are not inserted into old
flop keys or presented as propagated equilibrium ranges. Do not reuse the
resolver's hero-first private rebase helper, which changes the button.
At success require the exact decision street/board, actionable hero, each
seat's current stack/commitment/folded/all-in status, pot and native legal call
amount to agree with the structured snapshot. Native pending/raise rights
come from replay and are pinned by regression cases.

Blueprint reconstruction keeps its existing non-hero all-in restriction;
resolve reconstruction retains its existing facing-all-in admission. Both
still reject side-pot wire input. Folded-at-flop roots can now reconstruct,
but a mismatched artifact must miss under RFC 0020. Reaching a mock host or
uniform-prior resolver does not close strategy coverage or quality.

### Shared-change gate

Owner: bigshark_v1_protocol. Change only the private mapper helper/entry-point
arguments and envelope dispatch, plus native tests. The defect cannot be
repaired by training, a platform-only rewrite or compacting players. Existing
exact preflop replay is the smallest correct local closure; no public message,
host service signature or poker domain type changes. Callers are the envelope
and native tests. Major risks are accidental legacy routing, wrong flop rebase
and letting strict failure fall back to inference. Default minor 1, differential
legacy tests, exact native state comparisons and typed misses address them.
Rollback needs no data migration.

## Dependency Rules

Generated v1 types terminate in protocol. Native GameState owns actor/street,
payment, refund and action-right rules. No platform strings, storage handles,
solver type or generated field enters poker; no new module or dependency.

## Compatibility and Migration

No IDL, conformance vector, artifact or runtime-file changes. Existing valid
minor-0/1 and heads-up responses remain pinned. The new strict minor-2 profile
uses the existing coverage-miss vocabulary. No silent retry to legacy inference
after selecting it. The older postflop-only profile remains available but is
unqualified as whole-hand six-max evidence. Existing generations stay immutable.

## Security and Operational Impact

Use only caller-supplied public actions/board/status/chips and hero's cards.
Keep semantic validation, frame bounds, numeric limits and fail-closed partial
output cleanup. No hidden information, CLI/network operation, credentials,
live actions or training is introduced. Complexity is bounded by the existing
frame and ten-seat profile. No new real-time or policy-quality claim.

## Alternatives Considered

- Divide flop pot across remaining live seats: loses dead money and dealt identity.
- Replay only postflop folds: does not restore folds before the flop.
- Return the original preflop definition to host: wrong artifact root and keys.
- Add a separate multiway implementation: duplicates the proven preflop replay.
- Trust current stacks plus gross in refund hands: overstates initial stacks.
- Delete v1 side-pot refusal: loses eligibility and bypasses RFC 0006.
- Replace the River token grouper alone: lacks trusted full observation data.

## Risks

Partial flop dealing already changes native street after the first card: do
not rebase until all three cards and before any flop action. Rebase may erase
refund evidence: reject it before discarding the original ledger. A final-pot
match alone can hide offsetting call errors: compare each action. A caller can
supply a replayable but untrustworthy history: provider observation provenance
remains unresolved and cannot be certified by this mapper. Real policies may
not match the reconstructed root; preserve that honest miss.

## Verification Plan

Add a native test in the existing v1 CMake foreach. Six-seat 100 BB cases:
all positions and sparse/shuffled seats; preflop folds/dead money with a pot
not divisible by six; full turn/river check-through; exact postflop bets,
calls/raises, short all-in rights within the unchanged admission; correct
button and flop root/net contributions; root-relative history excludes preflop.
Compare final native states and explicit literal ledgers, not mapper-to-itself.
Negative cases omit/reorder folds/checks/cards, swap blind identity, corrupt
individual payments that cancel in totals, alter statuses/ledgers, include
refunds or existing unsupported utility, and verify cleared partial outputs.
Envelope fakes must be approximate and labeled mechanical, never certificates.
Pin minor-1/default and postflop-only legacy compatibility.

Run root configure/format/build/full release CTest, benchmark-multistreet,
Node/protocol/replay/docs/RFC gates and full ASan verification for protocol
changes. Record old pending runs separately; an old binary is not new evidence.
Do not shrink fixtures or silently count watchdog timeouts as passes.

## Rollout Plan

1. Independent approval of this bounded native reconstruction design.
2. Add native regressions and generalize the existing replay in its owner.
3. Verify all roots/streets/legacy paths, actual dispatch and project gates.
4. Independent review, then move to trusted observation and coherent v2 ledger
   design. Complete six-max GTO stays In Progress.

## Rollback Plan

Restore only the mapper/header/envelope and this test registration to the
prior implementation; retain RFC 0020's independently correct identity guard.
Keep immutable policies and existing supported regressions. No data migration,
credentials, artifact replacement or live rollout is performed.

## Open Questions

None for the supplied-history, no-refund native reconstruction profile.
Authoritative public observation completeness, refunds/side-pot v2, policy
ranges and actual six-max quality remain unresolved whole-feature tasks.

## Acceptance Criteria

- Supplied replayable minor-2 multiway histories preserve true six-seat flop
  root identity, dead money and all final native state invariants across streets.
- Strict failure never retries guessed equal-root reconstruction or exposes
  partial state; refunds and existing unsupported utility remain explicit misses.
- Minor-0/1, heads-up and supported postflop-only compatibility remains pinned.
- RFC 0020 protects folded-root policy lookup; no wrong artifact substitution.
- Native/project/sanitizer evidence and independent implementation review recorded.
- No whole-hand quality/completeness or complete six-max claim from this milestone.

## Decision

Author agent: Codex /root
Approved by: /root/sixmax_runtime_audit
Decision date: 2026-10-14
Review outcome: Approved
Reviewed scope: All 18 sections, current native replay/core/host contracts
and overlapping RFCs. Pre-decision proposal SHA-256:
e0646cb6f2d0e0904e0dc6d75db065cca47e22527f52d134e225904288efb7e7.
Review summary: No blocking findings. One protocol-owned shared replay,
minor-2 multiway supplied-history dispatch, checked no-refund reconstruction,
and true flop-boundary rebase preserve original positions/net contributions
and service-relative keys. RFC 0020 protects folded root identity. Strict
failure must never retry legacy inference. Refund detection must cover all
seats after actions/cards and before rebase discards the ledger. Do not copy
the legacy any-fold blueprint gate into the strict profile. Frozen legacy
routes remain compatibility only, never provider-completeness evidence.
No new IDL, format, strategy or quality claim. Design approval only;
native/project/sanitizer evidence and implementation review remain required.

## Implementation Plan

Protocol owner generalizes the existing hand-start replay and adds the private
minor arguments/dispatch. Native-test owner owns only the new
`test_v1_postflop_nseat.cpp` and its existing foreach registration. Preserve
all RFC 0019 tests and unrelated edits. First verify focused preflop, postflop,
envelope and frozen legacy fixtures, then the full root build/verification
contract. Do not mark Implemented until the actual same-source evidence and
independent implementation review are recorded.

## Implementation Review Findings

P1, independent source review by `/root/sixmax_runtime_audit`: inheriting the
preflop Bet/Raise normalization for postflop could accept a mislabeled BET
facing a wager, or a mislabeled RAISE with nothing owed, without changing the
final ledger. The strict postflop replay now checks observation kind against
native amount owed before transition. Preflop BB-option compatibility remains
unchanged. Both direction negative controls pass with explicit OffTree on
blueprint and resolve paths. Independent source/test re-review by
`/root/sixmax_runtime_audit`: Approved, P1 closed, no remaining material
source blocker. This does not waive the remaining full verification gates.

The complete new native postflop test passed directly on release and on the
current Clang 14 Debug ASan/UBSan build with halt-on-error options and `-g`.
An intermediate full release run included two incorrect extra test assertions
that assumed the native N-seat profile would not expose an active seat after
a refund. Actual native behavior opens that seat's next-street check. The
assertions now verify the true active actor, retaining the same full fixtures,
refund values and typed refusal checks; the complete native test passed again.
That earlier failed full run is not reported as passing. The final same-source
full release rerun passed 90/90 in 354.79 seconds. A subsequent full release
run with the native validation diagnostics passed 90/90 in 354.95 seconds.
The rebuilt true Debug ASan/UBSan protocol subset passed all six tests and the
complete native resolver passed in 86.53 seconds. Broader sanitizer evidence
and final implementation acceptance remain outstanding.

Independent local validation-clock review by `/root/sixmax_runtime_audit`:
Approved on 2026-10-14, with no fixture/iteration/seed/numerical-threshold or
production-limit changes. The unchanged complete template binary passed in
399.55 seconds after its prior 180-second CTest timeout; only its sanitizer
watchdog becomes 900 seconds. The estimator's original five-minute standalone
reference fixture returned ResourceLimit at 300712 ms, completing 4/8
iterations, 4798869/200000000 nodes, 104912/20000000 information sets and
667611424/1073741824 accounted bytes. The approved test-only correction gives
that one sanitizer training/evaluation fixture fifteen minutes per phase.
Failed training still fails the original assertion and does not evaluate a
partial policy. A clearly labeled single-case diagnostic entry is not full
verification. All other limits and the true Debug sanitizer flags stay intact;
the adjusted case and full suite must still actually pass.

The adjusted standalone cross-check subsequently passed in full: training
completed 8/8 iterations in 574555 ms (8868868 nodes, 104912 information sets,
667611424 accounted bytes); independent evaluation returned in 96775 ms.
Both original numerical checks and the 4000 confirmation samples passed;
the exact reference was 0.27244, the sampled estimate 0.28450, and the
residual CI was [-0.0474, +0.0716]. This two-seat verification fixture is not
six-seat quality evidence and does not replace the full ASan suite.

After the separate native ledger baseline was registered, the final release
configure/format/full build and full CTest passed 91/91 in 346.80 seconds.
The ledger's eighteen complete hands also passed focused true Debug ASan/UBSan
in 0.13 seconds. Its independently reviewed test/helper is local to the native
test owner and changes no production state transition or wire contract.
Review by `/root/sixmax_runtime_audit`: source/test checkpoint Approved,
no blocking findings; not final full-suite implementation acceptance.

On 2026-10-14 a four-agent independent audit (RFC 0019, RFC 0020, RFC 0021,
plus build wiring and docs) found this RFC conformant with no blocker or
major findings, confirming the strict minor-2 multiway postflop path derives
the true flop pot, per-seat contributions, stacks and root fold bitmap by
native replay with no `pot / seat_count` division, and never retries legacy
inference on failure. Current-source gates: release format/build passed;
full `ctest --preset release` passed 91/91 in 363.84 seconds, including
`v1_preflop_nseat`, `v1_postflop_nseat` and `sixmax_hand_ledger`; the same
pass added a positive preflop replay with all per-event ledgers omitted and
symmetric `kMaxHeadsUpChips` payment caps; `npm run check` passed 156/156
Node tests; direct `npm run proto:check` passed 25/25; `node bin/replay.mjs`
exited 0 with zero usable historical snapshots; `benchmark-multistreet`
passed all three quality gates. The true Debug ASan/UBSan build compiled and
linked the current source. The full ASan CTest suite was stopped at the
user's request after roughly 85 minutes with `heads_up_solver_sampled` and
`resident_policy` still running; no sanitizer error was reported by any
completed test. The full-suite sanitizer gate remains open and this RFC
stays Implementing. Provider observation completeness and the v2 ledger
contract remain separate open work (RFC 0022, T1b).
