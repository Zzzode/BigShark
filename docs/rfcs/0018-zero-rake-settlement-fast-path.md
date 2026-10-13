---
rfc: "0018"
subject: "Zero-Rake Settlement Fast Path"
status: "Implemented"
authors: "Codex /root"
created: "2026-10-13"
updated: "2026-10-13"
owners: "engine/poker, engine/tests, engine/benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0018: Zero-Rake Settlement Fast Path

## Summary

Skip the percentage-rake calculation when a validated policy has zero basis
points or zero cap. Preserve native settlement as the single payout engine,
all validations, exact integer outputs, and training identity. Verify the
shared change with native payout/security tests and matched end-to-end runs.
Defer request caches and broader extraction representations until this smaller
measured hotspot is removed and profiling establishes their value.

## Motivation

The user authorized another training-performance round. A fresh native
16-sample, batch-eight profile recorded 15,744 CPU-clock samples with no lost
samples across setup, training, and publication. `multiply_divide` accounts
for 14.00 percent self samples; the call graph places it predominantly under
native showdown settlement in template extraction. This is hotspot evidence,
not a training-wall-time attribution or promised speedup.

The user-provided 1K validation artifact's actual manifest reports 838.179
training seconds, 1,018.24 pipeline seconds, and 10,717,364,224 peak RSS bytes.
Those values do not establish a six-hour 100K run or a cache-thrashing cause.
Current extraction calls native settlement repeatedly. Zero-rake games still
run 53 rounds of integer multiplication/division at each positive-pot showdown.

## Goals

- Eliminate percentage arithmetic when the declared formula is identically
  zero, with no new allocation, cache, public type, or alternate settlement.
- Preserve every settlement field, validation/exception, integer bound, layer,
  odd-chip order, sample, raw/policy result, and artifact byte on matched runs.
- Demonstrate lower median extraction/training/end-to-end cost on at least
  three identical before/after native builder runs; report phase variability.
- Retain all native quality/resource gates and complete independent review.

## Non-Goals

- Changed rake rules, integer numeric profiles, rounding, payout/side-pot
  semantics, algorithms, worker/batch width, budgets, ranges, or abstractions.
- Faster wide-integer division for nonzero rake, custom payout logic inside
  extraction, new evaluator/public APIs, result caches, or streaming storage.
- A new scheduler, a full Oracle/library rebuild, 100K training, live play,
  external spending, promotion, or poker-quality certification.

## Current State and Evidence

- `engine/src/poker/settlement.cpp` validates rule IDs, rate bounds, cap bounds,
  chips, player identities, pot layers, eligibility, and required scores before
  computing rake. The final accounting validates all chip conservation laws.
- Before this RFC, its condition checked only positive pot and no-flop-no-drop. It called
  `multiply_divide(pot, basis_points, 10000)` even if rate or cap is zero.
- `RakePolicy` explicitly defines cap zero as no permitted rake. RFC 0006's
  formula is `min(cap, floor(pot * basis_points / 10000))`, hence either
  zero input suffices to make rake zero for all validated pots.
- `GameState::award` calls this same settlement implementation. Trainer,
  extractor, concrete/template oracle, and river paths must agree on payouts.
- Existing `test_settlement` independently enumerates contribution layers,
  tests every rate 0..10000, rate/cap/no-flop combinations, numeric boundaries,
  and exhaustive 2..6-seat plus sampled 7..10-seat contribution profiles.
- Comparable patterns are existing zero-pot/no-flop guards in this function
  and immutable calculation reuse in RFCs 0015/0017. No dependency is needed.

Overlapping contracts reviewed: RFCs 0006, 0013, 0014, 0015, 0016, and 0017.

## Design Principles

- Simplify a proven no-op inside its owning module.
- Preserve centralized chip conservation and all rejection behavior.
- Prefer a guard over an unmeasured caching/representation architecture.
- Keep cumulative CPU/phase measurements separate from wall time and quality.

## Proposed Design

Extend the existing rake-calculation condition with positive `basis_points`
and positive `cap`. Keep its location after all input and layer validation.
`ContributionSettlement::rake` already initializes to zero; for excluded cases
continue through normal awards, stacks, utility, and final conservation checks.
Do not add an early function return. The nonzero-rake calculation and its
53-bit exact multiplication/division fallback remain byte-for-byte unchanged.

Add explicit native zero-rate/zero-cap equivalence fixtures covering full
result fields, folded contributions, tied/odd-chip awards, refunds, near-limit
pots, and invalid inputs that must still throw. Keep the existing exhaustive
and nonzero-rake tests unchanged. Capture their baseline before the production
guard, and use the existing preflop builder for end-to-end performance/equality.

### Shared-change gate

The owner is `bigshark_poker`, not a benchmark or extraction adapter. The
shared change is only the existing calculation guard. Local extraction
substitution would duplicate payouts, while a complete-result cache needs
ledger identity, storage, and unproved reuse. All settlement callers are
affected by the implementation but no caller contract changes. Regression
risk is accidentally skipping validation or nonzero rake; retain guard order,
native exhaustive/security coverage, sanitizer checks, and same-output runs.
The smallest viable shared change is two conjuncts. Rollback removes them and
rebuilds through root presets without any artifact/data migration.

## Dependency Rules

The change remains in `bigshark_poker`, using existing value types. Poker does
not depend on solver, tree, artifacts, benchmarks, or stage6. Native tests own
verification. The existing offline builder supplies the benchmark workflow;
no new public cache or standalone disposable validation script is added.

## Compatibility and Migration

No API, ABI, configuration, numeric profile, wire, persisted format, training
revision, or sampled policy meaning changes. Existing policies/frontier tables
remain read-only inputs. Same-build completed fixed-input runs must produce
byte-identical SQLite artifacts. Deadline-capped runs may finish more samples
because they are faster; they are not fixed-sample equality evidence.

## Security and Operational Impact

All policy/chip/layer/score/rule checks remain before the new guard, and all
final conservation checks still execute. Zero rate/cap does not excuse invalid
policy IDs, overflowing caps, unmatched chips, or missing contested scores.
Offline profiling only; no credentials, network, live controllers, or budgets
are changed. Benchmarks use fresh temporary directories and fixed snapshots.

## Alternatives Considered

- Request-local bucket/result caches: potentially useful but evaluator self
  samples are only 2.79 percent here; extra identity/storage is not the first
  proportionate change. Profile again after removing this no-op.
- Skip settlement in the extractor or implement a heads-up payout shortcut:
  duplicates a shared rules invariant and expands compatibility risk.
- Add native 128-bit arithmetic or a generic division abstraction: unnecessary
  for the verified zero case and affects nonzero-rake arithmetic.
- Increase policy cache/batch width: cannot remove this calculation; width
  also changes revision-two update epochs and quality semantics.

## Risks

- Moving the guard before validation: explicitly forbidden; test malformed
  inputs under both zero-rate and zero-cap policies.
- Treating cap zero as unlimited or omitting a nonzero case: keep the RFC 0006
  formula, exhaustive rate grid, cap/no-flop cases, and large-integer tests.
- Overstating a short-run profile: disclose whole-process sampling, immutable
  legacy frontier provenance, cache/host noise, and no full-run certification.

## Verification Plan

Root release configure, format, full build/CTest, benchmark-multistreet, npm
check, direct proto:check, replay, docs/RFC checks, and focused Clang Debug
ASan/UBSan for settlement and its relevant native callers. Reuse independent
chance/payout oracles, unchanged parallel result fingerprints, and quality
gates. Replay with zero decisions supplies no historical coverage.

Measure three before/after runs of 16 samples, two epochs, batch eight, 100 BB
heads-up identity, seed 20261003, 128 cache entries, existing 1 GiB policy and
4 GiB template caps, same local FDT/NSP snapshot, and fresh output directories.
No builds/full suites overlap measurements. Report medians/ranges, training,
extraction cumulative time, full pipeline/shell time, RSS, accounted bytes,
counts, complete SHA-256, and byte comparison. Do not reset the OS cache.

## Rollout Plan

1. Profile the unchanged builder, propose this smaller owner-local change, and
   obtain independent approval before production/test implementation.
2. Add native coverage and capture unchanged baseline through root presets.
3. Apply the guard and measure the final binary on the identical workload.
4. Run full project/sanitizer gates, independent final review, and update
   current design/benchmark/status documents with limitations.

Stop or revert the guard on any numeric, validation, identity, or quality drift.

## Rollback Plan

Remove only the new guard/comment and rebuild. Preserve earlier RFC worktree
changes and independent test coverage. Existing immutable artifacts need no
migration, deletion, or relabeling. Discard speculative optimizations that do
not show proportionate measured value.

## Open Questions

None. Actual gain is a measurement gate, not a forecast.

## Acceptance Criteria

- Explicit full-field zero-rake fixtures and invalid-input rejections pass;
  existing integer/exhaustive/nonzero-rake tests remain unchanged and pass.
- Complete fixed-input results, accounting, algorithm identity, and artifacts
  remain identical; all three final outputs match the baseline bytes.
- Three matched before/after runs show lower median extraction, training, or
  whole-process cost without a material unexplained memory regression.
- Full native/project gates, relevant true ASan/UBSan, independent final
  approval, current documentation, and both index updates are complete.
- No changed contract, approximation, data rewrite, live operation, or full
  training campaign; unresolved limits are documented honestly.

## Decision

Author agent: Codex /root
Approved by: /root/profile_average_storage
Decision date: 2026-10-13
Review outcome: Approved
Reviewed scope: All 18 sections, RFC process, RFC 0006 rake contract,
RFC 0015/0016/0017 invariants, native settlement and its complete tests,
GameState/extractor callers, and the read-only CPU profile.
Review summary: No blocking findings. Two positive guards after all input,
layer, and contested-score validation preserve the exact declared formula,
all outputs/rejections, native centralized payouts, nonzero-rake arithmetic,
and final conservation checks. No public contract, budget, dependency, or
training identity changes. The reviewer independently verified zero lost
CPU-clock samples and 14.00 percent helper self samples, with 13.48 percent
under native settlement/template extraction. Implementation must cover each
independent zero condition, complete result fields and numeric boundaries,
unchanged malformed-input checks, three matched full-byte-equal measurements,
and native/sanitizer gates. CPU sample share is not a wall-time speedup claim;
this is design approval only, not full-run performance/quality certification.

Implementation evidence:

- Root owns only the guard/comment in `engine/src/poker/settlement.cpp` and
  current documentation/index updates. Worker `/root/compact_training_deltas`
  owns only the native additions in `engine/tests/test_settlement.cpp`; root
  reviewed and integrated its full-field literal fixtures and typed rejections.
  The old exhaustive/rate/nonzero tests remain unchanged. No code in solver,
  storage, protocols, adapters, or earlier dirty worktree changes was altered
  by this RFC.
- New tests passed against unchanged production in 4.73 seconds before the
  guard. Final focused release `settlement`, `nseat_parallel`, and
  `postflop_frontier` passed 3/3 in 5.43 seconds, including the twelve pinned
  complete pre-optimization raw/policy/scalar fingerprints.
- Three clean before and three final after native runs retain full file bytes,
  SHA-256, raw/sealed accounting, counts, and algorithm identity. All three
  paired `cmp` checks passed. Current complete tables, exclusions, and timing
  limits are in `docs/development/benchmarking.md`: training median
  14.00 -> 11.83 seconds (15.50 percent), cumulative extraction
  52.33327 -> 42.53221 seconds (18.73 percent), whole process
  35.963 -> 33.734 seconds (6.20 percent), and essentially unchanged RSS
  (+0.12 percent). Publication shows no independent improvement.
- A separate final profile records 13,992 CPU-clock samples, no losses, and
  no helper self samples. Allocation/free, settlement, transitions, and row
  lookup remain candidate costs, not optimizations implemented here.
- Release configure/format/full build succeeded (97 targets). Focused Clang
  14 Debug ASan/UBSan was rebuilt with sanitizers ON. `npm run check` passed
  127 Node and 25 protocol tests, and `benchmark-multistreet` passed all three
  unchanged quality cases. Direct `npm run proto:check` passed 25/25. Replay
  exited successfully with zero decisions, supplying no historical coverage.
- Final full release CTest passed all 88 tests in 360.67 seconds, including
  native payout/chance oracles, parallel result fingerprints, strict resource
  limits, resident policy, heads-up quality, artifact, and integration checks.
- Focused true ASan/UBSan CTest passed settlement (72.11 seconds), parallel
  trainer (34.98), game definition (9.29), and frontier (0.71). Its template
  test reached the existing 180-second watchdog after all six independent
  chance oracles and the first three structural buckets passed, without a
  numerical/sanitizer error. That CTest invocation remains an incomplete test,
  not a pass. The same final preset-built
  `./build/asan/engine/test_postflop_templates` then passed directly, completing
  all six independent chance oracles and all six structural buckets with exit
  zero. Structural bucket times were 14.864, 57.412, 89.075, 127.687, 37.687,
  and 77.250 seconds. Every assertion and production resource bound remained
  unchanged; no CMake/test-timeout/configuration or workload was reduced. The
  default sanitizer CTest watchdog remains a verification-workflow limitation,
  not an unresolved numeric or sanitizer failure.

Implementation reviewer: /root/profile_average_storage
Implementation review date: 2026-10-13
Implementation review outcome: Approved
Implementation review summary: Independent read-only review confirmed the
two positive guards preserve validation order, exact native payouts, nonzero
rake, conservation, and all public contracts. Full-field literal fixtures and
unchanged exhaustive/nonzero tests satisfy the coverage requirement. All 88
release tests pass. The same final Debug sanitizer template binary completed
all six chance oracles and structural buckets after the documented CTest
watchdog; this is not claimed as a single 5/5 CTest pass. All three byte
comparisons, fixed identities/counts/accounting, matched measurements, and
project gates satisfy acceptance. No unresolved blocking finding remains.
Bounded workload, uncontrolled host/cache noise, unchanged RSS/cache behavior,
zero historical replay decisions, and no 1K/100K certification remain disclosed.

Files changed by this RFC: `engine/src/poker/settlement.cpp`,
`engine/tests/test_settlement.cpp`, `docs/design/gto-engine.md`,
`docs/development/build-and-test.md`, `docs/development/benchmarking.md`,
`docs/README.md`, `docs/rfcs/README.md`, and this RFC. Final audit confirms no
other implementation edits, generated-source noise, speculative abstraction,
public contract change, or alteration of earlier unrelated worktree changes.
