# RFC 0046 Gate-2 Feasibility Probe — SUPERSEDED BY TWO INSTRUMENT CORRECTIONS

Status: **the slack/NashConv points below were INVALIDATED in two steps on
2026-11-04.**

1. **Margin-wiring bug (zero-margin gadget).** An independent architect
   review found the cell runner never copied the extracted locked margins
   into the gadget input (`gin.margins` left empty), so every "locked margin"
   probe actually solved the ZERO-margin augmented game while the certifier
   scored it against non-zero reference margins. Fixed with
   `gin.margins = locked_margins`.
2. **Resource-accounting bug (spurious ResourceExhausted).** `tick()` did
   `bytes += add_bytes` on EVERY walk visit (64+8·#actions per action-node
   visit), accumulating a monotone per-VISIT tally that it compared against
   the 1 GiB PEAK-memory cap. Measured RSS stays ~27 MB, but the cumulative
   charge crossed 1 GiB at exactly iteration **401,945**, so both the 1 M and
   5 M cells were killed there with status ResourceExhausted and identical
   slack 13.7541; no run longer than ~402k was possible. Fixed to
   resident-only accounting (rows charged once at creation). The
   401,945 / 13.7541 rows are therefore an instrument artifact of an
   unfinished solve, NOT a convergence data point.

**Evidence hygiene:** every ladder row must identify the instrument binary —
"post-margin-fix" (bug 1 fixed, bug 2 present, valid only for runs that
completed before 401,945) vs "post-byte-fix" (both fixed; the only binary
valid for the >=1 M long ladder). Treat every slack number below as a
zero-margin-gadget artifact, not a Gate-2 certification trajectory. What
survives: envelope non-binding, the plain `train_nseat` RM+ rate
corroboration, and the arithmetic note. Do not quote the 42.4/16.2/7.5
ladder as Gate-2 evidence.

---

## (Superseded) What was measured

Single W1 stratum (the easiest: ten-hand anchor), cap-1 turn-rooted object,
board `2c 3d 7h Js` / river `9c`, 37,830 concrete infosets, 10,923 tree
nodes. The endgame gadget was solved by external-sampling MCCFR with the
exact ConcreteCards key, per-combo margins locked from a 200k-iteration
production `train_nseat` concrete reference, and the independent
multi-street certifier computed the responder best-response continuation
slack `max_combo (BR_continue - b(c) - tol)`.

## Responder certifier slack vs gadget iterations (200k margin reference)

| gadget iters | wall (s) | termination reason | max slack (chips) |
|---|---:|---|---:|
| 20,000 | 57.9 | BoundExceeded | 42.43 |
| 96,556 | 300.4 | SolveDeadline (10s-equivalent wall probe) | 16.58 |
| 100,000 | 267.8 | BoundExceeded | 16.23 |
| 400,000 | 820.6/822.8 | BoundExceeded | 7.51 / 11.79* |

\*Two 400k cells appear because one used `--extract-margins` (slack against
the locked margin: 7.51) and one used `--probe-nc` with no margin
(11.79 against the zero baseline); both are the same order.

The shape among the (zero-margin-gadget) points is ~N^-0.5 to N^-0.6. The
frozen certification tolerance is `1e-6 * pot = 4e-6 chips`. The earlier
revision miscomputed the point extrapolation as ~1.4e14; the correct value of
`400,000 * (7.51/4e-6)^2` is **~1.4e18** (~14.5 orders), which only
strengthens the "not a budget gap" argument - but this extrapolation is moot
once the wiring bug is fixed, and slack against a fixed approximate reference
also has a non-shrinking floor (the reference's own CFV error), so a
single-seed three-point law cannot be extrapolated regardless. The valid
measurement is the corrected two-axis (gadget-iters x margin-iters) ladder.

## Pure hybrid NashConv (no margin confound)

A zero-margin 400,000-iteration gadget profile, partitioned by the exact
chance-free walker (`--probe-nc`; `note=probe`, rejected by the evaluator):

- `total_nc` = **6.20 summed BB** (B0 = 0.010 BB -> ~620x over)
- `t_boundary` = 3.11 BB
- `t_river` = **3.09 BB** (R0 = 0.002 BB -> ~1,500x over)
- `t_trunk` = 0 (turn-rooted), `t_action` = 0 (cap-1), identity residual = 0

The RIVER term - the component the exact endgame is supposed to drive to
~zero and that R0 gates most tightly - is the largest and converges at the
same external-sampling rate. Independent corroboration without the gadget:
plain `train_nseat` through the ConcreteCards seam on this object reaches
only ~2.82 summed BB at 40k iterations (see session diagnostic; restated in
the summed-BB convention).

## Interpretation (for the evidence reviewer, not a self-verdict)

- The binding constraint is **convergence rate, not the resource envelope**:
  W1 uses 37,830 of the allowed 1,000,000 infosets and 10,923 of 5,000,000
  nodes; peak RSS is ~28 MB of 1 GiB. No cap is near.
- The exact-key turn-rooted external-sampling solver does not approach the
  1e-6-pot responder bound (nor the 0.010/0.002 BB hybrid/river targets)
  inside 10 s even on the easiest stratum, with a measured ~N^-0.5 law whose
  extrapolation is off by ~10 orders of magnitude in iteration count
  (arithmetic corrected above to ~1.4e18 / ~14.5 orders; and the law itself
  is invalid here - zero-margin gadget).
- This is the same structural wall seen in RFC 0043 (nested continuation
  value ~0.26 BB) and RFC 0044/0045 (uniform exact EHS cannot certify the
  full-range river CERT bar; a non-improving 1e-2 BB plateau).
- Under the frozen Stage-0 decision table this evidence, if reproduced by
  the campaign and confirmed by the second non-author evidence review,
  points to Gate 2 **CLOSE** (bound unreachable within envelope) rather than
  REDIRECT (a bigger-budget but same-algorithm run cannot close a 10^14
  iteration gap [corrected above to ~1.4e18; and moot - wrong gadget]; the
  one-shot 10M/2M/30s diagnostic will likewise certify
  nowhere near the bound) - and therefore RFC 0046 would return for redesign,
  likely along RFC 0043's Route C (boundary-value nesting) rather than a
  real-time exact solve.

## Reproduce

```
# responder slack trend (locked 200k margin)
for it in 20000 100000 400000; do
  OMP_NUM_THREADS=1 ./build/release/engine/bigshark-rfc0046-cell \
    --stratum w1 --seed 4320 --iters $it --margin-iters 200000 \
    --extract-margins --wall-ms 3600000 --cap 1 --quiet
done
# pure hybrid NashConv at 400k (research row, note=probe)
OMP_NUM_THREADS=1 ./build/release/engine/bigshark-rfc0046-cell \
  --stratum w1 --seed 4320 --iters 400000 --wall-ms 3600000 --cap 1 \
  --probe-nc --quiet
```

Research-only flags `--wall-ms` and `--probe-nc` are clearly named; probe
rows carry `note=probe` which `rfc0046-gates.ts` rejects, so this code path
cannot emit a gate-accepted row.
