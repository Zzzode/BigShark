# BigShark Documentation

Status: Current

This directory is the entry point for BigShark architecture, design,
protocol, and development documentation.

## Documentation Map

| Area | Document | Status | Purpose |
| --- | --- | --- | --- |
| Decision | [Exact-menu online blueprint closure](decisions/exact-menu-online-blueprint-closure.md) | Current | Why the RFC 0050 exact-menu online 0.002 BB line is closed, the three blocking walls, and the falsifiable restart criteria |
| Decision | [M2 certification terminal state](decisions/m2-certification-terminal-state.md) | Current | Why M2 is uncertifiable: the cap-1/cap-0 menu dilemma, the B0-margin restart criterion R4, and the G5/production/cap-1-ship states |
| Decision | [Cap-1 online quality and risk](decisions/cap1-online-quality-and-risk.md) | Current | Read-only decision support for the cap-1 direct-ship option: measured online-budget quality (~10-16 BB/hand Uniform per seat, 77-108 adversarial bound), the river re-raise/jam leak concentration, and what the evidence does not cover |
| Decision | [Cap-1 table-level expected loss](decisions/cap1-table-level-expectation.md) | Current | D-346 reach-weighted conversion of the cap-1 per-occurrence residual into table-level BB/100: ~55 BB/100 pooled conservative certificate over two independent 2,400-hand campaigns with explicit bands, measured arrival frequencies, and disclosed unknowns |
| Decision | [Cap-3/cap-4 serving cost](decisions/cap34-serving-decision.md) | Current | D-380/D-382 two-track record: certification rail failed (0.0794 vs 0.002 BB, ~40x over); serving release withdrawn after the other-river-class measured 7.02 BB/100 (CI 6.58-7.47, end-to-end 7.25 vs the 1.0 rail); includes precise BB/100 method, orbit-mass coverage curve, and 20,000-iteration freeze |
| Decision | [Other-bucket root cause](decisions/other-bucket-rootcause.md) | Current | D-382 follow-up: 99.1% of the 7.02 BB/100 residual is the structural menu term (top two ledgers = 93.1%); equal-iteration cap-6 widening collapses it up to 211k-fold, so a line-conditioned cap-6 extension is a measured conditional-go with three gating receipts; certification rail unchanged |
| Decision | [D-390 Scenario A cap-6 receipts](decisions/d390-scenario-a-cap6-receipts.md) | Current | Measured cap-6 20k on all three aggressive-line ledgers: end-to-end 0.3328 BB/100 (CI 0.326-0.340) passes the serving rail and the 0.5 target on measured cells, while certification stays failed (2.3-6.6x over 0.002); non-monotone structural term disclosed, 1.14M machine-hour three-ledger campaign |
| Decision | [Cap-6 serving options](decisions/cap6-serving-options.md) | Current | D-390 quantified full/narrowed/retire option table (full 0.3328 BB/100 passes 0.5 at 1.20M mach-h; narrow-b 0.8017 passes only 1.0; narrow-a and cap-4 fail); cap-7 discriminator confirmed Rule A (L1 structural term 696x smaller, near-zero, 1.364x cost), so widening is an engineering cost choice not an abstraction limit; no option selected |
| Decision | [Pot>16 hull de-risk](decisions/cap6-serving-options.md#7-pot16-extrapolation-hull-measured-20k-restores-the-pass-with-a-correction) | Current | D-390 pot-32 cap-6: 3k point breaks the limped prediction band (Rule B, convergence-driven) but the measured 20k point closes to 0.00323, restoring a fully measured 0.3292 BB/100 end-to-end pass of both rails; includes a 139-hand remap correction and a family-specific convergence-factor premise |
| Decision | [Pot-40 final hull](decisions/cap6-serving-options.md#8-pot-40-final-hull-no-ledger-exists-residual-bounded-by-proxy) | Current | No pot-40 decision-bearing ledger exists (only terminal pot-400 all-in); the 37 non-terminal hands (5.35% of the other bucket) stay unmeasured but proxy-bounded, with every measured-severity bound still passing both rails; closure needs a frontier-enumeration harness change, not another bench run |
| Architecture | [System overview](architecture/system-overview.md) | Current | Components, boundaries, data flow, and runtime safety |
| Design | [GTO engine](design/gto-engine.md) | Current | Implemented decision logic, solvers, range tracking, RFC 0005 strategy artifacts, and limitations |
| Reference | [Protobuf engine protocol](reference/protobuf-engine-protocol.md) | Current | IDL, pinned generation, compatibility baseline, and conformance vectors |
| Reference | [Exploitability and NashConv](reference/exploitability.md) | Current | The three normalisations, which harness computes what, and the resident fixture conventions |
| Development | [Build and test](development/build-and-test.md) | Current | CMake presets, formatting, tests, sanitizers, and release publishing |
| Development | [Benchmarking](development/benchmarking.md) | Current | Reproducible solver-quality fixtures, metrics, and thresholds |
| Development | [Offline practice table](development/practice-table.md) | Current | Local human-vs-engine GTO practice for 2..10 seats (single engine-served opponent tier, fail-closed without the engine) and the offline boundary |
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
| RFC | [RFC 0022: Complete cash-hand protocol](rfcs/0022-complete-cash-hand-protocol.md) | Superseded by RFC 0040 | Coherent full-state ledger, public observations and rule identity; provider gates closed when the provider retired agent support |
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
| RFC | [RFC 0034: Unversioned engine protocol](rfcs/0034-unversioned-engine-protocol.md) | Implemented | One unversioned proto package, regenerated baseline, restored protocol fuzz target, and removal of internal generation naming |
| RFC | [RFC 0035: Multi-root flop library](rfcs/0035-multi-root-flop-library.md) | Implemented | BSFLIB03 root directory so one class answers several exact preflop-line ledgers; full RFC 0030 action identity in the header; ClassNotTrainedForRoot miss |
| RFC | [RFC 0036: Six-way turn/river](rfcs/0036-six-way-turn-river.md) | Accepted | Bounded cap-1 on-demand river (seats 5/6) and street-start turn (seats 4-6) resolves with measured serve/decline gates |
| RFC | [RFC 0037: Shape-keyed abstraction policy](rfcs/0037-shape-keyed-abstraction-policy.md) | Implemented | Frozen shape->raise-cap table (no cell promoted; heads-up cap 0, every 3+ seat shape cap 1 at both layers and bands; legacy 3-seat 10 BB cap-0 profile retired 2026-10-26) from a pre-registered ablation plus a legacy-retirement amendment; preflop profiles and rootless flop resolving within the BSFLIB03 single-cap constraint; complete 2..6 dealt-count 100 BB preflop profile set shipped |
| RFC | [RFC 0038: Retire schema-v1 artifacts](rfcs/0038-retire-schema-v1-artifacts.md) | Implemented | Deleted the two-seat schema-v1 SQLite dialect and algorithm-revision-1 read/write support; only schema v2/v3 rev2 artifacts remain, and a v1 file fails closed as UnsupportedVersion. Fixed-runout test/benchmark policies rekey into flop-rooted river-terminal v2 artifacts carrying their conditional runout |
| RFC | [RFC 0039: Nested river continuation values](rfcs/0039-nested-river-continuation-values.md) | Accepted (amended 2026-10-28) | Range-conditioned nested river subgame solve with cached RFC 0012 warm state replacing the measured-rejected equity leaf at the depth-limit river boundary; amended two-number gate (bucket BR convergence predicate + exact-hand/composed-M2 deployment evidence); direct-solve host enablement withdrawn (addendum D3), default-deny bounded entry instrumentation only; enablement deferred to separate Stage 3/4 RFCs |
| RFC | [RFC 0040: Retire the River Club Agent API v2.1 integration](rfcs/0040-retire-river-club-agent-api.md) | Implemented | Remove the discontinued provider's runner, platform adapter, bin launchers, journal replay tool, and imported upstream docs; repository becomes the platform-neutral engine, host, generic Node client, and offline practice table; supersedes RFC 0022 |
| RFC | [RFC 0041: Learned river continuation-value network](rfcs/0041-learned-river-continuation-value-network.md) | Accepted; **closed 2026-10-29 Stage 1 FAILURE** | DeepStack-style offline f32 neural estimator for RFC 0039 turn-leaf continuation values; never implemented. Stage 1 label campaign could not meet the frozen 0.003 BB noise floor on 15.3% of real turn boards (three-flush/monotone), and iteration/seed scaling, linear/discounted CFR, and opponent-action enumeration all failed; full evidence and rationale in Amendment 2. Amendment 1 (decision envelope p95/p99 <= 10,000 ms) remains in force independently |
| RFC | [RFC 0042: Equity-strength postflop card bucketing](rfcs/0042-equity-strength-bucketing.md) | Implemented (KEEP) | Measurement-only W5 result: exact-EHS buckets (K 32/64/64) cut exact gaps decisively (2p-river 0.335->0.129 BB) but do not converge in the 10 s online wall (river bucket 0.099/0.361 > 0.05, turn bytes 2.2-2.3x) — KEEP 2026-10-30, measurement code deleted, evidence + mechanical evaluator retained; rerouted as an offline-blueprint (W4) candidate requiring its own RFC and eight-site artifact flip |
| RFC | [RFC 0043: Offline equity-strength blueprint feasibility](rfcs/0043-offline-equity-blueprint-feasibility.md) | Implemented (no-go) | W4 measured 2026-10-31: equity reaches <=0.05 BB river gaps at 10M (2p 0.016/3p 0.027/4p 0.025) with large positive paired benefits, but the frozen 20% relative plateau never forms — the nine-tier control fails the same predicate (~T^-0.55 RM+ decay), so no r* exists and the 134,459-class AC6/AC7 cost gates were never measured; independent ruling structural no-go (not incomplete) for all three bands, measurement code deleted, evidence retained; a reachable recertification instrument requires a new RFC |
| Plan | [RFC 0001 and 0002 implementation](plans/0001-0002-implementation.md) | Current | Ordered stages, gates, rollback points, and verification |
| Plan | [RFC 0004-0006 implementation](plans/0004-0006-implementation.md) | Current | Task ownership, execution status, acceptance evidence, and rollback |
| Plan | [RFC 0009 implementation](plans/0009-implementation.md) | Current | Unified engine delivery stages W1-W5, status, and evidence |
| Plan | [RFC 0009 W2c-ii state layer](plans/0009-w2c-ii-state-layer.md) | Current | N-seat belief state layer design, generalization, and certification |
| Plan | [RFC 0029 implementation](plans/0029-implementation.md) | Current | Frontier walks, oracle API migration, resident measurement driver, and the deferred measurement gate |
| Plan | [RFC 0032 implementation](plans/0032-implementation.md) | Current | Digest repair first, then board preservation with the two-view state and labelling; the journal-replay gate was removed by RFC 0040 |
| Plan | [RFC 0039 implementation](plans/0039-implementation.md) | Current | Nested river continuation values: seam, nested model, M2/M1 gates, default-deny host entry |
| Plan | [GTO 10-seat 100BB milestone ledger](plans/gto-10seat-milestones.md) | Current | THE mainline: M1..M10 track complete 100BB GTO opponent support per table size; reporting granularity is the milestone only, with exploitability/coverage/serve metrics at completion |
| Plan | [GTO delivery roadmap](plans/gto-delivery-roadmap.md) | Proposed | Complete follow-up scope, dependencies, acceptance evidence, and outstanding decisions |
| Plan | [Post-RFC-0041 10-seat direction](plans/post-0041-10seat-direction.md) | Current | After the RFC 0041 neural closure: offline 7-10 seat preflop blueprints, live-seat-count-scoped bounded resolving, 10-second wall recalibration, sampled NashConv for high seats, card-abstraction refinement; no learned components |
| Plan | [RFC 0042 implementation](plans/0042-implementation.md) | Current | Measurement-only EquityStrengthV1 card bucketing: Stage 2 declared exact-EHS kind, in-memory trainer seams, benchmark ablation arm, ctests; freeze addendum pending pilot/flop-probe evidence; Stage 3 paired gates; any production flip is a separate RFC |
| Plan | [RFC 0043 implementation](plans/0043-implementation.md) | Current | COMPLETE: Stage 1-2 freeze + Stage 3 frozen campaign returned structural no-go for all river bands (plateau unreachable for both grids); Stage 4 independent ruling recorded and all measurement code deleted; evidence CSVs retained |
| Plan | [RFC 0044 implementation](plans/0044-implementation.md) | Current | Stage 1 complete (EquityStrengthV1 re-implementation, extended-ladder driver with BSWARM03 resume, frozen CERT/QUAL evaluator with 14 tests); Stage 2 freeze addendum under independent review, gating the HU 30M-300M campaign; measurement-only |
| Plan | [RFC 0045 implementation](plans/0045-implementation.md) | Current | COMPLETE 2026-11-04: terminal verdict instrument-inadequate-uniform-ehs (run 2 368/368 independently VALID; uniform EHS passes exact QUAL at k_river>=128 but fails the 0.005 BB CERT bar at every grid; conditional arm river-inert); G3 cap-0 HU river tree >100M nodes infeasible -> cap-1/depth-limited only; measurement code deleted, frozen TS evaluator/evidence/RFC retained; verdict + census handed to a separate production blueprint/mount RFC; no production change |
| Plan | [Complete six-max GTO delivery](plans/sixmax-gto-delivery.md) | Current | Whole-feature In Progress status, audited blockers, task dependencies, and no partial-completion claims |
| Plan | [Six-max GTO qualification](plans/sixmax-gto-qualification.md) | Proposed | Independent coverage, quality and resource gates; thresholds and complete-feature sign-off remain unapproved |
| Plan | [T3 six-seat feasibility](plans/t3-six-seat-feasibility.md) | Current | Six-seat tree/solve resource measurement; current abstraction infeasible for 5+ seats, coarser schedule required |
| Strategy | [Poker playbook](../strategy/PLAYBOOK.md) | Current | Baseline poker decisions and bankroll discipline |
| Strategy | [Style selector](../.codex/skills/INDEX.md) | Current | Exploitative style selection and overrides |

### Historical (closed RFCs)

| Plan | [RFC 0041 implementation](plans/0041-implementation.md) | Historical | Closed 2026-10-29 at Stage 1 (recorded FAILURE, RFC 0041 Amendment 2): label generator/PyTorch trainer/BSNCV01/inference/host path never built; kept as the failure record and sequencing reference |
| Plan | [RFC 0041 Stage 1 fixed label-cell plan](plans/0041-label-cells.md) | Historical | Frozen-2026-10-28 cell quotas (16,432 turn classes, 360-class campaign, 5 closures, 3 profiles, 10M x 3 scheme); withdrawn on 2026-10-29 after the texture pilot and hardness survey proved the floor unreachable; closure evidence pointer in RFC 0041 Amendment 2 |

## Document Status

- **Current** describes behavior implemented in the repository.
- **Proposed** describes a design that has not been fully implemented.
- **Historical** records obsolete behavior for debugging or migration context.

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
