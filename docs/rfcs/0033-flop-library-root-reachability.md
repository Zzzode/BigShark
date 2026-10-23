---
rfc: "0033"
subject: "Make the Flop Library's Root Identity Reachable from a Real Hand"
status: "Implementing"
authors: "BigShark maintainers"
created: "2026-10-22"
updated: "2026-10-23"
owners: "Engine flop library, engine resident, engine benchmarks, engine tests"
supersedes: ""
superseded-by: ""
---

# RFC 0033: Make the Flop Library's Root Identity Reachable from a Real Hand

## Summary

The shipped six-way flop library stores, for every class, exactly one game root:
`stacks = {200 x6}`, `contributions = {2 x6}`, `pot = 12`, no seat folded. That
root cannot be produced by a real six-handed hand that starts each seat with 200
chips, because the stored `stacks` and `contributions` overlap: a seat recorded
as having contributed 2 chips while still holding 200 has not paid for its
contribution out of the same 200. The resident matches roots by exact
per-seat equality, so the library misses on every real flop the table deals.

This RFC records the defect, decides the corrected root convention, and defines
the re-training and verification the corrected convention needs. It does not
change the betting abstraction, the card abstraction, or the packed format.

## Motivation

The flop library is the only bundled postflop strategy artifact. If it cannot
answer a real flop, the engine's flop decisions come from the uniform-prior
depth-limited resolver or from the labeled check/call/fold fallback, and the
trained policy is never consulted. That is not a quality shortfall — it is the
artifact being unreachable, and it is indistinguishable from working at every
layer that does not count lookups.

Measured on
`artifacts/flop-library-6way-fullrange/library.bin`
(sha256 `b67e3cb3db65827be97325d2913e40200956c16da604922d4dc9c190e49b20b4`),
reading the packed header through `MmapLibrary` and replicating `same_root`'s
comparison field by field:

| Real six-handed flop state (big blind 2, each seat starts with 200) | pot | result |
| --- | ---: | --- |
| everyone limps, all six alive | 12 | **miss** |
| everyone limps, starting stack 202 | 12 | match |
| raise to 6, two callers, three folded | 24 | **miss** |
| raise to 6, all six call | 36 | **miss** |
| three seats all-in for 200 on the flop | 606 | **miss** |

Two independent defects appear in that table.

**Defect A — the stored root is not chip-conserving.** The header declares
`stacks = {200 x6}`, `contributions = {2 x6}`, `pot = 12`, which implies
`6*200 + 12 = 1212` chips in play. A real six-handed table with 200-chip
buy-ins holds `6*200 = 1200`. The stored root requires a 202-chip starting
stack, which no published buy-in produces.

**Defect B — one class answers one root shape.** Even after the offset is
corrected, a single stored root means the library can only answer a state where
every seat is alive and every seat has contributed the same amount. Any fold,
any raise, and any unequal contribution misses. The three non-matching rows
above are all ordinary hands, not edge cases.

Defect A is a correctness bug in one function. Defect B is a scope question
about what a "flop library" is expected to cover. Both must be decided together,
because fixing A alone yields a library that answers exactly one real state per
class and still misses almost every hand.

## Goals

- Give the flop library a root convention that a real hand can reach, with the
  convention stated in the manifest and checked by a test.
- Make the reachable-root set explicit, so a reader can tell which real flop
  states a mounted library serves and which fall through.
- Re-train the six-way full-range library under the corrected convention and
  publish its digest.
- Add a test that drives a real-shaped hand through `hero_decision` and requires
  a hit, so this class of unreachability cannot return silently.

## Non-Goals

- Widening the action abstraction. `max_raises_per_street = 1` stays.
- Changing the card abstraction. `suit-canonical-v1` and 1,755 classes stay.
- Changing the packed `BSFLIB02` layout (RFC 0026) or the sealed checkpoint
  format (RFC 0028). The resume guard in Design #3 is logic only; it adds no
  field to `BSWARM01` and leaves the header free for RFC 0030.
- Covering arbitrary root shapes with a single class. That is a design
  question this RFC resolves by exclusion, not by generalizing the format.
- Any turn or river library. RFC 0032 governs that direction.

## Current State and Evidence

### The builder accepts a starting stack and stores a remaining stack

The builder CLI documents the parameter as a starting stack:

```
//   stack         starting stack in chips          (default 4 = 2 BB)
```
`engine/benchmarks/flop_library_builder.cpp:14`, repeated in the usage text at
`:85`.

The manifest structure documents the same field as a remaining stack:

```cpp
poker::Chips stack = 0;         // remaining per seat (chips)
poker::Chips contribution = 0;  // per-seat pot contribution (chips)
```
`engine/include/bs/flop_library.hpp:55-56`.

`make_def` assigns both without reconciling them:

```cpp
for (std::size_t s = 0; s < player_count; ++s) {
  def.stacks[s] = stack;
  def.contributions[s] = contribution;
}
def.pot = static_cast<poker::Chips>(player_count * contribution);
```
`engine/src/flop_library/flop_library.cpp:79-83`.

Nothing in the repository adjusts one from the other: a repository-wide search
for `stack - contribution` / `stack-contribution` in `engine/` returns no
matches.

### The resident compares roots by exact per-seat equality

```cpp
if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p] ||
    a.folded[p] != b.folded[p])
  return false;
```
`engine/src/resident/resident_policy.cpp:97-98`, inside `same_root`, which also
requires equal `player_count`, `pot`, `big_blind`, `button`, and identical board
cards (`:55-97`).

The live path supplies the hand's true ledger with no normalization:
`nodeDef.stacks[a] = seat.stack;` and
`nodeDef.contributions[a] = seat.net_contributed();`
(`engine/src/protocol/resident_mapper.cpp:538-541`). The preflop root is built
from the wire's `starting_stack` with `rootDef.contributions = {}`
(`:221-234`). There is no scaling layer between the wire and `hero_decision`.

### The preflop artifact is not affected, which isolates the defect

Probed through `probe_artifact` on
`artifacts/preflop-profile-6way/preflop-profile.db`:

```
pot=3  bb=2  board_size=0  preflop=1
stacks        : 200 200 200 200 200 200
contributions : 0 0 0 0 0 0
blinds_posted : 0 1 2 0 0 0
```

A real 1/2 table with 200-chip stacks posts exactly that. The preflop builder
records blinds in `blinds_posted` and leaves `contributions` empty, so its root
is chip-conserving. The flop builder does not, so the two artifacts disagree
about what `stack` means. This rules out a misreading of the convention: the
same convention is applied correctly one artifact over.

### No test has ever queried the shipped library with a real root

`hero_decision` is called from exactly two tests,
`engine/tests/test_resident_policy.cpp` and `engine/tests/test_lazy_loading.cpp`.
Neither loads a bundled artifact; both build synthetic `GameDef`s. The River
adapter's `platforms/river-club/tests/w1-resident-wiring.test.ts` writes a
manifest with `"classes": []` and only exercises path resolution. No test
constructs a six-handed 100 BB flop state and asks the published library for a
decision, which is why both defects could ship.

### Consequences in the decision path

The AUTOMATIC chain in the C++ host is blueprint, then resolving, then the
labeled operational fallback (`engine/src/protocol/envelope.cpp:363-384`); the
third step is `operationalFallbackDecision`, which returns check, else call,
else fold (`:171-190`). The TypeScript adapter has its own identically shaped
last resort, `safeFallback` (`platforms/river-club/src/mapper.ts:157-169`), which
applies only when the engine process is unavailable. A blueprint miss on a flop
falls to `resolve_flop` (`apps/engine-host/main.cpp:187-230`), which builds the
subgame from the live node with **uniform prior ranges**
(`engine/src/resolver/subgame_resolver.cpp:488-490`, comment: "The blueprint
cannot supply ranges after an off-tree action, so uniform priors are the honest
fallback"), one raise per street, and no certification.

### Which miss reason a real ledger mismatch produces

`resolve_root` reports `RootIdentityMismatch` only on the pinned path:

```cpp
if (!same_root(pinned->game.def, state.def(), ...))
  return {nullptr, MissReason::RootIdentityMismatch};
```
`engine/src/resident/resident_policy.cpp:815-817`.

The unpinned scan has no such branch: it skips every non-matching record and
falls out of the loop to `return {nullptr, MissReason::RootNotSupported};`
(`:826-837`). Production queries carry no pin — the host passes `{}` at
`engine/src/protocol/envelope.cpp:157` — so a live ledger mismatch surfaces as
`RootNotSupported`, not `RootIdentityMismatch`. Any test that asserts the
mismatch reason must pass the artifact digest explicitly.

## Design Principles

- A postflop stored root must be a physically realizable chip ledger: its
  `pot + sum(stacks)` must equal the seated seats' buy-in total. A preflop root
  is exempt because its `stacks` are pre-blind and the constructor posts the
  blinds itself (`engine/src/poker/game_definition.cpp:322-328`), so
  `pot + sum(stacks)` exceeds the buy-in by the posted blinds by design.
- The meaning of a persisted field must be stated once and read only one way.
  `stack` will mean "chips behind at the root", matching
  `poker::GameDef::stacks` everywhere else in the engine.
- Coverage must be an explicit, measured set, never an impression. A mounted
  library must be able to say which roots it serves.
- A strategy artifact that cannot be reached is a defect, not a limitation.
  Tests must query the artifact the way the runtime does.

## Proposed Design

### 1. Correct the root convention in the builder

The builder's `stack` parameter is the **starting stack** the CLI advertises.
`make_def` derives the root ledger from it:

```cpp
def.stacks[s]        = starting_stack - contribution;   // chips behind at the root
def.contributions[s] = contribution;
def.pot              = player_count * contribution;
```

with a validation that `starting_stack > contribution`.

`BuildParams::stack` and `BuildIdentity::stack` are **renamed** to
`starting_stack` rather than kept under the old name. Both serialize to an
artifact key: `BuildIdentity` writes `"stack"` into `.sections/build.json`
(`engine/src/flop_library/flop_library.cpp:559`) and the manifest writes
`"stack"` into `manifest.json` (`:1158`). Those two files live in the same
output directory, so keeping the name while changing the meaning would put two
persisted `"stack"` values with opposite meanings side by side. Renaming makes
the collision impossible and makes the diff mechanical. Because `read_identity`
fails closed on a missing key (`:573-598`), the rename also makes an old
`build.json` reject itself rather than be compared field by field; Design #3
relies on that.

The manifest's own `"stack"` value changes its **assignment site**, not its
meaning: it is written from the derived chips behind instead of `params.stack`
(`engine/src/flop_library/flop_library.cpp:1113`).

This makes the six-way full-range library's root `stacks = {198 x6}`,
`contributions = {2 x6}`, `pot = 12`, total 1200 — reachable by a real six-way
limped pot.

The CLI help text must state both numbers, because "stack 4 = 2 BB" is true only
of the starting stack: the root it trains has 2 chips behind and SPR 0.5, not
the SPR 1 the current text implies.

### 2. Manifest: `stack` keeps exactly one meaning

The manifest gains a provenance field and keeps `stack` unchanged in meaning.
The CLI argument and `BuildParams`/`BuildIdentity` field are now named
`starting_stack` (Design #1), while the **manifest** field `stack` keeps its
existing chips-behind meaning. They are different keys in different files:

```json
"starting_stack": 200,
"stack": 198,
```

`stack` continues to mean chips behind at the root, which is what every reader
already assumes and what the packed header carries. It is **not** repurposed,
and `starting_stack` is a new record that no lookup path reads. The rename in
Design #1 applies to `BuildParams` and `BuildIdentity` only; the manifest and
the packed header keep `stack` as chips behind, and the two must not be
conflated. Four sites derive the root ledger and must be audited in the same
change — three read the manifest, the fourth reads `build.json`:

| Reader | Location | What it does with `stack` |
| --- | --- | --- |
| engine host | `apps/engine-host/main.cpp:743-747` | builds `shared_game_def` for lazy records |
| stage-6 driver | `engine/benchmarks/stage6_resident_nash_conv.cpp:250-251` | rebuilds the fixture root, asserted against the loaded root |
| lazy-loading test | `engine/tests/test_lazy_loading.cpp:51-54,69-72,114-117` | reconstructs the root for lookups |
| builder | `engine/src/flop_library/flop_library.cpp:497-513,632-674` | `BuildIdentity`, which gates resume |

The `starting_stack` record is manifest provenance, not a second header
interpretation. The packed `BSFLIB02` header (RFC 0026) is untouched and keeps
carrying only the per-seat ledger, so an implementer cannot accidentally widen
this into the rejected "two meanings in the header" alternative.

### 3. Close the resume surface where the identity is missing

There are **three** things a resumed build can reuse, and the two an earlier
draft of this section named are not independent — they are complementary per
class, which is why a guard keyed on one of them protects the wrong set.

| Surface | Location | Kept when | Reaches `library.bin`? |
| --- | --- | --- | --- |
| `.sections/build.json` | `.sections/` | always (the parent writes it) | no |
| `class-NNNN.bsf` | `.sections/` | class **complete** | **yes** — packed into the artifact |
| `class-NNNN-checkpoint.bswarm` | `out_dir` | class **in flight** | no — deleted on completion (`flop_library.cpp:450`) |
| `manifest-part-*.jsonl` | `.sections/` | worker accounting | no — counters only (`:682-687`), cross-checked against the section at assembly (`:1038-1047`), which fails closed on conflict |

The first three are the resume surfaces this section guards. The fragments are
listed so the enumeration is honest, but they carry no root-dependent policy and
their assembly cross-check already rejects an inconsistency.

`train_one_class` removes the checkpoint once a class completes (`:450`) and the
worker skips any class whose section already exists (`:1004-1005`). A completed
class therefore has a `.bsf` and **no** checkpoint; an interrupted one has a
checkpoint and no `.bsf`. A guard whose predicate is "a checkpoint exists"
protects exactly the work that would be discarded anyway and protects nothing
for the work that is kept.

That is a correctness problem, not a tidiness one, because section teardown
lives only in `build_library` (`:921-923`). `preflight_resume` contains no
`remove_all` (`:226-256`), and the parallel parent performs no teardown on a
successful or interrupted run — its only `remove_all` is in the failure-cleanup
path (`flop_library_builder.cpp:257-266`). So on the parallel path this sequence
produces a silently wrong artifact:

1. build under the old convention; classes 0..N complete, writing `.bsf`
   sections and deleting their checkpoints; the build is interrupted, leaving
   checkpoints only for the in-flight classes;
2. a checkpoint-keyed guard fires on those in-flight checkpoints;
3. the operator follows the documented remedy and deletes them;
4. re-run `--resume --parallel N`: no checkpoint remains, so the guard is
   silent;
5. `prepare_build_identity` finds no `build.json`, `resume` is true, so it
   writes a fresh identity and keeps `.sections/` (`:955-962`);
6. workers skip every class whose stale `.bsf` exists (`:1004-1005`);
7. `assemble_library` reads those sections — `read_section` validates class id
   and length consistency only, never the root ledger;
8. the packed header is written from the **new** root (`make_unified_game` →
   `make_def`, `:1108-1116`) while the rows inside were trained at the old one.

The result is worse than the original defect. The artifact's header now matches
a real 198-behind flop, so `same_root` admits it, and it serves a policy trained
on a different game — a silent wrong answer where the original produced a clean
miss.

**The fix is a single resume decision owning every surface**, in one shared
helper reachable from every entry point. With `resume` set and
`.sections/build.json` absent, the helper owns four dispositions and defers a
fifth — see the enumerated list below, where the order and the split between
classification and removal are both load-bearing:

> torn → discard; incompatible revision → refuse; stale-but-complete →
> classify as disposable (removal happens later, in the training path); then
> refuse if any valid `.bsf` or valid incomplete `-checkpoint.bswarm` survives,
> for `index < class_count`. The guard **defers** the abstraction-digest and
> empty-row checks to `train_one_class`, which runs them before the stale
> removal.

Refusing on `.bsf` presence — not merely on checkpoints — is the predicate that
covers the surface reaching the artifact. The alternative, which `build_library`
already implements at `:921-923`, is to tear `.sections/` down in that condition;
refusing is chosen because it is uniform across entry points and because
silently discarding completed classes is not what an operator resuming a long
build expects.

Once the guard refuses that condition, the serial teardown at `:921-923` no
longer fires for it: a directory with surviving sections is refused upstream,
and one whose checkpoints were all torn has nothing to reuse. The teardown is
**kept** as defense in depth for a `build.json` whose own removal leaves state
behind on a path the guard does not cover (for example a directory edited
between the guard and the teardown). That is a stated decision, not an
unremarked overlap; deleting it would make the guard the sole protection against
a condition the existing code already handled.

**The ordering is load-bearing, and it preserves an implemented contract.**
RFC 0028 AC4 requires that a torn checkpoint be *discarded and the class
retrained*, not that the build refuse
(`docs/rfcs/0028-sealed-warm-start-checkpoints.md:542`), and
`test_sealed_preflight_branches` pins exactly that: a directory containing only
a truncated `class-0000-checkpoint.bswarm`, with `resume` set and no
`build.json`, must **succeed** and reproduce the reference bytes
(`engine/tests/test_flop_library_parallel.cpp:383-396`). A guard that evaluated
"does any checkpoint exist?" before the discard loop would see the torn file and
refuse, making the discard branch unreachable and failing that test.

The guard must therefore carry **four existing dispositions plus one new one**.
The code already makes the four, but in two different places: torn and
incompatible revision in the preflight (`:246-255`), the abstraction-digest and
empty-row checks plus stale-but-complete in the training path (`:366-385`). A
guard that names only two collapses the rest into refusal and breaks an
Implemented contract.

The guard owns dispositions 1, 2, 4, and 5 below; disposition 3 stays in
`train_one_class` and the guard only has to know it exists, because it runs
before the stale removal. They are applied in this order, which is
load-bearing:

1. **torn** (structurally invalid) → discard and retrain, unchanged (`:246-250`
   serial, `:299-303` parallel);
2. **incompatible algorithm revision** → refuse, unchanged (`:252-255`);
3. **incompatible action or card abstraction digest, or empty rows** → refuse
   with its own message, unchanged, and it must be evaluated **before** the
   stale test (`:366-378`, in `train_one_class` — the guard defers to it);
4. **stale but complete** (`completed_iterations >= iterations_per_class`) →
   classify as disposable, removed later by `train_one_class` (`:382-385`);
5. **valid, incomplete, and with no identity** → refuse, which is the new case.

**Dispositions 2, 3, and 4 can apply to the same file, and the order among them
must not change.** Two hazards exist, in opposite directions, and the design
must close both:

- *Stale must not precede revision.* A checkpoint can carry an incompatible
  revision **and** `completed_iterations >= iterations_per_class`. Today
  revision wins because it is evaluated in the preflight (`:252-255`) while the
  stale check runs later (`:382`); reordering would convert a hard refusal into
  a silent retrain of an entire library, which RFC 0028 records as the thing the
  revision check exists to prevent (`docs/rfcs/0028-sealed-warm-start-checkpoints.md:365-366`).
- *Stale must not precede the digest checks.* A checkpoint can carry a
  compatible revision but an incompatible **action-abstraction** digest — the
  reachable case is resuming after a change to `--raise-cap`, which RFC 0030
  puts in the action digest — **and** `completed_iterations >= iterations_per_class`.
  Today `train_one_class` raises "checkpoint action-abstraction digest mismatch"
  (`:366-368`) before it reaches the stale branch (`:382`), and RFC 0028 records
  that as "Hard refusal naming the field" (`0028:311`). If the stale disposition
  ran first, the checkpoint would be dropped, the class retrained, and the
  mismatch never reported — and the same mismatch would still refuse when
  `completed_iterations < target`, making one condition yield two outcomes
  decided by an unrelated field.

### The preflight classifies; the training path removes

Both hazards are closed by splitting the stale disposition's **judgement** from
its **mutation**:

- the shared preflight **classifies** a stale-complete checkpoint as disposable
  — so it is not counted as a survivor and does not trigger the no-identity
  refusal — **without deleting it**;
- the actual removal stays in `train_one_class` at `:382-385`, after the digest
  and empty-row checks, exactly where it is today.

Classification is read-only, so the preflight cannot race the training path, and
the preflight's answer stays correct: a directory holding only a stale
checkpoint is not refused, because that checkpoint will be dropped before it can
influence anything.

**Classification is not a pass.** A checkpoint classified as disposable is not
cleared for reuse; it is one that will be destroyed. If it also fails a digest
or empty-row check, the training path still refuses it — classification says
"this will not reach the artifact", not "this is acceptable". The two judgements
therefore cannot disagree: they answer different questions, and the refusal wins
whenever both apply.

Both judgements read the same `completed_iterations` header field from the same
path with the same `iterations_per_class`. Within one invocation nothing writes
a checkpoint between the preflight and the worker that consumes it — the serial
path is one process at `:920` then `:939`, and the parallel parent classifies at
`flop_library_builder.cpp:283-317` before forking while workers take disjoint
strides (`i = worker_id; i += parallel`) — so the classification cannot go stale
relative to the removal. Two processes building one `out_dir` concurrently were
already unsafe: their fragment and section conflict handling detects the clash at
assembly (`flop_library.cpp:1038-1047`), after both have written, rather than
preventing it. This RFC neither fixes nor worsens that case.

The three checks that sit ahead of the stale branch in `train_one_class` keep
their place and their messages: the action digest (`:366-368`), the card digest
(`:369-371`), and `rows.empty()` (`:376-378`). The last cannot move at all,
because `inspect_sealed_checkpoint` decodes only the header and never returns
`rows` (`engine/src/artifacts/sealed_checkpoint.cpp:481-500`). An implementer
must not fold the digest checks into the guard: doing so would leave the
dispositions split anyway, since `rows.empty()` cannot follow.

**One consequence of deferring, stated rather than left to be discovered.** A
checkpoint with an incompatible digest **or an empty-row checkpoint** and **no**
`build.json` is refused by disposition 5 with the no-identity message, not by
disposition 3 with its own message, because the design forbids the guard from
evaluating those checks. When `build.json` is present the guard does not apply at
all, so the same file still produces its own message. Both are hard refusals and
the operator's remedy is the same — delete the named file or drop `--resume` — so
the difference is in the diagnostic, not in the outcome. It is recorded here
because a reader comparing the two cases would otherwise conclude one of them is
a bug.

Note that "valid `.bsf`" in the guard predicate means simply "present": sections
are published atomically (`.tmp` + fsync + rename, `flop_library.cpp:470-472`), so
a `.bsf` on disk is always complete. There is no separate structural validator to
look for — `read_section` is file-local to `mmap_library.cpp:686` and not a public
API.

There is a second, smaller consequence for the stale case. A stale checkpoint
that also fails a digest check is *classified* by the guard, so the build
proceeds to the `.sections/` teardown (`:921-923`) and
`prepare_build_identity` writes a fresh `build.json` (`:925`) before
`train_one_class` raises the digest refusal (`:366`). The directory is then left
holding a fresh identity beside a refused checkpoint. It converges: on the next
`--resume` the guard's antecedent is false and `require_identity_match` passes,
so the digest refusal is what the operator sees again. This is stated so an
implementer does not read the leftover identity as a bug and add a cleanup step
that changes the refusal ordering.

The preflight still needs `iterations_per_class` to classify, which
`preflight_resume` does not receive today (`:226`). `inspect_sealed_checkpoint`
already returns `completed_iterations` header-only
(`sealed_checkpoint.cpp:486-504`), so the comparison is implementable once the
parameter is passed; promoting the function to public API is the natural moment
to do it.

`test_stale_but_complete` (`engine/tests/test_warm_start.cpp:310-328`) pins the
second disposition: a directory holding only a valid checkpoint already at the
target, with no `build.json`, must **succeed**, retrain, and leave no
checkpoint. `test_incompatible_resume_preserves_files`
(`test_warm_start.cpp:214-283`) pins the third: the incompatible-revision
refusal must still fire first, with its message unchanged. Both must pass
unchanged, alongside `test_sealed_preflight_branches`.

Once the first three dispositions are applied, the guard evaluates **survivors**
— `.bsf` sections, which are never discarded, and any valid incomplete
checkpoint left with no identity. A directory holding only torn or
stale-but-complete files falls through to a fresh build, exactly as it does
today.

`test_sealed_preflight_branches` must continue to pass unchanged. That is stated
as an acceptance criterion, not left implicit, because two plausible readings of
"refuse when state survives" differ precisely on this case.

The rename in Design #1 closes the `build.json` case by itself: `read_identity`
fails closed on the missing key (`:573-598`), so an old file is rejected before
any comparison and no revision field is added.

There are three entry points and only some reach a preflight:

| Entry point | Location | Runs a preflight |
| --- | --- | --- |
| serial / library API | `build_library` → `preflight_resume` | yes (`:920`) |
| parallel parent | `run_parallel` | **no** — inline duplicate (`flop_library_builder.cpp:282-311`), then `prepare_build_identity` at `:317` |
| worker | `build_library_worker` | **no** — trusts the parent's identity, fails only when `build.json` is missing (`:996-998`) |

The parallel parent is the configuration the shipped artifacts are built with
(`--parallel 16`, RFC 0027/0028), so a guard added only to the `preflight_resume`
library function would leave the production path open. `preflight_resume`
currently lives in the anonymous namespace of `flop_library.cpp` (`:42-787`,
declaration at `:226`), so sharing it requires promoting it to a declared API in
`bs/flop_library.hpp`. That is preferred over duplicating the guard: the builder
already includes that header (`flop_library_builder.cpp:37`) and links
`bigshark_flop_library`, so the promotion adds visibility only, and a second copy
of a preflight is what produced this defect in the first place. `run_parallel`
then calls the promoted function instead of keeping its inline duplicate, so all
three entry points share one preflight and one resume decision.

Because the function becomes public API, its contract belongs in the header. The
resume decision has five outcomes **in this order**, of which the function itself
owns four and defers one: torn checkpoint → discard and retrain; incompatible
algorithm revision → refuse; incompatible abstraction digest or empty training
rows → refuse, **deferred to the training path**; stale-but-complete checkpoint →
classify as disposable, removed later by the training path; valid incomplete
state with no identity → refuse. The order matters because three of these can
apply to the same file, the refusals must win over the classifications, and the
preflight classifies without mutating.

The operator's remedy is explicit: delete the named sections and checkpoints, or
run without `--resume`. Deleting sections **discards those classes' completed
work**, which is why the message names them instead of failing silently; for a
valid incomplete checkpoint the deletion likewise costs that class its
warm-start progress, and a one-class retrain is usually cheaper than accepting a
warm start of unknown provenance. The message lists the files that survived the
corrupt-file cleanup (`:246-250`
serial, `:299-303` parallel), not every file in the directory. The scan bound is
`index < class_count`, matching both existing preflights; a file above that
bound is out of scope for the same reason the existing preflights ignore it,
since a build with a smaller `class_count` would not reuse it.

This deliberately does **not** extend `BSWARM01`. Adding the root ledger to the
sealed checkpoint's fixed 184-byte header (`sealed_checkpoint.hpp:31-37`) would
be a persisted-format change this RFC lists as a non-goal, and it would collide
with RFC 0030, which is `Proposed` and already claims the same header for
`tree_shape_digest` at offset 184 with `header_bytes = 192`
(`docs/rfcs/0030-pin-raise-cap-in-identity.md:470-480`). The guard is strictly
narrower and leaves RFC 0030 free to take the header.

The underlying consequence is unchanged and belongs to a later proposal: neither
a checkpoint nor a section can prove which root it was trained against, so they
are only trustworthy while the identity naming their root is present. That is why
the guard is "refuse" rather than "verify".

### 3a. Every existing `stack` argument must be re-expressed

Because the parameter changes meaning, every caller that passes it currently
gets a **different game** after the fix unless the value is adjusted. Complete
enumeration (22 test call sites, plus the stage-6 driver and the builder):

| Caller | Sites | Today's argument | Today's game (behind / pot / SPR) | Preserve with |
| --- | ---: | --- | --- | --- |
| `test_flop_library_parallel.cpp` | 14 | `4, 2` | 4 / 4 / 1.0 | `6, 2` |
| `test_lazy_loading.cpp` | 2 | `4, 2` | 4 / 4 / 1.0 | `6, 2` |
| `test_warm_start.cpp` | 2 | `4, 2` | 4 / 4 / 1.0 | `6, 2` |
| `test_flop_library.cpp` | 4 (`:119,182,206,221`) | `4, 2` | 4 / 4 / 1.0 | `6, 2` |
| `stage6_resident_nash_conv.cpp` | 1 (`:301-303`) | `10, 2` | 10 / 4 / 2.5 | `12, 2` |
| `flop_library_builder.cpp` default | 1 | `4, 2` | 4 / 4 / 1.0 | `6, 2` |

The builder-default row carries no preservation risk in practice — every real
invocation and every test passes explicit positionals — so changing it is for
consistency, not correctness.

`test_flop_library.cpp:131` asserts `CHECK(manifest.stack == 4)` and `:117`
documents "4 behind, 2 in per seat, pot 4". Because the manifest field keeps the
chips-behind meaning (Design #2), raising the argument to `6` leaves that
assertion correct at `4`; only the argument changes.

**The stage-6 fixture is re-expressed, not accepted as a different game.** The
argument becomes `12` so `manifest.stack` stays `10`, which is what RFC 0029's
containment boundary is stated in terms of — "0 at stacks 4-10 and 1 at stack
11" (`docs/rfcs/0029-flop-terminal-best-response.md:547-549`). Both the census
and the printed line read `manifest.stack` (`engine/benchmarks/stage6_resident_nash_conv.cpp:250,810`),
so the fixture keeps its one-chip margin and AC4's zero-uncontained result. RFC
0029's prose pins the fixture by CLI parameter ("`stack = 10`,
`contribution = 2`", `:543-544`); that sentence becomes ambiguous under the new
parameter meaning and needs a factual erratum saying the argument is `12` and
the chips behind remain `10`.

The shipped library is **not** re-expressed: its 200-behind root is the defect,
and the corrected library deliberately plays 198 behind. The builder's help text
must stop calling the value "starting stack" without stating that chips behind
are `stack - contribution`.

### 4. Declare the served root, in a named field and a named log line

A class answers exactly the states whose `player_count`, `pot`, `big_blind`,
`button`, board, and per-seat `(stacks, contributions, folded)` equal the stored
tuple (`same_root`, `engine/src/resident/resident_policy.cpp:55-99`). For a
valid postflop ledger `pot` is implied by the contributions, but `same_root`
compares it, so it is named here.

The manifest gains a `served_root` object naming the one ledger shape, and the
host startup line gains the same information. The folded field is a per-seat
mask, not a count, because `same_root` compares `folded[p]` seat by seat
(`engine/src/resident/resident_policy.cpp:96-99`); a count would be equivalent
only for the shipped all-alive root and would silently misdescribe any future
root with folds:

```json
"served_root": {"stacks": [198,198,198,198,198,198],
                "contributions": [2,2,2,2,2,2],
                "folded": [false,false,false,false,false,false]}
```

Startup prints, beside the existing `resident-root` line
(`apps/engine-host/main.cpp:813-826`):

```text
resident-served-root <path> seats=6 stacks=198 contributions=2 folded=0/6
```

Without this an operator sees `advertised_roots = 1755` and concludes every flop
is served, when the library answers one ledger shape per class.

### 5. Do not generalize the format in this RFC

Covering folded seats, raised pots, or unequal stacks requires either one class
per root shape or a root dimension in the class key. Both change the packed
format's addressing and the resident's lookup, and neither has measured demand
yet. This RFC records that the shipped library serves one root shape; deciding
whether to widen that is a separate proposal with its own cost measurement.

### 6. Tests, with an unambiguous absent-artifact rule

Two tests, with different contracts:

**Convention test (runs in CI, no published artifact).** Builds a small library
at test time through `flop_library::build_library` — the pattern
`engine/tests/test_lazy_loading.cpp:100-158` already uses — then reads back the
header and asserts `stacks == starting_stack - contribution`, `pot ==
player_count * contribution`, and that `starting_stack <= contribution` throws.
It then drives the reconstructed root through `hero_decision` and requires a
hit. This proves the convention end-to-end without depending on a 188 MB file.

**Published-artifact test (opt-in, excluded from the default suite).** Loads the
bundled `artifacts/flop-library-6way-fullrange/library.bin` when it is present
and requires a hit for a real six-handed 100 BB flop state, passing the artifact
digest so a ledger mismatch reports `RootIdentityMismatch`. Because CI runs
`ctest -LE benchmark` for the default suite and `-L benchmark` separately
(`tools/ci/verify-linux.sh:20-21`), the only label that removes a test from the
default run is `benchmark`. This test carries the `benchmark` label — it is
excluded from CI's default pass and available on demand with `-L benchmark` —
and a comment on the registration states that the label is the exclusion
mechanism, not a claim that the test measures performance. No CI filter change
is required.

**The absent-artifact rule is one rule: the published-artifact test skips with an
explicit message when the file is missing, and the convention test — which is the
gate — never depends on it.** `artifacts/` is gitignored (`.gitignore:88`) and CI
does not fetch it (`tools/ci/verify-linux.sh`), so no test that must run in CI may
read it.

## Dependency Rules

- The builder keeps depending on `poker` and `solver`; no new edge. It already
  includes `bs/flop_library.hpp` (`flop_library_builder.cpp:37`), so promoting
  `preflight_resume` to that header adds no dependency, only visibility.
- The resident is unchanged. It already compares the fields this RFC makes
  consistent.
- The convention test builds its own library and depends on no bundled artifact;
  it is the gate and always runs.
- The published-artifact test is the only test that may read `artifacts/`; it
  skips with an explicit message when the file is absent and carries the
  `benchmark` label so CI's default pass excludes it.
- No new dependency from `resident` to `flop_library` or the reverse.

## Compatibility and Migration

The correction changes the trained artifact's root, therefore its bytes and its
sha256. Every caller that pins the current digest breaks:

- `.runtime/config.json` `residentRoots[].sha256` and `flopLibraries`;
- `docs/` references to `b67e3cb3...`.

Three accepted or proposed RFCs pin it or its parameters and need factual errata:

- RFC 0031 requires `b67e3cb3...20b4` for its Stage 0/3 reproduction
  (`docs/rfcs/0031-measure-card-abstraction-error.md:42,303,491,517`).
- RFC 0030 names the same artifact (`docs/rfcs/0030-pin-raise-cap-in-identity.md:651`).
- RFC 0029 pins its stage-6 fixture by CLI parameter — "`stack = 10`,
  `contribution = 2`" (`docs/rfcs/0029-flop-terminal-best-response.md:543-544`).
  Under the renamed parameter that sentence must read `starting_stack = 12`;
  the chips behind, and therefore the recorded NashConv and containment result,
  are unchanged because the fixture is re-expressed (Design #3a).

Deleting the artifact from RFC 0030/0031 makes their recorded evidence
irreproducible; the implementation adds a factual erratum naming the replacement
digest and re-baselines any RFC 0031 stage that has not yet run. RFC-immutability
permits factual errata; a material design change would need a new RFC.

Migration is a re-train, not a transform: the packed sections are keyed by
public node and bucket, and the root ledger is not a section field, so no
in-place rewrite exists. The old `.bin` is deleted in the same change, per the
repository's no-legacy rule, after the replacement reproduces the verification
RFC 0026's format needs.

In-repository callers must also be re-expressed (Design #3a): 22 test call
sites, the stage-6 driver, and the builder default pass `4, 2` (or `10, 2`) to
mean "4 (10) chips behind, SPR 1 (2.5)", and must pass `6` (or `12`) afterwards
to keep testing the same game. This is a source change in the same commit, not a
compatibility shim.

## Security and Operational Impact

No credential, trust-boundary, or fairness change. The decision surface narrows
in one respect worth stating: after the fix the library answers a limped six-way
pot on any flop, and continues to miss raised or folded pots, so a low hit rate
on an active table is expected behavior rather than a fault. Startup observability
must therefore print the served root beside the advertised class count, or an
operator will see `advertised_roots = 1755` and conclude every flop is served.

## Alternatives Considered

**Normalize the query instead of the artifact.** Rewrite the live `GameDef` to
the library's root before lookup. Rejected: it would report a hit for a state
whose stacks and pot are not the ones being played, so the returned policy would
be priced against a different game. A hit that measures a different game is
worse than a miss.

**Store both a starting and a remaining stack in the header and let the resident
choose.** Rejected: the header's `stacks` always means chips behind everywhere
else in the engine; adding a second interpretation preserves the ambiguity
instead of removing it.

**Train one root per preflop line (limped, single-raised, three-bet, with fold
masks).** Not rejected on merit — deferring it is what makes this RFC small. It
multiplies the class count and needs the cost measured before commitment; it is
recorded here as the natural follow-up.

**Leave the library as-is and treat the resolver as the flop strategy.**
Rejected: it discards a 180 MB verified artifact and contradicts the delivery
plan's claim that six-way flop libraries are complete.

## Risks

- **Re-training cost.** The corrected library needs the same 1,755-class build.
  RFC 0028 measured 13 min 37 s for a 16-worker direct build, so the compute risk
  is low, but the section round-trip control and the determinism gate are the
  long pole.
- **The fix changes the game, not only the header.** Moving from 200 behind to
  198 behind changes SPR and the abstract tree's raise geometry, so the retrained
  sections differ structurally. The new artifact is not a quality improvement and
  its served root is still one ledger shape; documentation must not imply
  otherwise.
- **Silent game change in existing callers.** `starting_stack 4` means SPR 1
  today and SPR 0.5 after the fix; 22 test call sites, the stage-6 driver, and
  the builder default assume the former. If they are not re-expressed, the tests
  still pass while measuring a different game — the same class of error this RFC
  exists to remove. Design #3a enumerates every site and the value that
  preserves its game.
- **Stale sections and checkpoints under a changed parameter meaning.** Neither
  the sealed checkpoint nor a `.bsf` section stores a root ledger, and the two
  are complementary: a completed class keeps its section and loses its
  checkpoint. A guard keyed on checkpoints alone would protect only the work
  that is discarded and would let a stale section be packed under a new header,
  producing an artifact whose identity admits it while its rows were trained on
  another game. Design #3 therefore keys the refusal on both surfaces; the
  residual is that neither can *prove* its root, only be refused when the
  identity is missing.
- **RFC 0029's recorded figure is the fixture's, and the fixture keeps its game.**
  Because the stage-6 fixture is re-expressed (`12, 2` → 10 chips behind), the
  containment census and the published NashConv stay attached to the same
  abstracted game. Only RFC 0029's prose, which names the CLI argument, needs a
  factual erratum; the number itself does not move. If an implementer instead
  leaves the argument at `10`, the fixture silently becomes an 8-behind game and
  the recorded figure becomes irreproducible — which is why Design #3a states
  the required value explicitly.
- **The corrected library still serves only limped pots.** Users may read the
  new digest as a quality improvement. The delivery and reference documentation
  must state the served root beside the digest.
- **Digest churn breaks pins.** Any environment pinning the old sha256 fails
  loudly at startup, which is the intended behavior; the rollout must update
  pins in the same change as the artifact, and RFC 0030/0031 need errata.
- **A vacuous artifact test.** The authority split above prevents it: the CI gate
  builds its own library, and the artifact-dependent test is opt-in.

## Verification Plan

`bigshark-flop-library-verify-pack` is **not** available as a gate here: its
contract is `<sqlite-dir> <packed.bin>` and it loads `class-%04u.db` per class
(`engine/benchmarks/flop_library_verify_pack.cpp:9,93-104`), while RFC 0027
removed the builder's SQLite publish path and the full-range SQLite set no longer
exists. RFC 0027 already recorded that direct builds are gated by equivalence and
determinism checks instead (`docs/rfcs/0027-train-and-pack-direct.md:480,777-778`).
This RFC follows that established strategy:

- The **convention test** builds a small library at test time and asserts the
  derived ledger, the `starting_stack <= contribution` rejection, that the
  manifest carries `starting_stack` and `served_root`, and that `hero_decision`
  hits on a reconstructed real-shaped root.
- The **section round-trip control** builds the same small class set and
  re-reads every stored row through `MmapLibrary`, requiring exact menu and
  probability equality and identical miss semantics. It deliberately does not
  re-derive from SQLite: RFC 0027 deleted the builder's SQLite publish path, so
  the serializer is unchanged by this RFC and its equivalence evidence is
  inherited from RFC 0027's verified gates. The only new question this RFC
  introduces is whether the rebuilt sections round-trip, which this control
  answers. (A SQLite re-derivation is still constructible through
  `write_sqlite_class_artifacts` at `engine/tests/test_lazy_loading.cpp:100-144`
  plus `bigshark-flop-library-pack`, but it would test the unchanged serializer,
  not this change.)
- The **determinism gate** rebuilds the same class set and requires byte
  identity. The resume guards are covered separately: a stale `.sections/build.json`
  (old `"stack"` key) is rejected because `read_identity` fails closed, and an
  orphaned checkpoint is rejected by the shared preflight guard on both the
  serial and the `--parallel` path.
- The **published-artifact test** (opt-in, skipped when the file is absent)
  requires a hit for a real six-handed 100 BB flop state with the digest pinned,
  and `RootIdentityMismatch` under a one-chip mutation.
- The full Engine Build Contract, plus `npm run check`, `npm run proto:check`,
  `node bin/replay.mjs`, `node bin/check-rfcs.mjs`, and `node bin/check-docs.mjs`.
- ASan covers the builder and resident paths touched.

## Rollout Plan

1. Correct `make_def`, rename the build parameter, add the convention test and
   the shared preflight guard, and add the manifest `starting_stack`/`served_root`
   fields plus the `resident-served-root` startup line. No artifact is republished
   yet; the old library keeps working and still misses real flops.
2. Rebuild the six-way full-range library, run the section round-trip control and
   the determinism gate, record the new digest, and retain a copy of the previous
   `library.bin` outside the repository.
3. Audit the four `stack` readers, update every pinned digest, add the errata to
   RFC 0030/0031, delete the superseded `.bin`, and update the delivery and
   reference documentation.
4. Stop condition: if the rebuilt library fails the section round-trip control or
   the determinism gate, discard it and keep the previous artifact; steps 1-2 are
   independently reversible.

## Rollback Plan

Steps 1-2 leave the published artifact untouched, so reverting the source change
restores today's behavior exactly. After step 3 the rollback is restoring the
`library.bin` copy retained in step 2 — a digest is not a backup, the file is
gitignored, and it is 188 MB, so the copy is an explicit rollout artifact rather
than an assumption. If that copy is lost, rollback means retraining under the
previous convention, which is a re-run, not a restore.

## Open Questions

- Should the corrected builder reject a `starting_stack` that does not leave a
  positive stack behind, or clamp it? Rejecting is assumed here.
- Does any deployed configuration pin the current digest outside the repository?
  Unknown; the rollout treats every external pin as broken until updated.
- Is one root shape per class acceptable for the shipped library, or does the
  follow-up (one root per preflop line) need to land in the same change to make
  the artifact useful? This RFC assumes the former and records the latter as the
  next proposal.
- **What is the real-table hit rate, and who measures it?** Even after the fix,
  any bet, raise, fold, or unequal contribution misses, which on an active
  six-max table is most decisions. No acceptance criterion here measures the
  resulting hit rate, so every gate could be green while the artifact still
  answers almost nothing. A follow-up must instrument `MissReason` counts per
  session and publish the distribution before any claim is made that the flop
  library is in use. That measurement needs live play and is out of scope for
  this RFC.

## Acceptance Criteria

- `make_def` derives `stacks = starting_stack - contribution`, validated, and the
  convention test pins the derivation, the rejection case, and the resulting
  `pot`.
- `BuildParams`/`BuildIdentity` use `starting_stack`; the manifest and the packed
  header keep `stack` as chips behind; the convention test asserts both keys and
  their values.
- `SealedCheckpointState` and the `BSWARM01` format are **unchanged**. The guard
  is a shared helper called by both `preflight_resume` (`flop_library.cpp:226-256`)
  and `run_parallel`'s inline scan (`flop_library_builder.cpp:282-311`), and it
  carries four dispositions in order: torn → discard; incompatible revision →
  refuse; stale-but-complete → classify as disposable **without removing**;
  valid incomplete with no identity → refuse. It **defers** the
  abstraction-digest and empty-row refusals to `train_one_class`, which runs
  them before the stale removal.
  `preflight_resume` is promoted to `bs/flop_library.hpp`, gains
  `iterations_per_class` so it can classify, has its contract documented there,
  and `run_parallel` calls it instead of its duplicate. The preflight performs no
  removal for the stale case; `train_one_class:382-385` keeps doing it.
- Three existing tests pass **unchanged**, pinning the Implemented contracts the
  guard must not disturb: `test_sealed_preflight_branches`
  (`test_flop_library_parallel.cpp:367-399`, torn → retrain),
  `test_stale_but_complete` (`test_warm_start.cpp:310-328`, stale → drop and
  retrain), and `test_incompatible_resume_preserves_files`
  (`test_warm_start.cpp:214-283`, incompatible → refuse with its message).
- Three messages that no test currently pins become pinned, because the order
  change in Design #3 would otherwise ship silently: "checkpoint
  action-abstraction digest mismatch", "checkpoint card-abstraction mismatch",
  and "checkpoint carries no training rows". A `grep` over `engine/tests/`
  returns zero hits for all three today; the only pinned checkpoint message is
  the revision refusal. Each fixture carries a `build.json`, because with no
  identity the guard refuses first and the message under test is unreachable.
- Two composition tests pin the two hazards Design #3 names, because no existing
  test reaches either intersection:
  - an incompatible **revision** with `completed_iterations >= target` must
    still produce the "incompatible checkpoint algorithm" refusal;
  - a compatible revision with an incompatible **action-abstraction digest** and
    `completed_iterations >= target` must still produce "checkpoint
    action-abstraction digest mismatch", **not** a silent drop-and-retrain.

  The second is the reachable one: resuming after a `--raise-cap` change puts
  that field in the action digest (RFC 0030), and today the digest check fires
  before the stale branch. Both cases carry a `build.json` in the test fixture,
  because with no identity the same digest mismatch is disposition 5 and reports
  the no-identity refusal instead — that diagnostic difference is stated in
  Design #3 and is not a defect.
- Two guard tests are required: one on the serial path, and one running
  `--parallel 2 --resume` that pre-seeds **stale valid `.bsf` sections with no
  checkpoints and no `build.json`** — the state a checkpoint-keyed guard would
  let through. Each refuses before any **valid** file is reused.
- A third guard test covers a valid `.bsf` surviving alongside a fresh identity:
  the build refuses rather than packing rows trained at the old root under a
  header describing the new one.
- An old `.sections/build.json` is rejected because the renamed key is missing,
  proven by a test that pre-seeds a file with the old `"stack"` key.
- The `.sections/build.json` identity is otherwise unchanged: no revision field,
  because the rename already closes that path.
- Every in-repository `starting_stack` caller is re-expressed so it still trains
  the game it was written against: the tests that assumed SPR 1 now pass `6`
  with contribution `2`; the stage-6 fixture passes `12` so `manifest.stack`
  stays `10` and its containment margin is unchanged. Tests pin both resulting
  roots.
- The section round-trip control and byte-identity determinism pass on the
  rebuilt class set, and the rebuilt six-way full-range library's digest is
  recorded.
- The four `stack` readers are audited and named in the change; the stage-6
  driver's root assertion still passes.
- Startup emits `resident-served-root` and a test asserts the line for a built
  library.
- The published-artifact test hits a real six-handed 100 BB flop state with the
  digest pinned and reports `RootIdentityMismatch` under a one-chip mutation; it
  skips only when the artifact is absent, and no CI gate depends on it.
- Documentation states the served root plainly, so no reader infers full flop
  coverage from `advertised_roots = 1755`.
- The full Engine Build Contract and documentation checks pass.

## Implementation Record

Status: Implementing. Stages 1-3 of the Rollout Plan have landed; the artifact
rebuild (stage 2 of the plan's numbering) has not run.

**Landed.** `make_def` derives `stacks = starting_stack - contribution` and
rejects a starting stack that does not exceed the contribution
(`engine/src/flop_library/flop_library.cpp`). `BuildParams::stack` and
`BuildIdentity::stack` are renamed `starting_stack`; `LibraryManifest` keeps
`stack` as chips behind and adds `starting_stack` plus a `served_root` object.
`preflight_resume` is promoted out of the anonymous namespace to
`bs/flop_library.hpp`, gains `iterations_per_class`, carries the four
dispositions, defers the digest and empty-row refusals to `train_one_class`, and
classifies rather than removes a stale checkpoint. `run_parallel` calls it
instead of its inline duplicate. The host prints `resident-served-root` from the
manifest's `served_root`.

**Call sites re-expressed** (Design #3a): the 22 test call sites moved from
`4, 2` to `6, 2`, preserving 4 chips behind and SPR 1; the stage-6 fixture moved
from `10, 2` to `12, 2`, preserving 10 chips behind. One test reconstructed its
own tree from `stack` directly and had to adopt the derivation; it failed with
"nseat warm start has invalid row projection" until it did, which is exactly the
silent-game-change hazard Design #3a names.

**Verified.** `test_flop_library` asserts the header is chip-conserving
(`pot + sum(stacks) == player_count * (stack + contribution)`) and pins the
derivation and its rejection cases. `test_flop_library_parallel`,
`test_warm_start`, and `test_flop_library` pass, so RFC 0028's torn /
incompatible-revision / stale-but-complete contracts are intact.
`test_resume_guard` adds the five states no earlier test reached, including
revision-beats-stale. The stage-6 fixture reports `stack: 10`,
`uncontained_nodes: 0`, `misses: 0`, `served: 1056` — the abstracted game is
unchanged.

**Pre-existing discrepancy, not caused by this change.** The recorded RFC 0029
figure `nash_conv 2.711476` does not reproduce on this host: both the pre-change
and post-change trees report `2.595987` at the driver's default 2000 iterations,
with identical `stack`, `uncontained_nodes`, `misses`, and `served`. Verified by
stashing every change in this RFC and re-running the same command. The
reproduction gap predates RFC 0033 and is recorded rather than attributed.

**Artifact rebuilt and deployed (2026-10-24).** The six-way full-range library
was rebuilt with `--parallel 16 --range full --raise-cap 1` at 1,755 classes,
1,000 iterations, `starting_stack 200`, `contribution 2`. Result:
188,411,904 bytes,
`sha256 b5a480af97518d0493583312d2dda467fa01f98f547ac904099f206d4ba453aa`,
1,755/1,755 classes, 22,100/22,100 flops, root `stacks = 198 x6`,
`contributions = 2 x6`, `pot = 12` — 1,200 chips, conserving. The superseded
`b67e3cb3…20b4` artifact is preserved outside the repository as the rollback
copy.

Reachability re-measured against the deployed artifact: a real six-handed
200-chip buy-in that limps to the flop now **matches**, where the same query
missed before the change. A raised pot, a pot with folded seats, and any other
buy-in still miss, which is the one-root-per-class scope limit this RFC records
rather than fixes.

Errata added to RFC 0030 and RFC 0031 naming the new digest, since both pinned
the superseded artifact as a measurement baseline.

## Decision

```text
Author agent: BigShark maintainers
Approved by: independent-approval-agent/opensource-deepseek-v4_1_flash/2026-10-23
Decision date: 2026-10-23
Review outcome: Approved
Reviewed scope: RFC 0033 revision 11 in full, across eleven review rounds, against
  docs/rfcs/README.md's review standard; verified against engine flop_library,
  resident, protocol, sealed-checkpoint, builder, engine-host and CI sources, and
  every existing resume test. Artifact digest and packed header re-measured read-only.
Review summary: Approved with four non-blocking errata. Earlier rounds' blocking
  findings were resolved and independently re-verified: the miss-reason path,
  verify_pack unavailability, the absent-artifact rule, manifest stack meaning,
  served-root observability, build-identity invalidation, the parallel-path gap,
  the section resume surface, the stale-but-complete disposition, the
  revision/stale and digest/stale precedence hazards, and the guard/defer split.
  The final design enumerates every directory state with a single intended,
  fail-closed outcome, preserves RFC 0028's torn/incompatible/stale contracts and
  RFC 0029's pinned fixture game, and changes no persisted format.
```

Approval covers the design only. Implementation, the artifact rebuild, and any
live play retain their own authorization requirements.

The four errata raised at approval are recorded as implementation notes rather
than design changes:

1. "valid `.bsf`" means "present", because sections are published atomically.
2. The no-identity diagnostic shift covers an empty-row checkpoint as well as a
   digest mismatch.
3. The message-pinning tests carry a `build.json`, or the message under test is
   unreachable.
4. The guard is *forbidden* from evaluating the digests, not unable to; the
   header carries both.
