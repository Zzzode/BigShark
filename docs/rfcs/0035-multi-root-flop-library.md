---
rfc: "0035"
subject: "Multi-Root Six-Way Flop Library"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-24"
updated: "2026-10-25"
owners: "engine flop library, engine resident, engine host"
supersedes: ""
superseded-by: ""
---

# RFC 0035: Multi-Root Six-Way Flop Library

## Summary

The shipped six-way flop library answers exactly one root per canonical class:
six live seats, equal 2-chip contributions, pot 12 (the limped family). A
real flop reached after a preflop raise, callers, or folds carries a different
`(pot, per-seat stacks, per-seat contributions, folded mask)` ledger and
misses the resident, falling through to depth-limited resolving or the
labeled fallback. RFC 0033 recorded this as the deliberate follow-up.

This RFC extends the artifact to carry **multiple reachable roots per
canonical class** under a new BSFLIB03 layout, lifts the resident/host
single-root assumption to a root-keyed lookup, and adds the root dimension to
the builder. It changes no card abstraction and no frontier evaluator; every
root stays `TerminalDepth::Flop`. It does NOT train the artifact: root
selection and the training budget are gated on the full-ledger miss histogram (rollout stage 3, the
instrumentation answering RFC 0033 Open Question 4) and an explicit
compute authorization.

## Motivation

Real six-max flops are reached through several preflop lines, but the shipped
artifact stores one root per class and answers only the all-live limped
ledger; raised and folded flops miss the resident and lose the trained
policy. The engine rules, side-pot settlement, and N-way trainer already
accept those shapes, so the gap is the artifact and its lookup, not the
strategy machinery. Multi-root support is the prerequisite for a six-way
flop policy on anything other than a limped pot.

## Current State and Evidence

The shipped six-way flop library answers exactly one root per canonical class:
six live seats, equal 2-chip contributions, pot 12 (the limped family). A
real flop reached after a preflop raise, callers, or folds carries a different
ledger and misses the resident (see the `same_root` evidence below). RFC 0033
recorded this as the deliberate follow-up.

- One global ledger per file. `make_def` is the sole root constructor
  (`engine/src/flop_library/flop_library.cpp:78-99`); the header carries the
  ledger once and every of the 1,755 per-class sections carries only board,
  range, node, bucket, menu, and probability columns — no ledger
  (`engine/src/resident/mmap_library.cpp:624-682`; layout in
  `engine/include/bs/mmap_library.hpp:14-67`). The folded mask does not exist
  anywhere in the format.
- Lookup matches the whole ledger. `same_root` compares player count, pot,
  big blind, button, exact/canonical board, and per-seat
  `(stacks, contributions, folded)` (`engine/src/resident/resident_policy.cpp:54-101`);
  `resolve_root` linearly scans every record for the first match
  (`:799-838`). The query already expresses raised/folded flops — the
  resident mapper attaches the net per-seat contributions and folded mask
  (`engine/src/protocol/resident_mapper.cpp:536-562`) — but no artifact can
  answer them.
- Mounting is a strict singleton. The host rejects two `.bin`s
  (`apps/engine-host/main.cpp:537-601`) and the resident opens one library
  (`engine/src/resident/resident_policy.cpp:496-545`); `board_map` assumes a
  1:1 board→class id (`mmap_library.cpp:1329`).
- The rules and trainer already accept the shapes. Folded seats, unequal net
  contributions, and dead money validate for 3+ seat rooted boards
  (`engine/src/poker/game_definition.cpp:193-266`), the tree tracks folded
  seats (`engine/src/tree/abstract_tree.cpp:91-108,212-229`), and the
  N-way frontier settles per side-pot layer
  (`engine/src/gto/equity_frontier.cpp:91-236`). The missing piece is purely
  artifact plumbing and per-root ranges.
- Cost is roughly linear in root count. The single-root six-way full-range
  build is 1,755 classes at 1,000 iterations/class, 13 min 37 s on 16
  workers, 188,411,904 bytes
  (`docs/rfcs/0033-flop-library-root-reachability.md:1017`). A K-root
  library is
  approximately K times the class section bytes (the per-root header/index
  overhead is negligible), raised further by deeper raised-pot trees.

## Goals

- **G1**: The artifact can carry more than one reachable root per canonical
  class, and a live flop decision whose ledger matches a trained root is
  served by the library.
- **G2**: Root selection is exact on ledger identity (no quantization): the
  resident never serves a root trained for a different pot/contribution/
  folded shape.
- **G3**: The builder trains and assembles a declared root set in one
  reproducible run, with per-(root,class) resume and identity guards.
- **G4**: The host mounts one multi-root library and reports exactly which
  roots it serves, so advertised coverage stays honest.
- **G5**: The trained root set is chosen from measured miss data, not
  guessed; this RFC delivers the FORMAT and the first measured root set is a
  separate, budget-gated build.

## Non-Goals

- No quantization of arbitrary live pots onto representative roots. A root
  matches only on exact ledger identity; bucketing live raise sizes is a
  separate proposal that would require its own error bound.
- No per-root distinct action/card abstractions in phase 1. Every root uses
  the shipped `rfc0007`-family action abstraction with
  `max_raises_per_street=1` (the cap pinned by RFC 0030) and the suit-canonical
  card abstraction. A raised pot that needs a different schedule is out of
  scope and misses honestly.
- No turn/river content; every root stays `TerminalDepth::Flop`.
- No training run or artifact publication in this RFC.

## Design Principles

- **Exact ledger identity only.** A trained root is served only when the live
  flop's pot, per-seat stacks/net contributions, and folded mask match; no
  quantization or nearest-root approximation.
- **One artifact, one mount, one pin.** Multiple roots live in one BSFLIB03
  file rather than N mounted files, preserving a single digest and mount.
- **Measure before training.** The format supports an arbitrary declared root
  set; which roots get trained is chosen from miss-frequency data, not assumed.
- **Reuse the rules and the trainer.** Folded seats, side pots, and joint
  dealing already work; the change is artifact plumbing plus root-scoped
  ranges, not new game logic.

## Proposed Design

### The root-key universe

A root is identified by the exact fields `same_root` compares. Within one
library the builder enumerates a finite declared set of root *specifications*:

```
RootSpec {
  preflop_family;     // limped | single_raised_pot | three_bet_pot (phase 1)
  live_position_mask; // button-relative positions still live
  caller_count;       // live non-blind callers shaping the pot
  pot;                // exact total at the flop
  per_seat_net;       // exact per-seat net contributions
  per_seat_stack;     // exact chips behind
}
```

**Fold masks are positional and never collapsed at service time.**
Coordinates are button-rotated (SB=1, BB=2), so a BTN fold and an SB fold
are different keys; there is no symmetry to exploit. The raw fold-subset
universe for 2..6 live seats is 57 masks before contribution vectors, and
training all 57 for every family is not the phase-1 target. Grouping is used
ONLY to choose build priority, never as a service key:

1. A grouping key is `(live_count, blind_status)` where `blind_status` is the
   four-state pair `{SB live, BB live}`; it ranks which exact masks are
   trained first. A seat that folded for zero chips (folded before the flop
   with net contribution zero) is a distinct bit in the mask and cannot be
   represented by a different representative root.
2. **One trained root serves exactly one exact positional mask.** The
   committed root table lists every concrete mask individually (including
   which zero-net folded positions it carries); there is no representative
   root that answers a sibling mask.
3. Roots are trained in miss-frequency order, but the frequency histogram is
   over the FULL exact ledger tuple (below), not the grouping key. The group
   only orders the queue of concrete masks within a family.

The exact phase-1 root list, with their pots and per-seat vectors, is a
build-time table committed to the builder (not runtime inference). A live
flop whose family is declared but whose exact pot/contributions are not in
the table misses — there is no nearest-root fallback.

### The measurement gate: full exact-ledger histograms

Exact root matching requires every field of the ledger tuple to be equal
(pot, per-seat stacks, per-seat net contributions, complete folded mask).
Real tables vary stacks continuously (rebuys, accumulated results) and open
sizes vary (2.5x/3x/…), so a miss share aggregated by family or mask
OVERSTATES achievable coverage: the most common exact tuple inside a family
may still be a single-digit share. The instrumentation (rollout stage 3,
answering RFC 0033 Open Question 4) therefore records:

- a histogram over the FULL exact ledger tuple for flop street-start misses
  (family and `(live_count,blind_status)` are rollup labels only),
- the per-seat buy-in/stacks dispersion and the open-size clustering, so it
  is visible how much quantization a non-exact future design would need.

Stage 4 (the first real build) is authorized only when the measurement shows
that a finite EXACT-ledger root set would cover a pre-declared share of
flop-street-start decisions. The threshold and the no-go outcome are decided
before the build from the histogram; if no quantization-free root set reaches
the threshold, no multi-root build runs and the project opens a separate
quantization RFC instead of spending the K× training cost. This makes
"measure before training" falsifiable rather than aspirational.

### Why not multiple BSFLIB02 files

One file per root (or per root/fold-mask) needs no format change, but it
multiplies mounts, digests, and operator pins, requires cross-file duplicate
and ambiguity arbitration, cannot represent one line's multiple fold masks
without even more files, and leaves the host's single-library budget and
`resolve_root` scan split across artifacts. The project already took the
format-bump path for a structural requirement (BSFLIB01→BSFLIB02); the same
no-legacy rule applies. In-place id-space encoding (a rejected option C)
breaks the ascending-unique class id, the 1:1 board map, section-bound
parsing, and per-class build identity — highest silent-corruption risk.

### Layout

BSFLIB03 keeps every BSFLIB02 per-class section body unchanged (40-byte class
header, range entries, codebook, node hashes/menus, bucket rows, actions,
probs). What changes is addressing:

- Magic `BSFLIB02` → `BSFLIB03`; the old magic is foreign and refused (no dual
  reader).
- **Action-abstraction parameters are added to the header in this same bump
  (resolving the RFC 0030 packed-library cap gap now, not later).** BSFLIB02
  stores only the action digest; the reader therefore leaves
  `UnifiedGame::action_id` empty and every packed-library turn/river
  continuation reconstructs at cap 0. Since the header is rebuilt for roots
  anyway, BSFLIB03 stores the full identity: action name, version, canonical
  parameters (including the `|max-raises=<N>` token and the fraction sizes),
  and the digest, alongside the card bucket kind the BSFLIB02 header already
  carries. The reader builds the
  `AbstractionId`, verifies the digest, and assigns `game.action_id` (as the
  eager SQLite path does, `engine/src/resident/resident_policy.cpp:576-578`),
  so continuation menus keep the trained cap. The strict parser rules from
  RFC 0030 apply; a recognized tokenless line fails closed.
- A root directory follows the fixed header:
  ```text
  root_count          u32
  root[root_count] {
    root_id           u32          // 0..root_count-1, ascending
    line_tag          u8           // 1=limped, 2=SRP, 3=3bet
    pot               u64
    stacks[10]        u64
    contributions[10] u64
    folded[10]        u8
    live_count        u8
    range_profile     u8           // which per-root range set (below)
    class_count       u32          // classes trained for this root
  }
  index[root_count][1755] u64   // DENSE per-root offset array indexed by
                                 // global class id (not the sparse dealable
                                 // id): 0 = absent, otherwise the section
                                 // file offset. At 10 roots this is
                                 // 10*1755*8 ≈ 140 KB, negligible versus the
                                 // sections, and gives O(1) addressing.
  ```
- Sections gain a `root_id u32` in their class header. The logical class
  header grows from 40 to 44 bytes and is then padded to an 8-byte boundary
  (effective 48 bytes before the range entries), matching the existing
  writer/reader section alignment (`engine/src/resident/mmap_library.cpp:484-505`
  and the exact-length validation at `:716-739`); uniqueness is
  `(root_id, class_id)`, and the reader cross-checks each section's `root_id`
  against the directory index path that addressed it (a misindexed section
  fails load, the section-level analog of the existing class-id check).
- The single file-level ledger is replaced by the directory; the file header
  retains player count, big blind, button (constant across roots), the two
  abstraction digests, the codebook layout, and the cap. Per-root stacks/
  contributions/pot/folded move into `root[]`.

### Per-root ranges and folded seats

Each root carries a `range_profile` selector. BSFLIB03 phase 1 supports one
profile value:

- `live-uniform = 1`: every live seat gets the same board-filtered full
  range the current builder uses.

The value `line-conditioned = 2` is **reserved and not representable in
BSFLIB03**: a BSFLIB02/03 section stores exactly one range vector
(`range_count` plus one entries block, `mmap_library.cpp:484-503`), which the
reader applies to every seat (`resident_policy.cpp:532-533`), so per-seat
continuation ranges cannot be expressed without a section format change.
Per-position line-conditioned ranges are the postflop-aware preflop
range-refinement follow-up in the six-max delivery worklist and require
their own section revision; this
RFC must not imply BSFLIB03 carries them. Until then roots train with
`live-uniform`.

Folded seats still need cards for blockage: the joint dealer draws them from
a declared removal range so their cards stay out of the runout deck
(`engine/src/gto/nseat_trainer.cpp:146-147,621-628`). Phase 1 assigns folded
seats the board-filtered full range as the removal distribution (the
unconditioned model); a conditioned fold range belongs to that same
range-refinement follow-up and is
recorded as a known approximation, matching the qualification proposal's
treatment of folded cards.

### Resident and host

- `MmapLibrary` parses the root directory and exposes
  `root_count()`/`root(id)` and a `find_bucket(root_id, class_id, key,
  bucket, ...)` lookup; `class_id_for_board` becomes
  `class_id_for_root_board(root_id, board)`, with one board map per root.
- `Record` carries `(library, root_id, class_id)`; `resolve_root` scans the
  Cartesian records and returns a hit only when the query ledger exactly
  equals a trained root AND the board resolves to a present class. Two roots
  with the same ledger are refused at load as ambiguous.
- The host keeps the single-`.bin` mount (`--flop-library`) and prints one
  `resident-served-root` line per root from the directory, replacing the
  single-line report. Capabilities do not enumerate roots; observability is
  the startup report and the per-decision `MissReason`.

### Builder identity and resume

- `BuildParams` gains the declared root table; `make_def` becomes
  `make_def(root, class_board)`. Build identity (`build.json`) records the
  root table hash (ordered) so two runs with different root sets cannot
  share sections.
- A worker stride is over `(root_id, class_id)`; a section is resumed
  independently. The sealed checkpoint already carries the RFC 0030
  tree-shape witness, which differs across roots and guards each
  `(root,class)` warm start.
- The manifest's single `served_root` becomes `served_roots[]`; each entry
  names line tag, ledger, live count, range profile, class coverage, and
  stored/flop totals for that root.

## Miss accounting (instrumentation seam)

The resident already returns a `MissReason`. BSFLIB03 reuses one existing
code and adds exactly one new code:

- `RootNotSupported` (existing) — no trained root matches the query's exact
  ledger; this is the count that drives which roots to add next.
- `ClassNotTrainedForRoot` (new) — a trained root matched the ledger but the
  canonical class is absent for that root (a zero offset in that root's dense
  index row).

**There is no ambiguous-root miss code.** Two directory entries with the same
exact ledger tuple are a corrupt/duplicate artifact and are refused at
load time (`MmapLibrary` open), before any decision can run. A loaded
directory therefore has at most one root per ledger and ambiguity is
unreachable at decision time; no dead defensive code is added on the hot
path. The load failure is a mount error surfaced by the host alongside the
existing bad-magic/truncated-file failures, not a `MissReason`.

The one new code must be mirrored through every layer of the existing
miss-reason plumbing. Five of the six sites switch exhaustively; the error
mapper has an explicit generic default, so the mapping is named here rather
than left to that default:

1. `engine/include/bs/resident_policy.hpp` — the `MissReason` enum;
2. `engine/src/resident/resident_policy.cpp:246` — `to_string(MissReason)`;
3. `engine/include/bs/protocol.hpp` — the mirrored `BlueprintMiss` enum;
4. `engine/src/protocol/frame_stream.cpp:73` — `to_string(BlueprintMiss)`;
5. `engine/src/protocol/envelope.cpp:198` — `mapMissToError` maps
   `ClassNotTrainedForRoot` to `ERROR_CODE_INCOMPATIBLE_ROOT` explicitly,
   not to the generic unsupported-feature default;
6. `apps/engine-host/main.cpp:421` — `mapMiss`.

These counters are the input to the stage-3 instrumentation and the next
root-set expansion; this RFC only defines the codes and their wiring.

## Dependency Rules

- The packed format (`engine/src/resident/mmap_library.*`) owns BSFLIB03; the
  builder emits it and no second writer exists.
- `bs::flop_library` owns the root table and per-(root,class) training.
- The resident owns exact root matching; no quantization lives anywhere.
- No wire-protocol change: the engine request/response already carries only
  the live ledger.

## Compatibility and Migration

- BSFLIB02 is not read after the bump; the current single-root library stays
  on disk until the first BSFLIB03 build is authorized and published. The
  phase-1 BSFLIB03 set always INCLUDES the limped six-live root with its
  exact current ledger, so the one reachable root today is preserved.
- Stats/verify/pack/cold-open tools and the bundled-artifact test move to the
  root-directory API.
- No artifact transform; the rebuild retrains.

### Migration surface (test and tool inventory)

The format bump touches every BSFLIB02 reader/writer test; the migration
work and its coverage are:

- `engine/tests/test_mmap_library.cpp`: BSFLIB03 magic and header parse;
  root-directory round-trip with several roots; dense
  `index[root][1755]` addressing including zero-offset absence; the
  section `root_id` cross-check; enumeration cross-check against the
  existing accessor tests. Corruption cases that must refuse at open:
  bad/old magic, truncated header or directory, duplicate root ledger,
  index offset outside the file, section whose `root_id` disagrees with
  the index path that addressed it, and a padded class header whose
  effective length violates the 8-byte alignment.
- `engine/tests/test_flop_library.cpp` and
  `test_flop_library_parallel.cpp`: a synthetic multi-root build round-trip;
  per-`(root_id,class_id)` section stride and independent resume; build
  identity (`build.json`) refusal when the root-table hash differs;
  `served_roots[]` manifest reconciliation, including per-root covered-flop
  totals.
- `ResidentPolicySet::build` round-trip: two `SupportedRootSpec` values
  (limped and one raised/folded ledger) against one synthetic BSFLIB03
  file; each spec serves its own ledger and each non-matching ledger
  returns `RootNotSupported`; a duplicate-root directory makes `build`
  report a load failure rather than a miss.
- Host (`apps/engine-host/main.cpp`): manifest parsing moves from
  `served_root` to `served_roots[]`; startup prints one
  `resident-served-root` line per directory entry; a test fixture covers a
  two-root manifest and a manifest/directory mismatch.
- The packed-library reader's assignment of `game.action_id` (the RFC 0030
  cap-gap closure in this bump) is covered by a continuation-menu test that
  reads a cap-1 BSFLIB03 file and reconstructs cap 1.

## Security and Operational Impact

Offline build; resident read path gains an O(classes × roots) scan (the
exploration measured the existing scan at 1,755 records; a phase-1 set of a
handful of roots keeps this in the low tens of thousands of comparisons per
decision, revisited if the root table grows). Fail-closed semantics are
unchanged — an unmatched root resolves or falls back, never approximates.

## Alternatives Considered

1. **Multiple BSFLIB02 files.** No format work, but N mounts/pins, cross-file
   ambiguity, and a fold-mask explosion of files. Rejected.
2. **In-place composite class ids (BSFLIB02 retained).** Breaks uniqueness,
   board-map, section-bound, and identity invariants. Rejected.
3. **Quantize live pots to representative roots.** Would serve roots trained
   for different ledgers; needs an error bound this project has not measured.
   Deferred.
4. **Train all 57 masks per family immediately.** Cost without frequency
   evidence; phased behind the stage-3 exact-ledger miss data.

## Risks

1. **Cost multiplier.** K roots ≈ K× section bytes plus deeper raised-pot
   trees. Mitigated by measured phasing and the per-(root,class) resume; the
   full build is a separately authorized stage.
2. **Wrong per-root ranges.** `live-uniform` ignores preflop conditioning;
   documented as an approximation and replaced when the line-conditioned
   range follow-up lands.
3. **Lookup growth.** The scan becomes classes × roots; root table stays
   small in phase 1 and the index is per-root offsets, so the hot path is two
   binary searches once a root resolves.
4. **Raised-pot trees at 100 BB under cap 1.** The only six-seat cap-1
   production measurement is the shipped RFC 0033 build itself: the limped
   root reaches the flop at the deepest stack-to-pot ratio in the phase-1
   family (198 chips behind into a pot of 12, roughly 16.5 SPR), and all
   1,755 classes completed at 1,000 iterations each under identity cap 1.
   SRP and 3-bet roots reach the flop at lower SPR with fewer chips behind
   per seat, and every root is `TerminalDepth::Flop`, so their single
   betting street cannot offer more raise depth than the shipped tree. Node
   and byte totals are not assumed monotone in menu width: each root's exact
   node and byte totals are recorded in the manifest before another root is
   added, and a root that crosses the builder resource cap is excluded from
   that build instead of failing the whole artifact.

## Verification Plan

- Round-trip: build a two-root synthetic library (limped + one SRP root with
  one folded seat), mmaps it, and serve a decision for each exact ledger;
  assert the non-trained raised ledger and the trained family's wrong pot
  both miss.
- Identity: ambiguous duplicate roots refuse at load; a folded mask mismatch
  never hits; `(root_id,class_id)` sections resume independently.
- Coverage math: per-root covered-flop counts and total section offsets
  reconcile exactly.
- Full Engine Build Contract and ASan over the codec and resident.

## Rollout Plan

1. Format: BSFLIB03 magic, root directory, 44-byte class header, index, and
   the mmap/API changes with synthetic multi-root tests (no production
   artifact).
2. Builder/residency: root table in `BuildParams`, per-(root,class) stride
   and resume, `served_roots[]`, host multi-root report and miss codes.
3. Measurement: exact-ledger miss/root-shape instrumentation on the CURRENT library (the stage answering RFC 0033 Open Question 4).
4. First real root set: chosen from step 3, built and published only under a
   compute authorization; record cost and coverage.

## Rollback Plan

Stages 1-2 are source changes behind a format bump; reverting restores the
BSFLIB02 reader and the single-root mount. The BSFLIB02 binary is retained
until the BSFLIB03 build is verified reachable.

## Open Questions

1. The exact phase-1 root table (families, concrete masks, pots) —
   selected from the stage-3 exact-ledger histogram before stage 4, not in
   this RFC.
2. Whether line-conditioned per-seat ranges (the postflop-aware preflop
   range-refinement follow-up in the six-max delivery worklist) are required before the first
   multi-root build is publishable, or `live-uniform` roots are an accepted
   first approximation.

## Acceptance Criteria

- **AC1 (format)**: A BSFLIB03 library with at least two roots round-trips,
  mmaps, and serves exact-ledger hits; the dense `index[root][1755]`
  addresses present sections and reports absence at zero offsets; a section
  whose `root_id` disagrees with its index path refuses at load. Every
  listed corruption case (old magic, truncation, duplicate root ledger,
  out-of-file offset, misindexed section, bad class-header alignment)
  refuses at open with a mount error — never a decision-time ambiguity.
- **AC2 (exact matching)**: A non-matching pot, per-seat stack/net
  contribution, or positional fold mask never hits; a trained root with the
  class absent returns `ClassNotTrainedForRoot`, mirrored through all six
  switch sites (resident enum/to_string, `BlueprintMiss`, frame-stream
  to_string, `mapMissToError`, host `mapMiss`); a ledger with no trained
  root returns the existing `RootNotSupported`.
- **AC3 (builder and resume)**: The builder trains per
  `(root_id,class_id)` stride and resumes each independently; build
  identity (`build.json`) carries the ordered root-table hash and a run
  with a different root table is refused; the RFC 0030 tree-shape witness
  guards each warm start across roots.
- **AC4 (host and manifest)**: The host mounts one library, parses
  `served_roots[]`, and prints one `resident-served-root` line per
  directory entry; the resident scan resolves an exact root before board
  lookup; the manifest's per-root coverage totals reconcile exactly with
  the sections.
- **AC5 (cap-gap closure)**: The BSFLIB03 header carries the full action
  identity (name, version, parameters incl. `|max-raises=`, digest) and
  card bucket kind; the reader verifies the digest and assigns
  `game.action_id`, demonstrated by a cap-1 continuation-menu reconstruction
  test; a recognized tokenless line fails closed.
- **AC6 (resident round-trip)**: `ResidentPolicySet::build` over one
  two-root synthetic file serves each ledger through its own spec and
  refuses a duplicate-root directory as a load failure.
- **AC7 (verification)**: The full Engine Build Contract and an ASan run
  pass; `check-rfcs`/`check-docs` pass; BSFLIB02 has no reader in the tree.

## Implementation Record

Status: Implemented for stages 1-3 (2026-10-25); stages 4-5 (the live
exact-ledger histogram gate and the first multi-root training build) stay
explicitly gated and unbuilt.

- BSFLIB03 codec in `mmap_library.{hpp,cpp}`: fixed header with magic
  `BSFLIB03`, version 3, the full RFC 0030 action identity (name, version,
  canonical parameters with the `|max-raises=` token, fraction schedule,
  verified digest) assigned to `game.action_id`; 192-byte root-directory
  records followed by the dense `index[root][1755] u64`; 44-byte class
  headers padded to 48 with leading `root_id`. Bad/old magic (BSFLIB02 is
  foreign), header/directory/section truncation, duplicate ledgers,
  out-of-file and non-page-aligned index offsets, misindexed
  `(root_id,class_id)` sections, nonzero padding, and reserved range
  profile 2 all refuse at open.
- Builder: `BuildParams.roots` plus the programmatic `build_library` entry,
  sole root constructor `make_def(root,board)`, independent
  `root-NNNN-class-NNNN` strides and checkpoints, ordered FNV root-table
  hash in `build.json` with cross-table resume refusal, `served_roots[]`
  manifest. The CLI defaults to the one limped root (no DSL).
- BSWARM03 sealed checkpoints (200-byte header, root-table hash at 192);
  BSWARM01/02 are foreign.
- Miss taxonomy: `ClassNotTrainedForRoot` threaded through the resident
  enum/to_string, `BlueprintMiss`, frame_stream, envelope
  (`ERROR_CODE_INCOMPATIBLE_ROOT`), and host mapMiss; exact-ledger match
  precedes board lookup; duplicate root directories fail the mount.
- Host prints one `resident-served-root` line per root and reconciles
  `served_roots[]` against the mounted directory.
- Tests cover every AC1-AC7 corruption and round-trip case (two-root
  mmap, resident set, builder/resume, checkpoint, host reconciliation,
  cap-1 action continuation, tokenless refusal).
- Verification (final merged source, 2026-10-25): release ctest 79/79;
  npm check 124/124; proto:check; replay; docs/RFC checks; an ASan subset
  covering the codec, checkpoint, flop-library, loading, resolver, and
  conformance tests passed 10/10; the production one-root libraries were
  rebuilt under BSFLIB03 with matching SHA-256 sidecars (six-way
  188,432,384 bytes, 1,755/1,755 classes) and pass the bundled
  reachability test. No BSFLIB02 reader exists in the tree.

## Review history

- Round 1 — independent approval agent `rfc0035-approval-1`,
  2026-10-24 — Changes Requested, findings F1–F9. Dispositions in this
  revision — F1: BSFLIB03 header carries the full action identity and the
  reader assigns `game.action_id`; F2: full exact-ledger histogram gate with
  a pre-declared coverage threshold and an explicit no-go to a quantization
  RFC; F3: root-identity evidence citations re-verified against HEAD;
  F4: `range_profile = 2` documented as reserved and unrepresentable in
  BSFLIB03 (one range vector per section); F5: mask grouping is
  priority-only, `blind_status` defined, one root serves one exact mask;
  F6: 44-byte padded class header, dense global-class-id index, root_id
  cross-check; F7: `RootAmbiguous` removed — duplicate ledgers are a
  load-time refusal, with the six miss-reason switch sites enumerated;
  F8: the 188,411,904-byte citation corrected to RFC 0033:1017;
  F9: migration inventory and AC1–AC7 added covering mmap corruption cases,
  builder identity/resume, host `served_roots[]`, `ResidentPolicySet::build`
  round-trip, and the cap-gap continuation test.

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0035-approval-2 (independent approval agent, general-purpose,
  auto_model/alwaysday1_max; the round-1 reviewer and one retried session
  produced hallucinated verdicts against a non-existent BSFLIB04 draft and
  were discarded)
Decision date: 2026-10-24
Review outcome: Approved
Reviewed scope: Full revised text (537 lines, Proposed) with source-level
  verification of the F1–F9 dispositions across resident_policy.cpp,
  resident_policy.hpp, mmap_library.{hpp,cpp}, protocol.hpp, frame_stream.cpp,
  envelope.cpp, the engine host, flop_library.cpp, game_definition.cpp,
  abstract_tree.cpp, equity_frontier.cpp, nseat_trainer.cpp, RFC 0033:1017,
  and the sixmax delivery register; `node bin/check-rfcs.mjs` green.
Review summary: Approved after round 2. Round 1 returned Changes Requested
  (F1–F9): the BSFLIB02 header carried only a digest leaving packed-library
  continuations at cap 0; the cost measurement was going to train first and
  measure later; fold masks were at risk of becoming service keys; the
  section header and addressing were underspecified for per-(root,class)
  storage; the ambiguity outcome was defined as a decision-time miss code
  despite duplicate roots being a load-time error; the byte-size citation
  pointed at the wrong RFC; and the migration test surface and acceptance
  criteria were incomplete. The revision closes the RFC 0030 cap gap in the
  new header, gates training on a pre-declared exact-ledger histogram
  threshold (no-go opens a separate quantization RFC), reserves
  line-conditioned ranges as unrepresentable in BSFLIB03, makes grouping
  priority-only with one root per exact positional mask, adds the dense
  global-class-id index with padded 44-byte class headers and root_id
  cross-checks, refuses duplicate ledgers at mount time with one new miss
  code mirrored through six named sites (ClassNotTrainedForRoot ->
  ERROR_CODE_INCOMPATIBLE_ROOT), corrects the citation to RFC 0033:1017,
  and adds the mmap/builder/host/round-trip migration inventory and
  AC1–AC7. Four non-blocking findings were applied in the status-flip edit:
  the wrong "T-07" task cross-reference (the real follow-up is the
  postflop-aware preflop range-refinement worklist item), the card bucket
  kind clarified as already present in BSFLIB02, the error-mapper default
  called out with the new code's explicit mapping named, and a root README
  broken link fixed. Residual risks: the exact-ledger coverage threshold is
  deliberately undeclared until stage 3; live-uniform roots have no quality
  error bound, only an exact-match guarantee; the dense index pins the
  1,755-class universe. Approval is design only; no training, artifact
  publication, live play, or spending is authorized, and stages 3-4 remain
  gated on the measured histogram and an explicit compute authorization.
```
