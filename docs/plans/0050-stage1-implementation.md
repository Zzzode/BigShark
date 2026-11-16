# RFC 0050 Stage 1 Implementation Plan

Status: Current. Scope owner: `bigshark_rfc0047_bench` only. Stage 1
tests whether the exact cap-0 menu that passed the Stage 0 memory gate
converges to bounded NashConv at or below 0.002 BB on the
measured-smallest decision-bearing anchor (srp pot 32, contributions
16/16; 1,600 action nodes, 4,797 total nodes). On the exact menu the
pure-menu structural term is zero by construction, so e_embed equals
the bounded NashConv and this is a pure convergence gate. Stage 2 (the
six-ledger boundary map) and the Stage 3 BSFLIB04 addendum remain
non-authorized and do not appear as work items.

## Hard precondition (D296): measured calibration before any long solve

The exact cap-0 anchor has 1,600 action nodes against the RFC 0049
cap-1 control's six. The cost structures are different trees on a
different root, so bounded-menu throughputs (17.153 ms/iteration for the
cap-1 control; ~700 ms for the 1,924-node cap-6) are priors only and
must not be extrapolated. A hard-coded 30-iteration calibration probe
with four scored rungs measures the actual steady-state per-iteration
wall and one reach-bound scoring wall on the exact anchor, then
extrapolates against the frozen 1,800 s wall and nine-rung ladder. The
long convergence run starts only after the measured budget estimate is
reported.

Artifacts (new, bench-only):

- `rfc0050_stage1.{hpp,cpp}` — the exact-domain builder (two cap-0
  trees plus a full-identity subset map, throwing on any unmapped node
  or extension edge), the pre-registered pure-menu zero guard
  (`kRfc0050PureMenuZeroBb = 1e-8` BB), and the pure-arithmetic
  calibration extrapolation that the native test pins.
- `rfc0050_stage1_calibration.cpp` — the no-argv 30-iteration
  calibration probe; also validates the zero-menu identity early and
  exits 5 on a structural contradiction.
- `rfc0050_exact_convergence_probe.cpp` — the frozen nine-rung gate
  probe with the 1,800 s per-iteration stop predicate, one reach-bound
  scoring per flushed rung, a `std::_Exit(5)` structural abort on any
  nonzero pure-menu reading, and the RFC 0049 four-verdict classifier
  (PASS / CONVERGENCE_NO_GO / BUDGET_NO_GO / INCONCLUSIVE).
- `test_rfc0050_stage1.cpp` — native ctest: full-identity domain on a
  shallow fixture and on the sealed anchor (4,797 nodes / 1,600 action
  nodes), a real exact solve whose pure-menu term reads zero through
  the reach-bound instrument, guard rejection of synthetic nonzero and
  non-finite readings, and pinned extrapolation arithmetic.

## Frozen constants (no argv; amendment-controlled)

- Ladder: {100, 200, 400, 800, 1,500, 3,000, 6,000, 12,000, 20,000}.
- In-process wall: 1,800 s, latched by the per-iteration stop
  predicate before any traversal FP work.
- Gate: e_embed <= 0.002 BB; with the zero menu term this is bounded
  NashConv <= 0.002 BB.
- Pure-menu zero tolerance: 1e-8 BB, derived from the reach-bound
  cross-walk accumulation bound and kept ~2e5 times below the gate.
- Solver settings: seedless exact full-deal kernel, single thread,
  linear averaging, RM+ on, DCFR off — the same mode the RFC 0049
  controls established as the strongest measured configuration.

## Pre-run budget estimate

Sealed in `docs/plans/ablation/rfc0050/stage1-budget-estimate.md` from
the 30-iteration calibration receipt. Headline measured figures:
6.87 s/iteration solve-only throughput, 10.46 s per reach-bound
scoring, 615 MB peak RSS, pure-menu zero at every calibration rung,
and a projected in-wall reach of rungs 100/200 with a typed
BUDGET_NO_GO expected at the 1,800 s latch. The full 20k ladder
projects to ~38 h; the two bracketed convergence laws (-1.01 early OLS
vs -0.52 decayed) both leave the in-wall curve 100-300x over the gate.
The estimate never pre-claims the gate result.

## Gate receipts and FAIL discipline

One CSV under `docs/plans/ablation/rfc0050/` with an empty stderr
sidecar and a recorded exit code. Every rung is flushed before the
next. The receipt schema has 16 columns (record type, anchor identity,
verdict, iteration/requested/rung counts, the four metrics, wall, RSS);
rung rows leave verdict and count columns empty and summary rows leave
metric columns empty, so every row aligns to the header. A nonzero
pure-menu term aborts as a structural failure (exit 5), not as a
convergence verdict; the 0.002 BB threshold is never relaxed and no
rung, ladder, or wall constant accepts an override. A full ladder
without a crossing is CONVERGENCE_NO_GO; a wall-tripped partial curve
is BUDGET_NO_GO; a pre-first-rung instrumentation failure is
INCONCLUSIVE with one journaled rerun.

The in-process 1,800 s predicate latches during the solve, but the
solver's post-loop best-response evaluation and final snapshot can add
up to about a minute of wall after the latch. The gate is therefore
launched with an outer `timeout 1900` backstop, the same 100 s margin
the RFC 0049 Gate 2 runner used; the outer timeout exists only as a
safety net and a clean run always stops inside the in-process wall.

