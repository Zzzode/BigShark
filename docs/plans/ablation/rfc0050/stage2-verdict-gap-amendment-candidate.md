# RFC 0050 Stage 2 — Verdict Classification Gap (Amendment Candidate)

Status: Proposed candidate, NOT applied. The frozen RFC 0049 verdict
classifier (`offline_convergence.cpp`) and the nine-rung ladder are
untouched; per the RFC, changing the ladder, wall, anchor list, gate,
or verdict semantics requires a fresh independent review. This document
records the gap, the two distinct situations the current code
collapses, and the impact, for that review. No frozen constant is
modified here.

Independent review (2026-11-17, non-author): Changes Requested,
documentation only — the problem framing, evidence, default-zero
withdrawal, and minimal-change intent were all accepted, all arithmetic
reproduced, and the frozen classifier confirmed byte-identical. The
findings are dispositioned in this revision: M1 (the two implementation
routes have different blast radius because `completed_iterations` is
not in the classifier signature; Route 1 probe-layer sub-reason
recommended), m1 (capacity estimate derivation and its ~6% optimistic
bias), m2 (the rerun/BLOCKED protocol belongs to case A only; the D316
rerun was a one-off directive), m3 (boundary RSS is a process-level
high-water mark, not per-anchor), n1 (the threshold is exactly 100, not
"near 90-100"), n2 (the 5,400 s cap is a D316 diagnostic gate, not an
RFC pre-registered wall). No recompute or code change was required.

## Observed case

Stage 2 anchor rank 4 (srp, pot 12, contributions 6/6) completed 82
CFR iterations normally, the in-process 1,800 s wall latched
(`stopped_early=true`), but emitted `verdict=inconclusive`,
`rung_rows=0`, `min_e_embed=inf`. It produced no rung because the first
frozen checkpoint is iteration 100 and 82 < 100. Anchor rank 3 (srp,
pot 14), one step smaller, reached exactly 100 iterations and one rung
in its wall. The first-rung threshold is frozen at exactly iteration
100; what varies across adjacent ledgers is in-wall capacity (rank 3
completed 100 and reached the rung, rank 4 only 82 and did not), not
the threshold itself.

## The two situations currently sharing one verdict

`classify_convergence` returns `Inconclusive` whenever `rows.empty()`,
with no further condition (`offline_convergence.cpp:36-37`). Two
qualitatively different runs land there:

### Situation A — pre-first-rung instrumentation failure (the documented meaning)

The RFC's INCONCLUSIVE definition: an instrumentation failure before
the first rung is scored — a non-finite/NaN result, a checksum or flag
failure, or a process that dies before any checkpoint. The classifier
already catches a non-finite row at lines 28-33; a crash produces no
row at all. The prescribed handling is one journaled rerun, and
reproduction means BLOCKED.

Distinguishing evidence: a non-finite/NaN/CRC/flag anomaly, an
abnormal process exit, or zero iterations completed.

### Situation B — budget exhausted before the first rung (anchor 4)

The solve ran correctly for a positive number of iterations, every
instrumented quantity was finite, and the wall latched before the
first checkpoint iteration. There is no instrumentation fault; the
process simply could not afford 100 iterations inside 1,800 s on this
larger tree.

Distinguishing evidence: `rung_rows=0` AND `stopped_early=true` AND
`completed_iterations > 0` AND no finite/instrumentation anomaly.

## Why they must not be silently merged

Situation A questions whether the measurement apparatus works and
carries a rerun/BLOCKED protocol. Situation B is a clean compute-budget
result: the policy is converging (the kernel ran without fault) but the
per-process envelope cannot reach even the first scoring point. Calling
both "inconclusive" mislabels a budget outcome as an instrumentation
one and hides that the deep-anchor campaign is under-resolved, which is
the more decision-relevant fact for Stage 2.

## Candidate semantics for review (proposal, not adopted)

Two implementation routes exist, with different blast radius. The
independent review (2026-11-17, Changes Requested on the first draft)
identified that `completed_iterations` is NOT currently in the frozen
classifier's scope — `classify_convergence` has signature
`(std::vector<OfflineRungRow> rows, bool stopped_early)` only — so the
two routes must not be presented as equally cheap.

### Route 1 (recommended): probe/receipt-layer sub-reason, no frozen change

Keep the RFC 0049 classifier byte-identical and distinguish the two
situations at the probe/receipt layer, where `completed_iterations` is
already available on `RiverSolveResult`. This is the pattern the
D316 deep-rerun already implements and validates
(`rfc0050_deep_rerun.cpp`): it emits `target_rung_reached`,
`budget_reached_partial_rungs`, or `budget_unreachable_first_rung`
from `stopped_early` + `pure_menu_measured` + `iterations_completed`,
with no change to the shared enum, its signature, or the native test
that pins it (`test_rfc0047_offline_convergence.cpp`). Zero frozen
code changes; the distinction is carried in the receipt and report.

### Route 2 (higher cost): a new classifier enum, which changes a frozen API

A new typed verdict such as `BUDGET_NO_GO_NO_RUNG` emitted by
`classify_convergence` is NOT a local empty-branch edit. It requires
(a) extending the frozen RFC 0049 function signature and header to
accept `completed_iterations`, (b) updating the pinned native test that
calls `classify_convergence({}, false)`, and (c) re-reviewing the
shared component used by every RFC 0049/0050 gate. It also needs a new
token in `verdict_token`. This route is only worth taking if the
distinction must be enforced inside the shared classifier rather than
at the bench boundary; the evidence so far does not require that.

The recommended decision is Route 1. Either way, no rung, wall, gate,
or threshold constant changes, and no new measurement is required
because the deciding signals are already recorded.

## Impact on the six-anchor conclusion

Per-iteration cost grows with action-node count, so deeper (lower-pot)
anchors complete fewer iterations in the fixed wall. Derivation of the
capacity estimate: the Stage 1 calibration receipt measures a
solve-only 6,870.268 ms/iteration on the smallest anchor (1,600 action
nodes), giving 1,800,000 / 6,870.268 = 262 iterations in-wall (the
receipt's own floor prints 261). Scaling linearly in action-node count
— an explicit assumption, because the terminal showdown matvec is
node-count-independent and only reach/backprop work scales with the
tree — projects 262 / (5,720/1,600) = 73 iterations for the largest
anchor. Measured capacity was 298 on the smallest and 69 on the
largest, so the linear figure is ~6% optimistic at the deep end and is
an upper bound, not a prediction. Anchors whose capacity is below the
first rung of 100 therefore fall into situation B; anchor 4 did at 82,
and the completed batch confirms anchor 5 (limped, pot 10, 5,720
action nodes) does too at 69 iterations. This is now observed as a
class, not an isolated case: two of the six anchors (the two deepest)
cannot reach the iteration-100 first rung inside 1,800 s. The frozen
100-iteration first rung is simply too expensive to reach in the
per-process envelope on the deepest ledgers. The boundary report must
list these two anchors separately as "verdict pending (budget, no
fault)" rather than counting them among anchors with a clean typed
convergence result. The earlier concern that their zero pure-menu term
was unmeasured is now resolved by the D316 fixed-rung rerun, which
scored both anchors with `pure_menu_measured=1` and a measured zero;
see `stage2-boundary-closure.md`. That closure also records the RSS
caveat (review m3): the fixed-wall receipt's `peak_rss_kb` is a
process-level high-water mark across the six-phase single process, not
an independent per-anchor measurement, with per-anchor residency taken
from the separate single-anchor deep-rerun processes.

## Rerun protocol ownership and the D316 rerun

The "one journaled rerun, reproduction means BLOCKED" protocol belongs
to situation A (instrumentation failure) only. Situation B does not
inherit it: its standing treatment is the budget sub-reason / distinct
budget verdict described above, because a clean run that simply cannot
afford the first rung is not an apparatus fault and rerunning under the
same fixed wall would reproduce by construction.

The D316 deep-anchor rerun was a one-off directive action, not the
standing case-B protocol. It changed the measurement shape (a fixed
second-rung target with a 3x budget cap rather than the fixed 1,800 s
wall) specifically to obtain comparable scored rungs. That 5,400 s cap
is a new diagnostic gate not pre-registered in RFC 0050; its authority
is the D316 directive, and a permanent diagnostic gate would need to be
aligned with the RFC's frozen-wall amendment rule. The journal facts it
established for anchor 4: the first-rung threshold is exactly iteration
100; anchor 4 completed 82 iterations, 18 short of the first scoring
point, at ~22.9 s/iteration (1,879,746 ms / 82), ending normally on the
wall latch with no NaN, checksum, or flag anomaly. The rerun then
reached iteration 200 under the 3x cap with `fault=0`, so no BLOCKED
state applies and the budget explanation is confirmed.
