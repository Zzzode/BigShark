---
rfc: "0015"
subject: "Offline Frontier Evaluation Reuse and Measurement"
status: "Implemented"
authors: "Codex /root"
created: "2026-10-13"
updated: "2026-10-13"
owners: "engine/gto, engine/benchmarks, engine/tests"
supersedes: ""
superseded-by: ""
---

# RFC 0015: Offline Frontier Evaluation Reuse and Measurement

## Summary

Reuse one immutable postflop evaluation template per structural configuration,
cache only board-specific policies with bounded memory and loading concurrency,
and remove temporary allocations from exact root-value extraction. Correct the
offline preflop quality tool to learn one action per information set and evaluate
that frozen deviation on independent samples. All work is offline. The existing
betting rules, action/card abstractions, CFR updates, frontier values, and file
formats remain unchanged.

## Motivation

Commit `b154533` already provides cursor parsing and serialized terminals. A
recorded eight-thread, 100-iteration run took 153.065 seconds. This short run is
not a steady-state throughput guarantee. The on-demand evaluator still retains
and reloads a full tree for every canonical flop and SPR bucket. Its quality
tool divides each seat's value by the combined seat sample count and chooses
responses separately for each complete hidden deal. Both defects prevent the
reported quality from supporting performance acceptance.

## Goals

- Preserve exact policy-following values, including chance conditioning and
  uniform behavior at unsealed postflop information sets.
- Construct or load at most one evaluation template for each of the six current
  postflop structural configurations during an evaluator's lifetime.
- Bound resident policies, in-use evicted policies, and transient loading work.
- Produce reproducible, independently confirmed legal deviation estimates with
  correct per-seat normalization and explicit coverage and uncertainty.
- Add native correctness, malformed-input, concurrency, and performance evidence.

## Non-Goals

- Positive-SPR all-in-equity substitution, changed SPR buckets or ranges, new
  card abstractions, neural values, or reordered CFR sampling/updates.
- A generic tree-view framework, public poker settlement redesign, or a second
  tree builder or strategy implementation.
- New ATF/NSP/BSF versions, legacy-format restoration, artifact regeneration,
  large training jobs, cloud spending, or live poker promotion.
- Claiming a true best-response certificate or a full-game equilibrium.

## Current State and Evidence

- `postflop_frontier.cpp` caches a separate `NSeatPolicy` and `AbstractTree` per
  `(canonical_flop, spr_bucket)`, scans all entries for eviction, and permits
  duplicate concurrent loads before its insertion double-check.
- `make_postflop_def` fixes every structural field except the three flop cards
  within one SPR bucket; the ordered action menu does not depend on card ranks.
- `root_value_extractor.cpp` uses runtime board-only card ordinals rather than
  the stored incoming-card labels. It needs node indices, actions, children,
  fold utility, and live-seat order. The root state comes from the policy game.
- The public `AbstractTree` includes a concrete game definition, card labels,
  and terminal boards. It cannot represent another board's complete game.
- `preflop_nash_conv.cpp` currently takes a separate maximum for each complete
  joint deal and divides each seat's sums by `deals * seats - uncovered`.
- Existing root-value cross-checks are river-rooted. The codec roundtrip has
  only a valid heads-up case. Native tests do not call `train_nseat_parallel`.
- `stage6/br_estimator` already separates learning and independent confirmation
  and pools information sets. Its complete-game API is not the preflop tool's
  frontier/tree contract; its methodology is the reference, not a new dependency.

## Design Principles

- Reuse immutable structure without changing the identity of a concrete tree.
- Keep policy keys, action order, doubles, missing-row behavior, and settlement.
- Place new representations only in the module that needs them.
- Treat measured cost as evidence; do not multiply speculative speedups.
- Fail a measurement on invalid input instead of selecting successful samples.

## Proposed Design

### Solver-owned evaluation template

Add a narrow `PostflopEvaluationTemplate` in `engine/gto`. It privately owns an
ordinary representative `AbstractTree`, initially using canonical flop
`{0,1,2}`. It exposes only the extraction projection: stable node indices,
kinds, actor/action menus, ordered children, fold utility, and live-seat order.
It does not expose representative board/card labels as request data. This
avoids a second builder and a large storage rewrite. Compact storage is deferred
until six representative trees themselves are a measured bottleneck.

An evaluator lazily obtains one template per bucket. Template initialization is
serialized, retries after failure, and counts the aggregate retained bytes.
Configuration equality excludes only the flop card values; it includes board
size, all chip fields, button, seat count, terminal depth, and action identity.
Each evaluation supplies its policy game, exact relabeled deal, and runtime
state. Chance children are addressed by the ascending board-only ordinal before
blocked cards are skipped. The template is not passed to the CFR trainer or
generic policy lookup as if it were that board's `AbstractTree`.

One extraction kernel serves concrete trees and templates. It uses fixed
seat-capacity arrays for internal utility accumulation and spans for boards,
avoiding a vector allocation at each recursive node. A request constructs its
hole-card blocker mask once. Native L1 transition and settlement functions stay
the source of truth. No probability truncation or approximate leaf is added.

### Policy cache and loading limits

Keep the existing entry-capacity argument as an upper bound. Add solver-owned
limits with defaults: 1 GiB for resident/in-use policy accounting, 64 MiB for
one policy load including its buffers, two concurrent policy loads, and 4 GiB
for all retained templates. A policy codec load receives an optional byte cap;
the cache always supplies it. Its estimator conservatively counts map-node
overhead and vector capacities. Raw/compressed buffers, headers, row counts,
action counts, duplicate keys, and probability distributions are validated
before allocation or publication. File bytes and format are unchanged.

Per-key single-flight loading shares successful results and exceptions among
current waiters. Failure removes the flight so a later repaired file can retry.
Loading and extraction never run under the cache mutex. Memory reservations
are acquired before loading and converted to measured retained policy charges
after loading. In-use evicted objects retain their charge until the last owner
releases them. Loading reservations and resident objects share the policy
budget, so eviction alone cannot conceal live memory. The configured budget
must accommodate at least one load. Bounded load concurrency covers transient
decompression work. The evaluator assumes its input policy directory is fixed
for the run; replacing artifacts requires a new evaluator instance.

The builder gains an additive `--cache-bytes N` option in bytes. It reports
cache hits/misses, successful policy/template loads, accounted policy/template
bytes and peaks, and load/extract elapsed totals. Existing positional arguments
and `--cache-capacity` remain valid. No compressed L2 cache is introduced.

### Preflop deviation measurement

Keep the existing executable and positional arguments, but replace the invalid
hidden-deal maximum. A benchmark-private helper operates on the existing
flop-terminal preflop tree, fixed profile-query function, and selected frontier.
Each seat learns a response from `deals` worlds and confirms it on a distinct
set of `deals` worlds. Domain-separated seeds identify both sets. Legal flop
samples are fixed per world and reused across alternative preflop actions;
opponent draws are keyed by world, public node, and purpose.

Learning traverses batches of worlds bottom-up. At opponent nodes it samples
the frozen profile and groups worlds by the chosen child. At responder nodes
it evaluates every child, pools child utility by the existing information key
(own cards, public board, complete public action path), and freezes one argmax
for that key. The preflop tree has no merged histories, so descendant actions
are frozen before ancestor actions and each key belongs to one public node.

Confirmation never changes that response. Unknown response keys use the
original profile and increment a response-coverage counter. Original profile
misses retain the existing check/call/fold behavior and have a separate count.
Each seat's profile and deviation values are averaged over its own confirmation
worlds. Paired differences yield signed gains and standard errors; no sample is
silently dropped and gains are not clipped. Invalid distributions, menu drift,
or a missing/erroring frontier abort the entire measurement.

Output is explicitly a frozen deviation-gain estimate, not exact NashConv or a
certificate. It includes per-seat values/gains, the sum, the existing average
per-seat chip/BB convention, confirmation uncertainty, seeds, sample counts,
profile fallback counts, response misses, and frontier identity. At 3+ seats
the average is labeled average deviation gain, not two-player exploitability.
Historical per-deal-max numbers cannot be compared to this estimator. Existing
CLI invocation remains valid; its misleading metric labels are corrected.

### Parallel-training verification

Add native calls of the existing parallel trainer for a small enumerable game,
checking finite normalized policies, exact iteration/visit accounting, serial
one-worker parity, resource and exception behavior, and quality under multiple
seeds. Document that multi-worker lock acquisition is scheduling-dependent;
fixed iteration assignment alone does not promise identical policies. Do not
change update scheduling in this RFC or assert unchanged convergence theory.

## Dependency Rules

- Template/extraction/cache code remains in `bigshark_solver`, depending on
  existing L1/L2/L3 components. None of those components depends on the template.
- The measurement helper is private to native benchmark/test targets and uses
  public solver/artifact types. It is not linked into the live decision path.
- Loader hardening stays in its existing tree/solver codec owners. No generic
  cache, view, or allocator is moved into shared libraries.

## Compatibility and Migration

No persisted file, Protobuf, NDJSON, resident lookup, or live strategy changes.
The current ATF format stays the only accepted tree format. Existing policy
files load with their declared board/configuration and unchanged probabilities.
No files are rewritten. Template reuse replaces the per-flop runtime tree path
after equivalence tests; there is no second selectable strategy implementation.
Quality-tool labels/numbers intentionally change to correct invalid claims.

## Security and Operational Impact

All operations use local offline artifacts, without tokens or network calls.
Tree/policy loading rejects malformed and over-budget data before indexing or
large allocation. Resource measurements are conservatively accounted payloads,
not a promise about total process RSS; training tables, allocators, and the OS
page cache require separate headroom. No long training run is automatically
authorized by RFC acceptance.

## Alternatives Considered

- Sharing a public representative `AbstractTree`: rejected because its concrete
  board/card identity would be false for other requests.
- Immediately rewriting flat trees or ATF records: deferred; local reuse
  removes the repeated cost without a format migration.
- A compressed L2 cache or complex replacement policy: deferred until profiling
  identifies a cost not already covered by the OS cache and policy residency.
- Positive-SPR equity or changed CFR batching: rejected for this change because
  either changes the model/update semantics rather than representation cost.
- Retaining per-deal maxima or only fixing the denominator: rejected because
  responder actions would still depend on hidden opponent holdings.
- Copying the complete stage6 estimator: unnecessary; the public preflop tree
  allows direct bottom-up information-set pooling with held-out confirmation.

## Risks

- Node-index alignment: compare complete structural projections and outputs
  across boards, not merely node counts. Refuse mismatched action menus.
- Live memory after eviction: tie charges to object lifetime and include load
  reservations; test concurrent callers and failure/retry paths.
- Sampled-response coverage: disclose missing keys and uncertainty; a low
  measured gain does not certify that a better response cannot exist.
- Scheduling-dependent CFR policies: test quality, correct misleading
  reproducibility prose, and retain single-worker deterministic evidence.

## Verification Plan

Use CMake presets and native CTest tests. Add malformed tree/policy and exact
resource-bound cases; template/concrete-tree projection and root-value
equivalence; independent flop/turn chance oracle fixtures including blockers
and missing rows; concurrent cache reuse, eviction, and failure recovery; hidden
opponent-card response counterexamples and normalization/confirmation tests;
and parallel trainer tests. Run focused ASan/UBSan on memory/lifetime changes.
Run the full release build, CTest, benchmark-multistreet, npm check, proto:check,
replay, documentation, and RFC gates required by `AGENTS.md`.

Record repeated bounded native throughput runs with a fixed policy snapshot,
seed, thread count, elapsed time, and cache counters. Include startup costs.
Do not gate on an unmeasured speedup or automatically run 100K iterations.

## Rollout Plan

1. Harden loaders and add regression tests.
2. Correct quality measurement and establish native independent fixtures.
3. Introduce templates, bounded policy caching, and allocation-light extraction.
4. Add concurrency/parallel tests, instrument and measure real bounded runs.
5. Complete native gates, independent implementation review, and current docs.

Stop on value, legality, information-set, or memory-bound violations.

## Rollback Plan

Revert the affected code change and rebuild; existing artifacts require no
rollback or migration. Do not restore the invalid quality estimator as an
acceptance gate. Preserve verification fixtures and recorded measurements.

## Open Questions

None. Cache tuning and longer training outcomes are measurements after the
bounded implementation, not prerequisites or promised quality outcomes.

## Acceptance Criteria

- Native malformed-input/resource tests pass under release and sanitizers.
- Template projections and values match concrete-board trees and independent
  chance fixtures within `1e-9 * max(1, root pot)`.
- One template per current bucket is initialized successfully at most once;
  concurrent repeated requests perform one policy load per key/flight.
- Resident/in-use policies plus load reservations stay inside the configured
  accounted-byte budget, and failures release waiters/reservations.
- Measurement tests demonstrate one action per information set, independent
  confirmation, correct per-seat means, explicit coverage, and signed estimates.
- Parallel native tests and all project release/sanitizer gates pass.
- Bounded throughput evidence and its limitations are recorded in current docs.
- Independent implementation review resolves material findings; no live or
  persisted-format changes are introduced.

## Decision

Final implementation evidence (2026-10-13), replacing the earlier failing
parallel-gate progress record: all 88 release tests have PASS evidence. The
full run passed 86/88; the two resource-exhausting functional fixtures were
adapted without changing games, ranges, production caps, or strict success
assertions, then `subgame_resolver` and the complete `resident` suite passed
in 53.76 and 436.86 seconds. This is aggregate full-suite plus focused-rerun
evidence, not one final all-green invocation. Eight focused tests passed with
actual Clang Debug AddressSanitizer/UndefinedBehaviorSanitizer instrumentation;
the cache test's future-lifetime defect was fixed and rerun successfully.
Node tests passed 127/127, protocol tests 25/25, and the multistreet benchmark,
documentation/RFC checks, and diff checks passed. Replay exited successfully
but found zero decisions, so no historical replay coverage is claimed.
The final release rebuild succeeded; all 11 affected smoke tests passed in
75.18 seconds after that rebuild. Existing unrelated compiler warnings remain
in the artifact codec and stage-six evaluator; no warning cleanup was included.

Independent implementation reviewer `/root/approve_frontier_rfc` returned
final Approved after resolving the cache-publication wakeup and native-path
terminal-validation findings. Measurement/CAS reviewer
`/root/review_cache_evidence` found no blockers; its conditional-reach fixture
was added. The user-authorized RFC 0016 separately fixes the serial/parallel
averaging defect and its unchanged quality gates now pass. Bounded cost
evidence, the denser-policy resource regression, and fixture/throughput
limitations are recorded in `docs/development/benchmarking.md`. None of this
certifies full poker equilibrium, old immutable policies, or long-run quality.

Author agent: Codex /root
Approved by: /root/approve_frontier_rfc
Decision date: 2026-10-13
Review outcome: Approved
Reviewed scope: All RFC 0015 sections; overlapping RFC 0004, 0008, 0013,
0014 contracts; cache, extraction, policy codec, information keys, stage6
measurement methodology, and current native tests.
Review summary: No unresolved blocking findings. The reviewer approved
information-set pooling with independent confirmation, a private structural
projection rather than false concrete-tree identity, lifetime-accounted
single-flight caching, bounded local ownership, explicit estimate/coverage
labels, and scheduling-aware parallel tests. Implementation must enforce
template caps before allocation, include simultaneous policy decode buffers,
account for covariance in summed confirmation uncertainty, and verify template
equivalence. Approval is architectural only; no performance or quality result
was asserted and native release/sanitizer and implementation-review gates remain.
