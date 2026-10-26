# BigShark Documentation

Status: Current

This directory is the entry point for BigShark architecture, design, protocol,
integration, and development documentation.

## Documentation Map

| Area | Document | Status | Purpose |
| --- | --- | --- | --- |
| Architecture | [System overview](architecture/system-overview.md) | Current | Components, boundaries, data flow, and runtime safety |
| Design | [GTO engine](design/gto-engine.md) | Current | Implemented decision logic, solvers, range tracking, RFC 0005 strategy artifacts, and limitations |
| Reference | [Protobuf engine protocol](reference/protobuf-engine-protocol.md) | Current | IDL, pinned generation, compatibility baseline, and conformance vectors |
| Reference | [Exploitability and NashConv](reference/exploitability.md) | Current | The three normalisations, which harness computes what, and the resident fixture conventions |
| Integration | [River Club](integrations/river-club.md) | Current | River Club adapter, state machine, and operational mapping |
| Integration | [Provider public-field audit](integrations/provider-public-field-audit.md) | Current | Authoritative vs undocumented provider fields and the T1b/T2 side-pot gate |
| Development | [Build and test](development/build-and-test.md) | Current | CMake presets, formatting, tests, sanitizers, and release publishing |
| Development | [Benchmarking](development/benchmarking.md) | Current | Reproducible solver-quality fixtures, metrics, and thresholds |
| Development | [Offline practice table](development/practice-table.md) | Current | Local human-vs-bot practice for 2..10 seats, bot difficulty labels, and the offline boundary |
| RFC | [RFC process](rfcs/README.md) | Current | How architecture proposals are written and accepted |
| RFC | [RFC template](rfcs/0000-template.md) | Draft | Required structure for new proposals |
| RFC | [RFC 0001: Engineering architecture](rfcs/0001-engineering-architecture.md) | Implementing | Repository, module ownership, dependency rules, and staged migration |
| RFC | [RFC 0002: Protobuf engine protocol](rfcs/0002-protobuf-engine-protocol.md) | Implementing | Typed messages, framing, compatibility, and generated bindings |
| RFC | [RFC 0003: Strict TypeScript application layer](rfcs/0003-typescript-application-layer.md) | Implemented | Strongly typed applications, adapters, clients, tools, and compiled launchers |
| RFC | [RFC 0004: Heads-up blueprint](rfcs/0004-heads-up-blueprint.md) | Implementing | Real betting rules, multi-size offline training, coverage, and quality gates |
| RFC | [RFC 0005: Artifacts and resolving](rfcs/0005-strategy-artifacts-and-resolving.md) | Implementing | Durable strategies, bounded lookup, certified on-tree resolving, and wire-contract integration |
| RFC | [RFC 0006: Extended poker utility](rfcs/0006-extended-poker-utility.md) | Implementing | Side pots, rake, ICM, experimental multiway evaluation, and production prerequisites |
| RFC | [RFC 0007: Preflop profile abstraction](rfcs/0007-preflop-profile-abstraction.md) | Implementing | Bounded preflop training for a declared small profile, policy-derived continuation ranges, and coexistence with existing artifacts |
| RFC | [RFC 0008: Unified N-player architecture](rfcs/0008-unified-n-player-architecture.md) | Implementing | One game definition for 2..10 seats, a versioned abstraction layer with measured error, one solver interface, explicit guarantee levels, and staged removal of superseded paths |
| RFC | [RFC 0009: Unified engine delivery](rfcs/0009-unified-engine-delivery.md) | Implementing | Live promotion and wiring, a seat-parameterized solve seam, artifact schema-v2 with abstraction identity, generalized resolving, flop coverage, and the heuristic demotion |
| RFC | [RFC 0010: Multiway frontier evaluation](rfcs/0010-multiway-frontier-evaluation.md) | Accepted | N-way equity frontier evaluator, relaxed player-count gates, and N-way preflop profile training |
| RFC | [RFC 0011: Turn and river continuation strategy](rfcs/0011-turn-river-strategy.md) | Accepted | On-demand subgame resolving for turn/river decisions, river frontier evaluator, and virtual turn/river deals |
| RFC | [RFC 0012: Flop library performance optimization](rfcs/0012-flop-library-performance.md) | Implemented | Parallel training, consolidated mmap binary format, lazy class loading, and warm-start training |
| RFC | [RFC 0013: Preflop Phase 2 — postflop-aware frontier evaluation](rfcs/0013-preflop-phase2.md) | Implementing | Postflop-aware frontier evaluator using CFR-solved flop-rooted river-terminal values in a declared table, with iterative range refinement. Stages 1-2 implemented (N-seat table, evaluator, SPR bucketing, root-value extraction, BSF1 codec, oracle driver, builder/NashConv wiring) |
| RFC | [RFC 0015: Offline frontier evaluation reuse and measurement](rfcs/0015-offline-frontier-evaluation.md) | Implemented | Shared evaluation templates, bounded policy caching, exact extraction, and independently confirmed information-set-consistent deviation measurement |
| RFC | [RFC 0016: Synchronous MCCFR batches and correct strategy averaging](rfcs/0016-synchronous-mccfr-averaging.md) | Implemented | Frozen-profile training epochs, independent full-action averaging, and revision-aware checkpoint compatibility |
| RFC | [RFC 0017: Training pipeline allocation cost](rfcs/0017-training-pipeline-allocation-cost.md) | Implemented | Allocation-efficient revision-two deltas, exact export reuse, ordered seat-artifact loading, and matched end-to-end measurements |
| RFC | [RFC 0018: Zero-rake settlement fast path](rfcs/0018-zero-rake-settlement-fast-path.md) | Implemented | Preserve exact settlement while avoiding provably zero percentage calculations |
| RFC | [RFC 0019: N-seat preflop reconstruction](rfcs/0019-nseat-preflop-reconstruction.md) | Implemented | Exact multiway replay on the negotiated wire contract; an internal prerequisite, not six-max GTO completion |
| RFC | [RFC 0020: Folded-root policy identity](rfcs/0020-folded-root-policy-identity.md) | Implemented | Reject a different root live set before resident or direct resolver lookup |
| RFC | [RFC 0021: N-seat cross-street reconstruction](rfcs/0021-nseat-cross-street-reconstruction.md) | Implemented | Replay supplied hand history into the true flop root; public provenance and complete six-max remain open |
| RFC | [RFC 0022: Complete cash-hand protocol](rfcs/0022-complete-cash-hand-protocol.md) | Implementing | Coherent full-state ledger, public observations and rule identity; provider gates remain open |
| RFC | [RFC 0023: Artifact startup and decision deadlines](rfcs/0023-artifact-startup-and-decision-deadlines.md) | Implemented | Root-aware startup allowance and bounded active waits; no six-max quality claim |
| RFC | [RFC 0024: Depth-limited resolving](rfcs/0024-depth-limited-resolving.md) | Implemented | Street-boundary value estimation for scalable subgame solving; enabling architecture for six-max |
| RFC | [RFC 0025: Off-tree decision handling](rfcs/0025-off-tree-decision-handling.md) | Implemented | Flop-rooted depth-limited resolving and off-tree action-line tolerance |
| RFC | [RFC 0026: Bucket-deduplicated flop library resident format](rfcs/0026-bucket-deduplicated-flop-library-format.md) | Implemented | Bucket-keyed BSFLIB02 packed layout: six-way library 334 GB to 108 MB, 1.37B rows verified lossless |
| RFC | [RFC 0027: Direct train-and-pack flop library production](rfcs/0027-train-and-pack-direct.md) | Implemented | Serialize each trained class straight into its BSFLIB02 section; removes the 440 GiB SQLite intermediate and the pack read pass |
| RFC | [RFC 0028: Sealed warm-start checkpoints](rfcs/0028-sealed-warm-start-checkpoints.md) | Implemented | Store checkpoints under the trainer's (bucket, node) key: 392x less checkpoint data, no SQLite on the training path |
| RFC | [RFC 0029: Stage-6 frontier walks and resident measurement](rfcs/0029-flop-terminal-best-response.md) | Implementing | Make the evaluation walkers model frontier leaves instead of returning zeros, hitting undefined behaviour, or spinning; measure the resident serving path |
| RFC | [RFC 0030: Pin strategy-shaping abstraction parameters](rfcs/0030-pin-raise-cap-in-identity.md) | Implemented | Put the raise cap in the action-abstraction digest and a tree-shape witness in the sealed checkpoint; no cap value change |
| RFC | [RFC 0031: Measure postflop card-abstraction error](rfcs/0031-measure-card-abstraction-error.md) | Implemented | Defer finer bucketing until abstraction loss is measured; pin the bucket scheme in build identity |
| RFC | [RFC 0032: Preserve the full board through the decision pipeline](rfcs/0032-turn-river-board-addressing.md) | Implementing | Stop re-rooting every postflop decision to three cards; repair the subgame digest contract; pin the current truncation with a baseline test |
| RFC | [RFC 0033: Flop library root reachability](rfcs/0033-flop-library-root-reachability.md) | Implemented | Correct the stored root ledger so the shipped six-way library can answer a real limped flop, and record the one-root-per-class scope limit |
| RFC | [RFC 0035: Multi-root flop library](rfcs/0035-multi-root-flop-library.md) | Implemented | BSFLIB03 root directory so one class answers several exact preflop-line ledgers; full RFC 0030 action identity in the header; ClassNotTrainedForRoot miss |
| RFC | [RFC 0036: Six-way turn/river](rfcs/0036-six-way-turn-river.md) | Accepted | Bounded cap-1 on-demand river (seats 5/6) and street-start turn (seats 4-6) resolves with measured serve/decline gates |
| RFC | [RFC 0037: Shape-keyed abstraction policy](rfcs/0037-shape-keyed-abstraction-policy.md) | Implemented | Frozen shape->raise-cap table (no cell promoted; heads-up cap 0, every 3+ seat shape cap 1 at both layers and bands; legacy 3-seat 10 BB cap-0 profile retired 2026-10-26) from a pre-registered ablation plus a legacy-retirement amendment; preflop profiles and rootless flop resolving within the BSFLIB03 single-cap constraint; complete 2..6 dealt-count 100 BB preflop profile set shipped |
| RFC | [RFC 0034: Unversioned engine protocol](rfcs/0034-unversioned-engine-protocol.md) | Implemented | One unversioned proto package, regenerated baseline, restored protocol fuzz target, and removal of internal generation naming |
| Plan | [RFC 0001 and 0002 implementation](plans/0001-0002-implementation.md) | Current | Ordered stages, gates, rollback points, and verification |
| Plan | [RFC 0004-0006 implementation](plans/0004-0006-implementation.md) | Current | Task ownership, execution status, acceptance evidence, and rollback |
| Plan | [RFC 0009 implementation](plans/0009-implementation.md) | Current | Unified engine delivery stages W1-W5, status, and evidence |
| Plan | [RFC 0009 W2c-ii state layer](plans/0009-w2c-ii-state-layer.md) | Current | N-seat belief state layer design, generalization, and certification |
| Plan | [RFC 0029 implementation](plans/0029-implementation.md) | Current | Frontier walks, oracle API migration, resident measurement driver, and the deferred measurement gate |
| Plan | [RFC 0032 implementation](plans/0032-implementation.md) | Current | Digest repair first, then board preservation with the two-view state, labelling, and the deferred replay gate |
| Plan | [GTO delivery roadmap](plans/gto-delivery-roadmap.md) | Proposed | Complete follow-up scope, dependencies, acceptance evidence, and outstanding decisions |
| Plan | [Complete six-max GTO delivery](plans/sixmax-gto-delivery.md) | Current | Whole-feature In Progress status, audited blockers, task dependencies, and no partial-completion claims |
| Plan | [Six-max GTO qualification](plans/sixmax-gto-qualification.md) | Proposed | Independent coverage, quality and resource gates; thresholds and complete-feature sign-off remain unapproved |
| Plan | [T3 six-seat feasibility](plans/t3-six-seat-feasibility.md) | Current | Six-seat tree/solve resource measurement; current abstraction infeasible for 5+ seats, coarser schedule required |
| Strategy | [Poker playbook](../strategy/PLAYBOOK.md) | Current | Baseline poker decisions and bankroll discipline |
| Strategy | [Style selector](../.codex/skills/INDEX.md) | Current | Exploitative style selection and overrides |
| Upstream | [River Club skill](SKILL.md) | Upstream | Official operating contract |
| Upstream | [River Club strategy](upstream-STRATEGY.md) | Upstream | Official decision guidance |
| Upstream | [River Club OpenAPI](openapi.json) | Upstream | Official HTTP protocol definition |

## Document Status

- **Current** describes behavior implemented in the repository.
- **Proposed** describes a design that has not been fully implemented.
- **Historical** records obsolete behavior for debugging or migration context.
- **Upstream** is an imported external source and must track its authoritative
  origin.

Every design document and RFC must declare its status near the title. Proposed
behavior must not be described as if it already exists.

## Documentation Rules

1. All first-party documentation and source-code comments must be written in
   English.
2. Preserve exact protocol identifiers, commands, file names, card notation,
   and required external string literals.
3. Update the relevant design or reference document in the same change as a
   behavior, protocol, build, or operational change.
4. Keep implementation truth in design and reference documents. Keep future
   architecture in RFCs until it is accepted and implemented.
5. Prefer links over duplicated explanations. The root `README.md` is an
   introduction; this file is the documentation index.
6. Use Mermaid for architecture and workflow diagrams.

## RFC Workflow

Cross-module architecture, protocol changes, persisted-format changes, and new
platform abstractions require an RFC. Follow [the RFC process](rfcs/README.md).
Launch an independent approval agent for acceptance; user confirmation is not
required. Record the agent's explicit decision before implementation.
An accepted RFC still describes intent until its status becomes `Implemented`.
Use the repository RFC skill at
`.codex/skills/rfc-authoring/SKILL.md` when drafting or reviewing one.
