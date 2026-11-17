# RFC 0050 Stage 2 — Verdict Classification Gap (Amendment Candidate)

Status: Proposed candidate, NOT applied. The frozen RFC 0049 verdict
classifier (`offline_convergence.cpp`) and the nine-rung ladder are
untouched; per the RFC, changing the ladder, wall, anchor list, gate,
or verdict semantics requires a fresh independent review. This document
records the gap, the two distinct situations the current code
collapses, and the impact, for that review. No frozen constant is
modified here.

## Observed case

Stage 2 anchor rank 4 (srp, pot 12, contributions 6/6) completed 82
CFR iterations normally, the in-process 1,800 s wall latched
(`stopped_early=true`), but emitted `verdict=inconclusive`,
`rung_rows=0`, `min_e_embed=inf`. It produced no rung because the first
frozen checkpoint is iteration 100 and 82 < 100. Anchor rank 3 (srp,
pot 14), one step smaller, reached exactly 100 iterations and one rung
in its wall, locating the first-rung threshold near 90-100 iterations
for these deeper ledgers.

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

One conservative option that changes no rung, wall, or gate constant:
refine only the empty-rows branch of the classifier into

- `Inconclusive` only for situation A (an actual instrumentation
  anomaly or zero completed iterations);
- a budget outcome for situation B, either a new typed verdict such as
  `BUDGET_NO_GO_NO_RUNG` or an explicit sub-reason on the existing
  budget path, selected by the reviewer. The distinction is decided
  purely from `rows.empty()`, `stopped_early`, and
  `completed_iterations`, all already recorded, so no new measurement is
  required.

The alternative of leaving the taxonomy as-is and recording situation B
as a documented "inconclusive (budget, no fault)" with an explicit
sub-reason is also acceptable to the reviewer; the requirement is that
the two situations be distinguishable in the receipt and report, not
that a new enum value exist.

## Impact on the six-anchor conclusion

Per-iteration cost grows with action-node count, so deeper (lower-pot)
anchors complete fewer iterations in the fixed wall. Estimated
in-wall capacity falls from ~262 iterations on the smallest anchor to
~73 on the largest (pot 10, 5,720 action nodes). Anchors whose capacity
is below the first rung of 100 therefore fall into situation B; anchor
4 did at 82, and the completed batch confirms anchor 5 (limped, pot 10,
5,720 action nodes) does too at 69 iterations. This is now observed as a
class, not an isolated case: two of the six anchors (the two deepest)
cannot reach the iteration-100 first rung inside 1,800 s. The frozen
100-iteration first rung is simply too expensive to reach in the
per-process envelope on the deepest ledgers. The boundary report must
list these two anchors separately as "verdict pending (budget, no
fault)" rather than counting them among anchors with a clean typed
convergence result, while their structural identity is only partially
evidenced — the zero pure-menu term could not be scored because no rung
fired, so it is neither confirmed nor refuted on these two ledgers.

## Journal note for the anchor-4 rerun

Per directive, one journaled rerun is owed. The journal must record:
the first-rung threshold is iteration 100; anchor 4 completed 82
iterations, i.e. 18 iterations short of the first scoring point, with
a per-iteration cost near 1,879,746 ms / 82 ~= 22.9 s; the run ended
normally on the wall latch with no NaN, checksum, or flag anomaly. If
the rerun again yields zero rungs for the same budget reason, it is
recorded as BLOCKED with the explicit note that this is a budget
conclusion, not an instrumentation failure.
