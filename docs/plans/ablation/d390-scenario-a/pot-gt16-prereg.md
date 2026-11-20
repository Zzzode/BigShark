# Pot>16 Extrapolation Hull — Pre-Registered De-Risk Judgment

Status: pre-registered BEFORE the pot-32 run (timestamp in the runner log)
Date: 2026-11-20
Scope: one bounded local measurement of the largest decision-bearing
ledger above the current pot-16 mapping hull. No engine source, cap
configuration, or prior receipt changes; no rerun is authorized if the
guard trips.

## Why this run exists

The D-390 end-to-end 0.3328 BB/100 headline carries the D-382 caveat that
42.8% of the 692 "other" hands (296 hands, practice pots 18-40) are
charged the pot-16 severity by nearest-ledger mapping because no larger
decision-bearing ledger was measured. This run adds a measured anchor at
pot 32.

## Target and configuration (frozen before launch)

Ledger `srp` family_index 4 = pot 32, contributions 16/16, per the sealed
eleven-ledger census (`stage0a-eleven-ledger-cap0-census.csv`: 4,797 total
nodes, 1,600 cap-0 action nodes, decision-bearing). It is the exact
preferred target; no family-index substitution is needed. Command:
`bigshark-rfc0047-ledger-embed-probe srp 4 3000 18 6 uniform`, outer guard
4,200 s, 1 GiB RSS hard line. `/usr/bin/time -v` is absent on this host, so
wall and RSS come from the probe's steady_clock and
getrusage(ru_maxrss), the same kernel counters GNU time reads.

Cross-family caveat, stated up front: the prediction model is fit on the
three limped-family cap-6 3,000-iteration points; the pot-32 target is an
SRP-family ledger. The test therefore also probes family transferability,
and any verdict is labeled with that scope.

## Model and locked numeric interval (computed before any pot-32 data)

Inputs (measured cap-6 @3k, Uniform, two-seat e_embed in BB):
(pot 8, 0.06420077), (pot 10, 0.05621249), (pot 16, 0.02656799).

- A linear fit gives e_embed = 0.102926 - 0.004759*pot and predicts
  -0.049353 at pot 32 — physically impossible, so linear extrapolation is
  rejected as a primary model and retained only as evidence the observed
  trend is steep (fit residuals max 8.74e-4 inside the range).
- Primary model: ordinary least squares on ln(e_embed) against pot,
  ln(y) = -1.795970 - 0.113662*pot (log residuals -0.0405, +0.0540,
  -0.0135; maximum absolute log residual 0.05397). Prediction at pot 32:
  e_embed = 0.004369. Pre-registered band applies a multiplicative
  exp(+/- 1.5 x max-residual) envelope = [0.004030, 0.004738].
- No-model reference: the observed three-point envelope
  [0.026568, 0.064201], reported alongside the model so a plateau
  outcome is visible even if the log-linear form is misspecified.
- A log-linear fit of pure_menu is degenerate (the L3 near-zero value
  2.23e-6 drives the prediction to numerical zero), so pure_menu has no
  prediction interval; it is reported descriptively against the observed
  structural-term range [2.23e-6, 0.00939246].

## Judgment rules (cannot be edited after the run)

Let y = measured pot-32 cap-6 e_embed at 3,000 iterations.

- **Rule A — hull not suspicious:** y is within [0.004030, 0.004738].
  The extrapolation discipline is supported by a measured point and the
  0.3328 components above pot 16 are treated as empirically grounded.
- **Rule B — hull re-labeled unvalidated / possibly worse:** y exceeds
  1.5 times the band upper edge, i.e. y > 0.007107 (a value inside the
  no-model observed envelope triggers this, which would indicate the
  steep shrink does not transfer to the larger-pot SRP ledger). The
  42.8% hull is re-labeled in the options page and each product route's
  evaluation is revised as specified below.
- **Middle zone** (0.004738 < y <= 0.007107): neither rule confirmed;
  reported inconclusive with the measured factor, not forced.
- A reading far below the lower band is logged as a favorable deviation
  (not a risk to the headline, which would then be conservative).

## Product-route impact if Rule B fires (pre-specified)

- Full cap-6: campaign cost unchanged, but the 0.3328 headline loses its
  extrapolated support over 42.8% of the bucket; a re-weighted worst case
  must be shown and the serving-rail pass downgraded to conditional.
- Narrow b: already fails the 0.5 target; a worse hull deepens the fail
  and removes any argument that more coverage could rescue it.
- Retirement: relatively more attractive, since the measured-vs-true gap
  on the largest pots becomes unbounded within the current ledger set.

## Post-run remapping (mechanical, decided now)

With pot 32 measured, the 296 beyond-16 hands are re-mapped to the nearest
of pots 16/32 by the existing nearest-ledger rule: 169 hands (practice
pots 18/20/22) stay on pot 16, 127 hands (pots 25/28/40) map to pot 32.
The residual unmeasured hull then contains only pot-40 hands
(37 hands, 5.3% of the other bucket), regardless of the A/B outcome.

## Outcome (recorded after the run; rules unchanged)

RC=0 in 405.4 s, peak RSS 88.1 MB, empty stderr
(`pot32-srp4-cap6-3000.csv`): bounded NC 0.12080013, pure_menu
0.00002653, e_embed 0.12082666 on 470 online nodes (1,600 cap-0).

- y = 0.12082666 is outside the locked band [0.004030, 0.004738] and
  17.0x the Rule-B threshold 0.007107 (27.7x the point prediction, 1.88x
  the largest observed limped value). **Rule B confirmed**; the
  log-linear model fit on limped ledgers does not transfer to the
  larger-pot SRP ledger.
- Composition: 99.978% of the deviation is bounded NashConv; the
  structural term (2.65e-5) is near-zero, in the same family as the
  cap-7 L1 result. The failed prediction is therefore a convergence-rate
  difference across families, not a hidden structural wall.
- Remap executed as pre-specified: of 296 beyond-16 hands, 169 stay on
  pot 16 and 127 map to pot 32; the unmeasured hull shrinks to pot-40
  only (37 hands, 5.3% of the other bucket).
- Rail arithmetic: at the measured 3k severity the remapped end-to-end is
  0.6270 (passes 1.0, fails 0.5). Applying the measured 6.39-15.17x
  3k -> 20k NashConv factors as a labeled projection gives pot-32 20k
  e_embed 0.00799-0.01893 (Rule B survives: still 1.7-4.0x the model band)
  and end-to-end 0.3284-0.3574, which passes both rails. The 0.5 pass in
  the D-390 headline is consequently downgraded from measured to
  conditional on a 20k pot-32 receipt; the 1.0 pass retains margin in
  every scenario.

## Pre-run note for the 20k receipt (correction logged before launch)

A pre-launch weight audit found the section-8 remap arithmetic dropped
139 hands: L3's correct post-remap count is 308 (139 hands at practice
pots <=16 plus 169 of the beyond-16 hands), not 169. Correct weights over
the 692 other hands are L1 52 / L2 205 / L3 308 / pot-32 127 (sum 692).
The correct raw 3k remapped end-to-end is therefore 1.0214 BB/100, which
fails BOTH rails at 3k severity — not 0.6270 as previously written. The
20k run below replaces the pot-32 3k cell with a measured 20k cell under
the same Rule-B thresholds; all end-to-end figures are recomputed with the
corrected weights and the earlier values explicitly superseded.

Command: srp 4 20000 18 6 uniform, 4,200 s guard, 1 GiB RSS line; estimate
2,702 s from the measured 135.1 ms/iter. Rule B (y > 0.007107) stays
locked; a reading inside the band is labeled convergence closure, not a
threshold change.
