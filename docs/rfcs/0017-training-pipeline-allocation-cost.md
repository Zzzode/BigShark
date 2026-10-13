---
rfc: "0017"
subject: "Training Pipeline Allocation Cost"
status: "Implemented"
authors: "Codex /root"
created: "2026-10-13"
updated: "2026-10-13"
owners: "engine/gto, engine/solver, engine/artifacts, engine/benchmarks, engine/tests"
supersedes: ""
superseded-by: ""
---

# RFC 0017: Training Pipeline Allocation Cost

## Summary

Reduce implementation overhead in revision-two training, concrete policy
export, and schema-v2/v3 artifact loading. Compact sample-local deltas and
cache their frozen strategies, avoid repeated export bucketing and key
reallocation, and assemble validated seat-policy rows from ordered SQLite
cursors instead of retaining complete intermediate state/action maps. Preserve
all strategy mathematics, file formats, publication safety, and existing
resource charges. Add phase timing and process peak-memory measurements to the
existing offline preflop builder and verify actual end-to-end improvement.

## Motivation

The user explicitly requested further performance optimization after RFCs 0015
and 0016. Correct averaging is now denser and can reach the existing budgets
sooner; reducing coverage or raising caps would not solve this requirement.
The recorded 16-sample, batch-eight smoke run trained in 13.45 seconds but
spent 44.477 seconds in the whole process. The difference includes startup,
export, persistence, and other non-training work, not a measured export-only
duration. Phase measurements are needed before attributing that difference.

## Goals

- Preserve raw regrets/sums, probabilities, keys, visits, PRNG witness,
  sample/epoch counts, algorithm identity, and accounted bytes exactly for a
  completed fixed-input run.
- Reduce repeated allocation, bucketing, and decoding work without changing
  coverage, sampling order, or error/resource safety.
- Measure training/export/checkpoint/publication/probe phases and peak RSS
  separately, using repeatable native workflows and fresh output directories.
- Demonstrate lower median end-to-end latency or lower peak memory on a
  declared identical workload; retain only proportionate beneficial changes.

## Non-Goals

- Another training revision, new strategy, zero-reach pruning, altered batch
  width, cache budgets, ranges, action menus, chance law, or frontier values.
- New SQL schemas, file formats, streaming public APIs, direct database
  publication shortcuts, weaker verification, or changed failure cleanup.
- Dropping concrete export rows, changing resident admission budgets, fixing
  every dense-policy capacity limitation, or adding a general storage layer.
- Live play, library regeneration, cloud spending, full-poker certification,
  or a large-scale training campaign.

## Current State and Evidence

Verified in the worktree before implementation:

- `nseat_trainer.cpp::SampleDelta` owns three vectors (actions, regrets,
  sums); `touch_row` and `frozen_strategy` repeatedly find the same immutable
  epoch profile. The budget reserves `row_bytes` before allocation.
- A read-only profile of the existing native `nseat_parallel` test passed.
  Its 146 CPU-clock samples attributed approximately 25 percent to allocator
  paths and 8 percent to evaluation. This is hotspot evidence only, not a
  reliable throughput estimate or promised speedup.
- `seat_policy.cpp::export_dfs` classifies every off-board combo at every
  action node, including nodes without sealed rows. `make_information_key`
  starts with four vector elements and grows while appending board/history.
- `load_path_v2` and `load_path_v3` each retain all `StoredState` and
  `StoredAction` map nodes, decode every key once for validation and again for
  reconstruction, and then construct the final map. Publication performs
  complete source loading and complete pinned output verification, so this
  cost is paid multiple times while caller-owned exported rows still exist.
- Existing row readers order state IDs and action ordinals explicitly and
  validate untrusted numeric values before narrowing. Schema, integrity,
  digest, immutable-reader, auxiliary-table, and probability checks are
  established security boundaries, not optional performance overhead.

Comparable project patterns are the caller-owned strategy spans in the
trainer, ordered SQL scans in the artifact reader, process peak-RSS reporting
in `resident_lookup_benchmark.cpp`, and the independent export oracle in
`test_seat_policy.cpp`. Overlapping contracts are RFCs 0005, 0007, 0009, 0012,
and 0016; no new dependency is needed.

## Design Principles

- Optimize inside the owner of each cost; keep solver independent of storage.
- Borrow only immutable data with a proven lifetime; cache only a pure value
  whose complete identity is available.
- Keep iteration and floating-point reduction order unchanged.
- Keep full validation and immutable publication; do not substitute probes
  for full verification or introduce trusted-file bypasses.
- Preserve conservative charges even when physical allocation shrinks, so
  budget admission and stored accounting do not silently change.

## Proposed Design

### Sample-local training storage

Replace each delta's copied action vector with a reference to its immutable
tree node's menu. Store regret deltas, average deltas, and cached sigma in one
variable-length double allocation with three action-count-sized slices.
On the first touch of a sample/key, find and validate the frozen master row
once and calculate sigma with the existing arithmetic. Subsequent touches
reuse that sigma, never a strategy derived from local deltas. Deltas are
destroyed after reduction and before the next epoch, so cached sigma cannot
cross an update. Preserve menu checks, exact delta addition order, and the
existing `row_bytes`, registry, reduction, and sealed-policy charges. The
current conservative charge bounds the smaller row structure and three
double slices; document and assert that relationship rather than adding free
temporary memory. Ordered-map hinted insertion is permitted when it preserves
key and floating-point order. No board cache is added to the trainer in this
stage unless independent exact-output evidence justifies its extra complexity.

### Concrete export

Reserve the exact existing information-key length before appending fields.
In the export owner, prepare a bounded local index of nodes with sealed rows,
so an uncovered action node does not enumerate and classify 1,326 combos.
Still traverse its children: an uncovered parent does not imply an uncovered
descendant. Preserve every emitted key and the same first-insertion semantics.

A single-board local combo/bucket cache may classify off-board holdings once
and reuse the results across betting nodes. Its identity is the complete
ordered board (including size); rebuild it when that identity changes,
including returning from a chance child to a sibling. Iterate combos in the
original card0/card1 order. Do not retain caches for all possible boards or
filter emitted holdings to training ranges. The final concrete map and its
public API remain unchanged and may still be large.

### Seat-artifact ordered assembly

Within `strategy_artifact.cpp`, use two ordered statements for schema v2/v3:
state rows by ID and action rows by `(info_id, ordinal)`. Merge their cursors
and build one validated policy row at a time. No complete state or action
map is needed. Extract owner-private numeric/state/action parsing helpers
shared with the existing readers to keep invariants centralized. Preserve
the version-specific manifest/card identity and all loaded result metadata.

Require contiguous state IDs from zero, raw seat/card bounds, contiguous
ordinals in 0..31, kind/target agreement, finite bounded probabilities,
canonical key decoding, nonempty normalized action rows, no orphan actions,
empty generic v2/v3 training tables, valid auxiliary tables, validated policy
tags, and manifest count agreement. Both cursors must be consumed completely.
Return no partial result on failure. Keep the existing raw-checkpoint reader
and schema-v1 path; share only necessary owner-local field parsing.

The final policy map still owns keys, actions, and probabilities. This removes
intermediate allocations, not the actual model size. `publish_policy` still
loads its source fully, writes a fresh database, fsyncs and exclusively links
it, then independently reopens the pinned generation for complete validation.
No SQL backup, source mutation, or validation-only fast path is introduced.

### Measurements

Additive diagnostics on `bigshark-preflop-profile-builder` report export,
checkpoint, publication, probe, pipeline elapsed time, and process peak RSS
in bytes (`-1` if unavailable). Reuse the existing platform normalization for
`getrusage`; keep training time and accounted bytes separate. Add matching
JSON fields without removing or changing existing fields. Timers do not
participate in strategy identity or SQLite content. Measure at least three
before/after runs of the same declared workload and report median and range,
page-cache/concurrency conditions, actual samples/epochs, digest, and limits.
Performance-only additions can be built before optimized implementations to
capture a phase-separated baseline after independent design approval.

## Dependency Rules

Trainer and exporter remain in `bigshark_solver`; SQLite remains private to
`bigshark_artifacts`. The existing benchmark executable composes these owners.
No production solver dependency on artifacts, benchmark code, or stage6 is
introduced. No public storage interface, service, protocol, adapter, or live
source-selection change is permitted.

## Compatibility and Migration

This is implementation-only: revision-two batch identity, sampled trajectory,
raw/checkpoint meaning, row coverage, v1/v2/v3 schemas, and wire contracts stay
unchanged. Existing artifacts are read-only inputs and are not rewritten.
New artifacts should retain byte-identical SQLite digests on the same build
and SQLite version for identical completed training output. Builder stdout
and JSON gain only performance diagnostics; positional arguments and defaults
stay unchanged. Simultaneously malformed files may report a different first
failure as streaming changes validation interleaving, but individually
malformed cases retain their typed rejection and no invalid input is accepted.
Wall-clock-capped runs can complete more epochs because they are faster; this
is existing deadline semantics, not fixed-sample equality evidence.

## Security and Operational Impact

Offline only, no credentials/network/live actions. Preserve all artifact
trust boundaries, sidecar refusal, heap/file limits, and atomic publication.
Borrowed menus live in the caller-owned tree for the complete training call.
Memory reporting is informational: RSS is not the trainer's portable budget,
and reduced heap use does not authorize greater caps or policy certification.

## Alternatives Considered

- Raise budgets or prune average paths: changes behavior/coverage and hides
  the cost; outside scope.
- SQL-copy publication or skip the pinned full reopen: adds a publication
  design/security problem when owner-local streaming removes known overhead.
- Public streamed export/writer APIs: potentially removes the final concrete
  map, but creates a larger cross-module contract. Defer until these smaller
  changes are measured.
- Global board/bucket cache or a new map implementation: unnecessary state,
  invalidation, memory, and ordering risks; bounded local reuse suffices.
- Only optimize training: misses the documented non-training cost and cannot
  establish the end-to-end result the user requested.

## Risks

- Borrowed menu or cached sigma survives its valid epoch: use explicit local
  ownership, exact baseline tests, and ASan/UBSan.
- Streaming weakens corrupted-file validation: preserve parsing helpers,
  exhaust both ordered streams, and extend native adversarial tests.
- Export skips descendants or reuses a stale board: independent exact export
  oracles cover sparse parents, chance siblings, preflop, and river roots.
- Timing differences are mistaken for quality gains: compare identical
  samples, width, identity, and output digest; keep quality gates unchanged.
- Physical memory may remain dominated by final maps/templates: report it
  honestly and do not claim all capacity regressions are fixed.

## Verification Plan

Before optimized code lands, native tests capture baseline fingerprints of
complete revision-two raw/policy/scalar results for seeds, widths 1/2/4,
partial batches, weighted ranges, and chance boards. Compare exact bit patterns
afterwards. Retain the independently checked averaging/update and 2,000-sample
quality gates. Add native export/reference equality and malformed streamed
artifact fixtures; run the real builder and pin unchanged output digests.

Run the root release configure/format/build/CTest, benchmark-multistreet,
`npm run check`, `npm run proto:check`, replay, docs/RFC checks, and focused
Clang ASan/UBSan for changed lifetime/input paths. Replay with zero decisions
is explicitly no historical coverage. Use only root presets for C++ builds.

## Rollout Plan

1. Independent RFC approval; instrument and capture untouched baseline.
2. Independently bounded trainer storage and artifact reader workstreams;
   main agent owns exporter, measurements, integration, and documentation.
3. Focused equivalence/security tests, repeated matched end-to-end benchmark,
   full project/sanitizer gates, and independent implementation review.
4. Record retained changes, observed benefit, exact equality, and remaining
   dense-policy/scale limitations; complete only when evidence supports it.

Stop on numeric, digest, validation, resource, lifetime, or quality regressions.

## Rollback Plan

Reverse only RFC 0017 implementation changes, preserving the already-correct
RFC 0016 trainer and all pre-existing worktree edits. Rebuild through release
presets. No artifact migration or deletion is required; baseline outputs and
old immutable policy files remain separate. Revert unhelpful speculative
optimizations rather than keeping extra complexity without measured value.

## Open Questions

None. Exact benefit is an implementation measurement gate, not a prediction.

## Acceptance Criteria

- Complete deterministic baseline fingerprints and same-build artifact
  digests remain identical for matched completed runs.
- Native independent export, malformed-input, resource, quality, and lifetime
  tests pass without smaller quality workloads or raised budgets.
- At least three matched before/after runs show a lower median end-to-end
  latency or peak memory; phase costs and limitations are documented.
- Existing publication verification, v1/raw-checkpoint semantics, immutable
  inputs, dependencies, and established contracts are preserved.
- Full project gates, focused sanitizers, independent final review, and
  documentation/index updates are complete; remaining scale risks are explicit.

## Decision

Author agent: Codex /root
Approved by: /root/profile_average_storage
Decision date: 2026-10-13
Review outcome: Approved
Reviewed scope: All 18 sections, the RFC process, RFC 0016 mathematics and
resource/compatibility contract, native trainer/exporter, schema-v2/v3 readers,
canonical keys, and relevant mathematical/export/adversarial tests.
Review summary: No blocking findings. Immutable sample-local sigma and menu
borrowing, exact export reuse, ordered cursor assembly with centralized field
validation, and matched native phase measurements are proportionate. The
reviewer requires unchanged conservative charges and explicit lifetime bounds,
full bit-pattern baselines, sparse-parent/chance/preflop export oracles, both
SQL cursors completely consumed, and all raw-field/training/auxiliary checks
preserved. Generic training-table parsing must not be replaced by a count-only
shortcut. Final maps may still dominate memory; three matched before/after
runs and independent implementation approval remain gates. This is design
approval, not measured speedup, implementation completion, or certification.

Implementation evidence:

- Production changes are limited to `engine/src/gto/nseat_trainer.cpp`,
  `engine/src/solver/seat_policy.cpp`,
  `engine/src/artifacts/strategy_artifact.cpp`,
  `engine/src/artifacts/artifact_codec.cpp`, and
  `engine/benchmarks/preflop_profile_builder.cpp`. Native regression coverage
  is in `engine/tests/test_nseat_parallel.cpp`,
  `engine/tests/test_seat_policy.cpp`, and `engine/tests/test_artifacts.cpp`.
  Current documentation updates are `docs/design/gto-engine.md`,
  `docs/development/build-and-test.md`, `docs/development/benchmarking.md`,
  this RFC, and the two documentation/RFC indexes. Earlier RFC 0015/0016
  worktree changes remain separate and were not reverted or broadened.
- Twelve complete raw/policy/scalar result fingerprints were captured before
  the production changes and pinned in the native parallel test. The matrix
  includes seeds 17/43, widths 1/2/4, weighted private ranges, partial final
  batches, and fixed/free chance boards. Existing 2,000-sample quality gates
  and conservative resource charges are unchanged.
- Export tests compare all concrete rows against an independent unoptimized
  oracle, including uncovered parents with covered children, preflop buckets,
  turn chance siblings, and the existing river/key cases.
- Schema-v2/v3 tests include 28 malformed mutations per version, exact valid
  reconstruction, empty tables, source-digest preservation, and valid reversed
  key order to exercise hinted-insertion fallback. Eleven mutations assert
  the specific streamed-reader domain rejection; other malformed files may
  be rejected by unchanged earlier preflight checks. Legacy v1 and raw
  checkpoint reader paths remain intact.
- The review found one P2 test-fixture issue: a duplicate-board mutation also
  collided with schema-v3 own cards, reaching the wrong validation branch.
  The fixture now selects off-hand cards dynamically and retains the original
  typed/detail assertion. The independent reviewer confirmed this finding
  resolved. A pre-existing structured-binding lambda capture in the test was
  moved outside the lambda to support this host's Clang 14 sanitizer build;
  its path and assertions remain unchanged.
- Three untouched baseline and three final optimized native builder runs use
  identical 16-sample, batch-eight, two-epoch inputs and limits. The complete
  tables and measurement boundaries are in `docs/development/benchmarking.md`.
  Whole-process median fell from 45.855 to 37.130 seconds (19.03 percent),
  publication median from 22.194 to 13.516 seconds (39.10 percent), and peak
  RSS median from 4,379,246,592 to 4,146,921,472 bytes (5.31 percent).
  Three complete before/after `cmp` checks passed. All six files share SHA-256
  `4d1344b2b3d106166d9ca433c7434d01c03c2293b37f42d33002d10aeda72316`,
  276,460 concrete rows, 836,364 actions, and unchanged accounted bytes.
  Training/export medians do not show independent throughput improvement;
  host load and OS cache are uncontrolled. Final maps/templates, large-run
  affordability, and old frontier-policy training provenance remain limits.
- Release configure/format/build, `benchmark-multistreet`, `npm run check`
  (127 Node tests and 25 protocol tests), direct `npm run proto:check`, replay,
  and documentation/RFC checks passed. Replay contained zero decisions and
  therefore supplies no historical decision coverage. Full release CTest
  passed all 88 tests in 371.02 seconds, including the final streamed-reader
  fixtures, complete fingerprints, strict resource tests, and resident
  integration. Focused Clang 14 ASan/UBSan passed all five tests (`artifacts`,
  `seat_policy`, `nseat_parallel`, `solve_nseat`, `warm_start`) in 1,613.34
  seconds; the full artifact case took 1,546.59 seconds. The root `asan`
  preset was Debug with sanitizers enabled and the compiler override required
  by this host's vendored dependency.

Final implementation review:

Implementation reviewer: /root/profile_average_storage
Implementation decision date: 2026-10-13
Implementation review outcome: Approved
Implementation reviewed scope: All RFC sections and the final trainer delta/sigma, concrete
exporter, schema-v2/v3 ordered reader, seat assembler hint, builder diagnostics,
native regression tests, and performance/current documentation. The reviewer
performed read-only work and did not author or edit the implementation.
Implementation review summary: No unresolved blocking findings. Frozen-strategy mathematics,
sampling and floating-point order, conservative charges, full export coverage,
untrusted-file validation, and atomic publication boundaries are preserved.
The P2 duplicate-board fixture finding is resolved; reverse-key state IDs
verify hinted-insertion fallback. The reviewer independently read the final
88/88 release and 5/5 true Debug ASan/UBSan logs and confirmed that the tested
binaries include the final source changes. Exact baseline fingerprints,
complete byte equality, and the three-run end-to-end/RSS improvement satisfy
this bounded implementation's acceptance criteria. No independent
training/export throughput gain, full-scale affordability, historical replay
coverage, or new poker-quality certification is claimed. Policy maps/templates
and immutable frontier values' old provenance remain disclosed limitations.
