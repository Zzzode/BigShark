---
rfc: "0030"
subject: "Pin Strategy-Shaping Abstraction Parameters in Persisted Identity"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-25"
owners: "engine abstraction, engine flop library, engine solver"
supersedes: ""
superseded-by: ""
---

# RFC 0030: Pin Strategy-Shaping Abstraction Parameters in Persisted Identity

## Summary

Two independent identity defects survive in the current tree:

1. The per-street raise cap shapes the strategy but participates in no
   minter's digest, so a persisted strategy identity (`preflop-profile-6way`)
   whose parameters carry no cap is reconstructed at cap 0 by the live subgame
   resolver — the eager resident path serves turn/river continuation menus one
   raise wider than the artifact was trained at.
2. Sealed warm-start rows are bound to tree nodes positionally. The checkpoint
   proves its revision, digests, node count and bucket count, but never the
   tree shape, so a row can be adopted onto a node that merely happens to sit
   at the same ordinal with the same action menu.

This RFC puts the cap into the action-abstraction digest (G1) and adds a
whole-tree shape witness to the sealed checkpoint under a new `BSWARM02`
magic (G2). It changes no cap value: raising the cap is a separate, measured
proposal.

## Motivation

**The cap is absent from the digest.** `make_id` hashes only the canonical
sizing schedule (`engine/src/abstraction/abstraction.cpp:110-117`);
`make_declared_id` appends only a cover-seeds token (`:119-131`).
`AbstractionId` carries `name`, `version`, `parameters`, `digest`
(`engine/include/bs/abstraction.hpp:95-110`) and no typed cap. Every producer
applies the cap *after* construction through `set_max_raises_per_street`
(`engine/include/bs/abstraction.hpp:238`), at five call sites:
`engine/benchmarks/flop_library_builder.cpp:192`,
`engine/benchmarks/preflop_profile_builder.cpp:207`,
`engine/src/resolver/subgame_resolver.cpp:505`,
`engine/benchmarks/subgame_nash_conv.cpp:1166,1195`. One non-production
caller, `engine/tests/test_flop_library.cpp:222`, is edited by the same change
(AC3's grep enforces it). The value never reaches the minter.
`engine/include/bs/flop_library.hpp` records the exclusion as deliberate in
its build-identity comment.

### Defect A — a capped artifact is reconstructed uncapped (live serving)

The eager resident path populates the full persisted id, name/version/
parameters/digest, from the stored artifact
(`engine/src/resident/resident_policy.cpp:576-578` and `:655-657`).
`reconstruct_action_abstraction` (`engine/src/resolver/subgame_resolver.cpp:112-117`)
rebuilds the abstraction from the persisted sizes alone, so the recovered cap
is always 0. Byte-verified against the shipped artifact:
`artifacts/preflop-profile-6way/preflop-profile.db` stores
`rfc0007-pot-fractions|1|422c245239c7a527` with exactly 176 tokenless
parameter bytes, while its `manifest.json` records
`max_raises_per_street: 1`. Its tree was built capped; the only live consumer
of the persisted id cannot know that; every turn/river continuation served
from it is allowed one raise too many.

Two secondary gaps belong to the same defect:

- **Direct minters.** Any code constructing an `ActionAbstraction` and
  forgetting the setter mints an identity that cannot record the omission.
- **The packed flop library stores no parameters at all.** Its header carries
  only the action digest u64, so its cap is unrecoverable regardless of G1;
  that is the separate Known Gap below, not something this RFC silently fixes.

What G1 does **not** need to fix is the builder warm-start sequence an earlier
revision led with: "delete `.sections`, keep the checkpoint, change the cap".
RFC 0033 (commit `bcfb942`, 2026-10-24) closed it before this RFC is
implemented — `preflight_resume` disposition 5 hard-refuses whenever reusable
checkpoint state survives without `.sections/build.json`
(`engine/src/flop_library/flop_library.cpp:837-846`, invoked from the serial
path and the parallel parent). With `build.json` present,
`require_identity_match` already compares `max_raises_per_street`
(`:641-643`) alongside stack, contribution, seat count, blind, digests,
range, seed, and iteration/class counts. The cap-change axis on the builder
path is therefore guarded today; the G1 work is the live reconstruction
defect plus identity correctness at the minter.

### Defect B — a row lands on a different node without any identity failing

The checkpoint keys rows by `(own_card_bucket, node_index)`
(`engine/include/bs/nseat_trainer.hpp:100-106`), and `node_index` is the node's
position in the builder's expansion order:
`node.index = tree_.nodes_.size()` (`engine/src/tree/abstract_tree.cpp:345`).
A stored row identifies a node only by its ordinal.

`validate_initial` (`engine/src/gto/nseat_trainer.cpp:905-918`) binds an
initial row whenever the ordinal is in range, the node is an action node, and
the stored action list equals the node's menu element for element —
`poker::Action::operator==` is defaulted over `type` and `target_total`
(`engine/include/bs/heads_up.hpp:25-30`). It has no other notion of which node
a row belongs to. Any direct `train_nseat(..., &initial)` /
`train_one_class` caller — tests today, any future consumer — silently warms
the wrong node after an ordinal shift that preserves menus, which is the
common case: check-through action nodes on the same board carry identical
menus.

A second reachable instance needs no unusual caller: **tree-code drift with
`build.json` present**. `node_cap` is bounds-checked but never shape-checked
(`engine/src/artifacts/sealed_checkpoint.cpp:178,553`). A change to tree
expansion in a later binary that leaves schedule, cap, both digests, the
algorithm revision, and every `BuildIdentity` scalar identical is invisible
to every existing guard, and checkpoint rows land on shifted ordinals.

The failure is not "resume is refused with a misleading error"; it is "resume
succeeds and silently trains the wrong node". Only a whole-tree witness
answers it: a proof that the tree is byte-identical, hence ordinal *k* means
the same node it meant when the checkpoint was written.

## Goals

- **G1**: The raise cap participates in the action-abstraction digest, so a
  cap-differing pair has differing identities, and a persisted identity that
  carries parameters yields its true cap at the live reconstruction site.
- **G2**: A checkpoint whose tree shape differs from the current tree is
  hard-refused by name, before the checkpoint's `rows` or
  `completed_iterations` are adopted.
- **G3**: The cap is recoverable from a persisted identity **that carries
  parameters**, without assuming a value. A persisted identity that carries
  only a digest is out of this RFC's reach and is named as a known gap rather
  than claimed fixed — see "The packed-library gap" below.

## Non-Goals

- **No cap value change.** The production cap stays one. Raising it forces a
  full retrain, changes every published artifact's digest, and — by the
  measurements a follow-up must take — may make five- and six-seat live
  resolving infeasible. That decision needs its own evidence.
- **No card-abstraction change.** Separate proposal (RFC 0031).
- **No turn/river strategy work.** Separate proposal (RFC 0032).
- **No menu-derivation unification.** See Open Question 1: the two paths are
  not redundant, and merging them is a separate change with its own risk.

## Current State and Evidence

### Facts (verified in this repository)

- **The digest ignores the cap.** Verified at
  `abstraction.cpp:110-131`; the golden value `0x422c245239c7a527` is pinned
  at `engine/tests/test_abstraction.cpp:214` and `:556` only. The other pinned
  lines in that file are card-bucket cardinality and flop-class merge pins and
  do not move.
- **The builder warm-start path already guards the cap.**
  `BuildIdentity::max_raises_per_street` is written (`flop_library.cpp:518`)
  and compared (`:641-643`); RFC 0033's disposition 5
  (`:837-846`) closes the build.json-absent sequence. This RFC does not
  duplicate either guard.
- **The checkpoint does not require a matching tree.** The digest checks are
  at `engine/src/flop_library/flop_library.cpp:341-347`; `node_cap`/
  `bucket_cap` are sealed at `:414-415` and enforced only as bounds in the
  codec (`sealed_checkpoint.cpp:178,553`).
- **`resolve_flop` mints an identity locally.** It constructs
  `ActionAbstraction::identity()` and sets the cap to one
  (`engine/src/resolver/subgame_resolver.cpp:504-505`), so it has no persisted
  abstraction to read a cap from. `resolve_uniform` leaves the cap at the
  identity's zero (`:442`), and `reconstruct_action_abstraction` rebuilds from
  `sizes` only (`:112-117`).
- **The cap counts opening bets.** `Bet` and `Raise` share one `case`
  (`engine/src/poker/game_definition.cpp`), so "N raises" means "N full bets
  or raises per street".
- **A mid-street subgame root resets the counter.** `GameDef` carries no count
  and `build_subgame_def_from_node`
  (`engine/src/resolver/subgame_resolver.cpp:340-377`) does not seed one, so
  `GameState::raises_this_street_` keeps its default zero and the cap gate
  (`engine/src/tree/abstract_tree.cpp:154-156`) permits one raise too many on
  a continued street.
- **Two menu paths exist but are not redundant.** `build_action_menu` is the
  body of `ActionAbstraction::menu` (`engine/src/abstraction/abstraction.cpp:242`)
  and the cap-aware `abstract_node_menu` calls it
  (`engine/src/tree/abstract_tree.cpp:170-176`). `abstract_actions` is a
  distinct entry point with production callers
  (`engine/src/resolver/counterfactual_reach.cpp:176`;
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
  that cannot prove it matches the current tree is refused whole and
  preserved; the operator, not the trainer, decides to discard it.
- **Change one thing.** This RFC fixes identity and resume validation. It does
  not change a strategy value.

## Proposed Design

### Identity-minting contract

`max_raises_per_street` becomes a constructor-level parameter of
`ActionAbstraction`, not a post-construction setter. Both minters and both
public constructors take it:

```cpp
// The cap is std::uint8_t throughout, matching the existing setter and member
// (engine/include/bs/abstraction.hpp:238); the RFC does not widen it.
AbstractionId make_id(const SizeSchedule& schedule, std::uint8_t max_raises = 0);
AbstractionId make_declared_id(const SizeSchedule& schedule,
                               abstraction::CoverSeeds seeds,
                               std::uint8_t max_raises = 0);

explicit ActionAbstraction(SizeSchedule schedule, std::uint8_t max_raises = 0);
static ActionAbstraction identity(std::uint8_t max_raises = 0);
static ActionAbstraction declared(SizeSchedule schedule, CoverSeeds seeds,
                                  std::uint8_t max_raises = 0);
```

The parameter line gains a canonical token appended **after** the existing
content, because the digest is order-sensitive and `make_declared_id`
currently appends `cover-seeds=` last (`engine/src/abstraction/abstraction.cpp:125-128`):

| Minter | Final bytes |
| --- | --- |
| `make_id` | `size-v1\n` `street=0 ...\n` … `street=3 ...\n` `\|max-raises=<N>` |
| `make_declared_id` | the same schedule, then `cover-seeds=declared-only\n` or `cover-seeds=min-and-cap\n`, then `\|max-raises=<N>` |

`<N>` is the decimal spelling of the `std::uint8_t`, no padding, no sign.
**It must be formatted as an integer, not streamed as a character:**
`out << std::uint8_t(1)` emits code point 1. Cast to `unsigned` or use
`std::to_string`, and pin the result in a golden test. The token carries no
trailing newline; the existing `\n` after `cover-seeds=` is untouched. With
cap 0 the token is still appended (`|max-raises=0`): omitting it at zero would
create two spellings for the default. The pre-change line, which has no token
at all, is never minted again.

`reconstruct_action_abstraction`
(`engine/src/resolver/subgame_resolver.cpp:112-117`) parses the trailing token
back out instead of assuming a value, so an artifact whose persisted identity
carries parameters recovers its cap.

**The packed-library gap.** This does *not* fix the shipped flop library.
`artifacts/flop-library-6way-fullrange/library.bin` stores only
`action_abstraction_digest u64` in its header; the name, version, and
parameter line are not persisted (BSFLIB02 header in
`engine/include/bs/mmap_library.hpp:31-34`), and `MmapLibrary` never assigns
`UnifiedGame::action_id`, leaving the empty default
(`engine/include/bs/unified_game.hpp:40`). Turn/river continuation menus
served from the packed library therefore remain uncapped after this RFC.
Closing that needs the parameter line in the packed header — a format change
with its own compatibility surface, recorded as Open Question 3, not folded
in silently.

`set_max_raises_per_street` is **removed** and the five producer sites pass
the cap into the constructor. Because `max_raises` keeps a `0` default, the
roughly 103 `identity()`, 34 `declared(...)`, and 23 `identity_action_id`
occurrences measured at HEAD across `engine/` and `apps/` compile unchanged;
only the strategy-shaping producers pass a value. `identity_action_id()`
continues to mean the cap-0 identity, and a tree built with a nonzero cap
deliberately hashes differently — that is the point.

The golden digest `0x422c245239c7a527` moves. It is pinned in
`engine/tests/test_abstraction.cpp` and embedded in local artifact directories
(`artifacts/` is gitignored); stage 3 rebuilds the artifacts in use.

### The admissibility gates

Making the cap part of the digest would silently reject every identity-family
tree with a nonzero cap, because three gates test exact equality against the
cap-0 identity: `validate_request`
(`engine/src/gto/nseat_trainer.cpp:870-873`, call site `:929`) and
`engine/src/gto/solve.cpp:26-29,82-85`. The design replaces them with an
**identity-family predicate** that admits the family name, the canonical
schedule, and a differing cap — a differing schedule is still refused — and
recomputes the digest:

```cpp
bool is_identity_action_family(const AbstractionId& id);
```

Two parser contracts are stated separately:

| Call site | Contract |
| --- | --- |
| `validate_request`, `solve.cpp` gates | **Strict.** Family, version, canonical schedule, parsed cap, and recomputed digest must all agree; anything else is refused. This contract applies to BOTH the identity family and the declared-coarse family — today coarse trees are admitted by name only (`is_declared_coarse_action_id`), and the new cap/recomputed-digest requirement must not be implemented for identity names alone. |
| `reconstruct_action_abstraction` (`subgame_resolver.cpp:112-117`) | **Lenient, and only here.** A recognized family parses its cap; the **empty** id (packed-library path) and an **unrecognized** name/version keep today's fallback `ActionAbstraction(sizes)` at cap 0, because that is the named packed gap whose consequence is an uncapped menu; a recognized family whose line is **tokenless or malformed is refused**, because the cap is then an unknown value, not 0. |

The distinction is deliberate: the empty id is a *known absence*; a tokenless
line is an *unknown value*. The concrete tokenless artifact is
`artifacts/preflop-profile-6way/preflop-profile.db` (`rfc0007-pot-fractions|1|…`
with 176 parameter bytes, manifest cap 1); reading it as 0 hands the resolver
an uncapped menu for a capped artifact, which is defect A. It is removed by
rebuilding (stage 3), not by guessing.

The strict predicate fails closed on: no token, empty/trailing-only value,
value above 255 or non-numeric, non-canonical spelling (`01`, `+1`, ` 1`),
token not last, more than one token, non-canonical schedule, unknown
name/version, or a digest that does not recompute. Each case gets a negative
test. The coarse branch of `reconstruct_action_abstraction` parses the token
as well, so a future `--coarse --raise-cap 1` artifact cannot silently return
cap 0; no coarse artifact ships today.

### Tree-shape witness (BSWARM02)

The sealed checkpoint records a 64-bit FNV-1a witness over the materialized
tree, so a resume proves the shape it is about to warm-start matches.

Per the no-legacy directive and the BSFLIB01→BSFLIB02 precedent, the magic
changes **`BSWARM01` → `BSWARM02`** and the u32 version field at offset 8 is
spelled **2**, giving one canonical on-disk form (magic plus version matching
the BSFLIB02 convention). There is no version negotiation and no old codec:
the decoder has exactly one accepted magic/version pair, and an old `BSWARM01`
file is indistinguishable from any other foreign/torn file at decode time.
`kSealedCheckpointHeaderBytes` goes from 184 to 192:

```text
  off  field              type   notes
  120  algorithm_revision char[64]  (unchanged, ends at 184)
  184  tree_shape_digest  u64       new; header_bytes = 192
```

The existing `static_assert` at `engine/src/artifacts/sealed_checkpoint.cpp:83`
is updated with the constant. Witness input:

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

All bytes little-endian. Non-action nodes contribute index, kind, and a zero
action count, so a node-kind change also moves the witness.

**Coverage limit, stated exactly.** The witness hashes index, kind, and menu;
it does not hash stack, pot, or settlements. When every declared pot fraction
jams onto the all-in cap (the small-stack clamp regime), a pot-only change can
leave node count, kinds, and menus all identical, so rows trained at one pot
would still resume at another. The builder's current defaults are
`starting_stack = 6, contribution = 2` (four chips behind, SPR 1;
`engine/benchmarks/flop_library_builder.cpp:65-67`). At the shipped scale
(starting stack 200) fraction targets vary with the pot, so the live artifact
is covered; the clamp regime is disclosed as a pre-existing witness-coverage
limit, not a regression. The implementation records the measured root menus at
the current defaults via a named builder probe (command and output in the
implementation plan) rather than pinning hand-derived numbers, and the
guarantee is worded as "rejects any difference that moves an ordinal or a
menu", never "rejects any difference".

**Enforcement and disposition.** On resume the builder recomputes the witness
for its own tree and requires equality **in `train_one_class`, before
`state.rows = ckpt.rows`** (the adoption point at
`engine/src/flop_library/flop_library.cpp` around `:347-360`):

```text
checkpoint tree shape does not match the current tree
(expected <hex>, found <hex>); refusing resume; remove the stale
checkpoint or resume a build with matching parameters
```

There is exactly one disposition table, shared by the serial and parallel
paths (RFC 0033 owns the ordering, this RFC adds one row):

| State | Disposition |
| --- | --- |
| torn / short / bad magic (including every legacy `BSWARM01` file) / unreadable | discard the file and retrain (preflight corruption disposition; silent, warning only) |
| incompatible algorithm revision | **hard refuse**, files preserved |
| missing `.sections/build.json` with reusable state | **hard refuse** (RFC 0033 disposition 5) |
| action- or card-abstraction digest mismatch | **hard refuse**, files preserved |
| **tree-shape witness mismatch** | **hard refuse, files preserved** (new) |
| empty training rows | **hard refuse** |
| stale-but-complete checkpoint | classify disposable without removing; `train_one_class` drops and retrains |

Check ordering is fixed: the witness (and digest/revision) refusals are
evaluated **before** the stale-but-complete branch, so a stale-complete
checkpoint whose witness mismatches hard-refuses rather than being dropped —
the mismatch is the drift signal this RFC exists to surface.

Witness mismatch is a hard refusal, not an automatic discard-and-retrain: it
signals that the checkpoint and the requested build disagree about the game,
which is operator error or binary drift, and silently burning the retrain
would hide that. This matches the existing incompatible-revision contract
(`test_incompatible_resume_preserves_files`). After the named stale file is
removed, or when the build is started without `--resume`, the next run
succeeds; the refusal message itself states that remediation. The witness
check is sound, not complete: it may over-reject a subtree change that holds
no trained rows (accepted retrain cost); it cannot accept a mismatched tree
except under the documented FNV-1a 64-bit collision threat.

### Mid-street roots

`GameDef` gains `raises_this_street`; the `GameState(const GameDef&)`
constructor assigns it, `build_subgame_def_from_node`
(`engine/src/resolver/subgame_resolver.cpp:340-377`) seeds it from the source
node, and `same_game_def` compares it — `NSeatPolicy::lookup` already gates on
that predicate (`nseat_trainer.cpp:1143`), so a count-0 policy cannot match a
count-2 tree. `validate(GameDef)` adds the cross-field rule that a nonzero
count requires a mid-street root (`some street_committed > 0`). The count is
intentionally in-memory only: persisting it requires an artifact schema
version, has no consumer (resolver results carry no `GameDef`), and would be
incoherent without the other three already-unpersisted mid-street fields.
This work ships in the same stage as the identity change, because until the
counter is seeded the reconstructed cap is correct but the cap gate still
allows one extra raise.

## Dependency Rules

- `bs::abstraction` owns the cap and its identity; no consumer re-derives one.
- The sealed codec gains the witness; only the builder computes it.
- No service, wire-protocol, or resident-serving interface change.

## Compatibility and Migration

- **Every built artifact is invalidated once** in stage 3, because the digest
  token moves and the checkpoint magic changes. Rebuild, never transform; no
  compatibility codec survives.
- **Measurement-baseline coordination.** RFC 0031's pinned library has already
  moved once (RFC 0033, 2026-10-24): the current artifact is
  `sha256 b5a480af97518d0493583312d2dda467fa01f98f547ac904099f206d4ba453aa`,
  188,411,904 bytes, root `stacks = 198 x6`; the superseded
  `b67e3cb3db65827be97325d2913e40200956c16da604922d4dc9c190e49b20b4` is
  preserved outside the repository. This RFC's rebuild invalidates
  `b5a480af…453aa` in turn; stage 3 preserves the pre-change binary outside
  the repository and records the new digest, and RFC 0031's Stage 3
  measurement names whichever artifact it used. RFC 0029's in-flight resident
  measurement fixture uses the action digest in its manifest and is likewise
  re-based; the stage-6 fixture builds its own small library, so its NashConv
  figure does not move. RFC 0028's header-arithmetic evidence
  (`184 + 2,808×28 + 5,688×19 = 186,880`) is re-stated for the 192-byte
  header in its Implemented record; no test pins the arithmetic.
- **`BSWARM01` checkpoints are discarded as corrupt and retrain** through the
  existing magic-failure path; there is no v1 reader by design.
- **`.sections/build.json` already carries the cap** and needs no change.
- **The packed flop library stays cap-0 at turn/river continuation** even
  after its stage-3 rebuild: the rebuild gives it a new digest, not a cap.
  Readers must not read "rebuilt" as "fixed" (Open Question 3).
- **No compatibility shim, no feature flag.**
- **Documentation**: `docs/design/gto-engine.md` gains the cap, witness, and
  disposition row; the build-identity comment in
  `engine/include/bs/flop_library.hpp` stating the cap is excluded from the
  digest is corrected in the same change.

## Security and Operational Impact

Offline for the build; the resident serve interface is unchanged (one serving
defect is corrected: capped artifacts no longer reconstruct uncapped). The
witness is an integrity check against operator error or binary drift, not a
security boundary; FNV-1a is not collision-resistant. Observability: every
refusal names the field and, for the witness, both hashes and the
remediation.

## Alternatives Considered

1. **Add a cap field to the checkpoint header only.** Catches the cap case and
   nothing else at the same cost; the witness subsumes it.
2. **Add a mandatory cap field to the manifest only.** The manifest is not read
   on resume. Rejected.
3. **Keep the post-construction setter and re-mint the digest on set.**
   Forgetting the setter mints a cap-less identity silently — the defect.
   Rejected.
4. **Witness mismatch discards-and-retrains automatically.** Rejected: an
   identity disagreement is evidence of drift or operator error; the project's
   existing incompatible-revision contract preserves files and hard-refuses,
   and an automatic full retrain would hide the signal.
5. **Keep magic `BSWARM01`, bump the version field.** Rejected: the
   BSFLIB01→BSFLIB02 precedent renamed the magic and deleted the old codec;
   under the no-legacy rule a version branch that exists only to reject is
   dead code, and a new magic makes old files fall into the single
   corruption-discard path naturally.
6. **Bundle a cap increase (1 → 2).** Rejected: it forces a full retrain and
   the feasibility of cap-2 depth-limited resolving at five/six seats is
   unmeasured.

## Risks

1. **The rebuild invalidates every artifact for a correctness fix.** The
   rebuild is reproducible from pinned parameters; stage 3 preserves the prior
   binaries outside the repository.
2. **Default-0 constructor keeps wide call surface unchanged.** Roughly 103
   `identity()`, 34 `declared(...)`, and 23 `identity_action_id` occurrences
   compile unchanged and keep meaning cap 0; only the five strategy-shaping
   producers change. One non-producer is load-bearing: the
   `preflop_nash_conv` reconstruction benchmark
   (`engine/benchmarks/preflop_nash_conv.cpp:29-64`) mints candidate ids and
   asserts equality with a persisted digest, so it must pass the recovered
   cap when run against a capped artifact and must **refuse** a tokenless
   input with a message naming the manifest key rather than reporting a
   NashConv against the wrong (cap-0) tree. Its default cap-0 input keeps
   working.
3. **The witness is computed differently in two places.** Mitigated by one
   shared function and a stored-constant test over a known tree.
4. **Tokenless artifact window.** Between stages 1 and 3 the resolver refuses
   the existing tokenless `preflop-profile-6way` by name; that is a visible,
   correct failure, and stage 3 closes it by rebuild. The packed-library gap
   (empty id) keeps its documented cap-0 fallback throughout.
5. **The mid-street count stays in memory only.** A reloaded artifact loses
   it, as it does the other three mid-street fields; persistence needs its own
   schema version and a named consumer.

## Verification Plan

- **Identity.** Two abstractions differing only in cap produce different
  digests; a resolve integration test round-trips the cap through
  `reconstruct_action_abstraction` for a parameter-carrying SQLite policy
  (built with `--raise-cap 1`), asserts cap 1 is recovered where today it is
  0, asserts the **gap remains** for an `MmapLibrary`-backed game (empty id →
  cap 0 fallback), and asserts tokenless/malformed lines refuse.
- **Admissibility.** `train_nseat` trains an identity-family cap-1 tree and
  `resolve_flop` returns a solved candidate; a custom schedule under the
  identity name is refused; a `--raise-cap 1` flop-library build completes.
- **Witness.** Stored-constant test on a known tree; changing one action
  target, one node kind, or the tree size each moves the digest; the
  disposition test uses a genuine small build, a present valid `build.json`,
  and a checkpoint whose witness field is tampered (digests and rows intact),
  asserting the hard refusal, file preservation, and the remediation message.
- **Mechanism regression (defect B).** A direct
  `train_nseat(..., &shifted_state)` call accepts rows shifted onto same-menu
  nodes, documenting the positional-binding hole the witness exists to close
  at the consumption site.
- **Mid-street root.** A rebuilt mid-street subgame reports the live legal
  action count; `validate` rejects a nonzero count at a street-start root; no
  artifact schema, DDL, or round-trip pin moves.
- **Project gates**: full Engine Build Contract, ASan over the codec/resolver
  surfaces, and a grep for `set_max_raises_per_street` over `engine/`,
  `apps/`, `bin/`, and `tools/` returning zero hits (historical RFC text
  exempt).

## Rollout Plan

1. **Identity and serving correctness (one commit).** Constructor parameter,
   canonical token, setter removal, strict predicate and the three gate
   replacements, the lenient reconstruction contract, the `preflop_nash_conv`
   recovered-cap change, **and** the mid-street counter (`GameDef`,
   constructor, seeding, `same_game_def`, validation rule). These ship
   together: token-without-counter would reconstruct the right cap but still
   allow the extra raise. Gate: identity/admissibility/mid-street tests pass,
   `resolve_flop` solves a capped fixture, no setter reference remains.
2. **Tree-shape witness.** `BSWARM02` magic, 192-byte header, the shared hash,
   the new disposition row, and its tests. Gate: tampered-witness refusal and
   stored-constant tests pass; legacy `BSWARM01` files discard-and-retrain
   through the corruption path.
3. **Rebuild and local audit.** Preserve current binaries outside the
   repository; rebuild the artifacts in use with explicit tokens; run the
   local pre-publication audit (AC9) opening every `.db` under `artifacts/`;
   record digests and add the RFC 0028/0029/0031 errata. Gate: audit output
   recorded; the resolver serves the rebuilt 6-way profile at cap 1; the
   packed library's rebuilt digest is named with its continued cap-0
   continuation gap.

Stop conditions: any existing measured number moving without an explanation;
any rebuilt artifact not reproducing from pinned parameters; any current
serving path newly returning a fold-shaped fallback on a state it previously
solved.

## Rollback Plan

- Each stage is a source change. Reverting stage 2 restores the previous codec
  and its checkpoints; reverting stage 1 restores the old digest; the
  pre-stage-3 binaries are retained outside the repository until the rollout
  completes. Checkpoints written as `BSWARM02` while stage 2 is in place are,
  after a revert, discarded-and-retrained by the restored BSWARM01 decoder
  through the corruption path (expected, source rollback only).
- No in-place artifact migration.

## Open Questions

1. **Should the two menu paths be unified?** `abstract_actions`
   (`counterfactual_reach.cpp:176`, `heads_up_solver.cpp:294,423,523`) is a
   distinct entry point from `ActionAbstraction::menu`; carrying a cap into
   the heads-up route needs an abstraction plus a count on
   `HeadsUpGame`/`HeadsUpState` or a `GameState` bridge. Separate change.
2. **None on the mid-street count's persistence.** Decided in-memory; a
   future proposal that needs it takes the schema-version path and names its
   consumer.
3. **Should the packed library persist the action-abstraction parameters?**
   `library.bin` stores a digest and no parameter line, so its cap is not
   recoverable and its turn/river continuation menus stay uncapped. Fixing it
   means growing the BSFLIB02 header (or a sidecar), changes the artifact
   SHA-256 RFC 0031 pins, and is a separate RFC.

## Acceptance Criteria

- **AC1**: The cap is part of the action-abstraction digest; cap-differing
  pairs have differing digests; a SQLite v2/v3 policy built with
  `--raise-cap 1` reconstructs cap 1 through `reconstruct_action_abstraction`
  (integration test, since the function is private) where today it yields 0;
  an `MmapLibrary`-backed game still reconstructs cap 0 (the gap is asserted,
  not hidden); a tokenless or malformed recognized-family line refuses.
- **AC2**: A cap-1 identity-family tree is admitted by `train_nseat` and
  `resolve_flop`; a custom schedule under the identity name is refused; a
  `--raise-cap 1` flop-library build completes.
- **AC3**: `set_max_raises_per_street` does not exist; a grep over
  `engine/`, `apps/`, `bin/`, and `tools/` returns zero hits outside
  historical RFC text.
- **AC4**: The witness enforces tree identity at the row-adoption site:
  1. a direct `train_nseat(..., &shifted_state)` call accepts rows shifted
     onto same-menu nodes (the hole the witness closes, demonstrated at API
     level);
  2. with a present, matching `build.json`, a checkpoint whose
     `tree_shape_digest` is tampered while digests, revision, and rows remain
     intact is hard-refused in `train_one_class` before rows are adopted, the
     checkpoint file is preserved, and the message names both hashes and the
     remediation;
  3. after the named stale checkpoint is removed, a follow-up build succeeds;
  4. a legacy `BSWARM01` file is discarded as corrupt and retrains through the
     preflight corruption disposition, not through any old codec.
- **AC5**: The witness matches a stored constant for a known tree, and
  changing an action target, a node kind, or the tree size each moves it; the
  clamp-regime coverage limit is recorded with measured root menus at the
  builder's current defaults (`starting_stack 6, contribution 2`).
- **AC6**: A mid-street subgame root reports the live legal action count;
  `validate` rejects a nonzero count on a street-start root; no artifact
  schema, DDL, or round-trip pin moves.
- **AC7**: The full Engine Build Contract passes, ASan is clean,
  `check-rfcs` / `check-docs` pass, `docs/design/gto-engine.md` documents
  cap, witness, and the disposition row, and the build-identity comment no
  longer claims the cap is excluded.
- **AC8**: Every strict-parser failure case has a negative test (missing
  token, empty value, above-255, non-numeric, non-canonical spelling,
  non-final, duplicated, wrong version, digest mismatch); the lenient branch
  asserts empty-id and unrecognized-name fallback to cap 0 and tokenless
  refusal.
- **AC9**: Stage 3's local pre-publication audit reports every `.db` under
  `artifacts/` carrying explicit `|max-raises=` tokens, and fails on a
  tokenless parameter line; the recorded output is stage evidence. This is a
  local operator gate, never a repository test (`artifacts/` is gitignored).

## Implementation Record

Status: Implemented (2026-10-25). Stages 1-2 (identity token minting, the
strict identity/coarse predicates, the mid-street raise counter, and the
BSWARM02 tree-shape witness) landed on 2026-10-24 with resolver/codec/warm
start tests. Stage 3 completed on 2026-10-25:

- Every service artifact was rebuilt pack-direct with the explicit
  `|max-raises=<N>` identity token under the correct identity family
  (`rfc0007-pot-fractions`; an initial rebuild mislabelled as the coarse
  family was discarded): `ac1-16w` (2-seat, 160 classes, cap 0 — retired
  2026-10-26 as the narrow early library; no consumer mounted it and the
  full-range six-way library is authoritative; the record here is the
  historical Stage-3 build fact),
  `preflop-profile` (2-seat, cap 0), `preflop-profile-3way` (3-seat 10 BB,
  cap 0; needed raised trainer caps `--max-infosets 2000000
  --train-bytes 8589934592` because Preflop169 retains ~1.58M information
  sets on the three-seat identity tree — this artifact was retired on
  2026-10-26 by RFC 0037's legacy-retirement amendment; the record here
  is the historical Stage-3 build fact), and `preflop-profile-6way`
  (6-seat, cap 1), all at their pinned iteration counts with termination
  Complete.
- The packed libraries are the final BSFLIB03 single form after RFC 0035;
  the six-way full-range library (1,755 classes, 100% flops, 188,432,384
  bytes, cap 1, 198-chip-behind 12-chip limped root) mounts with matching
  SHA-256 sidecars and passes the bundled reachability test. At Stage-3
  time ac1-16w mounted the same way (historical fact; that narrow early
  library was retired 2026-10-26). The tokenless predecessors were kept
  under `~/.bigshark-backups/` at the time; that backup tree was deleted
  on 2026-10-26 by maintainer direction.
- AC9 local audit: `python3 -I tools/artifact-audit/ac9-token-audit.py
  artifacts` reports 5 pass / 0 fail / 3 skip (the three preflop profile
  manifests delegate to their tokenized `.db` rows).
- The RFC 0029 `preflop-profile-validation` artifact is a measurement
  fixture whose `.nsp` policy generation is not pinned; it stays in the
  backup and is regenerated with the T-09 replay program rather than
  guessed.
- Final verification: release ctest 79/79, benchmark-multistreet three
  quality gates PASS, npm check 124/124, proto:check, replay, docs/RFC
  checks; ASan subset over the codec/resolver/checkpoint surface 10/10
  on the final merged source (full ASan suite 77/77 with the
  sanitizer-only measured-policy watchdog).

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0030-approval-1 (independent approval agent, auto_model/alwaysday1_max)
Decision date: 2026-10-24
Review outcome: Approved
Reviewed scope: Full RFC across two review rounds, verified against HEAD
  (commit bcfb942), the deployed flop library and preflop-profile-6way
  artifact (digests and the 176 tokenless parameter bytes re-checked),
  engine abstraction, flop_library, nseat trainer, sealed checkpoint codec,
  resolver, resident, host, and their tests.
Review summary: Round 1 returned Changes Requested with three blockers (stale
  resume-defect narrative after RFC 0033 disposition 5; an AC4 setup
  intercepted by that disposition; and a refuse-vs-discard contradiction) plus
  two majors and three minors. All were resolved in the round-2 revision:
  Defect A restated as the live tokenless-cap reconstruction defect, Defect B
  as API-level positional binding plus same-identity binary drift; one
  disposition table (corruption including BSWARM01 discards and retrains;
  digest/witness/revision mismatches hard-refuse with files preserved);
  BSWARM02 magic/version with no legacy codec; clamp evidence re-based on the
  current 6/2 defaults; mid-street counter merged into rollout stage 1;
  citations and call-site counts re-verified. Round 2 approved with two
  required editorial corrections (a garbled hybrid sha256 exemplar and three
  stale line numbers) and four recommended nits (test setter site named,
  BSWARM02 version field pinned to 2, the strict digest/cap contract applied
  to the coarse family as well, witness-before-stale ordering and a rollback
  clarification), all applied in the status-flip edit. Approval covers design
  only; it authorizes no spending, live play, or training.
```
