---
rfc: "0023"
subject: "Artifact Startup and Decision Deadlines"
status: "Implemented"
authors: "/root"
created: "2026-10-14"
updated: "2026-10-14"
owners: "Node framed client, River adapter, River runner, integration tests"
supersedes: ""
superseded-by: ""
---

# RFC 0023: Artifact Startup and Decision Deadlines

## Summary

Apply the existing bounded artifact-startup allowance to explicitly configured
resident roots as well as flop libraries. Independently bound the entire framed
adapter decision wait, including process readiness and any restart inside the
generic client's request, by the caller's existing decision timeout. Complete
off-table startup preflight before joining; never wait unconditionally for a
long preflight when already seated. This is a runtime correctness prerequisite,
not complete six-max coverage, a loader speedup, or strategy qualification.

## Motivation

Full true Debug ASan/UBSan verification of the six-max reconstruction changes
failed its Node gate: 14 of 132 Node cases failed, all in real resident-root
integration paths. Twelve failed at the ten-second capability warmup; two
received an operational fallback instead of the required blueprint. Changing
only tests would leave the actual implicit adapter path's same startup gate.
Extending startup waiting without bounding active-turn waits would instead
increase the existing live-clock risk. Both boundaries need a coherent fix.

## Goals

- Preserve real immutable-fixture integration and all strategy assertions.
- Give every configured artifact source the existing finite startup allowance.
- Include readiness and RPC waiting in one monotonic adapter decision deadline.
- Preserve negotiation, sources, digest checks, legal recovery, and wire fields.
- Finish off-table preflight before invoking the existing join workflow.

## Non-Goals

- Changing C++ loading, artifact formats, native build flags, fixtures, seeds,
  iterations, numerical quality tolerances, or any training algorithm.
- Adding a generic process-client API or changing negotiation/restart mechanics.
- Changing the requested solver budget or the host's solve/certify semantics.
- Implementing engine cancellation or guaranteeing real-time OS scheduling.
- Live play, new platform endpoints, credentials, spending, or publication.
- Completing six-max GTO or closing the complete resource/coverage/quality gate.

## Current State and Evidence

`ResidentPolicySet::build` validates and indexes eager artifacts before the
host answers its first frame. `platforms/river-club/src/engine.ts` already gives
one flop library 300 seconds and two or more 600 seconds, but
`warmupTimeoutMsFor(libraries)` ignores standalone resident roots and returns
ten seconds without libraries. Four real-binary test files also hardcode
ten-second root warmups.

A read-only startup diagnostic used the existing process client, capability
request, and one unchanged native `bs-resident-fixture` publication. The same
2.8 MiB policy had SHA-256
`06a604f3afe6d31841a2ac97637bd184d23e7c789e4ff4b0e91c79b45d4e16aa`.
Measured on this checkout on 2026-10-14:

| Build | No artifact | Same pinned resident artifact |
| --- | ---: | ---: |
| Release | 27 ms | 338 ms |
| True Debug ASan/UBSan | 163 ms | 10,798 ms |

These are diagnostic observations, not a cold-cache benchmark or six-seat
performance/quality result. No native sanitizer error was reported by this
diagnostic. They demonstrate the ten-second startup gate is insufficient on
this verification build, not that an untrusted loader check should be removed.

`decideV1` awaits `client.start()` before invoking `request(timeoutMs)`.
`ProtoEngineProcessClient.request()` itself awaits `start()` again before
its transport timer starts. Negotiation can resolve a start after an
out-of-band probe failure killed its child, so that second call can restart.
Bounding only the adapter's first readiness wait is insufficient.

The runner starts warmup concurrently, then currently joins before awaiting
it (`apps/river-club-agent/main.ts`). That ordering exposes the first hand to
startup delay. These verified facts overlap RFCs 0002/0005/0009/0012, whose
wire, artifact, and strategy contracts remain unchanged by this proposal.

## Design Principles

1. Startup allowance is not an action-clock allowance.
2. Reuse existing configuration, client lifecycle, and bounded startup policy.
3. One adapter deadline covers every asynchronous wait, including hidden restart;
   the generic client owns suppression of its internal expired dispatch.
4. A timeout cannot turn a late or failed strategy into a qualified action.
5. Prefer local changes over a new readiness service or generic client contract.

## Proposed Design

### Artifact startup

Extend the existing helper locally to
`warmupTimeoutMsFor(libraries, roots = [])`, where roots use the existing
`ResidentRootSpec` type. Count explicit root specs plus library arguments:
zero sources retains 10,000 ms; one uses 300,000 ms; two or more use 600,000 ms.
This is a finite administrative loading allowance, not an estimate of
per-source cost. Existing one-argument and library-only results are unchanged.
The implicit adapter supplies its actual roots. Explicit real-root test clients
use the same policy rather than a duplicate sanitizer-only timeout switch.

### Framed decisions

Anchor a monotonic deadline at `decideV1` entry using the caller's existing
`timeoutMs` (default 2,000 ms). Nonfinite or nonpositive framed budgets fail
through the existing legal operational recovery instead of starting work.

Use one private adapter helper to race a promise against the remaining deadline,
clearing its timer on every outcome. Bound both readiness and the request
promise with that helper. Recheck the remaining time after readiness and mapping
and before starting the RPC; an expired decision must not send a new RPC.
Give the request only its remaining transport time. The second race remains
necessary because the generic request can restart before its own timer starts.
Do not change requested `solve_time_budget_ms`, mode, seed, version, guarantee,
or artifact pin; hard adapter expiration still disqualifies a late result.

If startup outlives an active decision, return the existing legal fallback
without killing the shared startup promise. It may finish for a later turn;
its rejection must remain handled and its eventual completion must not resume
the expired decision into an RPC. A late request response is not accepted.
Generic transport timeout/exit cleanup stays unchanged. A timed-out startup may
also still end in the existing generic warmup failure. This proposal does not
provide unconditional GTO execution or a crash-safe baseline.

### Generic request deadline and local-closure finding

The first independent review found a P1: adapter promise races cannot cancel
the generic client's continuation after its internal `await start()`. It can
send an expired RPC and its short transport timer can kill the newly warmed
child. An adapter-only change cannot preserve shared warmup while suppressing
that private continuation. Calling `stop()` would instead cancel startup and
fail other pending requests. No existing adapter extension point can enforce
the wire-dispatch invariant. The root cause is in the generic framed client's
`request`, so this revision authorizes its smallest local correction.

Keep the public signature `request(envelope, timeoutMs = 2000)`. At method entry
anchor a monotonic deadline. Race its startup promise against remaining time
without killing shared startup, then recheck the deadline before `enqueue`.
If the wait expires, the request method rejects and cannot later continue into
`enqueue`, even if shared startup subsequently completes. Clear the private
startup-wait timer on all outcomes. Give `enqueue` only remaining time and
reject a response that arrives after the monotonic deadline, even if event-loop
delay prevented the transport timeout callback from running first.

Administrative `start()` and its per-phase warmup/negotiation timers are
unchanged. Existing transport timeout after enqueue still kills the child and
fails outstanding requests. This proposal does not cancel an already-enqueued
native computation; it prevents a not-yet-enqueued expired RPC and rejects
late output. The adapter's outer request race remains necessary for structural
injected clients that do not implement this concrete lifecycle.

This established-contract tightening affects every caller of the framed generic
client, including protocol tests and River integration; NDJSON is unchanged.
New native Node cases must prove both shared warmup reuse and no late actual
decision frame after a request-internal restart. The mechanism belongs here,
not in duplicated adapter-specific cleanup or a new readiness API.

### Runner preflight and tests

If the initial authoritative state is off-table, await the already-created
warmup promise before `findAndJoin()`. If already seated, do not await a long
preflight before entering the state machine; active decision waits are bounded
as above. Keep existing catch/log/recovery, join selection, and exit behavior.

The real implicit-client regression prewarms with `warmEngine(config)` before
`decide(config)`, without injecting a client or adding a fake library. It still
tests the shipped keyed singleton, root-only launch line, negotiated digest and
guarantee, and reuse on a second decision. Explicit real clients similarly
complete their declared startup before measuring decision calls.

Native Node tests add unresolved/failed/late startup, exhausted budgets,
remaining-time handoff, request-internal restart wait, late response rejection,
and warmed implicit reuse coverage. Use the existing timer-mocking convention
and original real fixture. Test the runner preflight ordering through the
project's native Node framework without performing live CLI operations.

## Dependency Rules

The generic framed client owns its request's pre-enqueue deadline; the River
adapter owns its broader mapping/readiness/request deadline and artifact startup
configuration; the existing runner owns off-table preflight ordering. No new
layer, readiness API, environment option, C++ dependency, strategy implementation,
or dependency edge is introduced. Tests remain within their current owners.

## Compatibility and Migration

No Protobuf, CLI, runtime file, or artifact schema changes. v0 is unchanged.
Existing helper callers retain the default roots argument. Generic and adapter
framed `timeoutMs`
now bounds readiness plus response waiting instead of granting an additional
unbounded readiness phase; this intentional tightening is documented. An
active cold turn may recover sooner rather than wait for startup. Library-only
startup allowances, negotiation opt-ins, and error/fallback semantics remain.
Requested solver fields are unchanged; no quality label is strengthened.

## Security and Operational Impact

No network or credential use. All artifact checks and pins remain mandatory.
Startup remains finite, while active waits consume only the caller's budget.
The outer adapter boundary rejects late results even during generic restart.
Promise races do not cancel native work: a child can continue startup in the
background. Callback scheduling and synchronous parsing/normalization can
overshoot elapsed wall time; this is not a hard real-time scheduling guarantee.

## Alternatives Considered

- Only increase test timeouts: leaves the real implicit root startup defective.
- Inject a test client or fake library: loses the original implicit root-only
  regression and does not fix production.
- Only increase root startup: worsens already-seated/cold-turn waiting.
- Only bound the first `start`: misses the generic request's internal restart.
- Kill shared startup from the adapter: fails other waiting calls and prevents
  the newly warmed child from being reused.
- Add generic readiness/cancellation APIs or a readiness service: larger than
  correcting the existing request method's proven late-dispatch continuation.
- Speed up or weaken native loading to fit ten seconds: no measured local
  loader defect justifies a native change or loss of validation.

## Risks

- Slower startup failure reporting uses the existing 300/600-second per-phase
  allowance. Warmup and each negotiation probe reuse it; this is not a single
  300/600-second total-preflight cap. The number of probes is already finite.
- Already-seated or restart decisions can still fall back; this is visible and
  cannot satisfy complete GTO coverage.
- An outer timeout must handle delayed rejection and suppress later dispatch;
  native timer/restart negative controls are mandatory.
- One unchanged requested solve budget can exceed remaining transport time;
  the client and adapter reject overdue output. Refining whole native work
  cancellation remains outside this narrow fix and the six-max T6 gate stays open.

## Verification Plan

Run focused Node regression tests, the full `npm run check`, protocol checks,
documentation/RFC checks, and `git diff --check`. Run the real full CTest Node
gate against both Release and true Debug ASan binaries without skipped real
cases, changed fixture/solver assertions, or new sanitizer optimization flags.
Retain the ongoing full native ASan suite's result separately; a repaired Node
gate alone cannot establish that whole suite passed. Native sources are unchanged.

## Rollout Plan

1. Independent approval of this complete proposal before runtime edits.
2. Add negative controls and implement the three local runtime changes.
3. Verify focused and complete Node/protocol workflows on both native builds.
4. Review the full diff and evidence; update current docs and delivery register.
   Do not promote six-max or mark older RFCs implemented without their own gates.

## Rollback Plan

Revert only this RFC's generic framed client, adapter, runner, and test changes.
No stored data changes
or deletion are needed. Existing artifact generations remain intact. Rollback
restores the known ten-second root startup and unbounded readiness behavior;
retain that risk in the delivery register rather than advertising qualification.

## Open Questions

None for this bounded runtime repair. Provider/rule/quality and complete native
resource/deadline acceptance remain the separate six-max delivery tasks.

## Acceptance Criteria

- Bare and library-only startup values and launch lines remain unchanged;
  explicit root-only and mixed sources use the documented bounded values.
- Timed-out or failed readiness sends no new decision RPC, including actual
  internal generic restart continuation; background readiness
  rejection is handled and an expired call cannot dispatch later.
- Readiness plus request waiting uses one monotonic deadline, including the
  generic request-internal restart case; late output cannot become an action.
- Off-table preflight precedes join; already-seated startup does not block the
  state machine unconditionally.
- All real-root Node integration cases pass against Release and true Debug
  ASan without skipping cases or weakening poker assertions.
- Full Node/protocol/doc/RFC gates pass; an independent source/evidence review
  confirms scope, compatibility, timer cleanup, and remaining six-max gaps.

## Decision

Author agent: /root
Approved by: /root/sixmax_runtime_audit
Decision date: 2026-10-14
Review outcome: Approved
Reviewed scope: All 18 sections of revised proposal SHA-256
`964ed2fba8bb7e42d4704651554d95fa1bd7ede3adc8e759edf835bb16784129`,
the relevant generic client, adapter, runner and existing test paths.
Review summary: The original P1 late internal enqueue finding is resolved by
the generic request-owned deadline, preserving shared startup with no new API.
P2 now distinguishes per-phase startup allowances from total preflight time.
No blocking findings remain. Non-blocking reminders: include the generic client
in rollback (corrected above) and reject invalid generic request budgets before
startup (to be pinned by implementation tests). Approval is design-only; native
verification, runtime source/evidence review and complete six-max gates remain.
No live, credential, spending or policy-publication authorization is inferred.

Review 2026-10-14 by `/root/sixmax_runtime_audit`: Changes Requested for
proposal SHA-256
`22e649549bb35236dfa2d182c7f78ccab4004e48b3630d0363b1ee6e190ad819`.
P1: an outer race cannot prevent late internal enqueue after restart.
Resolved by explicitly authorizing the smallest generic request-method fix,
with unchanged API shape and mandatory no-late-frame/shared-warmup tests.
P2: startup 300/600 seconds is per phase, not a total preflight deadline.
Resolved by specifying the unchanged warmup/negotiation phase budgets precisely.
That revision required resubmission; the later explicit approval above applies
only to the independently reviewed revised proposal.

## Implementation Plan and Evidence

The test worker owns the existing real-root/client test files and the new
adapter deadline regressions. The main agent owns generic request timing,
adapter timing/startup selection, the runner's local preflight order, its
source-order regression, integration documentation and final verification.
No worker may change native sources or unqualified strategy claims.

Stages: bounded negative controls against the old runtime; implement the
approved deadline and startup behavior; focused Node checks; full Release and
true Debug ASan Node gates; protocol/docs/diff checks and independent evidence
review. Retain the original full native ASan result separately, including its
failed Node gate, and never report a targeted rerun as a fresh full native run.
Complete six-max remains In Progress throughout this internal checkpoint.

Verified implementation checkpoint (2026-10-14):

- Old-runtime bounded RED: 43 client/adapter cases, 23 pass and 20 expected
  failures, zero skipped/cancelled, 3.17 seconds. These characterize missing
  startup/restart bounds, expired dispatch, reset budgets and accepted late output.
- Focused fixed-runtime client/adapter plus structural runner guards: 45/45
  passed in 3.23 seconds. The original 15 ms enqueued transport-timeout case
  still executes against an already-started real scripted child.
- `npm run check`: strict types/build/source policy, protocol lint/format/
  breaking and 25/25 protocol cases, docs/RFC checks and 156/156 Node cases
  passed; zero skips/cancellations. No wire or fixture/quality assertions changed.
- `ctest --preset release -R '^node$' --output-on-failure`: passed in
  9.38 seconds, with 156/156 Node cases and zero skipped/cancelled, using the
  actual Release target through the existing CMake wrapper.
- `ctest --preset asan -R '^node$' --output-on-failure`: passed in
  73.91 seconds, with 156/156 Node cases and zero skipped/cancelled, using
  `build/asan/apps/engine-host/bigshark-engine`. Actual configured/implicit
  root-only blueprint, digest and guarantee assertions passed. Cache remains
  `Debug`, `-g`, Clang and sanitizers enabled; no optimization override.
- Historical replay: zero usable snapshots, explicitly no historical coverage.
  Synthetic replay: six decisions, zero illegal actions/JavaScript fallbacks,
  average 303.90 ms and maximum 942.8 ms. This is engineering regression only.
- Documentation checks passed for 442 text files, RFC checks for 23 RFCs,
  and complete diff whitespace checks passed.

Source checkpoint by `/root/sixmax_runtime_audit`: Approved, no blocking
finding. Its non-blocking suggestion to construct the envelope before computing
remaining RPC time was applied. Final source/test/evidence review by the same
independent agent on 2026-10-14: Approved for RFC 0023 implementation acceptance.
The reviewer inspected the stable tests, both actual CTest Node logs, and the
ASan cache. All criteria for this narrow runtime change are met. Its decision
explicitly excludes a new full native sanitizer pass, RFCs 0019/0020/0021,
complete six-max GTO, strategy quality and live promotion.

The prior full native sanitizer command has 88 completed test entries:
87 passed, its original Node gate failed, and the resident target is still
running. The fixed Node rerun above repairs that integration gate but is not a
fresh full-suite result. This RFC's runtime changes add no C++ source changes;
RFCs 0019/0020/0021 still require their separate complete native evidence.
