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


## Frozen gate run (2026-11-17)

Launched after the reported budget estimate was confirmed:

```bash
timeout 1900 build/release/engine/bigshark-rfc0050-exact-convergence-probe \
  > docs/plans/ablation/rfc0050/stage1-exact-convergence.csv \
  2> docs/plans/ablation/rfc0050/stage1-exact-convergence.err
```

Receipt and typed verdict are recorded on completion. Closure analysis
must confirm, against the actual rows rather than the estimate:

1. every flushed rung's pure-menu term is inside the 1e-8 BB zero
   tolerance (the exact-menu structural identity held for the whole
   gate, not just the calibration);
2. the verdict is one of the four RFC 0049 typed outcomes and matches
   the stopped-early flag and rung count;
3. the measured rungs 100/200 bracket the early convergence regime and
   are compared against the two pre-registered extrapolation laws;
4. the outer 1,900 s timeout never fired (an outer kill would be an
   INCONCLUSIVE operational failure, not a convergence verdict);
5. the stderr sidecar is empty apart from the recorded exit code.

### D297 pre-registration and verdict discipline

Pre-registered expectation (stated before the run, not fitted after):
the stop predicate latches between rungs 200 and 400 and the probe
emits BUDGET_NO_GO with rungs 100 and 200 flushed. Any deviation — an
earlier wall trip, a full-ladder completion, or a gate crossing — is a
higher-information signal and must be called out explicitly in the
closure report rather than narrated as expected.

The pure-menu zero check is unconditional and more fundamental than
the wall. Even under BUDGET_NO_GO, both completed rungs (100 and 200)
must show a pure-menu term inside the 1e-8 BB tolerance. A nonzero
reading means the exact menu was not actually exact; the run aborts
with exit 5 and the cause is investigated before any convergence
interpretation, regardless of the wall outcome.

The three non-PASS outcomes are distinct and reported separately,
never collapsed into "did not pass":

- BUDGET_NO_GO: finite rungs flushed, then the in-process wall latched
  (a compute-envelope result; the curve is partial but valid).
- CONVERGENCE_NO_GO: the full nine-rung ladder completed and no rung
  crossed 0.002 BB (a convergence-law result on complete evidence).
- INCONCLUSIVE: pre-first-rung instrumentation failure or insufficient
  finite rows to judge (one journaled rerun, then BLOCKED if
  reproduced); an outer 1,900 s timeout kill lands here as an
  operational failure, not a convergence result.

## Stage 1 gate closure (2026-11-17): BUDGET_NO_GO, pre-registered expectation met

Receipt: `stage1-exact-convergence.csv` (RC 0, empty stderr sidecar
apart from the recorded exit marker; the outer `timeout 1900` did not
fire; total in-process wall 1,824.9 s = the 1,800 s latch plus 24.9 s
of post-loop best-response evaluation).

### Rows and the D297 checks

| rung | bounded NC = e_embed (BB) | pure-menu (BB) | wall (s) | RSS (MB) |
| --- | --- | --- | --- | --- |
| 100 | 0.53255290 | -0.00000000 (within 1e-8) | 608.2 | 615 |
| 200 | 0.26548740 | -0.00000000 (within 1e-8) | 1,211.7 | 615 |

1. Pre-registered expectation: MET exactly. The stop predicate latched
   at iteration 301 (between rungs 200 and 400), two rungs flushed,
   verdict `budget_no_go`. No deviation to flag; the only surprise was
   favorable — solve throughput improved to a ~5.95 s/iter blended
   steady state versus the calibration's conservative 6.87 s/iter
   cumulative average, so rungs landed earlier than estimated.
2. Pure-menu zero under BUDGET_NO_GO: HELD. Both completed rungs read
   a structural term inside the 1e-8 BB tolerance and e_embed equals
   bounded NashConv to all eight printed decimals. The exact menu was
   exact for the entire gate; no exit-5 structural abort occurred.
3. Verdict distinction: this is BUDGET_NO_GO (finite partial curve,
   in-process wall latched, ladder not completed), not
   CONVERGENCE_NO_GO (would require all nine rungs) and not
   INCONCLUSIVE (rows are finite and sufficient). The classifier
   output matches `stopped_early=true` and `rung_rows=2`.

### Convergence regime: the higher-information content

The two gate rungs measure a 100->200 doubling ratio of
0.26548740 / 0.53255290 = 0.4985, i.e. an observed slope of -1.004.
The exact-menu curve is still on the steep ~T^-1.0 law seen in the
calibration OLS (-1.012); the decayed ~T^-0.52 law from the RFC 0049
bounded controls has not appeared by iteration 200 (its prediction of
0.615 BB at 200 is 2.3x above the measured 0.265 BB). Extrapolation
is explicitly two-scenario, not a certified slope:

- If the -1.0 law persists: a two-point fit gives
  NC(T) ~= 54.3 T^-1.004, a 0.002 BB crossing near 26,000 iterations
  (~43 h single-thread at 5.95 s/iter), and even the frozen 20k ladder
  would end near 0.0026 BB (only ~1.3x over).
- If the slope decays to -0.52 later: the 20k ladder ends near
  0.034 BB (~17x over) and the crossing needs order 10^7 iterations.

Two rungs cannot certify which regime holds past 200; that is the open
scientific question and a single 1,800 s process cannot settle it. The
gate's measured contribution is that the structural term is genuinely
zero (Stage 1 boundary 1 proven on a real anchor) and convergence so
far follows the favorable steep law, reframing the campaign as a
compute-budget decision (a longer single run, parallelism, or
warm-start) rather than a structural impossibility. Any longer wall or
wider ladder is an RFC amendment with fresh independent review; the
0.002 BB gate, ladder, and verdict taxonomy were not changed.
