# D-370 Cap-4 Post-Convergence Menu-Term Measurement

Status: Current
Date: 2026-11-18
Scope: RFC 0049 Gate 2 wide raise-cap lineage, follow-up to D-350.
Bench-only measurement; no engine, sealed instrument, protocol, or contract
change; no commit. Receipts: `d370-w1-identity-cap4-embed-20000.csv`,
`d370-w1-identity-cap4-embed-6000.csv`, `d370-cap4-run.log` (this directory).

## 1. Question and pre-registered decision rule

D-350 observed that the "wide cap is infeasible" ruling rested on truncated
readings (cap-4 last scored at 1,500 iterations under the frozen 1,800 s
in-process wall; cap-6 at 1,000). The online response constraint was lifted
because wide-cap strategies are solved offline once and served by constant-
time table lookup. D-350 pre-registered three verdicts for a cap-4 20,000-
iteration run:

1. pure-menu term <= 1.28e-3 BB: terminal ruling overturned, certification
   path opens;
2. 0.002-0.01 BB: not certifiable, but the practical leak is negligible;
3. non-monotone rise (as on cap-0): close the wide-cap line.

The cap-6 ~30.6 h run is authorized only if cap-4 converges into the
0.002-0.01 band.

## 2. Instrument and configuration

The RFC 0049 `offline-convergence-probe` cannot run past its hard-coded
1,800 s wall (no argv, no environment override, no resume entry), so the
zero-code-change measurement used the existing D164 instrument
`bigshark-rfc0047-cap-materiality embed`, rebuilt from current sources:

- anchor: w1 deep check-to-river, same `enumerate_frontiers(w1, 1).front()`
  root as the sealed Gate 2 run;
- menu: `ActionAbstraction::identity(4)`, 320 action nodes, identical to the
  Gate 1 census (`w1-identity-cap4.csv` reports 320);
- solver: seedless linear-averaging RM+, 28 threads, no stop predicate;
- scoring: frozen T6 identity `e_embed = bounded NC + pure_menu term` under
  both completion arms, via the independent `EmbeddedGame` evaluator;
- two sequential processes, one iteration target each: 20,000 then 6,000,
  each emitting Uniform and BrFavorable rows. PIDs 1115696 and 1371228,
  both RC 0, empty stderr. Launch 2026-11-18T22:16:46+08:00; the 20,000
  process finished 23:06:28 and the 6,000 process 23:21:51. The untracked
  local runner log `d370-cap4-run.log` (repository policy ignores *.log)
  holds the same PID, RC, and timestamp lines.

Instrument caveat: the sealed 200/1,500 anchors were scored by
`reach_bound_from_snapshot`; the new 6,000/20,000 anchors by `EmbeddedGame`.
Both implement the same independently reviewed frozen identity but are
separate evaluators with no shared-iteration bitwise equivalence receipt.
The decisive 6,000 -> 20,000 direction is measured entirely within one
instrument and is unaffected by this caveat.

## 3. Four-anchor table (Uniform primary arm, BB)

| Iteration | Source | Bounded NC | e_embed | pure_menu |
| --- | --- | --- | --- | --- |
| 200 | sealed Gate 2 (reach_bound) | 0.18013040 | 0.40791978 | 0.22778937 |
| 1,500 | sealed Gate 2 (reach_bound) | 0.03405071 | 0.09802083 | 0.06397012 |
| 6,000 | D-370 (EmbeddedGame) | 0.01205936 | 0.07802624 | 0.06596689 |
| 20,000 | D-370 (EmbeddedGame) | 0.00714721 | 0.07938816 | 0.07224095 |

Identity holds on both new rows (NC + pure_menu = e_embed to display
precision). Segment movements:

- 200 -> 1,500: all three terms fall steeply (pure_menu -71.9%).
- 1,500 -> 6,000: bounded NC -64.6%, e_embed -20.4%, pure_menu **+3.1%**.
- 6,000 -> 20,000 (same instrument): bounded NC -40.7%, e_embed **+1.7%**,
  pure_menu **+9.5%**.

BrFavorable arm: e_embed 4.76210834 @6,000 -> 4.72611640 @20,000 (-0.8%,
flat at ~4.7 BB, ~2,363x over M); pure_menu ~4.72-4.75 BB throughout.

## 4. Verdict: D-350 branch 3 — non-monotone, wide-cap line closes

The policy's own NashConv keeps converging (0.0341 -> 0.00715 BB) while the
structural pure-menu term bottoms near iteration 1,500 and rises through
20,000. e_embed bottoms at 0.07802624 BB (iteration 6,000) and also turns
up. Convergence reveals the menu restriction rather than diluting it — the
same mechanism already measured on cap-1 (pure_menu grows to 31.19 BB at
20,000 iterations) and on cap-6's own D164 pair (pure_menu 0.00217 @300 ->
0.00610 @1,000).

Distance from every contract number at the best observed point:

- minimum e_embed 0.0780 BB is 39.0x over M = 0.002 BB;
- 61.0x over the B0 residual margin 1.28e-3 BB;
- 7.8x above the top of D-350's "negligible" 0.01 BB band;
- the 20,000-iteration pure_menu term alone is 36.1x over M.

The cap-4 trajectory never enters the 0.002-0.01 band and its tail is
rising, not falling. Per the pre-registered rule this is branch 3, and the
conditional cap-6 30.6-hour run is not triggered: its entry condition
(cap-4 converging inside 0.002-0.01) is falsified. The cap-6 1,000-
iteration 0.00610 reading is consistent with the same rising-structural-
term pattern, not a truncated point on a path to the gate.

### 4.1 Actual landing against the D-350 bands

No pre-registered branch is hit precisely; the measured landing is a
fourth, unlisted position:

- Branch 1 (pure_menu <= 1.28e-3 BB, ruling overturned): missed by 56-61x.
- Branch 2 (0.002-0.01 BB, negligible practical leak): the best e_embed,
  0.0780 BB at iteration 6,000, sits 7.8x **above the band's upper edge**.
- Branch 3 (non-monotone rise, close the line): the tail direction matches
  (pure_menu +9.5%, e_embed +1.7% from its floor) but the magnitude is a
  shallow structural-term reveal at the ~0.08 BB level, not a cap-0-style
  divergence. The ruling consequence is the same as branch 3 — the wide-cap
  certification line closes — while the practical quality level is that of
  a bounded, non-negligible numeric gap rather than a collapse.

Practical conversion. e_embed is a two-seat summed BB quantity, so the
per-player expected loss per dealt hand is one half: 0.0397 BB at 20,000
iterations (0.0390 BB at the 6,000-iteration floor). The anchor pot is
4 chips at big_blind = 2, i.e. a 2 BB pot, so the per-hand leak is about
1.98% (~2%) of pot. This is an exact per-anchor rate under the seedless
enumeration, not a frequency-weighted table-level rate.

The instrument caveat in section 2 (reach_bound for the 200/1,500 anchors
versus EmbeddedGame for 6,000/20,000, no shared-iteration bitwise
equivalence receipt) applies to the cross-instrument level comparisons
above; the decisive tail turn is measured within one instrument.

## 5. Option 2 assessment

D-370 conditionally authorized a bench-only long-wall probe with the full
rung ladder. It is not needed: the turn is established within one
instrument at 6,000 -> 20,000, additional intermediate rungs cannot move a
rising 0.078 BB floor toward 0.002 BB, and the only open numeric question
(cap-6's exact asymptote) was pre-decided not to be purchased unless
cap-4 entered the band. No new probe was written; zero source files
changed.

## 6. Resources

20,000 process: 2,982 s wall (49.7 min, both arms), 74.1 ms/iteration on
the Uniform arm at 28 threads, peak RSS 63.7 MB. 6,000 process: 923 s
(15.4 min). Campaign total ~65 minutes single machine, local only.
