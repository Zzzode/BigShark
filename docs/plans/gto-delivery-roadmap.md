# GTO Delivery Roadmap

Status: Proposed

This document tracks the complete follow-up scope requested on 2026-09-14.
It is a dependency and acceptance map, not evidence that future functionality
exists. New designs require independent approval-agent review before
implementation; user confirmation is not required for RFC acceptance.

RFCs 0004-0006 received formal independent approval on 2026-09-14 following
RFC 0005 resubmission. RFC 0004 is now Implementing; Stage 1 is complete
and Stage 2 is ready.
The [implementation plan](0004-0006-implementation.md) maps their accepted
scope to tasks and verification gates.

## Outcome and Boundaries

Deliver a platform-neutral engine with reproducible multi-size heads-up
training, durable strategies, bounded runtime resolving, a typed production
protocol, and separately validated extended poker utility.

Success means executable capability, native verification, measured coverage,
documented limitations, and exercised rollback. It does not mean unrestricted
full-game equilibrium, guaranteed profit, or a world-leading ranking without
independent competitive evidence.

No live poker session, higher stake, cloud training spend, new account, or
second-platform credential use is authorized by this development roadmap.
Do not manufacture provider facts or use simulated adapters as evidence of
a real second-platform integration.

## Verified Starting Point

- Multi-street CFR repair and three reproducible benchmark families are
  committed in `c29c81f`.
- That commit passed local Release, ASan/UBSan, Node, Protobuf, and historical
  replay checks. Linux CI run
  [34773707907](https://github.com/Zzzode/BigShark/actions/runs/34773707907)
  passed, including all three benchmark families.
- The multi-street solver is still offline/test-only, with a small validation
  tree. No stored blueprint or live multi-street resolver exists.
- RFC 0002 Stages 0-4 are complete; generated v1 messages are not connected to
  the production host.

These are recorded baseline results, not newly executed tests for this
documentation change.

## Work Register

| ID | Deliverable | Owner | Depends on | Status / completion evidence |
| --- | --- | --- | --- | --- |
| B0 | Multi-street repair and repeatable baseline | `engine/` | None | Complete at `c29c81f`; preserve existing gates |
| B1 | Real heads-up betting rules | Poker | RFC 0004 approval | Complete for flop-rooted profile; native and sanitizer tests, independent 150-root oracle |
| B2 | Multi-size offline blueprint trainer | Solver | B1 | Proposed; full/sampled traversal and independent oracle |
| B3 | Expanded quality and capacity benchmark | Solver benchmarks | B2 | Proposed; ranges, boards, depths, seeds, cost, coverage |
| S1 | Checkpoint and immutable strategy artifact | Artifact boundary | B2, RFC 0005 approval | Proposed; split-run parity, corruption and interruption tests |
| S2 | Policy-based range propagation and lookup | Solver, policy | S1 | Proposed; blockers, action reach, exact coverage and miss tests |
| S3 | Certified bounded on-tree resolving | Solver, service | S2 | Proposed; gadget, independent BR non-regression, timeout tests |
| P5 | Protobuf mapping, validation, host | Protocol, service, host | Existing RFC 0002 | Ready; framing/fuzz, exact amounts, domain parity |
| P6 | Binary Node client | `clients/node` | P5 | Pending; bigint, correlation, restart, capabilities |
| P7 | River v1 migration | River adapter, runner | P6 | Pending; replay parity, dry run, canary and rollback |
| P7a | Multi-size strategy protocol and live routing | Protocol, policy, River | P7, B3, S2, RFC 0005 | Proposed; minor-1 negotiation and full distributions |
| S4 | Restricted terminal-decision resolving promotion | Policy, River | P7a, S3 | Proposed; measured deadlines, eligibility, restart, fallback provenance |
| S5 | General multi-request live resolving | Solver, service, client, River | S4 and follow-up continuation RFC | Pending; accepted policy identity, reach/bounds, durable restart and certified fallback |
| F1 | Solved heads-up preflop and continuation ranges | Poker, solver, policy | B3, S1 | Proposed under RFC 0004; separate preflop quality evidence |
| U1 | Contribution ledger and side-pot settlement | Poker | B1, RFC 0006 approval | Proposed; independent exact payouts and refunds |
| U2 | Rake utility | Poker | U1 | Proposed; declared policy, cap/rounding, conservation |
| U3 | Bounded exact ICM | Poker | U1 | Proposed; full field, payouts, eliminations, prize units |
| U4 | Multiway training and evaluation | Solver | B2, U1 | Proposed; experimental status, independent deviations |
| U5 | Extended production protocol and adapters | Protocol, service, adapters | U1-U4, provider audit | Follow-up v2 RFC required; complete state and utility units |
| P8 | v0 removal and release packaging | Host, client, release tooling | P7 and explicit removal approval | Pending; no remaining callers, rollback release artifact |
| A2 | Real second platform | New adapter | P6 and selected official interface | Provider not selected; real conformance and lifecycle evidence required |
| F2 | Broader multiway preflop policy | Solver, policy | F1, U4 | Research/quality gate; HU policy cannot replace six-max charts |
| D1 | Current architecture, references, operations | Respective module owners | Each delivered stage | Update alongside code; docs/RFC checks |
| V1 | Linux HiGHS-enabled verification | Solver, CI | Existing Linux fallback lane | Pending; exact backend, dependency and benchmark evidence |

## Design References

- [RFC 0004](../rfcs/0004-heads-up-blueprint.md): real heads-up rules,
  multi-size training, information states, and evaluation.
- [RFC 0005](../rfcs/0005-strategy-artifacts-and-resolving.md): artifact
  persistence, policy lookup, resolving, and minor-1 integration.
- [RFC 0006](../rfcs/0006-extended-poker-utility.md): settlement, rake, ICM,
  experimental multiway evaluation, and the subsequent production gate.
- [Existing implementation plan](0001-0002-implementation.md): authoritative
  Stage 5-9 migration order and rollback criteria.

Do not amend accepted RFCs to smuggle in new design. Additive changes follow
their compatibility policy; material deviations need a superseding RFC.

## Dependency Order

After approval, produce implementation tasks mapped to each accepted RFC.
The recommended first implementation checkpoint is B1/B2: establish correct
rules and a small-game oracle before adding persistence or live routing.

P5/P6 form a separately testable engineering stream with existing design
approval. Keep its commits and parity evidence separate from new strategy
changes. Coordinate shared service-domain types before running both streams
in parallel; never let contributors edit the same contract independently.

S1/S2 follow the trainer's stable domain records. Start offline S3 at river
where exact certification is practical. S4 only admits roots with no later
hero decision; S5 explicitly owns the unfinished general continuation
contract. B3 and P7 precede P7a/S4 live promotion.

Implement U1-U3 arithmetic before attaching U4 strategy or U5 production.
Stage P8 remains gated even if newer strategy work is complete. A2 requires
an actual provider; it cannot be closed by a local fake engine.

## Verification and Release Gates

For C++ changes, run all `AGENTS.md` commands, including the multi-street
benchmark, Node checks, Protobuf checks, and historical replay. Run sanitizers
for memory, framing, persistence, arithmetic bounds, and traversal changes.
Protocol stages additionally run Buf and cross-language conformance.

Every checkpoint records:

- exact commit and changed modules;
- native tests and fixture revisions;
- game, ranges, utility, and action abstraction actually supported;
- quality measurements and whether exact or estimated;
- resource limits and measured elapsed time / memory;
- fallback, unsupported behavior, and rollback exercise;
- independent review findings and dispositions.

Runtime promotion requires no illegal actions, no unexplained fallback,
no changed freshness/leave/resume behavior, and source-accurate provenance.
Transport-parity tests use the old strategy; new-strategy tests declare
expected decision changes separately.

Large training runs and live canaries need an agreed resource/operating
budget. Record stake, buy-in, stop-loss, and duration before any live play.
Offline replay and synthetic tests can proceed without live authorization.

## Decisions Still Required

| Decision | Safe default | What remains blocked |
| --- | --- | --- |
| Follow-up RFC approval | Independently review new contracts; 0004-0006 already Accepted | General continuation, external crash-safe recovery, and extended v2 contracts |
| Larger training budget | RFC 0004 bounded local runs | Production-scale coverage claims |
| Reference deployment hardware | Record local measurements only | Default latency promotion |
| Second official platform and access | No platform selected | A2 production integration |
| Extended provider rule/data audit | Reject unknown variants | U5 wire contract and live extended utility |
| Live rollout operating limits | Offline-only verification | Live canary evidence |
| Removal checkpoint | Retain v0 | P8 destructive cleanup |

The unselected provider does not block local solver or protocol work.
Failure of a research-quality gate requires evidence-driven replanning;
it must not be hidden by marking a stage complete.

## Completion Policy

Mark an item complete only after its executable behavior and required evidence
exist. A proposed RFC, empty facade, calculator without policy integration,
sampled self-play win rate, or passing tiny fixture does not complete a
production feature.

This roadmap remains Proposed as a description of future delivery, even
though RFCs 0004-0006 have been approved. Proposed work-register entries above
mean implementation is pending, not that those RFC approvals are missing.
The earlier approved protocol work retains its existing approval status.

## Design Review Record

An independent read-only review identified the following findings. The
corrections are in the proposed RFCs; implementation evidence is still needed.

| Severity | Finding | Disposition |
| --- | --- | --- |
| P1 | Certified full continuation could be lost between requests | Restrict initial live resolver to terminal hero decisions; keep general continuation as S5 with a separate approval gate |
| P1 | Sampled average update rule insufficiently specified | Pin reference revision; specify update actor, equations, sampling, traversal, PRNG, and expected-update tests |
| P2 | Checkpoint identity and iteration rollback incomplete | Match full root/schedule identity and restore RNG plus newly created records |
| P2 | Gross and net contribution ledger conflated | Define gross, refunds, net, stack timing, and unequal-all-in example |
| P2 | Partial-load cache conflicts with startup-only I/O | Load explicit complete root subsets into bounded resident memory; no decision-path SQL |
| P2 | Minor-0 enum compatibility not fully isolated | Gate new mode/source values and capability lists as well as response shapes |

A second independent read-only review confirmed all six design findings were
resolved with no remaining P1/P2 blockers. Its P3 clarification about the
pre-award stack invariant was also applied. Those reviews were advisory under
the prior process and do not retroactively accept the RFCs. The current
process requires an explicit approval-agent decision and record. Design
acceptance is not numerical validation or authorization for live play.

## Formal Approval Record

On 2026-09-14, approval agent `7f86ec31-e5b6-4753-a478-a48366ac2edd`
approved RFC 0004 and RFC 0006, and requested changes to RFC 0005:

- P1: terminal-only resolving still allowed private-hand-dependent mixing
  of certified policies and did not define current-request failure recovery.
- P2: per-hand eligibility had no adapter-to-service mode enforcement path
  and did not distinguish artifact-root safety from full-hand safety.

RFC 0005 now requires private-independent whole-range selection, separate
solver/action seeds, explicit caller-side resolving mode gating, pinned
artifact identity, root enrollment, and monotone eligibility invalidation.
External process and operational failures explicitly lose certification.

The original review agent had finished and could not be resumed by the tool.
Replacement approval agent `a69eb9b5-dc85-4915-aa75-f7af38d5e72b` received the
original objections, reviewed the entire revised RFC, and approved it with
no remaining blockers. Each RFC Decision records the reviewed pre-decision
SHA-256, actual approver, scope, and risks.

No runtime capability, production promotion, or resource/operational
authorization is implied by these approvals.
