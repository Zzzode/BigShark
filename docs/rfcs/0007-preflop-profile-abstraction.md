---
rfc: "0007"
subject: "Preflop Profile Abstraction and Artifact Coexistence"
status: "Proposed"
authors: "BigShark engine agent"
created: "2026-09-18"
updated: "2026-09-18"
owners: "Poker, solver, artifact boundary, benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0007: Preflop Profile Abstraction and Artifact Coexistence

## Summary

Define how a heads-up preflop profile becomes a bounded, trainable, and
persistable game without changing the meaning of any existing postflop
artifact. The proposal introduces an explicit *terminal-depth* concept on the
game: a preflop game terminates at the flop rather than running out to the
river, with the postflop continuation represented by a declared range pair
rather than enumerated. It also extends the artifact size schedule and the
information-key encoding additively so a preflop-rooted artifact can coexist
with the flop-rooted artifacts already on disk.

## Motivation

RFC 0004 Stage 10 requires heads-up preflop roots and policy-derived
continuation ranges. The rules for that profile are implemented and verified
(`engine/src/poker/heads_up.cpp`, `engine/tests/test_heads_up_preflop.cpp`).
What is missing is everything downstream of the rules, and the missing part is
blocked by a measurable scale problem plus two artifact contracts.

A full traversal from a preflop root does not converge inside the declared
resource limits. Every preflop line reaches every flop, so the tree spans all
`C(48,3) = 17,296` boards per line times the action menu. A bounded walk on a
deliberately shallow three-big-blind stack visits over 3,000,000 nodes and more
than 1,000,000 distinct postflop information sets, while the preflop street
itself carries only two. The convergence gates of RFC 0004 cannot be met by
enumerating that tree, on any machine, at any iteration count — the workload is
structural, not a tuning problem.

Two artifact contracts currently hard-code the flop-rooted assumption:

- the `sizes` table constrains `street BETWEEN 0 AND 2`, and
  `insert_sizes`/`read_sizes`/`same_size_schedule` all iterate exactly three
  entries (`engine/src/artifacts/strategy_artifact.cpp:532`, `:751`, `:966`,
  `:63`), so a fourth schedule entry cannot be stored;
- `encode_public_key` derives the street as `board_count - 3` and rejects
  `board_count < 3` (`engine/src/artifacts/artifact_codec.cpp:64-80`), so a
  preflop information state has no canonical encoding.

A third constraint is subtler and is why this needs a decision rather than a
patch: `game_byte_charge` derives part of its accounting from
`sizeof(HeadsUpGame)` (`engine/src/gto/heads_up_solver.cpp:201`). Widening
`SizeSchedule` changes that size, which moves `accounted_bytes`, which is
written into the artifact manifest — and the artifact evidence requires a
split run and an uninterrupted run to publish byte-identical generations. The
same value appears in published benchmark output. The accounting input must
therefore stop depending on struct layout in the same change that changes the
struct.

The value of resolving this is the RFC 0004 Stage 10 acceptance criteria:
independently declared preflop coverage and quality evidence, with the live
six-max charts untouched.

## Goals

- A preflop-rooted game trains to a declared convergence gate inside the
  bounded local limits, with coverage and cost reported honestly.
- Policy-derived continuation ranges: the postflop range pair a preflop
  solution hands to a flop-rooted profile is computed from actual policy reach,
  not from a chart or a hand-written assumption.
- Every artifact already on disk keeps its exact meaning, digest, and
  readability. No existing artifact is reinterpreted, renumbered, or
  regenerated.
- Artifact byte identity (split run versus uninterrupted run) continues to
  hold exactly, and the accounting input no longer depends on struct layout.
- The live six-max preflop decision path and the v0/minor-0 wire behavior are
  byte-identical.

## Non-Goals

- Six-max preflop equilibrium coverage. A heads-up artifact must not claim it.
- Multiway preflop; RFC 0006 owns multiway.
- Replacing the live preflop charts. Promotion of any trained profile to
  production is a separate gate and is not authorized by this RFC.
- Full-game (preflop-through-river) equilibrium solving.
- Changing the information-key *algorithm* for existing roots. The historical
  keys and their encoded strings are frozen.

## Current State and Evidence

Verified in the working tree at commit `c16798f`:

- Preflop rules exist and are independent of the solver: `HeadsUpRoot::preflop`
  and `blinds_posted`, `Street::Preflop` appended after the historical values,
  button-first order, the big blind option after a limp, postflop order
  reversal, one-card-at-a-time flop dealing, and live-blind/all-in settlement.
  `engine/tests/test_heads_up_preflop.cpp` covers these plus the measured tree
  size. The Stage 1 exhaustive oracle still reports 12,226 nodes / 2,852 folds
  / 3,002 showdowns, so the flop-rooted profile is unchanged.
- `SizeSchedule` is `std::array<StreetSizes, 3>`
  (`engine/include/bs/heads_up_solver.hpp:39`), indexed by
  `static_cast<std::size_t>(state.street())` with a preflop clamp onto the flop
  entry (`engine/src/gto/heads_up_solver.cpp:567-573`).
- `HeadsUpTrainer`'s constructor reserves each `root.flop` entry as a distinct
  card (`:617-621`); it now skips the unset sentinel, so a preflop game
  constructs. Training it does not converge: measured `status=ResourceLimit`,
  `completed_iterations=0`.
- The artifact `sizes` table, size reader/writer, size comparison, and public
  key encoder all assume a flop root, cited above.
- `game_byte_charge` reads `2 * sizeof(HeadsUpGame)`.

Assumptions, stated as such: the flop-terminal abstraction below is expected to
make the preflop street's information-set count small (hundreds, not millions),
because the preflop betting tree is bounded by stacks and the action menu. This
RFC requires measuring it rather than asserting it (see Verification Plan).

## Design Principles

1. Additive only. Existing artifacts, keys, roots, and wire bytes keep their
   exact current meaning. New capability is expressed through new values, never
   by reinterpreting old ones.
2. The source of truth for a game's identity is an explicit declaration, not an
   inference from a sentinel. A preflop game declares that it is one.
3. Accounting inputs are declared constants, not struct-layout consequences.
   A field addition must not silently move a published number.
4. Representing a continuation is preferred over enumerating it. The preflop
   profile's job is to produce a postflop range pair, not a postflop policy.
5. Fail closed. An unrepresentable profile is rejected with an explicit reason,
   never approximated into a different game.

## Proposed Design

### Preflop terminal depth

Add an explicit terminal declaration to the offline game:

```
enum class TerminalDepth { River, Flop };   // River is the existing behavior

struct HeadsUpGame {
  ...
  TerminalDepth terminal = TerminalDepth::River;   // additive; River is default
};
```

A `TerminalDepth::Flop` game ends when a completed flop would open action: the
`Deal` phase that follows the preflop street is not entered. Instead the
traversal reaches a *frontier leaf* whose value is supplied by a declared
frontier evaluator rather than by the rules' showdown. The frontier is the
contract boundary of this RFC; the concrete evaluator for the first profile is
specified by RFC 0004's existing continuation-range requirement.

### Frontier representation

The preflop profile hands the postflop profile a range pair. At a frontier leaf
the value of a joint deal `d` is

```
V(d) = sum over the postflop policy's value at the flop root for deal d,
       weighted by that policy's action probabilities
```

which is exactly the locked-continuation expectation RFC 0005 already defines
for its resolving gadget. Reusing that definition keeps one meaning of
"continuation value" in the repository instead of two.

Two implementation options are credible and this RFC deliberately leaves the
choice to the implementation stage, with a measurement gate:

- **A. Declared frontier table.** The caller supplies an immutable map from
  (flop, joint-deal) to value, produced offline by evaluating an existing
  flop-rooted policy. Simple, exactly reproducible, and the artifact carries
  it as an ordinary declared input.
- **B. Nested evaluation.** The frontier evaluator runs the existing
  flop-rooted blueprint lookup in-process at each frontier leaf. No large table,
  but the preflop solve's cost now depends on postflop lookup performance and
  must state that dependency.

The stopping condition is measurement: whichever option keeps the preflop
solve inside the declared limits with the smaller accounted bytes wins, and the
result is recorded in the design document with the numbers.

### Size schedule

`SizeSchedule` gains a fourth entry. To keep accounting stable in the same
change, `game_byte_charge` stops deriving its structural term from
`sizeof(HeadsUpGame)` and uses a declared constant that is documented as the
accounting input, with a static assertion pinning the intended relationship so
a future layout change is a deliberate edit rather than a silent drift.

The preflop menu is a declared part of the preflop game's identity, like every
other size schedule, and is written to the artifact's `sizes` table.

### Artifact coexistence

The `sizes` table's `street` domain widens from `0..2` to `0..3`. This is
additive: every existing artifact contains only `0..2` rows and reads
unchanged, and its `manifest` bytes are untouched, so its digest is stable.

The preflop information state needs a canonical encoding. Rather than changing
`encode_public_key`'s existing outputs, the encoder gains a single new form for
`board_count == 0`, and the existing `board_count in 3..5` forms are frozen
byte-for-byte. A key with `board_count` outside `{0, 3, 4, 5}` remains invalid.

The root street recorded in an artifact is currently constrained to `0`
(flop). A preflop artifact records a new declared root street value; the
existing value keeps its meaning. Compaction, digest, and reader validation
treat the two root streets as distinct, non-comparable profiles: an artifact
never claims coverage of a root street it was not trained for.

### Continuation-range export

A preflop solve publishes, for each flop root it reaches with positive
probability, the hero and opponent range pairs derived from the average policy's
reach along the preflop line. The derivation is a tested pure function with an
independently checked fixture, not an inline computation inside the trainer.

## Dependency Rules

- `bigshark_poker` owns the rules and the preflop root; it gains no knowledge of
  artifacts, ranges, or solvers.
- `bigshark_solver` owns the terminal-depth declaration, the frontier
  evaluator, and the range export. It keeps depending only on `bigshark_poker`.
- The artifact boundary continues to own serialization and may depend on the
  solver's public types, never the reverse.
- No new dependency direction is introduced, and no target gains a transport,
  SQLite, or provider type it does not already have.

## Compatibility and Migration

Affected surfaces and their disposition:

| Surface | Effect |
| --- | --- |
| Existing flop-rooted artifacts | Unchanged bytes and digest; `street 0..2` rows still valid |
| Existing information keys and their encoded strings | Frozen; only `board_count == 0` gains a form |
| `HeadsUpGame` size and layout | Changes; the accounting term is decoupled in the same change |
| Published benchmark `accounted_bytes` | One-time, documented rebase; the value is an accounting input, not a performance claim |
| v0 / minor-0 wire behavior | Unchanged; no wire field is added |
| Live six-max preflop decisions | Unchanged; no production routing is touched |

Migration is by coexistence, not conversion. No existing artifact is rewritten,
and there is no flag day: a reader that does not understand `street == 3` or a
preflop root street rejects the artifact as an unsupported version rather than
misreading it.

## Security and Operational Impact

No credentials, network access, or live play are involved; training is offline
on synthetic or reviewed public data, as RFC 0004 already requires. The new
resource surface is the frontier table, which adds a bounded, declared input to
the artifact and therefore to the artifact reader's size limits; its maximum
size and the resulting reader bound are part of the acceptance evidence. No
change to trust boundaries, fairness invariants, or the existing default
resource limits is proposed.

## Alternatives Considered

- **Enumerate the whole preflop tree.** Rejected: measured to be structurally
  unbounded (over 3,000,000 nodes and over 1,000,000 postflop information sets
  at a three-big-blind stack), so no iteration budget reaches the gates.
- **Train only the preflop street with chance abstracted away.** Rejected: the
  preflop game's payoffs depend on which flop arrives; dropping that makes the
  objective a different game and the exported ranges unjustified.
- **A separate preflop solver beside the existing trainer.** Rejected: RFC 0004
  requires the same rules and trainer; a second implementation would duplicate
  the legal transition model that the repository deliberately keeps singular.
- **Change `encode_public_key` to renumber streets.** Rejected: it would change
  the encoding of keys already written to disk, breaking artifact readability
  for no benefit.
- **Fold the preflop schedule into the flop entry via a scaling rule.**
  Rejected: it makes the preflop game's size menu implicit in another street's
  declaration, so two different games could share a size identity.

## Risks

| Risk | Mitigation |
| --- | --- |
| The flop-terminal abstraction still overshoots the limits | Measure before choosing the frontier option; stop and re-scope if the preflop street itself exceeds the gate |
| Frontier-option-B solve cost depends on postflop lookup speed | The measurement gate compares both options on accounted bytes and wall time; the choice is recorded |
| Widening `SizeSchedule` moves published numbers | Decouple the accounting term in the same change; record the one-time rebase explicitly and keep the gates' semantics |
| The `board_count == 0` key form collides with an existing encoding | The encoder's output domain is checked for collisions against every existing form in the verification fixtures |
| Exported ranges are mistaken for certified equilibrium ranges | The design document and the export's own naming state that the ranges are policy-reach estimates at a declared profile, with no equilibrium claim |

## Verification Plan

- Enumerated preflop fixture: a small preflop game whose frontier values are
  computed independently, compared against the solver's traversal.
- Frontier-option comparison: measured accounted bytes and wall time for both
  options on the same fixture, recorded in the design document.
- Range-export fixture: hand-derived reach for a fixed small policy, checked
  against the exported range pair, including card removal and zero-reach combos.
- Key-encoding conformance: every existing encoding fixture still produces its
  exact current string; the new `board_count == 0` form round-trips and does not
  collide with any existing form.
- Artifact compatibility: a pre-existing flop-rooted artifact fixture loads with
  an unchanged digest; a preflop artifact round-trips; a reader meeting an
  unsupported root street rejects rather than misreads.
- Byte identity: split-run versus uninterrupted-run equality still holds
  exactly, on both profiles.
- No-regression gates: the Stage 1 exhaustive oracle, the replay suite
  (`156 decisions / 0 illegal / 0 JS fallbacks`, mix unchanged), the frozen
  matrix, and the full `AGENTS.md` gate including ASan/UBSan.

## Rollout Plan

1. Decouple the accounting term from struct layout and widen `SizeSchedule`,
   with no new profile enabled. Gates: full suite, artifact byte identity,
   benchmark rebase recorded.
2. Add the terminal-depth declaration and the frontier representation, with
   option A as the first implementation. Gate: the enumerated preflop fixture
   and the frontier comparison.
3. Add preflop artifact persistence and the new key form. Gate: the artifact
   compatibility and key conformance fixtures.
4. Add the continuation-range export and preflop training evidence. Gate:
   declared preflop coverage and quality numbers, plus the no-regression suite.

Stop on any legality, hidden-information, conservation, oracle, or convergence
failure. Do not weaken a threshold to finish a stage.

## Rollback Plan

Each stage is independently revertible. Stages 1 and 2 add offline capability
that no production path selects, so reverting them restores the exact previous
behavior. Stage 3's format extension is additive: an artifact written with
`street == 3` or a preflop root street is rejected by an older reader as an
unsupported version, so reverting the reader cannot corrupt or misinterpret
existing data. No old artifact is overwritten or reinterpreted at any point.

## Open Questions

- Which frontier option (declared table or nested evaluation) wins on the
  measured comparison? Decided by stage 2's evidence, not by preference.
- What is the maximum accepted frontier-table size, and what reader bound
  follows from it? To be set from stage 2's numbers.
- Does the preflop profile need its own declared coverage vocabulary beyond the
  existing blueprint coverage reporting? To be answered by stage 4's evidence.

## Acceptance Criteria

- A preflop-rooted game trains to a declared convergence gate within the
  bounded local limits, with nodes, information sets, accounted bytes, and
  wall time reported.
- Continuation ranges are policy-derived, independently checked, and named so
  they cannot be read as certified equilibrium ranges.
- Every pre-existing artifact loads with an unchanged digest, and the key
  encoder's existing outputs are byte-identical.
- Artifact byte identity holds on both profiles, and the accounting input no
  longer depends on struct layout.
- The live six-max preflop path, v0, and minor-0 wire bytes are unchanged.
- Documentation reports measured preflop coverage without full-game GTO claims.

## Decision

Pending independent approval-agent review.
