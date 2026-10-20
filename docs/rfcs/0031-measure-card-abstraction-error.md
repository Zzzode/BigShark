---
rfc: "0031"
subject: "Measure Postflop Card-Abstraction Error Before Changing It"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-20"
updated: "2026-10-20"
owners: "engine abstraction, engine solver, engine flop library"
supersedes: ""
superseded-by: ""
---

# RFC 0031: Measure Postflop Card-Abstraction Error Before Changing It

## Summary

The postflop card abstraction is the coarsest component in the strategy and has
never been measured. The instinctive next step — a finer bucketing scheme — is
currently neither supported nor refuted by evidence, because nobody has
segmented the shipped library correctly. **57.60% of its 1,642,680 public nodes
are entirely untrained** (every bucket row at the node is uniform). The
remaining 696,500 nodes carry **2.93 distinct probability vectors per node and
1.52 bucket rows per distinct vector** — that is, at the nodes where anything
was learned, adjacent buckets do differentiate. Whether a finer scheme is worth
its cost therefore depends on whether the untrained majority is a consequence of
the abstraction or of the training budget. This RFC builds the measurement that
answers that, and adopts a new scheme only if the measured abstraction loss
justifies it.

## Motivation

`Preflop169` falls back to `CategoryTiersV1` at any non-empty board
(`engine/src/abstraction/abstraction.cpp:287-292`), so every postflop artifact
is keyed by 3 to 7 made-hand categories. Two hands in the same bucket can have
completely different draws: `QhJh` and `QsJs` both bucket identically on
`AhKc2c` because `strength_bucket` evaluates only `board.size()+2` cards
(`:258-274`) and returns a pure made-hand score.

The instinct is to make buckets finer. The evidence neither confirms nor
refutes it, and the two answers come from the same artifact depending on how it
is segmented. Measured over
`artifacts/flop-library-6way-fullrange/library.bin` (sha256 `b67e3cb3…20b4`):

| Statistic (definition) | All 1,642,680 nodes | The 696,500 trained nodes |
| --- | --- | --- |
| Distinct probability vectors per node | 1.8214 | **2.93** |
| Bucket rows per distinct vector | 2.4503 | **1.52** |
| Nodes where every bucket row is uniform | 57.60% (946,180) | 0% by construction |

The aggregate figures are dominated by the untrained majority: a node nobody
trained has one distinct vector shared by its 4.44 rows, which drags both
aggregates toward "over-segmented". At the nodes that were actually trained,
adjacent buckets do differentiate — 2.93 distinct vectors for 4.44 rows.

So the honest statement is: **the library is over-segmented where it was not
trained, and differentiated where it was.** A finer abstraction would add rows
to both populations. Whether that is worth its cost is exactly the question the
measurement in this RFC answers, and it cannot be answered by re-reading the
existing artifact.

Two further rows bound the cost of the finest option:

| Measurement | Value |
| --- | --- |
| Identity bucketing row count | 7,331,423 → 161,518,032 (**22.03x**, derived by remapping each row's bucket to its evaluator score; 98.33 rows per node against a maximum of 1,176 off-board combos, so this is a full-range projection, not a trained-reach count) |
| Library size at Identity, Codebook16 | 188.4 MB → **2.05 GB** (derived estimate, not a measurement) |

`docs/rfcs/0008-unified-n-player-architecture.md:682-683` already requires a
measured abstraction error, and
`docs/plans/0004-0006-implementation.md:1932-1934` admits it "is not yet
measured; it ships only as a declared, currently-unwired bucket." That gap is
four RFCs old. This RFC closes it.

## Goals

- **G1**: A reproducible measurement of the strategy loss attributable to the
  postflop card abstraction, at a named fixture set, with an interval.
- **G2**: An ablation over at least three granularities (tiers, a concretely
  specified intermediate, and identity-or-projection) at two iteration counts,
  so the comparison isolates the abstraction and not the budget.
- **G3**: A decision rule declared *before* the measurement, as explicit
  inequalities over intervals: what gap would justify a finer scheme, and what
  would justify keeping tiers.
- **G4**: The bucket scheme pinned in `build.json`, the sealed checkpoint
  resume check, and the manifest, so a build cannot silently mix schemes.
- **G5**: The baseline statistics reproduced by a committed tool from a pinned
  artifact, so every number this RFC argues from is checkable.

## Non-Goals

- **No bucketing change in this RFC.** Adoption requires the measurement and,
  if positive, a follow-up proposal carrying the format work.
- **No equity/potential-aware bucketing implementation.** It is the leading
  candidate but is measured, not assumed.
- **No action-abstraction change.** Independent; RFC 0030 handles the cap.
- **No claim that tiers are adequate.** This RFC produces the evidence, not the
  verdict.

## Current State and Evidence

### Facts (verified in this repository)

- **There is exactly one lossy postflop scheme.**
  `CategoryTiersV1` returns `(score >> 20) & 0xF`
  (`engine/src/abstraction/abstraction.cpp:281-285`), the made-hand category.
  No draw-, runout-, equity-, or potential-aware bucket exists in the
  production path. The OCHS-style code in
  `engine/src/gto/multistreet_cfr.cpp:89-130` (12 buckets, its own default) is
  a separate experimental solver, not reachable from `card_bucket`, and has
  never been compared against tiers.
- **The production scheme is a compile-time constant.** `kNSeatCardKind`
  (`engine/include/bs/nseat_trainer.hpp:33-34`) is a `constexpr`. The
  `stage6` subtree already plumbs a runtime `card_kind`
  (`engine/src/stage6/mccfr_trainer.cpp:403`,
  `engine/src/stage6/candidate_policy.cpp:433`), but nothing on the
  flop-library, trainer, packer, or resident path can select a scheme without
  editing the constant.
- **Lettered bucket ids are `uint16`.** `BucketEntry`
  (`engine/include/bs/mmap_library.hpp:65`), `find_bucket` (`:238`), the
  resident `0xFFFF` guard (`engine/src/resident/resident_policy.cpp:429`), and
  the writer (`engine/src/resident/mmap_library.cpp:562`). Above 65,535 ids,
  RFC 0026 requires a format change and a new RFC
  (`docs/rfcs/0026-...:129-131,586-589`). The packer's range check
  (`engine/src/resident/mmap_library.cpp:239-243`) additionally hardcodes the
  postflop window `1..9`.
- **Identity is not representable as a library scheme.** Its bucket values are
  evaluator scores up to 10,354,688 (the royal-flush encoding,
  `9<<20 + (14&0xF)<<16`), so it exceeds `uint16`; the sealed checkpoint
  rejects the kind outright
  (`engine/src/artifacts/sealed_checkpoint.cpp:144-156` returns `fail`).
- **The bucket scheme is pinned in the library but not everywhere else.** The
  claim that it is unpinned would be wrong: BSFLIB02 stores the kind at header
  offset 20 (`engine/src/resident/mmap_library.cpp:647-649`) and `open()`
  refuses a mismatch with "card bucket kind does not match the compiled
  engine" (`:1151-1156`). The shipped `library.bin` carries `bucket_kind = 2`.
  Three narrower gaps do remain: `key_kind` is absent from
  `BuildIdentity`/`.sections/build.json`
  (`engine/src/flop_library/flop_library.cpp:497-511,636-644`), it is not
  compared on sealed-checkpoint resume (`:366-372` compares only the
  canonicalization digest), and it is absent from the JSON manifest. A scheme
  change mid-build could therefore mix sections undetected.
- **The finer-token machinery is implemented but production-unwired.**
  `own_card_token`/`OwnCardToken`
  (`engine/include/bs/abstraction.hpp:310-330`,
  `engine/src/abstraction/abstraction.cpp:358-376`) is fully implemented and
  unit-tested (`engine/tests/test_abstraction.cpp:781-916`), but no production
  path calls it.
- **Prior evidence is adjacent, not direct.** RFC 0007:188-196 upgraded
  preflop from two buckets to `Preflop169` on feasibility grounds only. RFC
  0011:901-905 argues the coarse postflop abstraction makes a 2-seat turn
  policy worse than uniform below about 10K iterations — a convergence claim,
  not an ablation. No granularity comparison exists at any seat count or stake.

### Assumptions (to be verified)

- Best-response loss against a fixed opponent model is a usable proxy for
  abstraction quality. It is the standard measure, but it prices a policy
  against one profile rather than estimating equilibrium loss, and G3's
  decision rule must be written with that limitation stated.

## Design Principles

- **Measure before changing.** A four-RFC-old unmeasured requirement is the
  reason the direction is currently guesswork.
- **Ablate one variable.** Fixed iterations, seeds, ranges, and action menu;
  only the bucket function moves.
- **Declare the decision rule first.** A threshold chosen after seeing the
  numbers is not a measurement.
- **A scheme is part of the identity.** If buckets change, the artifact must
  say so.

## Proposed Design

### Measurement

The quantity of interest is the **abstraction gap**: what a scheme costs
relative to the game it approximates. It is a difference of two
best-response values taken in the *unabstracted* game against two different
policies:

```text
abstraction_gap(scheme) = BR_full(policy_trained_at_scheme)
                        - BR_full(policy_uniform)
```

where `BR_full` is a full-game best response computed in the unabstracted
tree, evaluated on the same fixture set.

An earlier draft of this RFC subtracted `BR(uniform)` from `BR(trained)`,
which does not measure abstraction at all: the uniform term is
scheme-independent, so ordering schemes by it reduces to ordering them by
`-BR(trained)` — an exploitability comparison with a constant added. The
formulation above is the one implemented.

`policy_uniform` is the control: it fixes the scale by telling us what the
worst policy costs in the same metric. A scheme whose gap is close to the
control's has bought nothing.

Reported per seat, normalized in BB/hand per RFC 0029's canonical definition,
with a 95% interval and the policy miss count. The interval's width is part of
the result: a wide interval is a finding, not a failure.

Three granularities:

1. `CategoryTiersV1` — the current production scheme, buckets 1..9.
2. An intermediate, specified concretely: the made-hand category **combined
   with a draw flag** derived from `own_card_token`'s rank/suit-multiplicity
   decomposition. This RFC names it `TiersDrawV1` and fixes its bucket count
   before the run, because the decision rule compares against it. Note that
   `OwnCardToken` as it exists (`abstraction.cpp:358-376`) is *board-blind* —
   it describes a holding, not a holding-plus-board situation — so it cannot
   serve as a postflop scheme unchanged; the implementer must define the
   board-dependent part and record the resulting bucket count, which must fit
   `uint16`.
3. `Identity`, only where the fixture is small enough to train. It is not
   currently representable (see Current State), and making it representable is
   explicitly out of scope; this rung is therefore reported as a projection
   where it cannot be run, and labelled so.

Fixed at two iteration counts per cell, so the ranking is not a budget
artifact.

### Adoption criteria declared in advance

Let `gap(s)` be the measured abstraction gap of scheme `s` with 95% interval
`[lo(s), hi(s)]`, and let `bytes(s)` be the total `.bin` size of a library
built entirely under `s` on a stated class set. Then, evaluated only when
`gap(tiers) > 0`:

- **Adopt the intermediate** if `hi(TiersDrawV1) < lo(tiers)` **and**
  `gap(TiersDrawV1) < (2/3) * gap(tiers)` **and**
  `bytes(TiersDrawV1) <= 2 * bytes(tiers)`.
- **Keep tiers** if `hi(tiers) <= lo(TiersDrawV1)`, or if
  `gap(tiers) - gap(TiersDrawV1) < (1/3) * gap(tiers)`, or if
  `bytes(TiersDrawV1) > 2 * bytes(tiers)`.
- **Inconclusive otherwise**, and recorded as such; the material answer is that
  the fixture set or the interval width is inadequate, which is itself a
  finding this RFC must report rather than resolve.

Per-lookup cost is a gate, not a trade-off: a scheme whose measured
`card_bucket` cost exceeds **150 ns/combo** (twice the ~75 ns/combo the
public-belief replay budget assumes) is rejected regardless of its gap. The
replay issues up to 1,326 `card_bucket` calls per observed action
(`engine/src/resident/resident_policy.cpp:877-894`, via `find_row_words` at
`:887`), so this is a real budget and not a formality.

The `Identity` rung is reported but never adopted under this rule: it is
unrepresentable today and its cost is a projection.

### Reproducing the baseline statistics

This RFC's motivation rests on five statistics computed from the shipped
artifact. They are *derived*, not published anywhere, and `artifacts/` is
gitignored (`.gitignore:88`), so a clean checkout cannot reproduce them. Stage
0 therefore commits a small statistics tool (or a documented one-liner) plus
the pinned artifact sha256 `b67e3cb3…20b4`, and every statistic is defined
exactly:

| Statistic | Definition |
| --- | --- |
| Public nodes | distinct `node_hash` entries across all 1,755 sections |
| Bucket rows | sum of `node_bucket_count` |
| Distinct vectors per node | distinct `(actions, probabilities)` tuple over the node's bucket rows, averaged over nodes |
| Rows per distinct vector | bucket rows divided by distinct vectors, summed then divided |
| Untrained node | a node where **every** bucket row is uniform over its action menu |

The "untrained node" definition matters: "any row uniform" gives 98.55% and
"uniform rows" gives 81.34%, against 57.60% for "every row uniform". The RFC
uses the strict definition because it identifies nodes where nothing was
learned, and the segmented table in Motivation is the load-bearing one.

### Identity

Independently of the outcome, the bucket scheme is pinned where it currently is
not. This is narrower than an earlier draft claimed: BSFLIB02 **already** stores
the kind at offset 20 and `open()` already refuses a mismatch
(`engine/src/resident/mmap_library.cpp:647-649,1151-1156`). What is missing is
the three places that gate a *build* rather than a *load*:

1. `BuildIdentity`/`.sections/build.json` gains `card_bucket_kind`, so a
   `--resume` cannot mix sections built under two schemes.
2. The sealed checkpoint's `key_kind` (already in the header at offset 16) is
   compared on resume instead of merely being stored — today
   `engine/src/flop_library/flop_library.cpp:366-372` compares only the
   canonicalization digest.
3. The JSON manifest records it next to the digest, for human inspection and
   for the runner's source-selection logic.

No `.bin` field is added and no version bump is needed.

## Dependency Rules

- **RFC 0029 is a blocking prerequisite for Stage 3.** Stages 0-2 proceed
  independently; the measurement cannot run until 0029's policy adapter and
  frontier support exist and are accepted.
- The measurement lives in the offline eval surface and depends on RFC 0029's
  adapter.
- `bs::abstraction` owns the bucket function and its identity; no consumer
  re-derives a bucket.
- No live-path dependency is added.

## Compatibility and Migration

- **No published artifact is invalidated by this RFC.** The library format
  already carries the scheme; the added fields live in `build.json` and the
  manifest, both of which are regenerated on the next build and neither of
  which is read by the serve path. The 188.4 MB `library.bin` and its RFC 0027
  byte-identical reproduction are unaffected.
- If a finer scheme is later adopted *by a follow-up RFC*, every class section,
  the packer's bitwise-identity assertion, and the exhaustive verify tool are
  regenerated at that point (RFC 0026 AC2 covered 1.37B rows). This RFC does
  not do that.

## Security and Operational Impact

- Offline measurement; no credentials, no live play.
- Cost: the ablation is bounded by the fixture set, which must be small enough
  for the exact pooled walk. The command and the set are documented.
- Observability: each run prints scheme, iterations, seat count, byte cost, and
  the miss count.

## Alternatives Considered

1. **Adopt Identity directly, reasoning that more buckets cannot be worse.**
   Rejected on cost and representation: 22.03x the rows, a derived 2.05 GB,
   scores up to 10,354,688 against a `uint16` bucket id, and no way to train it
   without a format change. Note the argument is *not* that the library is
   uniformly over-segmented — at trained nodes it differentiates 2.93 vectors
   across 4.44 rows, so a finer scheme is plausible where the cost is
   affordable. That is what the measurement is for.
2. **Adopt an equity/potential-aware scheme immediately.** The strongest
   candidate, but it computes equity per combo, far above the ~75 ns/combo the
   replay budget assumes, and it is unmeasured against tiers. This RFC measures
   it rather than assuming it.
3. **Leave the abstraction alone and improve convergence instead.** Plausible
   given the 57.6% uniform finding: more iterations might differentiate more
   nodes within the existing buckets. The ablation's fixed-iteration design
   partially addresses this, and a follow-up can compare iteration budgets.
   Noted as the leading competing explanation.
4. **Measure abstraction error analytically from the library rows without a
   best response.** Much cheaper, but it cannot price the loss in chips and
   would not satisfy the declared decision rule.

## Risks

1. **The measurement may be dominated by convergence, not abstraction.** The
   57.6% uniform-fallback finding is consistent with under-training.
   *Mitigation:* run each cell at two iteration counts; if the ranking flips,
   the answer is "under-trained", which is itself actionable and is recorded.
2. **The exact pooled walk cannot cover production ranges.** *Mitigation:*
   fixtures are declared and bounded, and the result is labelled to its set.
3. **The cost of a finer scheme is prohibitive at identity.** *Mitigation:*
   the intermediate scheme is the intended candidate; identity is measured for
   the shape of the curve, not as a shipping option.
4. **The identity widening touches every `card_bucket` caller.** There are 11
   call expressions in `engine/src` (9 passing `kNSeatCardKind`) plus 7 in
   `engine/benchmarks`; the enumeration is in Rollout stage 1 and each is
   covered by a test that fails on a scheme mismatch.

## Verification Plan

- **Baseline tool.** The committed statistics tool reproduces all five
  baseline numbers and the segmented trained/untrained table from the pinned
  artifact sha256, and an independent recomputation agrees.
- **Ablation run** at three granularities and two iteration counts, on a
  recorded fixture set, reporting the abstraction gap with 95% intervals and
  miss counts.
- **Identity tests.** A library built under tiers and one built under the
  intermediate have different identities and cannot be mixed.
- **Resume tests.** A checkpoint written under one scheme is refused by the
  other, naming the scheme.
- **Per-lookup cost.** `card_bucket` timing for each measured scheme is
  recorded against the ~75 ns/combo budget.
- **Project gates**: full Engine Build Contract, ASan, `check-rfcs` /
  `check-docs`.

## Rollout Plan

0. **Baseline statistics.** Commit the tool and its pinned input; reproduce
   the five numbers and the segmented table. Gate: AC1. This stage exists
   because the RFC's motivation rests on derived numbers that a clean checkout
   currently cannot reproduce.
1. **Identity pinning.** Add the scheme to `build.json`, the sealed resume
   check, and the manifest; refuse cross-scheme mixes. The 11 `card_bucket`
   call expressions in `engine/src` and 7 in `engine/benchmarks` are
   enumerated here and each gets a mismatch test. Gate: identity and resume
   tests pass with the current scheme unchanged, and no number moves. This
   stage is independent of RFC 0029.
2. **Specify and wire `TiersDrawV1`.** Define the board-dependent draw flag,
   record its bucket count, confirm it fits `uint16`, and register it. Gate: it
   is selectable and its buckets are enumerable and bounded. Independent of
   RFC 0029.
3. **Ablation.** Requires RFC 0029 to be accepted and its adapter to exist.
   Run the declared matrix. Gate: G1-G3 satisfied.
4. **Decision.** Record the outcome against the declared rule. If adoption is
   indicated, stop and propose the format work; this RFC does not adopt.

Stop conditions: any step that moves an existing published artifact's bytes
without an intentional rebuild halts the rollout.

## Rollback Plan

- Stages 1-2 are additive and reversible; the production scheme stays tiers
  until a later RFC.
- Stage 3 produces a document, not a behavior.
- No artifact is migrated in place; any rebuild is from pinned parameters.

## Open Questions

1. **What is `TiersDrawV1`'s exact bucket function?** `OwnCardToken` is
   implemented (`engine/src/abstraction/abstraction.cpp:358-376`) but
   board-blind, so it cannot be used unchanged as a postflop scheme. The
   implementer must define the board-dependent part — a draw flag combined with
   the made-hand tier — record the resulting bucket count, and confirm it fits
   `uint16` **before** Stage 3 runs, because the decision rule compares against
   it.
2. **Should the decision rule be phrased in absolute or relative terms?** The
   one-third relative rule is chosen because an absolute chip threshold would
   differ per fixture; it will be revisited if the intervals turn out wide.
3. **Does the 57.6% uniform finding indicate under-training or a genuinely
   indifferent region?** Stage 3's second iteration count is the first evidence;
   distinguishing them may need a third.

## Acceptance Criteria

- **AC1**: A committed tool reproduces all five baseline statistics from the
  pinned artifact (`sha256 b67e3cb3…20b4`) with their exact definitions, and
  the segmented trained/untrained table matches.
- **AC2**: The abstraction gap is measured for at least three granularities at
  two iteration counts on a recorded fixture set, with 95% intervals and miss
  counts.
- **AC3**: The decision rule was declared before the run, and the outcome is
  recorded as adopt / keep / inconclusive against it.
- **AC4**: The bucket scheme is recorded in `.sections/build.json`, compared on
  sealed-checkpoint resume, and present in the manifest; a build or checkpoint
  under another scheme is refused by name. No `.bin` version bump is required
  and the shipped library still loads.
- **AC5**: Per-lookup cost for each measured scheme is recorded against the
  ~75 ns/combo budget and the 150 ns/combo rejection gate.
- **AC6**: The full Engine Build Contract passes, ASan is clean, and
  `check-rfcs` / `check-docs` pass.

## Decision

Pending independent approval-agent review.
