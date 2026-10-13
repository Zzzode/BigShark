---
rfc: "0016"
subject: "Synchronous MCCFR Batches and Correct Strategy Averaging"
status: "Implemented"
authors: "Codex /root"
created: "2026-10-13"
updated: "2026-10-13"
owners: "engine/gto, engine/artifacts, engine/flop_library, engine/benchmarks, engine/tests"
supersedes: ""
superseded-by: ""
---

# RFC 0016: Synchronous MCCFR Batches and Correct Strategy Averaging

## Summary

Replace the n-seat trainer's asynchronous shared-table updates with bounded
synchronous Monte Carlo counterfactual regret minimization (MCCFR) batches.
Workers evaluate one immutable strategy profile and return sampled regret
deltas. The coordinator merges those deltas in sample order and applies one
regret-matching-plus (RM+) clipping step per batch. An independent,
chance-sampled, full-action traversal accumulates each player's own-reach
weighted average strategy, including paths an opponent currently never takes.
Serial training uses the same algorithm with batch width one. Stamp revision
two and batch width into the existing artifact algorithm identity, reject
incompatible warm starts, and preserve old immutable policy reads. No game,
action abstraction, wire protocol, or artifact schema changes.

## Motivation

RFC 0015's new independent exact-deviation tests uncovered a genuine defect,
not an optimization tolerance issue. In a singleton-range heads-up river
fixture, seed 1 produced a serial summed deviation gain of 0.001274860 chips,
but a two-worker run produced 0.501274860 chips. Repeated tests expose the
same approximate 0.5-chip failure in serial seed 43, and three-seat cases
produce gains above one chip. The existing thresholds remain unchanged.
The user explicitly authorized this additional algorithm scope on 2026-10-13.

The winner's off-path Fold/Call average can remain 50/50 after its latest
regret strategy learns to call. The losing player then has a profitable
deviation against the published average even though its sampled training
profile almost never bets. Protecting individual table accesses does not
correct the average or produce a coherent iteration profile.

## Goals

- Correct average-strategy sampling without introducing a new strategy core.
- Use a coherent frozen profile per update epoch in every worker count.
- Reproduce a fixed seed, batch width, sample count, and input snapshot.
- Preserve legal chance conditioning, own-information keys, native settlement,
  frontier values, and strict shared resource limits.
- Pass existing serial/parallel exact-deviation thresholds, including zero
  opponent-reach and independently checked average-weight fixtures.
- Distinguish sample count from update epochs and identify batch width.
- Prevent old raw checkpoints being resumed under changed update mathematics.

## Non-Goals

- Full-game equilibrium certification, multi-player Nash convergence claims,
  or automatic live promotion of newly trained policies.
- Changing betting rules, ranges, card/action abstraction, SPR buckets,
  frontier models, or immutable policies already saved on disk.
- Neural approximation, GPU work, new storage schema, compatibility shims,
  keeping the defective asynchronous algorithm as a selectable strategy,
  full-library regeneration, or a 100K-iteration training campaign.
- Preserving revision-one trajectory or raw-row equality after this explicitly
  versioned algorithm correction. Existing unrelated heads-up/stage6 routes
  remain unchanged.

## Current State and Evidence

`nseat_trainer.cpp::walk_action` adds `own_reach * sigma[a]` to average sums
only when the acting player is the regret traverser and an externally sampled
opponent path reaches that node. If that sampling reach is `q_-i,t(I)`, the
expected contribution includes an extra time-varying opponent-reach factor.
The desired numerator is `sum_t pi_i,t(I) * sigma_i,t(I,a)`, not that quantity
multiplied by opponent reach. When an opponent assigns zero reach, averaging
can stop forever at an otherwise strategically relevant information set.

The current parallel path releases its table lock before recursion. Workers
read and update overlapping epochs, so one sweep can combine strategies from
different times; locking prevents races but not stale/inconsistent profiles.
The current serial path updates each seat immediately before the next seat.
Both paths identify themselves as algorithm revision one.

The reference methodology is OpenSpiel's external-sampling MCCFR
[implementation](https://github.com/google-deepmind/open_spiel/blob/master/open_spiel/algorithms/external_sampling_mccfr.cc)
and [averaging contract](https://github.com/google-deepmind/open_spiel/blob/master/open_spiel/algorithms/external_sampling_mccfr.h),
which separate full averaging from sampled regret passes. This proposal uses
the project's native rules and keys, not a new dependency on that solver.

The SQLite schema already stores a free-form `algorithm_revision` string.
Current v2/v3 loads nevertheless assign their numeric revision from the
current compiler constant, and `CheckpointTrainingState` omits the stored
algorithm identity. A revision bump without fixing those boundaries would
mislabel old policies and mix incompatible raw sums during warm starts.

## Design Principles

- Repair the incorrect average, rather than substitute latest regret policy.
- Freeze immutable existing storage; stage only worker deltas, not copies of
  the full master table.
- Use one training kernel and one averaging kernel for all supported seats.
- Prefer bounded, deterministic update epochs over unmeasured asynchronous
  convergence assertions.
- Publish only complete epochs and treat algorithm identity as compatibility.

## Proposed Design

### Frozen batch epochs

A run has requested sample count `iterations`, retaining the existing API and
CLI meaning of one sampled episode containing one regret sweep per seat.
Effective batch width is the worker count clamped to `[1, iterations]`.
Each epoch uses `m = min(batch_width, remaining_samples)` samples. The master
row map is immutable throughout worker computation; a missing row means the
existing uniform regret-matching initialization. New rows and all deltas live
in worker-local maps. Workers never derive their strategy from local deltas.

For every sample, retain the existing independent per-seat joint-deal,
opponent-action, and public-chance streams keyed by absolute sample index.
All seat sweeps read the same epoch profile, unlike revision-one alternating
updates. Each regret contribution is the current native sampled
`child_value - profile_value`. After all workers finish, combine deltas in
ascending sample/key/action order and apply
`R_next[a] = max(0, R_old[a] + sum(delta[a]) / m)` once. Do not clip local
deltas, repeatedly clip during reduction, or sum independent trained policies.

Use a persistent bounded worker cohort with an explicit epoch rendezvous,
not threads created for every sample. All workers join before a result or
exception escapes. Worker failure aborts the run without publishing a result;
the wall deadline is checked only between complete epochs. A successful wall
stop reports the complete sample prefix, not scattered iteration identifiers.
Partial cohort creation must also be safe: workers wait at an abortable startup
gate until all requested threads exist, or use a rendezvous that does not
require uncreated participants. A worker computation exception still completes
the epoch-end rendezvous before the coordinator stops and joins the cohort.

### Independent average pass

For each sample, draw one additional independent joint deal, using new
explicit stream-purpose identifiers, and traverse all players' abstract
actions. Sample legal public cards from the existing board/hole-conditioned
distribution; never evaluate terminal utility or invoke the frontier at a leaf.
Virtual flop-deal leaves stop the average traversal because they contain no
remaining preflop information sets.

Carry a fixed-capacity per-seat reach array, initially one. At a node owned
by seat `i`, accumulate `reach[i] * sigma[a]`, then recurse into every action,
changing only seat `i`'s reach component. Do not multiply by any opponent's
reach and do not prune merely because an opponent's reach is zero. Chance
sampling uses its own stream and correct board-only child ordinal. This is
chance-sampled full-action averaging, not exhaustive private-card/chance
enumeration, and does not certify a full-game equilibrium.

The average pass uses exactly the same frozen epoch profile as regret passes.
Average deltas are reduced and divided by `m`, giving each epoch equal weight
including the final smaller batch. Row `visits` continues to count regret
own-node touches, not averaging contributions. Global walk visits include
both kinds of traversal and retain their strict checked cap.

### Memory and resource accounting

Retain the existing finite-tree, information-set, depth, byte, walk-visit,
and wall limits. The byte ledger includes the master map and all simultaneously
live local delta maps; a worker reserves its conservative row charge before
allocation and releases it after reduction/destruction. New master rows
transfer or acquire their charge before publication. New information keys are
counted centrally without double-counting concurrent discoveries. Never
divide each worker's budget or conceal temporary storage. Reduction is also
bounded and cannot allocate a second unaccounted master table.
The new-key deduplication registry and any reduction staging are included in
the ledger before allocation, not treated as free metadata. Deltas for a row
not touched by a sample are zero; divide by the entire epoch's `m`, not only
the samples that touched that key.

### Revision and provenance

Increase `kNSeatAlgorithmRevision` to two and add actual batch width and
completed update-epoch counters to solver training results. Native producer
output reports samples, epochs, and batch width. Preserve original stream
purpose values; add only the independent average purposes.

Revision-two immutable artifacts use the existing algorithm string to record
both revision and batch width. Writers derive or validate that identity from
the exported training result rather than silently applying a batch-one
default to a parallel run. Old revision-one identifiers decode as revision
one, never the current constant. Unknown identifiers remain explicit unknown
provenance, not falsely reclassified as the new trainer.

The canonical new identifier is
`rfc0016-nseat-rev2-frozen-batch-<B>-sampled-splitmix64`, where `<B>` is a
positive canonical decimal integer without leading zeros. The old exact
`rfc0009-nseat-rev1-sampled-splitmix64` maps to revision one. Unknown strings
remain readable with numeric revision/batch/epochs zero and their original
manifest string. Revision-two results export `batch_size` and
`completed_epochs`; persisted epochs are derived from the completed sample
prefix as `samples / B + (samples % B != 0)`. No new database column is needed.
Default seat-checkpoint provenance means derive identity from the result;
an explicitly supplied known identity must match it. Republishing a loaded
unknown immutable policy preserves the explicit original string.

Carry algorithm identity through `CheckpointTrainingState` and numeric
revision/batch identity through `NSeatInitialState`. The serial warm-start
route accepts only revision-two, batch-one state and preserves the absolute
sample streams; incompatible/unknown checkpoints are refused before any raw
row mapping or training. Empty fresh initial state remains supported. Schema,
key grammar, immutable row bytes, and NSP decoding stay unchanged. Existing
saved NSP policies do not contain a training revision and must not be used as
raw warm-start checkpoints or implicitly certified as revision two.

The flop-library resume producer performs a read-only identity preflight
before entering existing training/failure-cleanup scopes or spawning workers.
A refused incompatible checkpoint remains untouched; refusal must not cause
the existing cleanup path to delete the checkpoint that triggered it.

## Dependency Rules

Trainer changes stay in `bigshark_solver`. Averaging depends only on existing
rules, tree, abstraction, sampler, and stream code, never artifact/stage6
libraries. The artifact layer consumes solver result identity; the flop-library
builder consumes checked artifact checkpoint identity. Benchmark producers
compose those existing owners. No new shared layer or reverse dependency.

## Compatibility and Migration

This deliberately changes n-seat trained output. Revision-one published
policies continue loading with their stored identity and unchanged values.
Revision-one raw checkpoints cannot resume into revision two; retraining is
an explicit operator action, not automatic migration. Existing positional
CLI arguments and `--threads` remain valid, while output adds samples/epochs
and actual batch identity. Worker count is an algorithm parameter, so changing
it is not promised to preserve a policy. Serial callers use batch one.
Unrelated heads-up, stage6, wire, replay, and live-source selection are unchanged.

## Security and Operational Impact

Offline only: no tokens, network calls, spending, or table actions. Learning
still indexes only own-card abstraction plus public history. Joint deals are
sampling inputs, not information available to a policy. Increased averaging
work is reported and bounded; it does not call the costly frontier. Temporary
worker maps share the trainer's budget and all error paths join workers.

## Alternatives Considered

- Full average pass with existing asynchronous regrets: smaller and fixes
  the observed averaging bias, but leaves a sweep without a coherent profile
  and does not meet the chosen reproducibility/correct-update goals.
- Serializing whole regret sweeps: simple but removes the useful parallel
  frontier computation; it still needs an independent correct average pass.
- Opponent-node simple averaging: established for heads-up external sampling,
  but not generally unbiased for the project's 3..10 seat contract.
- Importance-corrected sampled averaging: possible only with proper positive
  sampling support and all players' corrections; zero-support repair and deep
  path variance add unnecessary complexity for this bounded materialized tree.
- Latest-policy fallback or weakened quality gates: rejected because they
  hide the defect rather than correct the time-average semantics.
- Independent worker policies merged at completion: not the same update
  algorithm and needlessly forfeits shared learning.

## Risks

- Larger batches have fewer update epochs for a fixed sample count. Measure
  quality versus elapsed time and disclose both counts, not just samples/s.
- Full-action averaging visits additional rows and may reach a byte/visit
  cap sooner. Enforce limits before allocation; report the added cost.
- Chance-sampled averaging has variance and abstraction limitations; native
  fixtures verify its mechanics, not a complete poker equilibrium.
- Thread rendezvous and exceptions can deadlock if ownership is unclear.
  Use explicit lifetime/stop rules and timeout-bounded native regression tests.
- Artifact revision mistakes can poison resumes; test old/unknown identities
  and split-versus-uninterrupted revision-two batch-one training.

## Verification Plan

Use native tests for independently enumerated average contributions including
zero-opponent reach and separate own reach; frozen-profile delta and one-time
clipping; deterministic identical fixed-width repeated runs; one-worker/serial
raw parity; final partial batch; worker errors; strict global visit, byte,
information-set, depth, and wall caps; revision/provenance and old/unknown
checkpoint refusal; and resumed batch-one equality. Keep RFC 0015's exact
known-range quality thresholds and seeds for both two and three seats.

Run the complete project release build/CTest, benchmark-multistreet, Node,
Protobuf, replay, documentation/RFC gates and focused ASan/UBSan. Record
bounded serial and eight-worker measurements after correction, including
samples, epochs, width, startup, cost counters, and quality limitations.

## Rollout Plan

1. Obtain independent design approval and finalize identity mechanics.
2. Implement frozen-delta training and independent full-action averaging with
   native mathematical/resource tests; preserve existing quality thresholds.
3. Implement revision-aware artifact writers/readers and checkpoint refusal.
4. Run focused/full gates, bounded measurements, and independent implementation
   review; update current design/benchmark documents and RFC evidence.

Stop on legality, numeric, resource, identity, quality, or worker-lifetime errors.

## Rollback Plan

Revert the trainer and associated identity changes together, then rebuild.
Do not rewrite artifacts or blend raw revisions. Keep revision-two output
separate from existing generation paths; an older trainer must not resume
revision-two raw checkpoints. Rollback is not a quality endorsement of the
known-defective revision-one average.

## Open Questions

None. Bounded cost and quality measurements remain implementation gates.

## Acceptance Criteria

- Independently checked average and frozen-delta tests pass without relying
  on the trainer implementation as their oracle.
- All unchanged singleton-range serial/parallel quality gates pass across
  seeds 1, 17, 43, widths 1, 2, 4, and repeated runs.
- Fixed-width repeated runs are identical; width one equals serial training;
  counts and partial batches match documented sample/epoch semantics.
- All temporary and retained training storage respects native resource gates;
  worker failures produce no result or abandoned threads.
- Revision-one immutable policies retain old identity and values; incompatible
  checkpoints refuse; compatible warm starts match uninterrupted training.
- Full project/focused sanitizer verification and independent implementation
  review pass, with bounded cost evidence and honest remaining limitations.
- No schema, wire, hidden-information, live-source, or unrelated solver change.

## Decision

Final implementation review: Approved by `/root/approve_frontier_rfc` on
2026-10-13, with no unresolved blockers. Independent average/update oracles,
zero-opponent-reach coverage, partial-batch mathematics, serial/one-worker
parity, deterministic repeated fixed-width results, strict shared resources,
worker failure cleanup, artifact identity, and compatible/incompatible resume
tests pass. The unchanged 2,000-sample singleton-range gates cover seeds
1/17/43 and widths 1/2/4; measured gains and their limited game scope are in
`docs/development/benchmarking.md`.

All 88 release tests have PASS evidence: an original full run passed 86/88,
then the two functional fixture adaptations passed focused reruns. The
complete resident suite freshly generated its artifacts and passed all 21
cases. Eight relevant Clang Debug ASan/UBSan tests passed, including a rerun
after correcting a test-only future lifetime bug. Node 127/127, protocol
25/25, multistreet, documentation/RFC, and diff gates pass. Replay ran but
found zero decisions and supplies no historical validation. These results
are not one final all-green release invocation or a full-poker certificate.
The final release rebuild and all 11 affected smoke tests also passed
(75.18 seconds for the smoke suite).
The corrected average creates denser policies and can hit existing budgets
sooner; production caps and strict refusal are unchanged. Functional fixtures
retain their original games/ranges and strict assertions while using fewer
samples; independent quality gates were not reduced. No old immutable policy
was rewritten or retroactively certified, and no live/full-scale campaign ran.

Author agent: Codex /root
Approved by: /root/approve_frontier_rfc
Decision date: 2026-10-13
Review outcome: Approved
Reviewed scope: All 18 sections of the current proposal, native trainer and
quality/zero-opponent-reach fixtures, artifact identity, checkpoint and
flop-library cleanup paths, and overlapping RFC 0009/0012/0014/0015 contracts.
Review summary: No unresolved blocking findings. The reviewer verified the
opponent-reach average bias, inconsistent asynchronous profile, old artifact
relabeling, lost checkpoint identity, and destructive resume-refusal cleanup
as actual defects. Frozen master/local deltas, full-action chance-sampled
averaging, whole-batch mean then single clipping, strict temporary memory
accounting, revision-aware provenance and preflight are approved as a
proportionate correction. Sample/epoch and final-tail semantics are explicit.
Remaining risks require unchanged quality gates, independently checked
average/update tests, thread/resource failure tests, and bounded performance
evidence. Compatible resume guarantees numerical regret/sum/policy equality,
not cumulative runtime counters absent from checkpoints. Rollback must isolate
revision-two raw state because old binaries cannot enforce the new guard.
Approval is design-only, not a convergence certificate or verification result.
