---
rfc: "0030"
subject: "Pin Strategy-Shaping Abstraction Parameters in Persisted Identity"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-21"
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

**Two defects, and the digest closes only the first.** They must not be
conflated, because a prior draft conflated them and so justified the witness
with a sequence its own G1 closes.

*Defect A: a cap change is not an identity change.* A normal resume is already
protected on this axis: `.sections/build.json` records
`max_raises_per_street` and `require_identity_match` refuses a mismatch
(`engine/src/flop_library/flop_library.cpp:662-664`). The gap is the path where
that file is absent. `build_library` removes only `.sections` when `build.json`
is missing (`:922`), while the checkpoint lives in `out_dir` (`:352`). So:
delete `.sections`, keep the checkpoint, change the cap, and the builder writes
a fresh identity from the new cap and resumes the old checkpoint's rows.

**G1 closes defect A by itself.** Once the cap is in the digest, that same
sequence refuses at `flop_library.cpp:366-368` on the
`ckpt.abstraction_digest != params.action.id().digest` check, before any
witness is consulted. The witness cannot be justified by this sequence, and AC4
was written against it; both are corrected below.

*Defect B: a row lands on a different node.* This is the case G1 cannot reach,
and two earlier drafts described it wrongly — first as a cap change G1 already
closes, then as a silent resume that `validate_initial` in fact catches. The
real mechanism is an **index shift**.

The checkpoint keys rows by `(own_card_bucket, node_index)`
(`engine/include/bs/nseat_trainer.hpp:100-106`), and `node_index` is nothing but
the node's position in the builder's expansion order:
`node.index = tree_.nodes_.size()` (`engine/src/tree/abstract_tree.cpp:345`).
A stored row therefore identifies a node only by its ordinal.

`validate_initial` (`engine/src/gto/nseat_trainer.cpp:905-921`) checks that the
ordinal is in range, that the node is an action node, and that the stored action
list equals the node's menu element for element — and `poker::Action::operator==`
is defaulted over `type` and `target_total`
(`engine/include/bs/heads_up.hpp:25-30`). It has no other notion of which node a
row belongs to.

Now change the stack or the contribution with `.sections` deleted. Every digest
still matches, because neither field enters `AbstractionId` — `BuildIdentity`
records them (`engine/src/flop_library/flop_library.cpp:506-507`) but only in
`build.json`, which this path deletes (`:922`). The change moves some node's
menu, so the expansion inserts or removes a child earlier in the order, and every
later ordinal shifts by one. A stored row at ordinal *k* now names a different
node. `validate_initial` accepts it whenever that different node happens to carry
the same menu — which is the common case, not a contrived one: any two
check-through action nodes on the same board have identical menus, so a shift
re-associates rows freely among them.

So the failure is not "resume is refused with a misleading error"; it is
"resume succeeds and silently trains the wrong node". The `validate_initial`
guard catches only the shifts that also change the menu at the shifted ordinal,
which is a subset. Two drafts missed this because they asked whether the
sequence resumes and whether it throws, and never asked *which node a row
lands on*.

That is what a whole-tree witness buys, and it is the only thing that buys it: a
proof that the tree is byte-identical, hence ordinal *k* means the same node it
meant when the checkpoint was written.

## Goals

- **G1**: The raise cap participates in the action-abstraction digest, so a
  cap-differing pair has differing identities.
- **G2**: A checkpoint whose tree shape differs from the current tree is
  refused by name, before any trainer state is built.
- **G3**: The cap is recoverable from a persisted identity **that carries
  parameters**, without assuming a value. A persisted identity that carries only
  a digest is out of this RFC's reach and is named as a known gap rather than
  claimed fixed — see "The packed-library gap" below.

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
appends `cover-seeds=` last (`engine/src/abstraction/abstraction.cpp:125-128`).

The digest is byte-sensitive — `abstraction_digest` hashes the parameter string
as-is — so the exact bytes must be pinned or two implementations mint different
digests from the same configuration. `canonical_schedule` terminates **every**
line, including the last street's, with `'\n'`
(`engine/src/abstraction/abstraction.cpp:55-68`), and `make_declared_id`
appends `cover-seeds=<value>\n`, newline included (`:126-127`). The two minters
therefore end as:

| Minter | Final bytes |
| --- | --- |
| `make_id` | `size-v1\n` `street=0 ...\n` … `street=3 ...\n` `|max-raises=<N>` |
| `make_declared_id` | the same schedule, then `cover-seeds=declared-only\n` or `cover-seeds=min-and-cap\n`, then `|max-raises=<N>` |

`<N>` is the decimal spelling of the `std::uint8_t`, no padding, no sign.
**It must be formatted as an integer, not streamed as a character.**
`canonical_schedule` and `make_declared_id` build their strings with
`std::ostringstream` (`engine/src/abstraction/abstraction.cpp:55-68,125-128`),
and `out << std::uint8_t(1)` emits code point 1, not `"1"`. An implementer who
appends with `out << max_raises` and one who uses `std::to_string` would mint
different digests from the same configuration. Cast to `unsigned` (or use
`std::to_string`) explicitly, and pin the result in a golden test.

The new token carries **no** trailing newline: it is the last thing in the string,
so a trailing newline would be a second, silent spelling of the same cap. The
existing `\n` after `cover-seeds=` is left exactly as it is — changing it would
move the declared digest for a reason unrelated to the cap. `cover-seeds=`
keeps its current spelling with **no** separator; only the new token is
`|`-prefixed.

With cap 0 the token is still appended (`|max-raises=0`). Omitting it at zero
would create two spellings for the default and make the "no token" row of the
parser table ambiguous, so zero is spelled explicitly and the pre-change line —
which has no token at all — is accepted only by the parser, never minted.

`reconstruct_action_abstraction`
(`engine/src/resolver/subgame_resolver.cpp:112-117`) parses the trailing token
back out instead of assuming a value, so an artifact whose persisted identity
carries parameters recovers its cap.

**The packed-library gap.** This does *not* fix the shipped artifact, and a
prior draft claimed it did. `artifacts/flop-library-6way-fullrange/library.bin`
stores only `action_abstraction_digest u64` in its header
(`engine/src/resident/mmap_library.cpp:1158`); the name, version, and parameter
line are not persisted (`engine/include/bs/mmap_library.hpp:31-34`), and
`MmapLibrary` never assigns `UnifiedGame::action_id`, leaving the empty default
(`engine/include/bs/unified_game.hpp:40`). `resident_policy.cpp:497` copies that
game into the record, and `subgame_resolver.cpp:600-601` passes the empty id to
`reconstruct_action_abstraction`, which falls through to
`ActionAbstraction(sizes)` at `:116` and mints cap 0. Turn/river continuation
menus served from the packed library are therefore still uncapped after this
RFC.

Closing that gap needs the parameter line in the packed header, which is a
format change with its own compatibility surface and is **not** proposed here.
It is recorded as a Known Gap and an Open Question rather than folded in
silently.

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

**Two parsers, two contracts, and a prior draft conflated them.** An earlier
revision gave `reconstruct_action_abstraction` the same strict predicate as the
admissibility gates, which contradicts its own packed-library gap: an
`MmapLibrary`-backed game carries an **empty** `AbstractionId`
(`engine/include/bs/unified_game.hpp:40`, copied at
`engine/src/resident/resident_policy.cpp:497`, passed at
`engine/src/resolver/subgame_resolver.cpp:600-601`). A strict predicate rejects
the empty id, so it could not "fall through to `ActionAbstraction(sizes)` and
mint cap 0" as AC1 requires, and the throw would reach `resolvingDecision` —
which is `noexcept` with a whole-body catch (`apps/engine-host/main.cpp:172,374`)
— degrading every shipped turn/river subgame to a miss.

The two contracts are therefore stated separately and neither is implicit:

| Call site | Contract |
| --- | --- |
| `validate_request` (`nseat_trainer.cpp:870-873`), `solve.cpp:26-29,82-85` | **Strict.** Family, version, schedule, cap, and recomputed digest must all agree; anything else is refused. This is where a wrong admission trains a wrong tree. |
| `reconstruct_action_abstraction` (`subgame_resolver.cpp:112-117`) | **Lenient, and only here.** A recognized family parses its cap; an unrecognized or empty id keeps today's fallback — `ActionAbstraction(sizes)`, cap 0 — because it is the documented packed-library gap (below), whose consequence is an uncapped menu, not a wrong tree. |

The lenient branch is a **named gap with a named consequence**, not a silent
default: it is the packed path, it is recorded as Open Question 3, and AC1
asserts its behaviour rather than pretending it away.

**The lenient fallback accepts the empty id and nothing else.** A prior revision
said "unrecognized or empty", which is ambiguous in the one way that matters: a
tokenless line has a *recognized* name, version, and schedule with a valid
recomputable digest, so it is neither unrecognized nor malformed, and an
implementer could route it through the fallback. That would reconstruct cap 0
for `artifacts/preflop-profile-6way` — the exact artifact whose manifest says 1
— at the one production call site (`engine/src/resolver/subgame_resolver.cpp:601`).
It is reachable: `ResidentBlueprintSource::game()` returns the record's game
(`engine/src/resident/resident_policy.cpp:1159`) and the eager SQLite path
populates `record.game.action_id` from the manifest (`:624-626`).

| Input | Strict gates | Lenient reconstruction |
| --- | --- | --- |
| empty id (packed) | n/a — never matched | **fallback, cap 0** (the packed gap) |
| recognized family, explicit token | admit if all rows pass | parse and forward the cap |
| recognized family, **tokenless** | reject | **reject** — cap unknown, fail closed |
| recognized family, malformed token | reject | reject |
| unrecognized name/version | reject | fallback, cap 0 |

The empty id is a *known absence* (the packed header stores no parameters, so
there is nothing to read). A tokenless line is an *unknown value* (parameters
exist but omit the cap). Conflating them is what made the earlier revision
silently mint 0. Because stage 1 removes every tokenless artifact from the
shipped set, rejecting here costs nothing and closes the hole.

The strict predicate **fails closed** on anything it cannot parse. The parameter line
is order-sensitive and deterministic (`engine/src/abstraction/abstraction.cpp:110-131`),
so the cases are enumerable and each must reject rather than fall back to cap 0:

| Parameter line | Result |
| --- | --- |
| no `|max-raises=` token | **legacy, cap unknown** — see below |
| `|max-raises=` with an empty or trailing-only value | reject |
| a value above 255, non-numeric, or with trailing characters | reject |
| a value whose canonical decimal spelling differs (`01`, `+1`, ` 1`) | reject |
| the token not last | reject |
| more than one `|max-raises=` token | reject |
| identity name with a schedule that does not match the canonical one | reject (existing rule) |
| a name/version pair other than `rfc0007-pot-fractions` v1, or `rfc0008-declared-coarse` v1 | reject |

**A tokenless line is information loss, not cap 0, and reading it as 0 is the
defect this RFC exists to close.** A prior revision accepted it as "the literal
pre-change line", which is true as bytes and false as evidence: the line proves
nothing about the cap that shaped the tree. This is concrete, not hypothetical
— `artifacts/preflop-profile-6way/preflop-profile.db` stores
`rfc0007-pot-fractions|1|422c245239c7a527` with 176 bytes of tokenless
parameters (verified against the file), while its JSON manifest records
`max_raises_per_street: 1`. Its tree was built capped; its identity cannot say
so; the digest recomputes correctly from the old line and every new check
passes. Reading it as 0 would hand the subgame resolver an uncapped menu for a
capped artifact.

The RFC therefore treats tokenless as **legacy, with the cap unknown**, and
resolves it by rebuild rather than by guessing:

- The parser reports "no token" as a distinct outcome, not as cap 0. The three
  strict gates reject it, because a tree whose cap is unknown cannot be
  certified admissible.
- Stage 1 **rebuilds every artifact in use**, so the rebuilt set carries
  explicit tokens and no strict path ever sees a tokenless line again. The set,
  enumerated rather than summarized, with the cap each manifest records
  (`ABSENT` meaning 0): `flop-library-6way-fullrange` (1), `preflop-profile`
  (0), `preflop-profile-6way` (1), `preflop-profile-3way` (0), and `ac1-16w`
  (0). `preflop-profile-validation` and `preflop-profile-postflop` are
  validation and experimental outputs referenced by no document or code path
  (verified by grep) and are deleted rather than rebuilt.
- **The rebuild cannot be a CI gate, and the RFC does not pretend otherwise.**
  `artifacts/` is entirely gitignored (`.gitignore:88`), so no test can assert
  anything about it from a clean checkout. The check is therefore a local
  pre-publication step: a tool that opens every `.db` under `artifacts/` and
  fails on a tokenless `abstraction_parameters`, run by the operator as part of
  stage 1 and recorded in the stage's evidence. AC9 states it as a local gate
  with recorded output, not as a repository test, because a repository test
  would pass vacuously on any machine without artifacts.

`AbstractionId::operator==` includes `version`
(`engine/include/bs/abstraction.hpp:103-106`), so a predicate that matched name
and schedule but ignored version would admit an id equality says is different.
The non-canonical-numerics row exists for the same reason: `01` and `1` parse to
the same integer, so an ordinary `stoi` would silently collapse two distinct
parameter lines onto one cap.

**The coarse branch must parse too, and a prior draft left it out.**
`reconstruct_action_abstraction` has two branches
(`engine/src/resolver/subgame_resolver.cpp:112-117`): the identity branch and
`ActionAbstraction::declared(sizes, CoverSeeds::DeclaredOnly)` for a coarse id.
Once `make_declared_id` also carries the token, a coarse artifact built with
`--coarse --raise-cap 1`
(`engine/benchmarks/flop_library_builder.cpp:163-188`;
`engine/benchmarks/preflop_profile_builder.cpp:203-207`) mints
`|max-raises=1`, and a coarse branch that does not forward it returns cap 0 —
uncapped menus, the exact defect this RFC exists to close. Both branches take
the parsed cap. No coarse artifact ships today (the published manifest names
`rfc0007-pot-fractions`), so this is a correctness requirement for a supported
builder option rather than a live serving bug, but it is not deferred: the
fail-closed promise is about the parser, and a branch that never calls it is the
same silent cap 0 by a different route.

**The digest must agree with the parameters.** A persisted id read from an
artifact carries name, version, parameters, and digest as independent values
(`engine/src/artifacts/strategy_artifact.cpp:2579-2580`;
`engine/src/resident/resident_policy.cpp:545-547,624-626`). The predicate
recomputes `abstraction_digest(name, version, parameters)` and rejects a
mismatch, mirroring the check the codebase already applies to the card identity
(`engine/src/artifacts/strategy_artifact.cpp:2088,2878`). Without it a
hand-edited parameter line could claim a cap the digest does not cover.

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

**Terminal payloads are deliberately omitted, and the limit of that choice must
be stated precisely.** The witness hashes node index, kind, and menu; it does not
hash stack or pot settlements. A prior revision argued that a stack or
contribution change always reaches the hash through a menu first, because
`cover_total` and the pot-fraction targets derive from the opponent's stack and
the pot (`engine/src/abstraction/abstraction.cpp:210,245`). That holds **at the
shipping scale** and fails at the builder's own defaults.

The builder defaults to `stack = 4`, `contribution = 2`
(`engine/benchmarks/flop_library_builder.cpp:65-66`), with `kDefaultBigBlind = 2`
(`engine/src/flop_library/flop_library.cpp:48`). At that scale every declared
fraction clamps onto the all-in cap, so the menus stop varying:

| contribution | pot | root menu |
| --- | --- | --- |
| 2 | 4 | `[2, 3, 4]` |
| 3 | 6 | `[2, 4]` |
| 5 | 10 | `[2, 4]` |
| 6 | 12 | `[2, 4]` |

From contribution 3 onward the root menu is identical while the pot differs.
At the shipped scale (stack 200) a 2 → 3 change does move menus
(`[2,3,6,200]` vs `[2,5,9,200]`), so the shipped configuration is covered.

Consequence, stated rather than argued away: in the clamp regime a pot-only
change is invisible to both `validate_initial` and the witness — node count,
kind, and menus are all unchanged — so rows trained at one pot could be reused at
another. This is a **pre-existing property of the witness's coverage, not a
regression**, and it is why the guarantee is worded as "rejects any difference
that moves an ordinal or a menu", never "rejects any difference". A caller that
varies a terminal-only field, or that builds at small stacks, must extend the
hash; this RFC does not do that and does not claim it.

On resume the builder recomputes the witness for its own tree and requires
equality **in `train_one_class`, before `state.rows = ckpt.rows`
(`engine/src/flop_library/flop_library.cpp:372`)** — not later inside
`validate_initial`, which would already have accepted rows:

```text
checkpoint tree shape does not match the current tree
(expected <hex>, found <hex>); refusing resume
```

The witness is a **conservative whole-tree equality check**, and that is a
different thing from a per-row validity check. It is worth being precise,
because a prior draft claimed a no-false-positive guarantee the design does not
have:

- **It rejects every difference that changes the hash.** Any change that moves
  a node's ordinal or menu moves the witness. A cap difference is one instance;
  a sizing, stack, contribution, or node-kind change are others. It cannot miss
  *modulo a collision*: FNV-1a is non-cryptographic (Assumptions), so "cannot
  miss" means "cannot miss without a collision" — the same threat model stated
  under Security.
- **It can over-reject, and that is accepted.** Validity is per stored row;
  identity is per tree. A change confined to a subtree that holds no stored rows
  leaves every row valid and still moves the witness, so the class retrains
  instead of resuming. The cost is one class's training time, and the retrain is
  repeatable — the stale checkpoint is deleted first, so the next run starts
  clean rather than aborting again. A check that never over-rejected would have
  to compare rows
  positionally against a tree it cannot yet name as the same tree — which is the
  defect.
- **Two different caps that produce identical trees resume, correctly.** The
  witness sees one shape, and rows trained under one cap are valid under the
  other because the tree — and therefore every node and menu — is the same. This
  is the one case where "different configuration" and "valid resume" coexist,
  and the witness gets it right by construction rather than by exemption.

The property the RFC claims is therefore *soundness*, not *completeness*: never
accepts a mismatched tree, may reject a matched row set.

The checkpoint extension is a persisted-format change. A version-1 file is
**discarded, not refused**: `preflight_resume` catches the version failure as
corruption, prints "discarding corrupt checkpoint", deletes the file, and
retrains the class (`engine/src/flop_library/flop_library.cpp:241-251`; the
parent path does the same at
`engine/benchmarks/flop_library_builder.cpp:294-305`). The outcome matches the
intent — the class retrains — but a prior draft called it a refusal, which is a
different code path with a different operator-visible message. This is distinct
from the witness and digest mismatches, which discard the checkpoint and retrain
the class (see below). No `.bswarm` file exists at the time of writing.

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
   `street_committed` and `raise_rights` (`engine/src/poker/game_definition.cpp:285`)
   and `last_full_raise` (`:275`), and none of those is persisted, so a
   mid-street definition already cannot round-trip. Persisting one of the four
   does not make it round-trip.

The count therefore lives in memory for the duration of a resolve. If a future
proposal needs it persisted, that proposal takes the v4 path and names its
consumer — it is not smuggled in here.

**`same_game_def` does compare it — a separate decision from persistence.** That
predicate has six production consumers (`nseat_trainer.cpp:1143`,
`root_value_extractor.cpp:24,73,185`, `continuation_range.cpp:133`,
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
- **This invalidates an accepted RFC's measurement baseline, and that is
  recorded rather than discovered.** RFC 0031 (Accepted) pins
  `artifacts/flop-library-6way-fullrange/library.bin` sha256
  `b67e3cb3…20b4` as its measurement baseline
  (`docs/rfcs/0031-measure-card-abstraction-error.md:42,303,491,517`). The
  packed header embeds the action digest at offset 208
  (`engine/src/resident/mmap_library.cpp:1158`), so stage 1's rebuild changes
  that byte string and 0031's pinned baseline no longer resolves. 0031's
  measurement must be re-taken against whatever library this RFC leaves in
  place, or against a copy of the pre-change binary preserved for it. RFC 0028's
  AC1 evidence — `186,880 = 184 + 2,808×28 + 5,688×19` — also becomes stale at a
  192-byte header and takes an errata note in its Implemented record.

> **Erratum (2026-10-24).** The baseline moved before this RFC was implemented,
> for a different reason: RFC 0033 corrected the flop library's stored root
> ledger and retrained the six-way artifact. The current library is
> `sha256 b5a480af97518d0493583312d2dda467fa01f98f547ac904099f206d4ba453aa`,
> 188,411,904 bytes, root `stacks = 198 x6` / `contributions = 2 x6`. The
> superseded `b67e3cb3…20b4` artifact is preserved outside the repository. Any
> measurement baseline this RFC pins must now name the newer digest, and the
> rebuild this RFC's Rollout stage 1 anticipates will invalidate that one in
> turn.

- **BSWARM01 goes to version 2**; version-1 checkpoints are discarded as
  corrupt and their classes retrain, as `preflight_resume` does for any
  unreadable header (`engine/src/flop_library/flop_library.cpp:241-251`).
- **`.sections/build.json` already carries the cap** and needs no change.
- **The shipped six-way library stays cap-0 at turn/river continuation, even
  after its stage-1 rebuild.** The packed header stores a digest and no
  parameter line, so nothing in the RFC changes what `MmapLibrary` can recover;
  the rebuild gives it a new digest, not a cap. Readers must not treat "rebuilt"
  as "fixed". Closing it needs the parameters in the header (Open Question 3).
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
   setter count, counting a file with two calls as one site. Making
   `max_raises` mandatory on `ActionAbstraction(...)`, `identity(...)`, and
   `declared(...)` would touch **88** `identity()` and **31** `declared(...)`
   call sites plus **24** lines naming `identity_action_id`, across tests,
   benchmarks, and production headers (`flop_library.hpp:148,163` use
   `identity()` as default arguments; `stage6/trainer.hpp:97`,
   `stage6/candidate_policy.hpp:53`, `postflop_config.cpp:50`,
   `solve.hpp:193`). *Resolution:* `max_raises` **keeps a default of `0`** on
   all three entry points, so existing call sites compile unchanged and
   represent "no cap"; only the five producers that shape a strategy pass a
   value. The digest still distinguishes them, because the default is part of
   the hashed parameter line.

   The default does **not** make every consumer behavior-neutral, and a prior
   draft implied it did. `engine/benchmarks/preflop_nash_conv.cpp:56-64`
   reconstructs an identity by trying three mints against a persisted digest and
   then asserting equality. After this RFC a persisted id carrying
   `|max-raises=1` matches none of the three mints and the benchmark throws. The
   benchmark's *default* invocation stays cap 0 and keeps working: its default
   input `artifacts/preflop-profile/preflop-profile.db` (`:35`) has a manifest
   with no `max_raises_per_street` key at all. The break requires passing
   `artifacts/preflop-profile-6way`, whose manifest records
   `max_raises_per_street: 1` (`:29`), as an explicit argument. That is a
   supported invocation and it is the one that must keep working, so the call
   site changes to pass the recovered cap; it is added to the producer list and
   to stage 1. For a **rebuilt** artifact the cap is in the token, so passing
   it is straightforward. For a pre-change tokenless artifact there is nothing
   to recover from the database; the sibling JSON manifest has the value but the
   benchmark reads only the `.db`. The benchmark therefore rejects a tokenless
   input with a message naming the missing token and the manifest key, rather
   than silently running at cap 0 — a tokenless run would build a cap-0 tree
   and report a NashConv for a tree the artifact was not trained on, which is
   worse than failing. **Intentional consequence:** the benchmark cannot run
   against any pre-rebuild artifact until stage 1 has rebuilt it. That is the
   point of the gate, and it is stated here so it is not discovered mid-stage. No other load path recomputes the action digest
   (`strategy_artifact.cpp` recomputes only the card digest at `:2088` and
   `:2878`), so the "inert records" claim for stale artifacts otherwise holds.
3. **The witness is computed differently in two places.** *Mitigation:* one
   function, one test that recomputes it for a known tree and compares to a
   stored constant.
4. **The mid-street count stays in memory only.** A reloaded artifact loses it,
   which is already true of the other three mid-street fields. *Mitigation:*
   the limitation is stated, and persistence would need its own schema version
   and a named consumer.

## Verification Plan

- **Identity.** Two abstractions differing only in cap produce different
  digests; a `resolve()` integration test round-trips the cap through
  `reconstruct_action_abstraction` for a parameter-carrying policy, and asserts
  it does *not* round-trip for an `MmapLibrary`-backed game (the known gap).
  The function is private (`engine/include/bs/subgame_resolver.hpp:171`), so no
  direct unit test is possible without a new seam, which this RFC does not add.
- **Witness.** A stored-constant test on a known tree; changing one node's
  action target, one node's kind, or the tree size each moves the digest.
- **Refusal.** A checkpoint written for a different tree is refused by name
  before `state.rows` is assigned. The regression test is defect B from
  Motivation — `.sections` deleted, checkpoint kept, stack or contribution
  changed — with an input chosen so that some stored row's ordinal shifts onto a
  node carrying the *same* menu, which is the case `validate_initial` accepts and
  the witness catches. The test demonstrates the silent association by calling
  `train_nseat(..., &shifted_state)` directly — which exercises
  `validate_initial` and needs no builder flag — and then shows the builder
  discarding the checkpoint instead of resuming. The same sequence with a
  changed **cap** must be caught earlier, at the abstraction digest, confirming
  the two mechanisms cover disjoint cases. A separate case asserts the accepted
  over-rejection: a change confined to an untrained node retrains although every
  stored row is valid.
- **Mid-street root.** A subgame rebuilt mid-street reports the same legal
  action count as the live state.
- **Project gates**: full Engine Build Contract, ASan, and a `grep` for
  `set_max_raises_per_street` over `engine/`, `apps/`, `bin/`, and `tools/`
  returning zero hits, with historical RFC text exempt (see AC2).

## Rollout Plan

1. **Identity minting.** Constructor parameter, canonical `max-raises=` token,
   remove the setter, the fail-closed parser, the identity-family predicate, and
   the three gate replacements. Update `preflop_nash_conv.cpp:56-64` to pass the
   recovered cap. Update the two digest pins, preserve a copy of the current
   `library.bin` for RFC 0031's re-measurement, and rebuild the shipping
   artifacts. Gate: identity tests pass; `resolve_flop` still returns a solved
   candidate; `preflop_nash_conv` runs against the 6way artifact; no
   `set_max_raises_per_street` reference remains.
2. **Tree-shape witness.** BSWARM01 version 2, header 192, the hash function
   and its stored-constant test. Gate: the defect-B test passes — the direct
   `train_nseat` call accepts the shifted rows, the builder discards the
   checkpoint and retrains, and a second `--resume` run succeeds — and the
   cap-change variant is caught earlier at the abstraction digest.
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
2. **None remaining on the mid-street count's persistence.** Decided: the count
   stays in memory. It has no artifact consumer today, and persisting it would
   require a schema version plus the other three mid-street fields to be
   coherent. A future proposal that needs it takes that path and names its
   consumer.
3. **Should the packed library persist the action-abstraction parameters?**
   `library.bin` stores a digest and no parameter line, so its cap is not
   recoverable and its turn/river continuation menus stay uncapped. Fixing it
   means growing the BSFLIB02 header and bumping the format, which changes the
   artifact SHA-256 that RFC 0031 pins. Out of scope here; a separate RFC should
   take it, and should decide whether the parameters belong in the header or in
   a sidecar manifest.

## Acceptance Criteria

- **AC1**: The raise cap is part of the action-abstraction digest; a
  cap-differing pair has differing digests, and
  `reconstruct_action_abstraction` recovers the cap from a persisted identity
  **that carries parameters**. The test asserts the real case, not only a
  synthetic round trip: a SQLite v2/v3 policy built with `--raise-cap 1` — the
  path that populates `record.game.action_id` at `resident_policy.cpp:545-547`
  and `:624-626` — reconstructs cap 1, where today it returns cap 0. The test
  also asserts the **gap remains**: an `MmapLibrary`-backed game reconstructs
  cap 0, so the RFC cannot be read as having fixed the packed path. And it
  asserts the tokenless case is **refused**, not silently read as 0: a
  parameter line with a recognized name, version, schedule, and digest but no
  `|max-raises=` token throws rather than returning cap 0. This is an
  `NSeatPolicy`/`resolve()` integration test, because
  `reconstruct_action_abstraction` is private
  (`engine/include/bs/subgame_resolver.hpp:171`).
- **AC2**: An identity-family tree built with a nonzero cap is admitted:
  `train_nseat` trains it, and `resolve_flop` still returns a solved candidate
  rather than `SubgameResolveStatus::InvalidInput`. A custom **schedule** under
  the identity name is still refused. A `--raise-cap 1` flop-library build
  completes.
- **AC3**: `set_max_raises_per_street` no longer exists, and a grep over
  `engine/`, `apps/`, `bin/`, and `tools/` returns zero hits. Historical RFC
  text is exempt: `docs/rfcs/0025-off-tree-decision-handling.md:238` quotes the
  call and accepted RFCs are immutable, the same exemption RFC 0028 AC5 took.
- **AC4**: A checkpoint whose rows would land on the wrong nodes is refused by
  name, through **defect B**, which G1 does not cover. Setup: `.sections`
  deleted, checkpoint retained, stack or contribution changed by an amount that
  shifts ordinal *k* onto a node carrying the same menu. The test asserts four
  things.

  1. A direct `train_nseat(..., &shifted_state)` call **accepts** the shifted
     rows. This exercises `validate_initial` and demonstrates the silent
     association; it uses no builder flag, which the no-legacy rule forbids.
  2. The builder **discards the checkpoint and retrains**, warning with both
     hashes, rather than resuming. The witness check runs before
     `state.rows = ckpt.rows` (`engine/src/flop_library/flop_library.cpp:372`).
  3. A second run with `--resume` **succeeds** — the retrain is not the sticky
     abort the discard rule exists to prevent.
  4. The **cap**-changed variant is caught earlier, at the abstraction-digest
     check (`:366-368`), and also discards and retrains. This shows the two
     mechanisms cover disjoint cases.

  Test-design constraints, from the mechanism: the changed tree must not shrink,
  or `node_index >= tree.size()` rejects the rows and the test exercises the
  wrong guard; and the checkpoint must be partial, or the stale-but-complete
  branch (`:382-386`) discards it before the witness is reached.
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
- **AC8**: Each fail-closed parser case has a **negative** test, not only a
  passing one: a missing token, an empty value, a value above 255, a
  non-canonical spelling (`01`, `+1`, ` 1`), a non-final token, a duplicated
  token, a wrong version, and a parameter line whose digest does not recompute.
  Each asserts refusal. The lenient reconstruction branch asserts its two named
  outcomes: an **empty** identity falls back to cap 0 (the packed gap), and an
  **unrecognized** name or version also falls back. A recognized family with a
  **tokenless** line refuses — the case that separates "known absence" from
  "unknown value", and the one that would otherwise reconstruct cap 0 for
  `artifacts/preflop-profile-6way`.
- **AC9**: Stage 1's local pre-publication check reports every `.db` under
  `artifacts/` carrying explicit `|max-raises=` tokens, and its output is
  recorded as stage evidence. This cannot be a repository test: `artifacts/` is
  gitignored (`.gitignore:88`), so such a test would pass vacuously on a clean
  checkout. The check fails on a tokenless parameter line, which is how a
  forgotten artifact is caught instead of silently reading cap 0.

## Decision

Pending independent approval-agent review.
