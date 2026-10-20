---
rfc: "0030"
subject: "Pin Strategy-Shaping Abstraction Parameters in Persisted Identity"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-20"
owners: "engine abstraction, engine flop library, engine solver"
supersedes: ""
superseded-by: ""
---

# RFC 0030: Pin Strategy-Shaping Abstraction Parameters in Persisted Identity

## Summary

The per-street raise cap shapes the strategy but participates in no persisted
identity, so a warm-start checkpoint written under one cap can be resumed by a
build running another, and a stale row is accepted because validation checks
`node_index` range and the action menu — both of which can coincide across
different tree shapes. This RFC makes the cap part of the action-abstraction
digest and adds a tree-shape witness to the sealed checkpoint. It changes no
cap value: raising the cap is a separate, measured proposal.

## Motivation

**The cap is absent from the digest.** `make_id` hashes only the canonical
sizing schedule (`engine/src/abstraction/abstraction.cpp:110-117`);
`make_declared_id` appends only a cover-seeds token (`:119-131`).
`AbstractionId` carries `name`, `version`, `parameters`, `digest`
(`engine/include/bs/abstraction.hpp:95-110`) and no typed cap. Every producer
applies the cap *after* construction through `set_max_raises_per_street`
(`engine/include/bs/abstraction.hpp:238`), at five code sites:
`engine/benchmarks/flop_library_builder.cpp:188`,
`engine/benchmarks/preflop_profile_builder.cpp:207`,
`engine/src/resolver/subgame_resolver.cpp:505`,
`engine/benchmarks/subgame_nash_conv.cpp:1169,1198`, and
`engine/tests/test_flop_library.cpp:204`. The value never reaches the minter.
`flop_library.hpp:59-62` records the exclusion as deliberate.

**A checkpoint can be resumed against a tree it does not match.** Sealed
resume compares two digests and nothing about tree shape
(`engine/src/flop_library/flop_library.cpp:366-372`); `node_cap`/`bucket_cap`
are recorded at `:356-357,433-434` and enforced only as bounds
(`engine/src/artifacts/sealed_checkpoint.cpp:177,552`). `validate_initial`
then accepts an initial row whenever `node_index < tree.size()`, the node is an
action node, and `raw.actions == tree.node(index).actions`
(`engine/src/gto/nseat_trainer.cpp:905-921`). Two different tree shapes can
satisfy all three for a stale row.

**The defect is reachable through a specific, verified sequence.** A normal
resume is already protected: `.sections/build.json` records
`max_raises_per_street` and `require_identity_match` refuses a mismatch
(`engine/src/flop_library/flop_library.cpp:662-664`). The gap is the path where
that file is absent. `build_library` removes only `.sections` when
`build.json` is missing (`:922`), while the checkpoint lives in `out_dir`
(`:352`). So: delete `.sections`, keep the checkpoint, change the cap, and the
builder writes a fresh identity from the new cap and resumes the old
checkpoint's rows. This is the case a tree-shape witness must catch.

## Goals

- **G1**: The raise cap participates in the action-abstraction digest, so a
  cap-differing pair has differing identities.
- **G2**: A checkpoint whose tree shape differs from the current tree is
  refused by name, before any trainer state is built.
- **G3**: The cap is recoverable from a persisted identity without assuming a
  value.

## Non-Goals

- **No cap value change.** The production cap stays one. Raising it forces a
  full retrain, changes every published artifact's digest, and — by the
  measurements a follow-up must take — may make five- and six-seat live
  resolving infeasible. That decision needs its own evidence.
- **No card-abstraction change.** Separate proposal (RFC 0031).
- **No turn/river work.** Separate proposal (RFC 0032).
- **No menu-derivation unification.** See Open Question 1: the two paths are
  not redundant, and merging them is a separate change with its own risk.

## Current State and Evidence

### Facts (verified in this repository)

- **The digest ignores the cap.** Verified above at
  `abstraction.cpp:110-131`; the golden value `0x422c245239c7a527` is pinned at
  `engine/tests/test_abstraction.cpp:214` and `:556` only. The other lines a
  prior draft listed (`:342,433,437,854-861`) are card-bucket cardinality and
  flop-class merge pins and do not move.
- **`build.json` already guards the cap.** `BuildIdentity::max_raises_per_street`
  is written and compared (`engine/src/flop_library/flop_library.cpp:662-664`).
  This RFC does not duplicate that guard; it closes the path where the file is
  absent.
- **The checkpoint does not require a matching tree.** `:366-372` plus
  `nseat_trainer.cpp:905-921` as quoted in Motivation.
- **`resolve_flop` mints an identity locally.** It constructs
  `ActionAbstraction::identity()` and sets the cap to one
  (`engine/src/resolver/subgame_resolver.cpp:504-505`), so it has no persisted
  abstraction to read a cap from. `resolve_uniform` leaves the cap at the
  identity's zero (`:442`), and `reconstruct_action_abstraction` rebuilds from
  `sizes` only (`:112-117`).
- **The cap counts opening bets.** `Bet` and `Raise` share one `case`
  (`engine/src/poker/game_definition.cpp:652-653,667`), so "N raises" means
  "N full bets or raises per street".
- **A mid-street subgame root resets the counter.** `GameDef` carries no count
  (`engine/include/bs/game_definition.hpp:75-164`) and
  `build_subgame_def_from_node` (`engine/src/resolver/subgame_resolver.cpp:340-377`)
  does not seed one, so `GameState::raises_this_street_` keeps its default zero.
- **Two menu paths exist but are not redundant.** `build_action_menu` is the
  body of `ActionAbstraction::menu` (`engine/include/bs/abstraction.hpp:247`,
  implemented at `engine/src/abstraction/abstraction.cpp:242`), and the
  cap-aware `abstract_node_menu` calls it (`engine/src/tree/abstract_tree.cpp:171`).
  `abstract_actions` is a distinct entry point with production callers
  (`engine/src/resolver/counterfactual_reach.cpp:172`;
  `engine/src/gto/heads_up_solver.cpp:294,423,523`). They are layers, not
  duplicates.
- **The heads-up route cannot carry a cap today.** `HeadsUpGame` has no
  `ActionAbstraction` and `HeadsUpState` has no raise count.
- **The tree builder throws rather than truncating.** `TreeLimits` raises
  `tree_resource_exhausted` before allocation
  (`engine/src/tree/abstract_tree.cpp:52-53`), so a checkpoint can only come
  from a tree that was fully built — "smaller tree" means a different
  configuration, never a truncation.

### Assumptions (to be verified)

- A 64-bit FNV-1a witness is adequate for offline integrity. It is
  non-cryptographic and can collide; it is an integrity check, not a proof.

## Design Principles

- **A strategy-shaping parameter belongs in the identity.** If two artifacts
  can differ in behavior, their identities must differ.
- **Fail closed on resume, at the strongest available witness.** A checkpoint
  that cannot prove it matches the current tree is refused whole.
- **Change one thing.** This RFC fixes identity and resume validation. It does
  not change a strategy value.

## Proposed Design

### Identity-minting contract

`max_raises_per_street` becomes a constructor-level parameter of
`ActionAbstraction`, not a post-construction setter. Both minters and both
public constructors take it:

```cpp
// The cap is std::uint8_t throughout, matching the existing setter and member
// (engine/include/bs/abstraction.hpp:238-239); the RFC does not widen it.
// Minters: the cap joins the hashed parameters.
AbstractionId make_id(const SizeSchedule& schedule, std::uint8_t max_raises = 0);
AbstractionId make_declared_id(const SizeSchedule& schedule,
                               abstraction::CoverSeeds seeds,
                               std::uint8_t max_raises = 0);

// Consumed surface (engine/include/bs/abstraction.hpp:220-231).
explicit ActionAbstraction(SizeSchedule schedule, std::uint8_t max_raises = 0);
static ActionAbstraction identity(std::uint8_t max_raises = 0);
static ActionAbstraction declared(SizeSchedule schedule, CoverSeeds seeds,
                                  std::uint8_t max_raises = 0);
```

The parameter line gains a canonical token appended **after** the existing
content, because the digest is order-sensitive and `make_declared_id` currently
appends `cover-seeds=` last (`engine/src/abstraction/abstraction.cpp:125-128`):

```text
<canonical schedule>[cover-seeds=...]|max-raises=<N>
```

`cover-seeds=` keeps its current spelling with **no** separator, because
changing it would move the declared digest for a second, unrelated reason. Only
the new token is `|`-prefixed.

`reconstruct_action_abstraction`
(`engine/src/resolver/subgame_resolver.cpp:112-117`) parses the trailing token
back out instead of assuming a value. This also fixes a pre-existing gap the
RFC claims only as a side effect: that function currently returns cap 0 for a
blueprint built with `--raise-cap 1`, so turn/river continuation menus are
uncapped today.

`set_max_raises_per_street` is **removed**. Every producer passes the cap into
the constructor — five code sites, named in Motivation. Because `max_raises`
keeps a `0` default (see Risk 2), `identity_action_id()` — which is
`ActionAbstraction::identity().id()` (`engine/src/abstraction/abstraction.cpp:156-158`)
and is used as a default member initializer at `engine/include/bs/solve.hpp:193`,
compared against `tree.action_id()` at `engine/src/gto/solve.cpp:26,82` —
continues to mean the cap-0 identity. A tree built with a nonzero cap therefore
has a different `action_id()` than `identity_action_id()`, which is correct: they
are different abstractions. That is the point: a
setter that must remember to re-mint the digest is exactly how the value was
lost.

The golden digest `0x422c245239c7a527` moves. It is pinned at
`engine/tests/test_abstraction.cpp:214,556` and embedded in
`artifacts/flop-library-6way-fullrange`, `artifacts/preflop-profile-6way`,
`artifacts/preflop-profile-3way`, `artifacts/preflop-profile-validation`, and
`artifacts/ac1-16w`. The three non-shipping directories are historical and are
not republished; they remain as inert records and are named here so a reader is
not surprised by a stale digest.

### The admissibility gates

Making the cap part of the digest would silently disable every identity-family
tree with a nonzero cap, because three gates test **exact equality** against the
cap-0 identity:

- `validate_request` (`engine/src/gto/nseat_trainer.cpp:870-873`) admits a tree
  only if `tree.action_id() == identity_action_id()` or
  `is_declared_coarse_action_id(...)`;
- `engine/src/gto/solve.cpp:26-29` and `:82-85` do the same.

`is_declared_coarse_action_id` matches by **name** only
(`engine/src/abstraction/abstraction.cpp:160-162`), so coarse trees survive. But
an identity tree built with `identity(1)` keeps the name
`rfc0007-pot-fractions` and gains `|max-raises=1`: it is neither equal to the
cap-0 id nor coarse.

Two consequences are reachable today:

1. `resolve_flop` builds `identity()` with cap 1
   (`engine/src/resolver/subgame_resolver.cpp:504-505`) and calls `train_nseat`
   (`:652`), whose `train_impl` calls `validate_request`
   (`engine/src/gto/nseat_trainer.cpp:929`). The throw is caught as
   `std::exception` and converted to `SubgameResolveStatus::InvalidInput`, so
   **every RFC 0025 flop-rooted depth-limited resolve silently stops working**.
2. The flop-library builder at `--raise-cap 1` — the configuration recorded by
   the published `flop-library-6way-fullrange` manifest — throws and fails the
   build.

The design therefore adds an **identity-family predicate** and replaces the
three exact-equality tests with it:

```cpp
// True for any id whose name is the identity family's, regardless of the cap.
bool is_identity_action_family(const AbstractionId& id);
```

A differing **schedule** under the identity name must still be refused — that is
the existing intent, recorded at `engine/src/gto/nseat_trainer.cpp:868-869` — so
the predicate matches the family *and* the canonical schedule, and admits only a
differing cap. This is the first place the new parameter parser becomes
load-bearing, and it is the highest-risk item in the change.

### Tree-shape witness

The sealed checkpoint records a 64-bit FNV-1a witness over the materialized
tree, so a resume can prove the shape it is about to warm-start matches.

BSWARM01 gains version 2. `kSealedCheckpointHeaderBytes` goes from 184 to 192:

```text
  off  field              type   notes
  120  algorithm_revision char[64]  (unchanged, ends at 184)
  184  tree_shape_digest  u64       new; header_bytes = 192
```

The existing `static_assert` at
`engine/src/artifacts/sealed_checkpoint.cpp:83` ties the header constant to the
last field and is updated with it. The witness is computed as:

```text
h = FNV-1a-64 offset basis
for node_index in 0 .. tree.size()-1, ascending:
    h = fnv(h, u32 node_index)
    h = fnv(h, u8 node kind enum value)
    h = fnv(h, u32 node.actions.size())
    for action in node.actions, in order:
        h = fnv(h, u8 action type)
        h = fnv(h, u64 action target total)
```

Every byte is little-endian, matching the rest of the format. Non-action nodes
contribute their index, kind, and a zero action count, so a node-kind change
also moves the witness.

On resume the builder recomputes the witness for its own tree and requires
equality **in `train_one_class`, before `state.rows = ckpt.rows`
(`engine/src/flop_library/flop_library.cpp:372`)** — not later inside
`validate_initial`, which would already have accepted rows:

```text
checkpoint tree shape does not match the current tree
(expected <hex>, found <hex>); refusing resume
```

A cap difference is one instance of a shape mismatch. The witness is **strictly
stronger for row validity** than a cap field — it also catches a sizing or
stack change that alters any action target or the tree shape, and a node-kind
change — and it never rejects a resume whose rows are valid, including two
different caps that happen to produce identical trees, which is the correct
outcome.

The checkpoint extension is a persisted-format change: a version-1 file is
refused and its class retrains, which under RFC 0028's rules is the normal
discard path. No `.bswarm` file exists at the time of writing.

### Mid-street roots

`GameDef` gains `raises_this_street`, `GameState(const GameDef&)` assigns it to
`raises_this_street_` (`engine/src/poker/game_definition.cpp:294+`; accessor at
`engine/include/bs/game_definition.hpp:240`), and `build_subgame_def_from_node` seeds
it from the source node
(`engine/src/resolver/subgame_resolver.cpp:340-377`). Without the constructor
assignment the new field would be inert.

**It is not persisted, and this is deliberate.** A prior draft also added
`insert_game_v2`/`read_game_v2` and `validate_game_v2` to the scope. Three
problems make *persistence* wrong:

1. `kSchemaDdlV3` reuses `kSchemaDdlV2[1]` for the `game` table verbatim
   (`engine/src/artifacts/strategy_artifact.cpp:861-873`) and
   `verify_schema_objects` requires the stored CREATE text to be byte-identical
   (`:2396-2425`). Adding a `game` column without a new schema set would make
   every existing v2 and v3 artifact fail with `InvalidSchema`; doing it
   properly needs `kArtifactSchemaVersionV4` with its own DDL, table set, and
   `open_validated` branch.
2. There is no consumer. `SubgameResolveResult` carries no `GameDef`, and
   nothing in `engine/src/resolver` or the host writes a policy artifact — every
   artifact-producing path is street-start. A persisted field with no reader is
   a speculative surface.
3. It would be incoherent alone: `same_game_def` already compares
   `street_committed`, `raise_rights`, and `last_full_raise`
   (`engine/src/poker/game_definition.cpp:283-286`) and none of those is
   persisted, so a mid-street definition already cannot round-trip. Persisting
   one of the four does not make it round-trip.

The count therefore lives in memory for the duration of a resolve. If a future
proposal needs it persisted, that proposal takes the v4 path and names its
consumer — it is not smuggled in here.

**`same_game_def` does compare it — a separate decision from persistence.** That
predicate has four consumers (`nseat_trainer.cpp:1143`,
`root_value_extractor.cpp:185`, `continuation_range.cpp:133`,
`seat_policy.cpp:192`) and is documented as "the ONE
root/game identity predicate ... Compares every field that changes the game"
(`engine/include/bs/game_definition.hpp:172-178`), and this field changes legal
actions — it gates the cap at `engine/src/tree/abstract_tree.cpp:154-156`.
`NSeatPolicy::lookup` returns null unless
`poker::same_game_def(game_, tree.def())`
(`engine/src/gto/nseat_trainer.cpp:1143`), so without the comparison a count-0
policy could match a count-2 tree, which is the same class of defect this RFC
exists to close. Comparison is identity-only: no persisted surface moves, and
`validate(GameDef)` adds the cross-field rule that matters: a nonzero count
requires a mid-street root — some `street_committed > 0` — mirroring the
existing mid-street convention at `engine/src/poker/game_definition.cpp:108-160`.
A nonzero count with every `street_committed == 0` would be a street-start root
that the cap gate (`engine/src/tree/abstract_tree.cpp:154-156`) treats as
already capped, suppressing betting illegally. (A plain `0..255` bound is
vacuous for a `uint8_t`.)

## Dependency Rules

- `bs::abstraction` owns the cap and its identity; no consumer re-derives one.
- The sealed codec gains the witness; only the builder computes it.
- No service, protocol, or resident-serving change.

## Compatibility and Migration

- **Every artifact is invalidated once**, in Rollout stage 1, even if no cap
  value changes, because the digest moves. This is stated here rather than
  buried: the correctness fix has an unavoidable rebuild cost.
- **BSWARM01 goes to version 2**; version-1 checkpoints are refused and their
  classes retrain.
- **`.sections/build.json` already carries the cap** and needs no change.
- **No compatibility shim.** Per the no-legacy rule there is no dual-cap
  reader and no feature flag.
- **Documentation**: `docs/design/gto-engine.md` gains the cap and witness, and
  the comment at `engine/include/bs/flop_library.hpp:59-62` — which currently
  states the cap is not part of the digest — is corrected in the same change.

## Security and Operational Impact

- Offline for the build; the resident serve path is unaffected.
- The witness is an integrity check, not a security boundary: FNV-1a is not
  collision-resistant, and a crafted checkpoint could match a wrong tree. The
  threat model is operator error, not an adversary writing into the build
  directory.
- Observability: a rejected resume names both digests.

## Alternatives Considered

1. **Add a cap field to the checkpoint header only.** Catches the cap case and
   nothing else, at the same cost. The witness subsumes it.
2. **Add a mandatory cap field to the manifest only.** The manifest is not read
   on resume. Rejected.
3. **Keep the post-construction setter and re-mint the digest on set.**
   Preserves the API but leaves the lossy path in place: any caller that forgets
   to re-mint silently produces a cap-less identity. Rejected.
4. **Bundle a cap increase (1 → 2).** Rejected for this RFC: it forces a full
   retrain, and the evidence that a cap-2 identity depth-limited configuration
   is resolvable at five and six seats is not recorded anywhere. A follow-up
   RFC must measure that before proposing the change.

## Risks

1. **The rebuild invalidates every artifact for a correctness fix.** *Mitigation:*
   the rebuild is reproducible from pinned parameters, and the RFC states the
   cost up front rather than presenting the change as free.
2. **The constructor change is far larger than the setter list.** Five is the
   setter count. Making `max_raises` mandatory on `ActionAbstraction(...)`,
   `identity(...)`, and `declared(...)` would touch **88** `identity()` and
   **30** `declared(...)` call sites plus **22** `identity_action_id`
   references, across tests, benchmarks, and production headers
   (`flop_library.hpp:148,163` use `identity()` as default arguments;
   `stage6/trainer.hpp:97`, `stage6/candidate_policy.hpp:53`,
   `postflop_config.cpp:50`, `solve.hpp:193`). *Resolution:* `max_raises`
   **keeps a default of `0`** on all three entry points, so existing call sites
   compile unchanged and represent "no cap"; only the five producers that shape
   a strategy pass a value. The digest still distinguishes them, because the
   default is part of the hashed parameter line.
3. **The witness is computed differently in two places.** *Mitigation:* one
   function, one test that recomputes it for a known tree and compares to a
   stored constant.
4. **The mid-street count stays in memory only.** A reloaded artifact loses it,
   which is already true of the other three mid-street fields. *Mitigation:*
   the limitation is stated, and persistence would need its own schema version
   and a named consumer.

## Verification Plan

- **Identity.** Two abstractions differing only in cap produce different
  digests; a constructor-minted id round-trips the cap through
  `reconstruct_action_abstraction`.
- **Witness.** A stored-constant test on a known tree; changing one node's
  action target, one node's kind, or the tree size each moves the digest.
- **Refusal.** A checkpoint written for a smaller tree is refused by name.
  The `.sections`-absent sequence from Motivation is the regression test: it
  previously resumed stale rows, and must now refuse once.
- **Mid-street root.** A subgame rebuilt mid-street reports the same legal
  action count as the live state.
- **Project gates**: full Engine Build Contract, ASan, and a `grep` for
  `set_max_raises_per_street` over `engine/`, `apps/`, `bin/`, and `tools/`
  returning zero hits, with historical RFC text exempt (see AC2).

## Rollout Plan

1. **Identity minting.** Constructor parameter, canonical `max-raises=` token,
   remove the setter, update the two digest pins, rebuild the shipping
   artifacts. Gate: identity tests pass and no `set_max_raises_per_street`
   reference remains.
2. **Tree-shape witness.** BSWARM01 version 2, header 192, the hash function
   and its stored-constant test. Gate: refusal test passes.
3. **Mid-street roots.** `GameDef` field, the `GameState` constructor
   assignment, and the seeding in `build_subgame_def_from_node`; no schema
   change. Gate: the mid-street test passes and no artifact pin moves.

Stop conditions: any existing number changing without an explanation halts the
rollout; any artifact rebuild that does not reproduce from pinned parameters
halts it.

## Rollback Plan

- Each stage is a source change. Reverting stage 2 restores version-1
  checkpoints; reverting stage 1 restores the old digest and the current
  artifacts, which are retained until the rollout completes.
- No in-place artifact migration.

## Open Questions

1. **Should the two menu paths be unified?** `abstract_actions`
   (`counterfactual_reach.cpp:172`, `heads_up_solver.cpp:294,423,523`) is a
   distinct entry point from `ActionAbstraction::menu`, used where a
   `HeadsUpState` with no raise count is in hand. A prior draft proposed
   deleting it; that is wrong, because it is not redundant with
   `build_action_menu`. Carrying a cap into the heads-up route needs either an
   abstraction plus a count on `HeadsUpGame`/`HeadsUpState` or a `GameState`
   bridge — a separate change with its own fixtures. Not attempted here.
2. **None remaining on persistence.** Decided: the count stays in memory. It
   has no artifact consumer today, and persisting it would require a schema
   version plus the other three mid-street fields to be coherent. A future
   proposal that needs it takes that path and names its consumer.

## Acceptance Criteria

- **AC1**: The raise cap is part of the action-abstraction digest; a
  cap-differing pair has differing digests, and
  `reconstruct_action_abstraction` recovers the cap from a persisted identity.
  The test asserts the **real** case, not only a synthetic round trip: a
  blueprint built with `--raise-cap 1` reconstructs cap 1, where today it
  returns cap 0.
- **AC2**: An identity-family tree built with a nonzero cap is admitted:
  `train_nseat` trains it, and `resolve_flop` still returns a solved candidate
  rather than `SubgameResolveStatus::InvalidInput`. A custom **schedule** under
  the identity name is still refused. A `--raise-cap 1` flop-library build
  completes.
- **AC3**: `set_max_raises_per_street` no longer exists, and a grep over
  `engine/`, `apps/`, `bin/`, and `tools/` returns zero hits. Historical RFC
  text is exempt: `docs/rfcs/0025-off-tree-decision-handling.md:238` quotes the
  call and accepted RFCs are immutable, the same exemption RFC 0028 AC5 took.
- **AC4**: A checkpoint written for a different tree shape is refused by name;
  the `.sections`-absent sequence that previously resumed stale rows now
  produces exactly one refusal.
- **AC5**: The tree-shape witness matches a stored constant for a known tree,
  and changing a node's action target, a node's kind, or the tree size each
  changes it.
- **AC6**: A mid-street subgame root reports the same legal action count as the
  live state; `validate` rejects a nonzero count on a street-start root; and no
  artifact schema, DDL, or round-trip pin moves.
- **AC7**: The full Engine Build Contract passes, ASan is clean,
  `check-rfcs` / `check-docs` pass, `docs/design/gto-engine.md` documents the
  cap and the witness, and the `flop_library.hpp:59-62` comment no longer
  claims the cap is excluded from the digest.

## Decision

Pending independent approval-agent review.
