---
rfc: "0005"
subject: "Strategy Artifacts and Bounded Heads-Up Resolving"
status: "Accepted"
authors: "BigShark maintainers"
created: "2026-09-14"
updated: "2026-09-14"
owners: "engine artifact boundary, engine solver, engine policy, engine service, protocol, engine host, tools"
supersedes: ""
superseded-by: ""
---

# RFC 0005: Strategy Artifacts and Bounded Heads-Up Resolving

## Summary

Persist RFC 0004 training checkpoints and immutable blueprint policies in a
versioned SQLite artifact, then add bounded, opt-in heads-up resolving.
Keep storage outside the solver and use a conservative acceptance check:
publish a replacement only when opponent counterfactual best-response values
do not exceed the baseline bounds in the supported game. Unsupported,
off-tree, incomplete, or uncertified states retain a valid baseline or return
an explicit unsupported result.

## Motivation

Offline strategies are not operationally useful without restartable training,
reproducible policy selection, and bounded lookup. A naïve local re-solve can
increase exploitability in an imperfect-information game even when its local
value improves. Storage identity, range provenance, and resolving safety must
therefore be designed together.

## Goals

- Resume a completed training iteration without changing its numeric state.
- Publish immutable, validated, versioned policies with bounded random access.
- Bind every lookup to exact game, range, history, and artifact identities.
- Re-solve supported heads-up subgames within the caller's deadline.
- Record source, fallback reason, coverage, and artifact identity.
- Keep existing RPC v1.0 and v0 behavior compatible during migration.

## Non-Goals

- A general storage framework, cloud registry, remote artifact download, or
  network inference service.
- Memory-mapped custom binary formats or compressed model shards in v1.
- General safe off-tree nested solving, neural leaf values, or multiway safety.
- Claiming that the current heuristic range tracker supplies certified bounds.
- Removing v0 before the existing explicit removal checkpoint.

## Current State and Evidence

- `engine/src/gto/multistreet_cfr.*` stores training tables only in memory.
- `engine/include/bs/river_gto.hpp` exposes an in-memory river policy facade.
- `engine/include/bs/service.hpp` returns one `Decision`; it cannot yet carry
  a full strategy, structured errors, or artifact provenance.
- `apps/engine-host/main.cpp` is a v0 JSON composition root.
- RFC 0002 Stages 5-7 already authorize typed mapping, framed transport, and
  migration; they do not define artifact storage.
- `Strategy.actions` in `proto/bigshark/engine/v1/engine.proto` has a five-entry
  validation limit. Multiple sizes can exceed that limit.
- SQLite's [atomic commit design](https://www.sqlite.org/atomiccommit.html)
  provides established checkpoint transactions and indexed partial reads.
- [Brown and Sandholm's Libratus description](https://doi.org/10.1126/science.aao1733)
  motivates blueprint-based safe subgame solving. Its guarantees do not
  automatically apply to a finite, approximate, independently implemented
  resolver.

## Design Principles

1. Immutable published policies and mutable checkpoints are separate files.
2. A missing row is a coverage miss, never an invented uniform policy.
3. A lookup result includes the complete game identity used to produce it.
4. Bounds use the same utility, reach convention, and opponent information
   sets as the replacement evaluation.
5. A deadline is a resource limit, not evidence of convergence.
6. Storage errors and unsupported features never become strategic folds.

## Proposed Design

### Ownership and dependency boundary

Add one `bigshark_artifacts` target owned by `engine/`, with public domain-only
read/write functions and a private SQLite implementation. It serializes the
RFC 0004 immutable policy and checkpoint records. SQL handles do not cross
this boundary.

The host composes the artifact reader and decision service at startup and
injects a read-only policy view. The solver consumes domain records only.
The policy layer selects blueprint, certified resolver, existing river
solver, or heuristic according to supported coverage. It never opens files.

This new target solves a concrete dependency problem: linking storage into
`bigshark_solver` would impose SQL on every in-process solver and benchmark.
Moving serialization into the host would duplicate it for offline training.
No abstract multi-backend repository interface is introduced.

### Artifact v1

Use a SQLite database with `application_id = 0x42534754` and `user_version = 1`.
Use STRICT tables, foreign keys, and prepared statements; require SQLite
3.37 or newer. A single engine-owned SQL schema is authoritative. Generated
artifacts are ignored; small sanitized compatibility fixtures are tracked.

Required logical tables and fields:

| Table | Required content and key |
| --- | --- |
| `manifest` | Singleton: artifact kind, algorithm revision, information-key revision, numeric profile, completed iteration, PRNG identifier/state, engine revision, validation state |
| `game` | Singleton: rules identifier, button, blinds, root street/board, initial stacks and contributions, utility identifier |
| `sizes` | Street, bet/raise kind, ordered numerator and denominator |
| `ranges` | Player and combo ID primary key, nonnegative finite weight |
| `information_states` | Integer ID, player, own combo, exact public key; unique full information identity |
| `actions` | Information ID and action ordinal primary key, kind, optional target total, finite probability |
| `training` | Same action key, cumulative regret and unnormalized average weight; checkpoints only |
| `bounds` | Public root, responder combo, baseline counterfactual mass, normalized BR bound, certification method |
| `measurements` | Fixture ID, seed, iterations, metric name, value, exact/estimated classification |

The public key is a canonical ASCII sequence:
`street|board_ids|events`, where board IDs preserve street order and events
are comma-separated `actor:kind:target` triples. Card IDs, actors, and amounts
are decimal with no leading zeros; absent targets are `-`; kind is one of
`f`, `x`, `c`, `b`, `r`, or `d` (public deal). Public deals use actor `-` and
card ID as target. The root game supplies fixed cards and commitments.
The key contains the entire path from that root; history is not hash-truncated.
Any later abstraction requires a new key revision and explicit compatibility.

SQLite INTEGER stores bounded signed integers. RFC 0004's training profile
caps total chips at `2^53 - 1`, so supported amounts fit exactly. Full-width
seeds and PRNG words use exactly 16 lowercase hexadecimal digits per 64-bit
word; preserve the specified word order. Store doubles as SQLite REAL and
reject NaN/infinity, invalid probabilities, missing actions, or negative
average weights. Require action probabilities to sum to one within `1e-12`.
Metadata never silently narrows the RPC amount domain.

### Checkpoint and publication lifecycle

Checkpoint one complete iteration and its RNG/average/regret state in one
transaction, with rollback journaling and `synchronous=FULL`. Single writer;
no concurrent training on the same checkpoint. Resume only with matching
rules, ranges, algorithm/key/numeric revisions, and PRNG. Do not resume a
published policy as if it contained training state.
Matching includes the full root board/street, button, all stacks and
contributions, blinds, utility, ordered rational sizing schedule, action
ordering, and every range weight. Compare canonical values, not only labels.
Interrupted traversal restores RNG and newly inserted records as specified
in RFC 0004 before any checkpoint is eligible to commit.

Export a fresh database under a unique temporary filename on the destination
filesystem. Validate schema, foreign keys, integrity, probabilities, coverage,
and required metadata. Close it, flush file data, and rename to a new immutable
generation filename; flush the parent directory. Never rename a live
checkpoint or its hot journal, and never overwrite an existing generation.

Identify the closed database by SHA-256 of its bytes. A deployment selects an
explicit path and expected digest; the digest itself is not stored inside the
hashed file. Use OpenSSL 3 Crypto privately in `bigshark_artifacts` for native
SHA-256 rather than implement cryptography. SQLite and Crypto are new native
dependencies and require build/packaging tests and third-party notice updates.
Record exact library versions in build evidence.

Open read-only, verify digest and size before use, disable extension loading,
use defensive/untrusted-schema settings, and reject unexpected schema objects.
Use one verified immutable file generation for the lifetime of a host process;
no hot swap in v1. Only operator-controlled local files are allowed.
A restart selects a new generation and retains the old one for rollback.

Limits: one artifact up to 8 GiB, 64 MiB SQLite page cache, and at most 256 MiB
loaded policy rows by default. At startup an explicit list of supported roots
is loaded completely, including continuations and bounds, into immutable
resident domain records. No LRU eviction or lazy database read is permitted
during a decision. Roots outside this resident set are coverage misses even
if their rows exist on disk. A root exceeding the resident budget is not
advertised. Offline inspection can read other roots through SQLite.
Validate at startup, outside the decision critical path. Capacity failure
prevents feature advertisement; it does not affect the old solver.

### Range and policy lookup

At root, use the artifact's declared pair of weighted ranges. For observed
actions multiply actor reach by that actor's policy probability for each
combination, filter incompatible public cards, and normalize the legal joint
distribution. Keep public range reach separate from the hero-specific blocker
filter used to choose the final action. The opponent's belief is never
conditioned on the hero's actual secret hand.

Zero-probability observed actions, missing history, mismatched stack/board,
untrained combinations, or off-tree amounts produce a coverage miss.
Heuristic estimates cannot be relabeled equilibrium ranges. Return the
existing supported fallback in automatic mode; a forced blueprint mode returns
`UNSUPPORTED_FEATURE`. Store no cross-hand opponent profile in the artifact.

### Bounded resolver and certification

The first resolver supports on-tree, heads-up, zero-sum, exact-card subgames
with a complete continuation policy and exactly enumerable opponent bounds.
The offline evaluator may inspect any postflop root. The initial live profile
is more restrictive: every action at the selected hero decision must leave
no future hero decision in any reachable branch. Examples include facing an
all-in with only fold/call available, including automatic public runouts.
The game continues to terminal; no heuristic leaf utility.

This terminal-decision restriction prevents a certified continuation from
being lost on a later request. It does not by itself certify selection of
the current action or recovery from a failed current request.
No cross-request candidate policy is assumed durable.

### Eligibility and caller control

Eligibility begins at the exact artifact root, not hand start. The guarantee
is relative to that root's declared game and ranges; preflop chart play does
not establish full-hand safety or certify those ranges as equilibrium.
A flop-root profile therefore does not require F1 preflop training. Claims
about a preflop-root game require an artifact covering that root.

The adapter maintains a per-hand, monotone eligibility flag, initially false.
It becomes true only on a fresh complete artifact-root snapshot after an
explicit blueprint profile is selected and the host returns the expected
operator-pinned artifact digest. All prior hero execution from that root
must use the matching blueprint distribution. Any manual override, operational
fallback, heuristic action, missing history, changed artifact, failed or
malformed response, or process/session restart makes it false until a new
hand starts. Reaching the same public history does not restore eligibility.
Opponent actions outside covered history also make it false.

Only an eligible adapter may request `SolverMode.RESOLVING`. Automatic mode
never selects resolving in minor 1; it may select blueprint or existing
sources. For ineligible hands the caller requests those modes explicitly
and does not attach an execution-safety claim. Forced resolving requires
the operator-pinned artifact digest to match the host's process-pinned artifact
on every result; mismatch rejects the result and clears eligibility.

No eligibility bit is added to the RPC. The explicit mode request is the
caller's assertion of eligibility; the stateless host verifies covered state,
terminal-decision restrictions, and mathematical bounds but cannot reconstruct
execution provenance. The host's certificate is conditional on that caller
assertion and applies to the modeled game only. Clients outside the River
adapter carry the same responsibility. This control path is testable with
identical snapshots but different adapter provenance.

General live river opens and turn/flop resolving need a follow-up contract
for hand-scoped continuation identity, accepted-policy reach propagation,
durable restart recovery, eviction, and certified fallback. They remain
explicitly unfinished; passing the restricted resolver does not complete
general live subgame resolving.

For opponent information set `I`, define counterfactual weight using chance
and the hero's prefix reach, excluding the opponent's own action reach.
Let `m(I)` be its total mass and `b(I)` the baseline opponent best-response
value divided by `m(I)`. Store both and the common utility reference.
Zero mass is recorded explicitly and never divided by.

Build the standard resolving gadget: chance selects compatible root histories
proportional to those counterfactual weights; the opponent observes only its
own information set and chooses termination at `b(I)` or continuation.
The hero gets the negated termination payoff in the centered zero-sum game.
Train the gadget using the RFC 0004 trainer.

Before accepting a finite-iteration replacement, independently recompute the
opponent BR value against the candidate for every positive-mass root
information set under the unchanged prefix weights. Require
`candidate_bound(I) <= b(I) + 1e-9 * root_pot`.
Also verify zero-mass histories cannot become newly reachable through a
modified prefix; prefix play is never changed by this operation.
If the check is unavailable, exceeds the deadline, or fails, discard the
candidate. Publish the baseline policy and record why. Approximate certificates
must never be labeled exact.

This is a conservative non-regression criterion within the modeled game.
It is not a proof against actions absent from its abstraction. Nested
off-tree solving requires separate bound construction and is deferred.

### Whole-range selection and failure boundary

Construct, certify, and select a single candidate for the complete hero
range before looking up the actual hero combination. The solve context omits
actual hero cards and hero-conditioned opponent ranges; it uses the public
root, declared ranges, public history, utility, and artifact identity.
The solver seed is derived deterministically from that public context and
algorithm revision. The request's `seed` is used only after candidate selection
for sampling an action from the actual combination's row.

Solver iteration caps, budget selection, cache keys, candidate ordering,
certificate acceptance, and baseline-versus-candidate selection cannot depend
on the actual hero hand or its row quality. Cache complete certified policies,
never independently selected rows. Validate all hero rows, action identities,
and required continuations before publishing a candidate. At a fixed public
state a timeout chooses the whole baseline, not a private-card-dependent
mixture of rows. Deadline noise must be independent of actual private input
for this modeled selection claim; use identical solve inputs and code paths
for every counterfactual hero combination. No row-specific retry is allowed.

The certificate assumes the chosen full-range policy is actually executed.
It does not cover current-request host death, client timeout before a valid
response, malformed responses, failed action submission, or adapter fallback.
There is no independently available external-process blueprint recovery in
this RFC. On those failures the adapter retains its existing legal operational
fallback, clears eligibility, and labels execution uncertified in its report
reason. It must never copy `modeled_exact_bound` from a failed/unexecuted
response. This explicit exclusion avoids claiming unconditional end-to-end
non-regression. Providing crash-safe baseline execution is a future contract.

Host `guarantee=modeled_exact_bound` therefore means only that the selected
whole-range candidate passed modeled bounds, conditional on eligible prefix,
private-independent policy selection, and successful execution. An internally
selected blueprint fallback reports `baseline`; external operational recovery
has no certificate. The adapter separately checks that its selected action
is the validated result, bound to the latest authoritative snapshot.

### Deadline and failure behavior

Start the monotonic deadline on service receipt; include lookup, range update,
solve, certification, and serialization reserve. Reserve at least 5 ms and
10 percent of the request budget for return processing. Check cancellation
within traversal, not only every iteration. Database work is startup-only;
SQLite progress callbacks are not a hard bound on blocking filesystem I/O.
Cache keys include artifact digest, root, reach identity, utility, and sizes.

Only completed, validated whole-range policies can replace a baseline.
Automatic mode does not resolve. In forced resolving mode, an in-process
deadline before certification returns the complete resident blueprint's
selected row with source `BLUEPRINT` and guarantee `baseline`, provided
the full baseline is available and validated. Otherwise return
`DEADLINE_EXCEEDED`. The Node client still enforces a hard process timeout
and restart; this external failure falls outside the modeled guarantee as
specified above. No exact real-time OS scheduling guarantee is claimed.

### Additive RPC v1.1 integration

Complete RFC 0002 Stages 5-7 on minor 0 with existing policy behavior first.
Do not loosen the five-entry v1.0 `Strategy.actions` annotation or truncate a
multi-size distribution to fit it.

For negotiated minor 1, add:

- `SolverMode`: `BLUEPRINT = 6`, `RESOLVING = 7`.
- `SolverSource`: `BLUEPRINT = 6`, `RESOLVING = 7`.
- `ExpandedStrategy`: repeated `ActionPolicy actions = 1` (1..32),
  optional `SelectedAction selected_action = 2`, `SolverMetadata solver = 3`.
- `DecisionResponse.result`: `ExpandedStrategy expanded_strategy = 3`.
- `SolverMetadata`: optional `string artifact_sha256 = 9` (64 lowercase hex),
  optional `string guarantee = 10` (`modeled_exact_bound`, `uncertified`, or
  `baseline`); absence on old responses means no guarantee reported.

Existing mode numbers, messages, field meanings, amount units, and error
behavior stay unchanged. `supported_protocol_minors` negotiates support and
the request envelope chooses minor 1 explicitly. Minor-0 requests never
receive the new oneof alternative, new solver sources, or new guarantees,
even for a distribution of five or fewer actions. All minor-0 forced
blueprint/resolving requests return unsupported; automatic mode uses only
minor-0-compatible policy sources. Minor-0 capability responses omit new
mode enum values while still listing the supported minor versions. Clients
query capabilities at minor 0 first, then re-query at minor 1 before using
the new features.

Validate selected action membership by kind and exact target, not kind alone.
Heuristic fallback continues reporting its actual source. Do not report
multistreet support merely because an offline checkpoint exists.

## Dependency Rules

`host -> artifacts -> solver domain records`, and
`service -> policy -> solver -> poker`. SQLite and OpenSSL headers are private
to the artifact implementation. Protobuf-generated types terminate at the
protocol mapper. No platform imports in these modules.

## Compatibility and Migration

No existing persisted artifact exists to migrate. New schema majors reject
unsupported files; migrations produce new immutable generations and never
rewrite the selected release artifact. Additional optional fields do not
authorize interpreting unknown rules or numeric revisions.

The service-domain result expansion is already required by RFC 0002.
New runtime policy sources are additive and remain opt-in until coverage and
latency evidence passes. Session and `.runtime` formats are preserved.

## Security and Operational Impact

Treat databases as untrusted native input even when supplied by an operator.
Cap size/rows/allocations, fuzz load paths, never execute embedded SQL, and
allow no plugin or extension loading. Digests detect corruption and identity
mismatch; trusted deployment configuration provides authenticity.

Log source, artifact digest, completed iterations, lookup/certification
latency, cache outcome, coverage miss, and fallback reason. Avoid credentials,
opponent identities, hidden cards, and full live requests in diagnostics.

## Alternatives Considered

- JSON regret dumps: poor bounded random access and fragile large-file
  checkpointing.
- Custom mmap chunks: may improve throughput, but requires custom offsets,
  checksums, evolution, and crash recovery before evidence justifies it.
- Cap'n Proto or FlatBuffers: credible future read-only formats, but do not
  independently solve transactional checkpointing. Benchmark SQLite first.
- Unconstrained local resolving: insufficient non-regression guarantees.
- Re-solve every request: violates short action clocks and coverage constraints.

## Risks

- SQLite row overhead can dominate large models. Measure before considering
  a second backend or format; failure of capacity gates keeps rollout blocked.
- Exact BR certification is expensive. First support small/river subgames;
  retain baseline when larger certification does not fit.
- A wrong reach convention invalidates safety. Hand-computed gadget tests and
  an independent BR implementation are required.
- New native dependencies increase packaging cost. Keep them private and test
  clean macOS/Linux builds with feature-disabled rollback.

## Verification Plan

Run native CTest, ASan/UBSan, the full `AGENTS.md` gate, and protocol conformance.
Add checkpoint split-run versus uninterrupted-run comparisons within `1e-12`
on the same build; round-trip exact seed/amount tests; unknown version,
corruption, truncation, full-disk, failed-transaction, and missing-row tests.
Kill a checkpoint writer before and after commit and verify old-or-new state.

Resolving fixtures must include a locally improved but globally exploitable
candidate that the gate rejects; hidden-card permutations; zero-reach roots;
timeout during solve and certification; rejection of nonterminal live roots;
and restart/manual-override loss of hand eligibility. Simulate the actual
executed hand, including prior requests and fallbacks, rather than validating
an unused full replacement policy.
Include the terminal-only counterexample: each player has contributed 1,
opponent bets its last 1, and hero has equally weighted winning/losing hands.
Opponent utility is +1 on fold and -2/+2 on calls. All-fold has bound 1,
all-call has bound 0, but selecting fold only for the winning hand and call
only for the losing hand yields 1.5. The execution selector must never
assemble that uncertified policy from independently certified candidates.

Enumerate all hero combinations using identical public inputs and injected
deadline/cache schedules: candidate identity and baseline/candidate selection
must match before row lookup. Vary action-sampling seeds without changing the
solver candidate. Test process death, client timeout, malformed output, and
submission failure: existing operational fallback remains legal, execution
is uncertified, and eligibility cannot recover mid-hand. Verify in-process
baseline recovery separately from unavailable external-process recovery.
Identical snapshots after blueprint play and manual override must request
different modes. Test artifact mismatch, fresh-root enrollment, missed root,
restart, and new-hand reset. Automatic requests must never enter resolving.
Whole small-game NashConv after replacement must not exceed the baseline by
more than `1e-8` root-pot units.

Test minor-0/minor-1 host-client combinations, distributions above five
entries, selected action identity, fallback provenance, and unchanged v0
fixtures. An old validating client must accept minor-0 responses even when
a small blueprint is installed in the new host. Reject resume with changed
root, stacks, button, or sizing schedule. Benchmark cold startup, unsupported
resident roots, and p50/p95/p99 warm decisions separately.
Target warm lookup p99 <= 10 ms on a declared reference machine with 100,000
information sets; missing the target blocks default promotion, not correctness
tests on variable-speed CI.

## Rollout Plan

1. Schema, checkpoint API, and fault tests behind offline training only.
2. Immutable export, bounded loader, and artifact lookup benchmarks.
3. Minor-0 protocol migration under RFC 0002, independently verified.
4. Minor-1 negotiated fields and opt-in blueprint replay.
5. Offline resolving and independent certification; restricted terminal-only
   live decisions after eligibility and restart tests pass.
6. Dry runs and approved live canary with rollback exercised.

## Rollback Plan

Restart with the prior artifact generation or disable blueprint/resolving.
Retain the current river/heuristic paths and v0 protocol until the existing
removal checkpoint. Never delete the prior artifact automatically.

## Open Questions

- Reference hardware and larger deployment artifact budget must be recorded
  before default promotion; local defaults above apply until then.
- General off-tree and approximate large-subgame safety need subsequent
  research evidence and a new design, not a renamed certification flag.
- General multi-request live resolving requires the hand-scoped continuation
  contract described above before removing the terminal-only restriction.
- Crash-safe external-process baseline recovery is outside the certificate;
  a future execution contract is required for unconditional failure recovery.

## Acceptance Criteria

- Checkpoints resume deterministically and survive interrupted writes.
- Published artifacts are versioned, immutable, bounded, and validated.
- Loaded policy probabilities and action targets match exported values.
- Coverage misses and range provenance remain explicit.
- Resolving passes independent non-regression and deadline tests.
- Minor-0 compatibility and minor-1 capability negotiation pass conformance.
- Default promotion includes measured coverage, latency, and rollback evidence.

## Decision

Author agent: /root
Approved by: a69eb9b5-dc85-4915-aa75-f7af38d5e72b
Decision date: 2026-09-14
Review outcome: Approved
Reviewed scope: Entire revised proposal including storage, conditional resolving, adapter eligibility, minor-1 protocol, migration and rollback; pre-decision SHA-256 962d325ceb11583b71d7d6f7ad473fff431e251ff62045771ea39734ad3b2bdb.
Review summary: Formal resubmission confirmed P1 whole-range execution selection and P2 eligibility control are resolved. No remaining blocking findings. Numerical certification, execution/failure tests, storage fault tests, packaging, capacity, and latency remain implementation gates. General multi-request/off-tree resolving and crash-safe external recovery are excluded.

Formal review 2026-09-14 by agent 7f86ec31-e5b6-4753-a478-a48366ac2edd:
Changes Requested for proposal SHA-256
`bea60c954bdc1fc24ed4dd2952e359fff1f9fd0cd92ca311e2a0b2854b812d2d`.

- P1: Certified policies could be mixed by private-hand-dependent selection;
  process failure lacked guaranteed baseline execution. Revised to require
  whole-range private-independent selection and explicitly exclude external
  process/operational failures from the modeled certificate.
- P2: Adapter eligibility lacked a service control path. Revised to require
  explicit resolving mode gating, artifact-root enrollment, pinned digest,
  monotone invalidation, and conditional host-certificate semantics.

The original reviewer had finished and the review tool could not resume it.
The replacement approval agent received both original findings and their
dispositions, reviewed the entire revised RFC, and explicitly approved it on
2026-09-14. No objection was waived. Design approval does not establish
implementation correctness or production readiness.
