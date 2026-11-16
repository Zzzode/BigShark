# RFC 0050 Stage 1 — Pre-Run Budget Estimate

Status: Current. Produced before any frozen-gate solve, per the D296
hard precondition and the D280 budget discipline. All projections are
estimates from the sealed calibration receipt; the gate receipt
replaces them. Bounded-menu throughputs were deliberately not
extrapolated: the exact anchor has 1,600 action nodes against the RFC
0049 cap-1 control's six, and the roots differ.

## Calibration measurement (30 iterations, four scored rungs)

Anchor: hard-coded rank-0 srp (pot 32, contributions 16/16), exact
cap-0 menu, single-thread seedless kernel, linear averaging, RM+ on,
DCFR off. Receipt: `stage1-calibration.csv` (solve-only accounting;
`stage1-calibration-run1-raw.csv` retains the first run's raw
cumulative accounting, whose NC columns are bitwise identical,
confirming seedless determinism across processes).

| quantity | measured value |
| --- | --- |
| steady-state solve throughput | 6.87 s per iteration at rung 30 (solve-only, all scoring excluded; per-rung 7.92 / 7.90 / 7.40 / 7.22 s while construction amortizes) |
| reach-bound scoring | 10.46 s per rung (mean of four) |
| one-time construction | embedded in the first interval, order 10-15 s |
| peak RSS | 615 MB (~13.5x under the 8 GiB envelope) |
| pure-menu term at rungs 5/10/20/30 | zero within 1e-8 BB at every rung (prints -0.00000000 / 0.00000000) |
| bounded NashConv (BB) at 5/10/20/30 | 10.46245731 / 4.89227959 / 2.64788157 / 1.64954217 |

The structural guard is therefore validated on the real anchor before
any long spend: an exact cap-0 policy reads a zero pure-menu term
through the reach-bound instrument, so e_embed equals the bounded
NashConv and the gate is purely about convergence.

## Projection against the frozen constants

Frozen: nine-rung ladder {100 .. 20,000}, 1,800 s in-process wall,
0.002 BB gate, no argv overrides.

- Solve-only capacity of the 1,800 s wall: about 261 iterations.
- Reachable rungs once per-rung scoring is included: 100 (~790 s total
  solve plus ~105 s scoring) and 200 (~1,374 s solve plus ~210 s
  scoring, about 1,585 s total). Rung 400 alone needs ~2,750 s of
  solve, so the stop predicate latches between rungs 200 and 400 and
  the frozen probe emits a typed BUDGET_NO_GO with two finite rungs
  (`last_rung_inside_wall=200` as computed by the pinned extrapolator).
- Full 20,000-iteration ladder solve time: about 137,405 s ≈ 38.2 hours
  single-threaded, plus about 94 s of scoring.
- Convergence extrapolation (label: estimate from four early rungs;
  two bracketed laws, not a certified slope):
  - Four-point OLS fit of the measured rungs gives
    NC(T) ~= 52.5 x T^-1.012, an early slope near -1.0. Under this law
    NC(200) ~= 0.25 BB and the 0.002 BB crossing sits near 23,000
    iterations (~44 h at 6.87 s/iter), just beyond the frozen ladder.
  - If the slope decays to the ~T^-0.52 law measured on the RFC 0049
    bounded controls, NC(200) ~= 0.62 BB and the crossing needs order
    1.2 x 10^7 iterations (~2.3 years).
  - Under either law the in-wall result is 100-300x over the gate. The
    frozen run's rungs 100/200 have scientific value precisely because
    they distinguish which regime the exact-menu curve enters; the
    four-rung calibration alone cannot certify the later slope.

## Tiering proposal

1. Run the frozen 1,800 s gate unchanged, launched with the outer
   `timeout 1900` backstop. Expected typed result: BUDGET_NO_GO with
   rungs 100 and 200 flushed, an early measured convergence curve, and
   the wall-latch record. Cost about 30 minutes of wall; no constant
   changes; produces the honest typed receipt the RFC requires.
2. A longer wall or a wider rung schedule is an RFC amendment with
   fresh independent review, not an argv change. The projection gives
   it low expected value: even the full 20k ladder sits decades of
   iterations short of the extrapolated crossing under the -0.52 law,
   so the open question is the convergence law itself rather than the
   per-process budget.
3. If the frozen gate confirms BUDGET_NO_GO, Stage 1 closes on measured
   evidence and the surviving levers (different convergence mode,
   warm-started trunk, or a coarser exact subgame) are decided in a
   separate RFC; the 0.002 BB threshold is not relaxed and the
   zero-menu structural result stands regardless.
