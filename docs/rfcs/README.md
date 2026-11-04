# BigShark RFC Process

Status: Current

Request for Comments (RFC) documents are the decision system for material
engineering changes. They make intent, evidence, tradeoffs, approval, rollout,
and implementation status reviewable over time.

RFC approval is delegated to a separately launched approval agent. User
confirmation is not required for RFC acceptance. The author cannot approve
their own RFC.

## RFC Index

| RFC | Subject | Status | Owners |
| --- | --- | --- | --- |
| [0001](0001-engineering-architecture.md) | BigShark Engineering Architecture | Implementing | Engine, platform integrations, developer experience |
| [0002](0002-protobuf-engine-protocol.md) | Protobuf Engine Protocol | Implementing | Protocol, engine host, engine clients |
| [0003](0003-typescript-application-layer.md) | Strict TypeScript Application and Integration Layer | Implemented | Applications, platform integrations, engine clients, tools |
| [0004](0004-heads-up-blueprint.md) | Heads-Up Multi-Size Blueprint Training | Implementing | Poker, solver, benchmarks |
| [0005](0005-strategy-artifacts-and-resolving.md) | Strategy Artifacts and Bounded Heads-Up Resolving | Accepted | Artifact boundary, solver, policy, service, protocol, host |
| [0006](0006-extended-poker-utility.md) | Extended Poker Utility and Multiway Evaluation | Implementing | Poker, solver, benchmarks, protocol, service |
| [0007](0007-preflop-profile-abstraction.md) | Preflop Profile Abstraction and Artifact Coexistence | Implementing | Poker, solver, artifact boundary, benchmarks |
| [0008](0008-unified-n-player-architecture.md) | Unified N-Player Engine Architecture | Implementing | Poker, solver, policy, service, protocol, artifact boundary |
| [0009](0009-unified-engine-delivery.md) | Unified Engine Delivery and Live Promotion | Implementing | Poker, solver, abstraction, artifact boundary, resolver, resident, protocol, engine host, River adapter, benchmarks |
| [0010](0010-multiway-frontier-evaluation.md) | Multiway Frontier Evaluation and N-Way Preflop Training | Accepted | Engine, solver, artifact boundary, benchmarks |
| [0011](0011-turn-river-strategy.md) | Turn and River Continuation Strategy | Implemented | Engine, solver, resolver, artifact boundary, benchmarks |
| [0012](0012-flop-library-performance.md) | Flop Library Performance Optimization | Implemented | Engine, solver, artifact boundary, resident, benchmarks |
| [0013](0013-preflop-phase2.md) | Preflop Phase 2: Postflop-Aware Frontier Evaluation | Accepted | Engine, solver, artifact boundary, benchmarks |
| [0014](0014-postflop-policy-persistence.md) | Postflop Policy Persistence and On-Demand Evaluation | Accepted | Engine, solver, benchmarks |
| [0015](0015-offline-frontier-evaluation.md) | Offline Frontier Evaluation Reuse and Measurement | Implemented | Engine, benchmarks, tests |
| [0016](0016-synchronous-mccfr-averaging.md) | Synchronous MCCFR Batches and Correct Strategy Averaging | Implemented | Engine, artifacts, flop library, benchmarks, tests |
| [0017](0017-training-pipeline-allocation-cost.md) | Training Pipeline Allocation Cost | Implemented | Engine, solver, artifacts, benchmarks, tests |
| [0018](0018-zero-rake-settlement-fast-path.md) | Zero-Rake Settlement Fast Path | Implemented | Poker, tests, benchmarks |
| [0019](0019-nseat-preflop-reconstruction.md) | N-Seat Preflop Reconstruction | Implemented | Protocol, tests |
| [0020](0020-folded-root-policy-identity.md) | Folded-Root Policy Identity | Implemented | Resident, resolver, tests |
| [0021](0021-nseat-cross-street-reconstruction.md) | N-Seat Cross-Street Reconstruction | Implemented | Protocol, tests |
| [0022](0022-complete-cash-hand-protocol.md) | Complete Cash-Hand Protocol | Superseded by [0040](0040-retire-river-club-agent-api.md) | Protocol, host, clients, River adapter/runner, poker, service |
| [0023](0023-artifact-startup-and-decision-deadlines.md) | Artifact Startup and Decision Deadlines | Implemented | Node framed client, River adapter, River runner, integration tests |
| [0024](0024-depth-limited-resolving.md) | Depth-Limited Resolving with Value Estimation | Implemented | Engine resolver, engine tree, engine solver, engine benchmarks |
| [0025](0025-off-tree-decision-handling.md) | Off-Tree Decision Handling | Implemented | Engine resolver, engine solver, engine protocol |
| [0026](0026-bucket-deduplicated-flop-library-format.md) | Bucket-Deduplicated Flop Library Resident Format | Implemented | Engine resident, artifact boundary, engine benchmarks |
| [0027](0027-train-and-pack-direct.md) | Direct Train-and-Pack Flop Library Production | Implemented | Engine flop library, engine resident, artifact boundary, engine-host, engine benchmarks |
| [0028](0028-sealed-warm-start-checkpoints.md) | Sealed Warm-Start Checkpoints | Implemented | Artifact boundary, engine flop library, engine solver, engine benchmarks |
| [0029](0029-flop-terminal-best-response.md) | Make the Stage-6 Walkers Model Frontier Leaves and Measure the Resident Path | Accepted | Stage6 evaluation, engine resident, engine benchmarks |
| [0030](0030-pin-raise-cap-in-identity.md) | Pin Strategy-Shaping Abstraction Parameters in Persisted Identity | Implemented | Engine abstraction, engine flop library, engine solver |
| [0031](0031-measure-card-abstraction-error.md) | Measure Postflop Card-Abstraction Error Before Changing It | Implemented | Engine abstraction, engine solver, engine flop library |
| [0032](0032-turn-river-board-addressing.md) | Preserve the Full Board Through the Decision Pipeline | Implementing | Engine resident, River adapter, engine protocol |
| [0033](0033-flop-library-root-reachability.md) | Make the Flop Library's Root Identity Reachable from a Real Hand | Implemented | Engine flop library, engine resident, engine benchmarks, engine tests |
| [0034](0034-unversioned-engine-protocol.md) | Unversioned Single-Form Engine Protocol | Implemented | Proto, engine protocol, engine host, node client, river adapter, engine tests, docs |
| [0035](0035-multi-root-flop-library.md) | Multi-Root Six-Way Flop Library | Implemented | Engine flop library, engine resident, engine host |
| [0036](0036-six-way-turn-river.md) | Six-Way Turn and River On-Demand Strategy | Accepted | Engine resolver, engine solver, engine tree |
| [0037](0037-shape-keyed-abstraction-policy.md) | Shape-Keyed Action-Abstraction Selection Policy | Implemented | Engine abstraction, engine resolver, engine flop library, engine benchmarks |
| [0038](0038-retire-schema-v1-artifacts.md) | Retire the Schema-v1 Heads-up Artifact Format and Algorithm Revision 1 | Implemented | Engine artifacts, engine resident |
| [0039](0039-nested-river-continuation-values.md) | Nested River Continuation Values for Multiway Turn Strategy | Accepted (amended 2026-10-28) | Engine resolver, engine tree, engine solver, engine host, engine benchmarks |
| [0040](0040-retire-river-club-agent-api.md) | Retire the River Club Agent API v2.1 integration | Implemented | Applications, platform integrations, tooling, documentation, developer experience |
| [0041](0041-learned-river-continuation-value-network.md) | Learned River Continuation-Value Network for Multiway Turn Strategy | Accepted; **closed 2026-10-29 at Stage 1 (recorded FAILURE, Amendment 2)** — label noise floor unreachable on 15.3% of turn boards; Amendment 1 10-second envelope remains in force | Engine resolver, engine solver, engine artifacts, engine benchmarks, tools (offline trainer), engine host |
| [0042](0042-equity-strength-bucketing.md) | Equity-Strength Postflop Card Bucketing (W5 Abstraction Refinement) | Implemented (KEEP 2026-10-30; measurement code deleted; evidence + TS evaluator retained) | Engine abstraction, engine solver (in-memory measurement seam), engine benchmarks; build/pack, artifact, resident, codec, and serve-path changes deferred to a follow-on flip RFC |
| [0043](0043-offline-equity-blueprint-feasibility.md) | Offline Equity-Strength Blueprint Feasibility (W4 Measurement) | Implemented (no-go 2026-10-31; structural no-go all three river bands after frozen campaign, measurement code deleted, evidence retained) | Measurement-only offline convergence/cost feasibility; equity gaps reached <=0.05 BB with positive benefits but the frozen relative plateau never forms for equity or the nine-tier control (~T^-0.55 RM+), so no r*, AC6/AC7 unmeasured; independent ruling against the incomplete reading; 134,459-class cost question stays open for a new-RFC recertification instrument |
| [0044](0044-convergence-certification.md) | Reachable Convergence Certification for Offline Equilibrium Artifacts (M2 Prerequisite) | Implemented (2026-11-01; Stage 3 campaign + independent evidence review complete; Stage 4 measurement code deleted) | HU campaign: EquityStrengthV1 CERT at 100M (bucket gap 0.0049) but QUAL fails (exact 0.0484 BB > 0.03; 300M 0.0459 card-abstraction floor) — predeclared no-grid outcome, no mount; evaluator+evidence retained, next instrument needs a new RFC |
| [0045](0045-m2-instrument-scope-census.md) | M2 Heads-Up Full-Street Blueprint — Instrument Resolution and Blueprint-Scope Census | Implemented (2026-11-04; run 2 368/368 independently VALID; measurement code deleted, evaluator/evidence retained) | Terminal verdict instrument-inadequate-uniform-ehs: uniform EHS passes exact QUAL at k_river>=128 (0.019/0.022 BB) but fails the 0.005 BB within-abstraction CERT at every grid (worst bucket 0.0124-0.0159, non-improving); conditional arm river-inert; G3 cap-0 HU river tree >100M nodes infeasible -> cap-1/depth-limited only; hands the bounded-trunk + real-time endgame route to RFC 0046 |
| [0046](0046-m2-fullstreet-hu-blueprint.md) | M2 Full-Street Heads-Up Blueprint — Bounded Cap-1 Trunk plus a New Exact Certified River Endgame Solver | Accepted (2026-11-04 round 2; authorizes measurement only) | Round-1 Changes-Requested corrected: the RFC 0011 subgame resolver is uncertified and merges postflop to nine categories, so production requires a NEW exact per-combo-keyed independently certified turn-rooted endgame solver + per-combo margins; Stage 0 pre-frozen gate predicates + Stage 1 benchmark-only no-legacy harness precede Gates 1-3 (trunk selection, 10s exact-key HU certifiability, cap-1) with CLOSE/REDIRECT; primary metric full-range hybrid NashConv; turn fan-out 49/45 erratum fixed; production solver/schema/mount/serving/enablement NOT authorized |
| [0047](0047-depth-limited-certified-river-resolve.md) | Depth-Limited Per-River Certified Resolve with an Offline Per-Combo Turn Continuation-Value Oracle | **Accepted** (2026-11-04, two independent review rounds; measurement-only; Stage-0 addendum and Stage-1 benchmark-only authorized, no production/implementation/spend without user budget) | Successor to the closed RFC 0046 Route A: small depth-limited turn solve plus one exact per-river re-solve fed by an offline deterministic per-combo five-card-frontier CV oracle with a one-sided certified error; Brown-Sandholm continual-resolving margin safety with an absolute-BB error-to-exploitability slope gate; G1/G2/G3/G4/G5/G6; benchmark-only Stage 1 under a new link guard; reuses the bigshark_resolver certified gadget spine and existing CERTIFIED_BOUND wire token (no proto change); no production/mount/enablement authorized |

`0000-template.md` is the authoring template and does not consume an RFC
number.

## When an RFC Is Required

An RFC is mandatory for:

- cross-module architecture or dependency-direction changes;
- external or engine-facing protocols;
- persisted data, model artifact, or session format changes;
- platform adapter boundaries;
- production solver architecture or strategy model changes;
- long-lived extension points;
- migrations that require compatibility or rollback;
- repository-wide build, deployment, security, or observability policy.

An RFC is not required for a local bug fix, a narrow implementation detail,
or a documentation correction that preserves established contracts.

## Lifecycle

```mermaid
stateDiagram-v2
  [*] --> Draft
  Draft --> Proposed
  Proposed --> Accepted: independent approval agent approves
  Proposed --> Rejected: independent approval agent rejects
  Accepted --> Implementing: implementation starts
  Implementing --> Implemented: all acceptance criteria verified
  Draft --> Superseded
  Proposed --> Superseded
  Accepted --> Superseded
  Implementing --> Superseded
  Implemented --> Superseded
```

| Status | Meaning |
| --- | --- |
| `Draft` | Incomplete and not ready for a decision |
| `Proposed` | Complete and ready for independent approval-agent review |
| `Accepted` | Explicitly approved by an independent agent; implementation may start |
| `Implementing` | Approved implementation is in progress |
| `Implemented` | Acceptance criteria and verification are complete |
| `Rejected` | Explicitly declined |
| `Superseded` | Replaced by another numbered RFC |

The author may set `Draft` or `Proposed`. A separate approval agent must
explicitly approve or reject the current proposal before the author records
`Accepted` or `Rejected`. `Implemented` requires verified acceptance criteria;
approval alone is not implementation evidence.

## Metadata

Every RFC begins with quoted scalar YAML front matter:

```yaml
---
rfc: "0001"
subject: "Example Subject"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-09-13"
updated: "2026-09-13"
owners: "engine, protocol"
supersedes: ""
superseded-by: ""
---
```

- `rfc` matches the four-digit filename prefix.
- `subject` matches the top-level heading.
- `owners` names modules or ownership areas, not individual implementers.
- `supersedes` and `superseded-by` contain comma-separated RFC numbers or an
  empty string.
- Dates use `YYYY-MM-DD`.

## Required Sections

Every RFC must contain these level-two sections:

1. Summary
2. Motivation
3. Goals
4. Non-Goals
5. Current State and Evidence
6. Design Principles
7. Proposed Design
8. Dependency Rules
9. Compatibility and Migration
10. Security and Operational Impact
11. Alternatives Considered
12. Risks
13. Verification Plan
14. Rollout Plan
15. Rollback Plan
16. Open Questions
17. Acceptance Criteria
18. Decision

Use [the RFC template](0000-template.md).

## Decision Record

A `Draft` or `Proposed` RFC ends with:

```text
Pending independent approval-agent review.
```

An accepted RFC records:

```text
Author agent: author session or agent ID
Approved by: independent approval agent ID
Decision date: YYYY-MM-DD
Review outcome: Approved
Reviewed scope: RFC revision and sections reviewed
Review summary: Findings, dispositions, evidence, and remaining non-blocking risks
```

A rejected RFC uses `Rejected by:` and `Review outcome: Rejected`, with the
same author, date, scope, and review-summary fields. A request for changes
keeps the RFC `Proposed`; record the findings and resubmit after corrections.
Use actual agent identities and conclusions returned by the review tool.
Never fabricate a review or turn silence, a timeout, or a conditional approval
with unresolved blockers into acceptance.

RFCs 0001-0003 retain their existing maintainer approval records as historical
decisions. This exception is limited to those records; new decisions follow
the approval-agent rule. Changing the approval mechanism does not accept
existing Proposed RFCs or reinterpret their earlier advisory reviews.

An accepted RFC is
immutable except for status transitions, implementation evidence, factual
errata, and links to a superseding RFC. Material design changes require a new
RFC.

## Approval Agent Workflow

1. Finish the proposal and run the RFC and documentation checks.
2. Launch a separate agent that did not author or edit the proposal. Give it
   the RFC, relevant source and tests, overlapping contracts, and this review
   standard. Its task is approval, not implementation or authoring.
3. Require `Approved`, `Changes Requested`, or `Rejected`, with reviewed
   scope, evidence, findings, and residual risks. Blocking questions or
   material correctness, security, compatibility, or feasibility findings
   prevent approval. State scope exclusions explicitly.
4. Resolve requested changes and send the revised proposal back for review.
   Do not switch reviewers merely to bypass unresolved objections. A
   replacement reviewer must receive the outstanding findings and dispositions.
5. Record the independent result in the Decision section, update both
   indexes, and rerun checks. Implementation can then proceed without asking
   the user to approve the RFC.

If separate-agent tools are unavailable or the reviewer fails, retain
`Proposed`, report the blocker, and retry when an independent agent is
available. A role-played self-review is not an independent approval.

This delegation covers RFC design approval only. It does not grant credentials,
cloud spending, live poker authorization, destructive operations outside the
authorized scope, or waive separate release/removal checkpoints. The user can
override or stop work at any time.

## Review Standard

Reviewers challenge:

- whether the expected value justifies the complexity;
- whether a smaller local solution satisfies the requirement;
- whether module ownership and dependency direction are correct;
- whether source-of-truth and generated files are clearly separated;
- whether compatibility, rollout, and rollback are credible;
- whether protocol evolution and unknown-field behavior are explicit;
- whether security, fairness, observability, and resource limits are covered;
- whether acceptance criteria are measurable;
- whether implementation can proceed without a flag day.

Current facts, assumptions, proposals, and unresolved questions must be
distinguishable without conversation context.

## Implementation Gate

No implementation of a `Proposed` RFC may begin. After independent agent approval:

1. Change the RFC to `Accepted` and record the complete approval-agent decision.
2. Create an implementation plan mapped to acceptance criteria.
3. Change status to `Implementing` when implementation starts.
4. Update current architecture, design, and reference documentation as code
   lands.
5. Change status to `Implemented` only after every acceptance criterion is
   verified.

Acceptance authorizes only the scope written in the RFC.

## Automated Checks

Run:

```bash
node bin/check-rfcs.mjs
node bin/check-docs.mjs
```

The RFC check verifies metadata, filename and heading consistency, lifecycle
status, required sections, decision records, unique IDs, dates, and index
membership. Decision checks enforce nonempty review records and distinct
author/approver IDs while preserving the three historical approvals. These
structural checks cannot authenticate an agent session or judge review
quality; the orchestrator must retain actual tool evidence. The checker is
also registered as the `rfc` CTest, with decision-record regression tests in
the native Node test suite.

The repository skill at `.codex/skills/rfc-authoring/SKILL.md` defines the
agent workflow. `.trae/skills`, `.agents/skills`, and `.claude/skills` resolve
to the same canonical skill content.
