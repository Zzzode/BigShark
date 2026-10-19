---
rfc: "0028"
subject: "Sealed Warm-Start Checkpoints"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-19"
updated: "2026-10-20"
owners: "artifact boundary, engine flop library, engine solver, engine benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0028: Sealed Warm-Start Checkpoints

## Summary

Store flop-library warm-start checkpoints under the trainer's own
`(card bucket, public node)` key instead of expanding every sealed row onto
every concrete hole-card combo. RFC 0027 removed the per-class SQLite *policy*
artifact, but the per-class warm-start checkpoint still writes the fully
expanded table: a measured **347.8 MB** file for a class whose distinct
training content is **2,808 vectors** — a **392×** redundancy. Checkpoints
become a compact binary sidecar (**≈ 187 KB** for the six-way full-range
parameters, a **1,860×** reduction from 347.8 MB), which also removes the
SQLite dependency from the training critical path and cuts the rebuild cost of
a crashed class.

## Motivation

The six-way full-range library trained on 2026-10-18/19 used 16 workers. RFC
0027's direct build measured a **10.1 GB peak** output directory. That peak is
the only remaining large intermediate and it sits within 1% of the RFC 0027
AC1 bound, so any deeper stack, wider library, or higher worker count exceeds
the budget.

**Peak breakdown.** 16 live `class-NNNN-checkpoint.db` files at 347.8 MB each
are **5.56 GB, about 55%** of the peak. The remaining ~4.5 GB was not
separately attributed: the peak was sampled without per-file instrumentation.
The code shows the mechanism that makes it plausible rather than mysterious.
`save_training_checkpoint` builds a complete new database at a temp path in
the same directory and only then renames it over the destination
(`engine/src/artifacts/strategy_artifact.cpp:3164-3175`), so a worker caught
mid-write holds **two** ~348 MB files. Sixteen workers in a write phase is
≈ 11.1 GB, which brackets the measured 10.1 GB; the sample is consistent with
a mixed state of completed checkpoints plus in-flight temp copies. This RFC
does not claim that split as measured. It matters here because the unexplained
portion alone is 4.5× the AC3 target, so AC3 cannot be assumed — it must be
measured, and both terms must disappear, not just one.

Both terms do disappear. After this change a class checkpoint is a single
~187 KB sealed file written through temp-and-rename, so the live term is
≈ 3 MB for 16 workers and the transient term is another ≈ 3 MB. What remains
in the directory is the section set and the assembled library: 1,755 classes
averaging ~107 KB give ≈ 188 MB of sections plus the 188.4 MB `library.bin`,
≈ 0.38 GB, which is the floor any build of this library pays.

Measured on `~/ckpt-probe/class-0000-checkpoint.db` (full-range six-way
parameters, 256 iterations, 347,848,704 bytes):

| Measurement | Value |
| --- | --- |
| `information_states` rows | 1,100,736 |
| `training` rows | 2,229,696 (one per (state, action ordinal), each carrying regret and average weight; 2.03 per state on average) |
| concrete states per public node | 1,176 |
| public nodes | 936 |
| **distinct weight vectors** | **2,808** (= 936 nodes × 3 trained buckets) |
| redundancy | **392.0×** |
| per-node distinct vectors | min 3, max 3, mean 3.00 |

The trainer already holds exactly this compact form. `NSeatInitialState::rows`
is `std::map<NSeatInformationKey, NSeatRawRow>` — keyed by
`{own_card_bucket, node_index}` (`engine/include/bs/nseat_trainer.hpp:237`)
— and `NSeatTrainingResult::raw_rows` is the same map
(`engine/include/bs/nseat_trainer.hpp:227`). The builder downcasts this to
`(regret, average_weight)` pairs, expands it onto every concrete combo with
`export_seat_training_rows`, writes it through the schema-v3 SQLite artifact
writer, and on resume walks the tree again with `build_initial_state` to undo
the expansion. Both directions are pure transcription, and the round trip is
where the invariant is stated. The forward pass,
`export_training_dfs` (`engine/src/flop_library/flop_library.cpp:152-209`),
hooks `raw_rows.find({bucket, node})` at `:189-190` and stamps the same
vector onto every off-board combo. The reverse pass, `build_initial_dfs`
(`:220-282`), skips a key it has already filled — "The first combo for a
(bucket, node) fills the trainer row; later combos are skipped because they
carry identical regrets" (`:216-219`). The comment belongs to the reverse
pass; the forward pass is the side that actually reads one row per bucket.
RFC 0026 already proved the same invariant for the policy rows and fails
closed when it is violated (`engine/src/resident/mmap_library.cpp`,
`build_class_data`).

## Goals

- **G1**: A checkpoint's on-disk size is proportional to (public nodes ×
  trained buckets), never to concrete combos. Measured target: **≤ 1 MB** for
  the six-way full-range class that currently takes 347.8 MB (the layout
  computes to ≈ 187 KB: 2,808 keys × 28 B = 78,624 B, plus 5,688 sealed
  actions × 19 B = 108,072 B, plus the 184-byte header).
- **G2**: Warm-start equivalence is exact: resuming from a sealed checkpoint
  produces bitwise-identical final artifacts to an uninterrupted run, for
  every interrupted iteration count the checkpoint interval can produce.
- **G3**: The 16-worker build's measured peak output directory drops to
  **≤ 1 GB** for the 1,755-class full-range library (from 10.1 GB). The
  floor is ≈ 0.38 GB: 1,755 sections at ~107 KB average ≈ 188 MB plus the
  188.4 MB `library.bin`, with 16 sealed checkpoints and their in-flight temp
  copies adding ≈ 6 MB. The 1 GB bound is therefore ~2.6× the prediction, and
  it is verified by measurement rather than derived.
- **G4**: A checkpoint no longer requires SQLite. Nothing on the training
  path links or opens a database after this change.
- **G5**: The sealed checkpoint fails closed if a bucket ever carries
  non-identical rows for one node, mirroring RFC 0026's policy assertion.

## Non-Goals

- **Changing the training algorithm, bucketing, or exported policy.** The
  CFR kernel, `kNSeatCardKind`, `CategoryTiersV1`, and the section serializer
  are untouched.
- **Changing the published library format.** `BSFLIB02` stays version 2.
- **Checkpointing turn/river or preflop roots.** This RFC covers the
  flop-library builder's per-class warm-start state.
- **A general artifact-format replacement.** `create_checkpoint`,
  `publish_policy`, and the schema-v3 SQLite reader stay for other
  consumers; only the builder stops calling them.
- **Multiple binary libraries, live wiring, or quality claims.** Unchanged
  from RFC 0027.

## Current State and Evidence

### Facts (verified in this repository)

- **The builder is the only consumer of the expanded form.**
  `train_one_class` calls `export_seat_training_rows` and
  `save_training_checkpoint` every `kCheckpointInterval` (256) iterations
  (`engine/src/flop_library/flop_library.cpp:533-557`), and resume calls
  `load_checkpoint` then `build_initial_state`
  (`engine/src/flop_library/flop_library.cpp:496-520`).
  `build_initial_state` walks the tree and keeps "the first concrete combo
  for a (bucket, node)" because "all combos share identical regrets"
  (`engine/src/flop_library/flop_library.cpp:216-219`).
- **The trainer's own state is already sealed.** `NSeatInitialState::rows`
  and `NSeatTrainingResult::raw_rows` are
  `std::map<NSeatInformationKey, NSeatRawRow>` where `NSeatRawRow` carries
  `actions`, `regrets`, `sums`, and `visits`
  (`engine/include/bs/nseat_trainer.hpp:193-198`, `:237-239`). The builder's
  current export keeps only two of those four fields and re-expands the key.
- **Measured redundancy.** Table in "Motivation", from a real
  `class-0000-checkpoint.db` read with `sqlite3` against the immutable URI.
- **The checkpoint writer is SQLite-bound.**
  `save_training_checkpoint` builds a full schema-v3 database through
  `write_all_v3_training` → `insert_policy_v2` → `insert_manifest_v3` etc.
  (`engine/src/artifacts/strategy_artifact.cpp:3139-3189`,
  `:1618-1644`, `:1490-1556`), with per-row `std::map` lookups and one
  SQLite `INSERT` per action and per training pair.
- **RFC 0027 measured context.** Full-range direct build: 5 h 38 min total,
  3.6 s assembly, 10.1 GB peak, no `class-*.db`. Assembly already loads
  ≤ 136 KB per class, so the `.bin` is not the disk driver.
- **Existing failure behavior.** Corrupt checkpoints are discarded with a
  warning (`preflight_resume`,
  `engine/src/flop_library/flop_library.cpp:372-404`), and an incompatible
  algorithm revision is a hard refusal. Both must survive the format change.

### Assumptions (to be verified)

- Sealed rows in a checkpoint fit the same bitwise-identity invariant the
  policy rows satisfy. The trainer's map is keyed by the bucket, so a
  violation can only come from a producer bug; the checkpoint writer asserts
  it rather than assuming it (AC2).
- A checkpoint needs `regrets`, `sums`, and `visits`, not only the two
  exported fields. The current SQLite path stores `regret` and
  `average_weight`; `visits` is dropped today, and `build_initial_state`
  reconstructs a state that the trainer accepts. The sealed format therefore
  stores exactly what the trainer consumes, and AC2 pins bitwise equivalence
  against the current path.

## Design Principles

- **Persist the trainer's own state, not a projection of it.** The checkpoint
  key is `NSeatInformationKey`; serialization is a direct encoding of that
  map.
- **One assertion, both consumers.** The bucket-identity check lives beside
  the serializer and reuses the same failure semantics as the section
  writer.
- **Compact format is still validated.** A sealed checkpoint carries its own
  magic, version, identity digests, and an exact expected byte length, so a
  torn or foreign file is rejected before any trainer state is built.
- **Resume parity is the acceptance bar**, not merely "loads".

## Proposed Design

### File format `BSWARM01`

Little-endian, all arrays naturally aligned with explicit padding:

```text
Header (184 bytes, fixed)
  off  field                     type      notes
    0  magic                     char[8]   "BSWARM01"
    8  version                   u32       1
   12  header_bytes              u32       total header length, for forward extension
   16  key_kind                  u8        kNSeatCardKind ordinal
   17  reserved                  u8[7]
   24  algorithm_revision_u32    u32       trainer value (NSeatInitialState field)
   28  reserved                  u32       keeps the next field 8-aligned
   32  completed_iterations      u64
   40  batch_size                u64
   48  completed_epochs          u64
   56  abstraction_digest        u64       action abstraction
   64  card_abstraction_digest   u64
   72  card_abstraction_present  u8
   73  reserved                  u8[7]
   80  node_cap                  u32       trainer node-index upper bound
   84  bucket_cap                u32       bucket-count upper bound
   88  key_count                 u64
   96  total_actions             u64
  104  payload_bytes             u64
  112  payload_digest            u64       FNV-1a 64 over the payload
  120  algorithm_revision        char[64]  NUL-padded provenance string

Payload (sorted by key: bucket asc, then node_index asc)
  keys     u32 bucket[key_count], u64 node_index[key_count]
  offsets  u32 actions_offset[key_count]   payload-relative bytes
           u32 actions_count[key_count]
  visits   u64[key_count]
  actions  CompactAction[total_actions]    (3 bytes, as BSFLIB02)
  regrets  f64[total_actions]
  sums     f64[total_actions]
```

Both revision fields are present by design. `algorithm_revision_u32` is what
the trainer's `NSeatInitialState` carries
(`engine/include/bs/nseat_trainer.hpp:240`) and is the value a resumed run
must feed back. `algorithm_revision` is the 64-byte string the current SQLite
path compares against `kDefaultAlgorithmRevisionV2`. Writing both keeps the
refusal rule and its message unchanged while giving the trainer the numeric
form it actually consumes; a file whose two fields disagree is rejected on
load.

`CompactAction` is reused from the resident format so both files agree on the
action encoding. A key's `regrets`/`sums` runs are contiguous at
`actions_offset`, which is validated against `actions_count`.

### Ownership

`bs::artifacts` gains the codec (`bs/sealed_checkpoint.hpp`); the builder
calls it instead of `save_training_checkpoint`/`load_checkpoint`. The codec
takes and returns `std::map<NSeatInformationKey, NSeatRawRow>`, so no
expansion or tree walk happens in either direction.

```cpp
struct SealedCheckpointInput {
  std::string algorithm_revision;
  std::uint64_t abstraction_digest = 0;
  std::optional<abstraction::AbstractionId> card_abstraction;
  std::uint64_t completed_iterations = 0;
  std::uint64_t batch_size = 1;
  std::uint64_t completed_epochs = 0;
  std::map<solver::NSeatInformationKey, solver::NSeatRawRow> rows;
};

void save_sealed_checkpoint(const std::filesystem::path& path,
                            const SealedCheckpointInput& state);
solver::NSeatInitialState load_sealed_checkpoint(const std::filesystem::path& path);
```

Writes use the same temp-file + `fsync` + atomic rename discipline the SQLite
writer uses today, so an interrupted write leaves the previous file intact.

### Builder changes

- `train_one_class` keeps `trained.raw_rows` in its native form and passes it
  to `save_sealed_checkpoint`. The chunk loop's `solver::export_seat_policy`
  call (`flop_library.cpp:552-553`) exists only to feed the checkpoint writer
  and performs the same full concrete-combo expansion, so it moves out of the
  loop as well; the final export at class completion stays.
- **`test_warm_start.cpp` is rewritten, not preserved.** It is the only other
  caller of `export_seat_training_rows` (`:99, :184, :266, :278`) and
  `build_initial_state` (`:118, :220`), and it is registered as the
  `warm_start` CTest (`engine/CMakeLists.txt:1139-1143`). Its three jobs are
  kept, at the sealed boundary: (a) a chunk-boundary resume reproduces an
  uninterrupted run, now via `save_sealed_checkpoint`/`load_sealed_checkpoint`
  and `NSeatInitialState` directly; (b) an incompatible algorithm revision is
  still refused; (c) `save_training_checkpoint`'s atomic-overwrite behavior
  moves to the sealed codec's own test. The SQLite-specific assertions
  (`:104-109`, `:206-212`) go with the functions they exercise. `export_seat_training_rows` and
  `build_initial_state` are deleted (No Legacy Code): nothing else calls them.
- `preflight_resume` validates the sealed header instead of opening SQLite.
  A torn file is discarded with a warning; an incompatible algorithm revision
  remains a hard refusal.
- **The builder parent has its own preflight**
  (`engine/benchmarks/flop_library_builder.cpp:281-313`) that independently
  calls `load_checkpoint` on every `class-NNNN-checkpoint.db` before forking
  and deletes any file that fails to load as "corrupt". It is converted in the
  same change: it scans the sealed extension, validates the sealed header, and
  keeps the existing rule (torn → discard and retrain; incompatible revision →
  hard refusal). Without this the 16-worker path would delete every sealed
  checkpoint as corrupt and retrain every class, silently defeating G2.
- **Checkpoint filename.** Sealed checkpoints use
  `class-NNNN-checkpoint.bswarm` so the two formats are never confused and the
  old name stays recognizable for the one-time migration rule below.
  `cleanup_failed_build` and `cleanup_incomplete_artifacts` learn the new
  extension.
- The chunk loop already hands `trained.raw_rows` back through
  `state.rows`; the checkpoint becomes a serialization of exactly that map.

### Failure behavior

| Condition | Behavior |
| --- | --- |
| Non-identical rows under one `(bucket, node)` key | Impossible by construction (the map is keyed by that pair); a malformed file that claims otherwise is rejected on load. |
| Torn or truncated file | Magic/version/payload-length/digest mismatch → discarded by `preflight_resume`, class retrains. |
| `algorithm_revision` differs | Hard refusal, exactly as today. |
| Card or action abstraction digest differs | Hard refusal naming the field. |
| Bucket outside the compiled kind's range | Rejected on load. |
| Node index ≥ `node_cap` | Rejected on load. |

## Dependency Rules

- `bs::artifacts` owns the codec and may depend on `bs::solver` types that are
  already public in `nseat_trainer.hpp`.
- **`CompactAction` moves to a public header.** It is currently defined only in
  `engine/src/resident/resident_index.hpp:49-59`, a private header. Including
  it from the codec would compile — `bigshark_artifacts` carries
  `PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src` (`engine/CMakeLists.txt:166-169`)
  and `bigshark_resident` links it `PUBLIC` rather than the reverse, so no
  cycle arises — but it would create a `bs::artifacts` → resident-private
  header edge, which is exactly the kind of undeclared coupling the retained
  formats should not have. `CompactAction` moves into
  `engine/include/bs/mmap_library.hpp` beside `ProbabilityLayout`
  (`:106`), which RFC 0027 placed there for the same reason;
  `resident_index.hpp` then includes that public header. The struct is
  unchanged, so `resident_index.hpp`'s existing consumers see no difference.
- The builder depends on the codec; it must not reach into SQLite for
  checkpoint state.
- No service, host, protocol, or resident target gains a dependency.
- The SQLite schema-v3 writer and reader stay unchanged for their other
  consumers.

## Compatibility and Migration

- **No published artifact changes.** `BSFLIB02` is untouched; AC4 of RFC 0027
  remains valid.
- **In-flight checkpoints.** A directory holding SQLite
  `class-NNNN-checkpoint.db` files cannot be resumed by the new builder. The
  preflight deletes an unrecognized checkpoint with a warning and retrains
  that class, which is the same behavior as today's corrupt-checkpoint path.
  This is acceptable because checkpoints are ephemeral and the full-range
  library has no pending run.
- **Deletions.** `export_seat_training_rows` and `build_initial_state` are
  deleted with `test_warm_start` rewritten (above).
  `save_training_checkpoint` and `load_checkpoint` are **also deleted**: after
  this change the builder is their only production consumer, and the
  No Legacy Code rule forbids keeping them "just in case". `test_warm_start`'s
  SQLite checkpoint assertions and the parent preflight are the remaining
  callers, and both go with the switch. `CheckpointTrainingState` and
  `TrainingWeights` are deleted with them unless an unrelated consumer is
  found during implementation, in which case that consumer is named here
  before the change lands.
- **Deletion cascade.** Removing that pair leaves two more symbols dead, and
  they are deleted in the same change rather than parked:
  `write_all_v3_training`
  (`engine/src/artifacts/strategy_artifact.cpp:1618`) is one function with one
  caller, `save_training_checkpoint` at `:3168`; and `insert_policy_v2`'s
  optional `training` parameter (declared `:1490`, used `:1534-1542`, passed
  only by `write_all_v3_training` at `:1632`) loses its only non-null caller.
  `insert_policy_v2` itself stays — `:1570` and `:1601` call it for the
  policy-only writer — and its signature drops to
  `insert_policy_v2(dt::Db&, const SeatTrainingResult&)` with the training
  branch removed.
- **The two DFS helpers go with their only callers.** `export_training_dfs`
  (`:156-209`) and `build_initial_dfs` (`:220-282`) live in the anonymous
  namespace and are called only from `export_seat_training_rows` (`:919`) and
  `build_initial_state` (`:933`), both deleted above. With `-Wall -Wextra`
  and no global `-Werror` they would surface as unused-function warnings
  rather than failures, so leaving them would still build — which is exactly
  why the deletion must be explicit. They are deleted, and AC5's grep covers
  them so the change leaves no dead code behind.
- `create_checkpoint`, `publish_policy`, and the
  schema-v3 SQLite reader are a separate, genuinely consumed surface and are
  untouched.

## Security and Operational Impact

- **Disk:** the six-way full-range 16-worker peak drops from 10.1 GB to
  ≤ 1 GB (G3), and a crashed class resumes from a ≈ 187 KB file instead of
  347.8 MB.
- **Speed:** checkpointing no longer builds a SQLite database per 256
  iterations. The write becomes one buffer plus one `fsync`; the read becomes
  one map decode, with no tree walk.
- **Fairness and credentials:** unchanged; the builder remains offline.
- **Observability:** the builder prints the checkpoint byte count per class,
  and a rejected checkpoint names its reason on stderr.

## Alternatives Considered

1. **Keep SQLite and normalize the training table to bucket keys.** Smaller
   change, but still writes a database, still pays per-row SQLite overhead,
   and still links the SQLite dependency into the training path. Rejected:
   the format carries no value the binary codec lacks.
2. **Compress the existing SQLite file (page-level or zstd).** Delivers
   roughly the same ratio with no schema change, but keeps the row count, the
   expansion, and the dependency. Rejected as treating the symptom.
3. **Store only regrets and sums, reconstructing visits.** Saves 8 bytes per
   action at the cost of a guess. Rejected: `visits` is trainer state.
4. **Checkpoint less often.** Trades retrain time for disk and does not
   change the asymptotics. Rejected as a tunable, not a design.
5. **Skip checkpoints entirely in direct mode.** The cheapest option, but a
   crash would lose a whole 1,000-iteration class. Rejected.

## Risks

1. **Resume divergence.** A missing field or a different action encoding
   would change the trained result. *Mitigation:* AC2 requires bitwise-identical
   final artifacts across three interruption points, and the codec reuses
   `CompactAction`, whose encoding is already exercised by BSFLIB02.
2. **Silent acceptance of a foreign file.** *Mitigation:* magic, version,
   exact payload length, payload digest, both identity digests, and bucket
   and node bounds are all checked before a return value exists.
3. **Deleting `save_training_checkpoint`/`load_checkpoint` breaks an
   external consumer we have not found.** *Mitigation:* the implementation
   greps the whole repository before deleting and names any surviving caller
   in this RFC; if one exists, the pair is retained with that consumer named
   as the justification rather than as a general-purpose escape hatch.
4. **`visits` semantics drift between the two paths.** Today the SQLite path
   does not persist `visits` at all. *Mitigation:* AC2 pins byte equality of
   the final artifacts, which is the property that matters; if `visits`
   proves irrelevant, a later RFC can drop it.

## Verification Plan

- **Native tests (`test_sealed_checkpoint`)**
  - round-trip a synthetic multi-bucket map and compare the decoded map
    exactly, including `visits`;
  - reject each malformed case: bad magic, wrong version, truncated payload,
    payload-digest mismatch, bucket out of range, node index above `node_cap`,
    mismatched abstraction digest, mismatched algorithm revision;
  - a torn temp file leaves the previous checkpoint intact.
- **Builder resume test (`test_flop_library_parallel`)**
  - run a class to a chunk boundary, capture the checkpoint, resume, and
    assert the final section bytes equal an uninterrupted run;
  - repeat at each of the three interruption points AC2 names — 256, 512, and
    768 of 1,000 iterations — and add the fourth case for the
    stale-but-complete branch, where a checkpoint at or past the target is
    discarded and the class retrains;
  - assert the checkpoint file size is proportional to
    `nodes × buckets` and independent of the range width.
- **Measurements**
  - checkpoint size for the full-range six-way class before and after;
  - 16-worker peak output directory for the 1,755-class build;
  - checkpoint write and load wall clock.
- **Project gates**: full Engine Build Contract, ASan for the new codec and
  the builder path, and `check-rfcs` / `check-docs`.

## Rollout Plan

1. **Codec.** Add `bs/sealed_checkpoint.hpp` and its implementation with the
   round-trip and rejection tests. Gate: tests pass; nothing else uses it.
2. **Builder switch.** Replace the checkpoint calls and delete
   `export_seat_training_rows` / `build_initial_state`. Gate: resume test
   passes and a 50-class build reproduces the RFC 0027 bytes.
3. **Measurement.** Full-range 1,755-class build with RSS and disk sampling.
   Gate: AC1/AC3.
4. **Documentation.** Update `docs/design/gto-engine.md` and the RFC 0027
   evidence table's peak-disk entry.

Stop conditions: any resume divergence or artifact byte difference halts the
rollout and the previous checkpoint path stays until the cause is understood.

## Rollback Plan

- Stages 1–2 are source changes with no published-format change; reverting
  restores SQLite checkpoints.
- Existing `.bin` libraries are unaffected in every direction.
- A sealed checkpoint is ephemeral: discarding it costs at most one
  checkpoint interval of retraining.

## Open Questions

1. **Should `visits` stay?** The current SQLite checkpoint does not persist
   it, and the trainer reconstructs a workable state without it. Keeping it
   costs 8 bytes per key (~22 KB per class) and makes the checkpoint a faithful
   copy of trainer state. This RFC keeps it; a later RFC may drop it with a
   measurement.

## Format details the reviewer required

- **`visits` is kept and can only be verified by the round trip.** The trainer
  increments and seals `visits` but the strategy path reads only `regrets`
  (`engine/src/gto/nseat_trainer.cpp:196-212`), and the resident format does
  not carry it. The sealed file therefore stores it (8 bytes per key), and
  AC2's artifact-byte comparison **cannot** detect a `visits` error because
  no artifact contains it. The round-trip test in AC2's codec section is the
  only check for that field, and the RFC now says so.
- **Algorithm revision mapping.** `NSeatInitialState::algorithm_revision` is a
  `std::uint32_t` (`engine/include/bs/nseat_trainer.hpp:240`), while the
  current SQLite checkpoint stores a revision *string* and compares it to
  `kDefaultAlgorithmRevisionV2`. The sealed header stores both: a `u32`
  numeric revision for the trainer and the 64-byte string for provenance and
  the existing hard-refusal message. The refusal rule is unchanged: an
  unknown or mismatched revision is a hard error, never a silent retrain.

## Acceptance Criteria

- **AC1 (size):** The full-range six-way class that currently produces a
  347,848,704-byte SQLite checkpoint produces a sealed checkpoint **≤ 1 MB**,
  with the measured size recorded. The layout computes to ≈ 187 KB for this
  class (2,808 keys and 5,688 sealed actions; note this is sealed actions, not
  the 936 nodes × 3 buckets = 2,808 key count), so a 1 MB bound still catches
  an accidental re-expansion while leaving room for a wider menu.
- **AC2 (resume bitwise):** For interruption points at 256, 512, and the
  final chunk boundary (768 of 1,000), resuming from a sealed checkpoint
  produces final section bytes identical to an uninterrupted run, and the
  resulting `.bin` matches the RFC 0027 verified artifact. The
  stale-but-complete branch (`flop_library.cpp:517-519`, a checkpoint at or
  past the target is discarded and the class retrains) is exercised by a
  fourth case. A separate codec round trip asserts every field including
  `visits`, which no artifact can reveal.
- **AC3 (build peak):** A 16-worker direct build of the 1,755-class full-range
  library measures a peak output directory **≤ 1 GB**, recorded with its
  component breakdown, and still
  produces `sha256 b67e3cb3db65827be97325d2913e40200956c16da604922d4dc9c190e49b20b4`.
- **AC4 (failure behavior):** Each malformed-file case is rejected with a
  named reason; a torn file is discarded and its class retrains; an
  incompatible algorithm revision is a hard refusal.
- **AC5 (dependency):** The builder, its parent preflight, and every test no
  longer call the removed surface, and a repository-wide grep finds no
  reference to any of these nine names: the four functions
  `save_training_checkpoint`, `load_checkpoint`, `export_seat_training_rows`,
  `build_initial_state`; the two file-local DFS helpers they alone called,
  `export_training_dfs`, `build_initial_dfs`; the two types
  `CheckpointTrainingState`, `TrainingWeights`; and `write_all_v3_training`.
  `insert_policy_v2` remains, with its `training` parameter removed.
- **AC6 (quality gates):** The full Engine Build Contract passes, ASan covers
  the codec and builder paths, and `check-rfcs` / `check-docs` pass.

## Implementation evidence (2026-10-20)

| Criterion | Result |
| --- | --- |
| AC1 (size) | A live `class-0000-checkpoint.bswarm` for the same parameters as the 347.8 MB probe (six-way, stack 200, contribution 2, full range, raise cap 1, identity) measured **186,880 bytes**, exactly the layout's predicted 184 + 2,808 × 28 + 5,688 × 19. That is a **1,861×** reduction. The build trained 1,897 nodes / 2,808 information sets for the class. |
| AC2 (resume bitwise) | `test_flop_library_parallel::test_sealed_resume_at_interruption_points` trains to 256, 512 and 768 of 1,000 iterations, writes a sealed checkpoint through the production codec, then resumes through the real `build_library` entry point; each resume reproduces the uninterrupted `library.bin` sha256 and reports `completed_iterations == 1000`. The stale-but-complete case (checkpoint at the target, section absent) drops the checkpoint and retrains to the same bytes. `test_warm_start` pins the trainer-level property: a resumed 512-iteration run is bitwise equal to a from-scratch run across `regrets`, `sums`, `visits`, and the derived policy. |
| AC3 (build peak) | **Measured 2026-10-20.** A 16-worker direct build of all 1,755 classes (full range, 6 players, stack 200, contribution 2, raise cap 1, 1,000 iterations each, seed 20261002) sampled every 5 s peaked at **372,609,726 bytes (355 MiB)**, well under the 1 GB bound. Component breakdown at the peak: the `.sections/` staging set (1,755 sections) dominates, with `library.bin` (188,440,576 B) written during the final assembly; the 16 live sealed checkpoints and their in-flight temp copies contribute ~6 MB and are not visible at the peak. Coverage 22,100/22,100 flops, and the output is `sha256 b67e3cb3db65827be97325d2913e40200956c16da604922d4dc9c190e49b20b4`, matching the RFC 0027 verified artifact byte for byte. |
| AC3 (wall clock, unpredicted) | The same build completed in **13 min 37 s** (03:56:00 → 04:09:37) against RFC 0027's recorded **5 h 38 min** for identical parameters and identical output bytes. The mechanism is consistent with the checkpoint write volume: the SQLite path wrote 1,755 classes × 4 chunks × 347.8 MB ≈ **2.44 TB** (≈ 120 MB/s sustained over 5 h 38 min), while the sealed path writes 1,755 × 4 × 186,880 B ≈ **1.31 GB**, a 1,860× reduction in bytes written. This RFC did not predict the wall-clock effect: it argued only that disk occupancy would fall. The figure is reported as an observation of a single run, not as an instrumented attribution — confirming it would require profiling the retired path. |
| AC4 (failure behavior) | `test_sealed_checkpoint` rejects bad magic, wrong version, truncated header, truncated payload, payload-digest mismatch, count/length disagreement, and a known revision whose string disagrees. `test_flop_library_parallel::test_sealed_preflight_branches` confirms the builder **discards** a torn file with the warning and retrains to the reference bytes. `test_warm_start` confirms a well-formed incompatible revision is a **hard refusal** that leaves the file byte-identical. An unknown revision is deliberately loaded and reported rather than treated as corruption. |
| AC5 (dependency) | Repository-wide grep after the change: `save_training_checkpoint`, `load_checkpoint`, `export_seat_training_rows`, `build_initial_state`, `export_training_dfs`, `build_initial_dfs`, `CheckpointTrainingState`, `TrainingWeights`, and `write_all_v3_training` have **zero** references in code. The only survivors are historical RFC text (0012, 0016), which the RFC policy forbids editing. |
| AC6 (quality gates) | Release build clean with no warnings on the touched files; `ctest --preset release` **76/76 passed** (1,386 s) on a frozen tree, which includes the new `sealed_checkpoint` test and the extended `flop_library_parallel` and `warm_start` tests. ASan `sealed_checkpoint` / `warm_start` / `flop_library_parallel` all pass with **zero** sanitizer reports. `npm run check` 119/119, `proto:check` clean, `replay` exit 0, multistreet benchmark PASS, `check-rfcs` (28) and `check-docs` (395 files) pass. |


## Decision

```text
Author agent: BigShark maintainers
Approved by: independent-approval-agent/deepseek-v4_1-flash/2026-10-20
Decision date: 2026-10-20
Review outcome: Approved
Reviewed scope: Third revision of RFC 0028 in full, checked line by line
  against all eight round-2 findings and their stated dispositions, and
  re-verified against engine/src/flop_library/flop_library.cpp,
  engine/include/bs/nseat_trainer.hpp, engine/include/bs/flop_library.hpp,
  engine/include/bs/strategy_artifact.hpp,
  engine/src/artifacts/strategy_artifact.cpp,
  engine/src/resident/resident_index.hpp,
  engine/benchmarks/flop_library_builder.cpp, engine/CMakeLists.txt,
  engine/tests/test_warm_start.cpp, and docs/rfcs/0027. Scope excludes the
  turn/river, preflop, and service paths and all RFC 0027 non-goals.
Review summary: Approved on the third round. Round 2's blocking finding
  (the 10.1 GB peak still attributed entirely to live checkpoints) is fixed
  and was independently recomputed: 16 x 347.8 MB = 5,565,579,264 B = 55.1%
  of the peak, with the remainder explicitly labeled unattributed. The
  reviewer confirmed the temp-copy mechanism against
  strategy_artifact.cpp:3164-3175 (make_unique_temp at :3164,
  write_all_v3_training at :3168, rename at :3175), the 0.38 GB post-change
  floor (1,755 x ~107 KB + 188.4 MB bin = 376 MB) and the resulting 2.66x
  margin, the 184-byte header layout and its dual revision fields against
  nseat_trainer.hpp:240 and strategy_artifact.hpp:76-77, and an independent
  recomputation of 2,808 x 28 + 5,688 x 19 + 184 = 186,880 B. It derived the
  5,688 sealed-action count independently from the probe (1,093,680 states
  x 2 actions + 7,056 x 6 = 2,229,696, a format subset). The load-bearing
  claims -- 392x redundancy, sealed-state sufficiency, both preflights
  converted, the filename migration, dead-code removal, and AC1-AC6 being
  measurable -- were each verified against the tree. Five non-blocking
  findings were returned and all five are resolved in this revision before
  acceptance: the Summary size figure aligned to 187 KB; the two DFS helpers
  added to the deletion scope and to AC5; the symbol count made explicit at
  nine names; the Facts citation corrected to :216-219; and CompactAction
  recorded as moving to the public header rather than creating an
  artifacts-to-resident-private edge. Residual risks recorded: the ~4.5 GB
  unexplained peak portion is untested and AC3 is a measurement gate that
  will surface any third term; visits is verifiable only by the codec round
  trip; deleting the SQLite warm-start pair assumes the repository grep is
  complete, which the RFC requires before landing; bucket-identity
  enforcement and actions_offset validation are the most likely places for a
  subtle bug and are covered by AC4 plus ASan; rollout step 2's 50-class
  byte-reproduction gate bounds the blast radius.
```

Review history: the first approval agent was launched 2026-10-19 and
terminated on an API server_error before returning a decision; its resumed run
produced no output for 2.5 hours and was stopped, so it contributed no
findings. A replacement reviewer
(`independent-approval-agent/deepseek-v4_1-flash/2026-10-20`) reviewed the
first revision (Changes Requested, 8 findings), the second (Changes
Requested, 1 blocking plus 7 minor), and the third (Approved, 5 non-blocking).
Every finding in every round was independently verified against the source
before being accepted; none was disputed.
