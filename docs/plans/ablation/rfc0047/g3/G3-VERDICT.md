# RFC 0047 G3 verdict — six-stratum error-propagation certificate

Date: 2026-11-06. Evaluator: `tools/rfc/rfc0047-gates.ts` (`evaluateS2G3`).
Frame: `g3-frame6.csv` (six strata x 25 rows = 150 injections + 6 identity
rows), built from `g3-gate-w1.csv` and `g3-campaign-<stratum>.csv`.
Cert: `../s2/g2-frame12-6strata-6k20k.csv`. Campaign solve: river 600 /
turn 3200 / reference 2400, RM+, 24 threads, seedless deterministic.

## Operating point (from the shipped G2 per-entry bounds over root pot)

- e_max_frozen E = max(cert_max_bb * 2 / pot_chips) = **1.8370e-2**
  (unpaired-rainbow-ahigh, 8-chip pot).
- e_l2_frozen  L = max(cert_rms_bb * 2 / pot_chips) = **2.6974e-3**.

## Result

Joint (e_max,e_l2) certificate along the REAL signed per-combo G2 drift ray
(the `act` arm imports each leaf's actual candidate-minus-reference root-CV
drift, normalized to e_max), delta interpolated at E:

| stratum                  | joint delta BB | act e_l2 @ E | coherent (single-norm) BB |
|--------------------------|---------------:|-------------:|--------------------------:|
| w1                       |      9.328e-4  |    5.004e-3  |                 1.178e-2  |
| unpaired-rainbow-ahigh   |      8.606e-4  |    6.356e-3  |                 8.656e-3  |
| paired                   |      6.810e-4  |    6.258e-3  |                 8.078e-3  |
| monotone                 |      3.012e-3  |    7.781e-3  |                 1.519e-2  |
| unpaired-twotone         |      7.279e-4  |    4.981e-3  |                 5.443e-3  |
| raised-srp               |      1.712e-3  |    5.553e-3  |                 1.813e-2  |

One-sided 95% Student-t across the six strata (t = 2.015, df = 5):

- **JOINT propagated turn delta: mean 1.3211e-3, UCB 2.0694e-3 BB**
  <= B0 - R0 = 0.010 - 0.002 = **0.008 BB — PASS**; headroom 5.93e-3 (~3.9x).
- The real-drift ray's e_l2 at E (4.98e-3 .. 7.78e-3) is >= the shipped
  L = 2.70e-3 on EVERY stratum, so the certified point is no less correlated
  (no more adversarial) than any mounted node meeting the G2 RMS bound — the
  interpolation is conservative, not optimistic.

Single-norm e_max-only coherent stress arm (RECORDED, not the gated
coordinate): mean 1.1213e-2, **UCB 1.5135e-2 > 0.008**. A hypothetical fully
coherent same-sign bias of one seat's entire range at the max per-entry bound
is not a realizable residual of the deterministic exact solver (its
convergence residuals are diffuse mixed-sign, captured by e_l2). This is the
load-bearing reason RFC 0047 G3(a) defines both norms: a max-only bound would
fail; the joint (e_max,e_l2) bound passes with margin.

## Conservatism notes

- The `act` shape is the r600-vs-r2400 drift (4x solve ratio); production G2
  ships r6000-vs-r20000, whose drift is strictly smaller, so the campaign
  real-error shape overstates the production residual.
- The injected delta is the change from the perturbation; the baseline turn
  gap at t3200 is 5.1e-4..8.5e-4 BB and small-level (<=1e-4) points sit in the
  +-3.5e-5 residual noise band. Gate-scale points (1e-2/2e-2) are 20-500x
  above that band.
- The campaign t_river (2.5e-3..5.2e-3) uses deliberately loose r600 leaves
  and is a DIAGNOSTIC, not the production R0. The online exact-key river
  re-solve (G2-converged ~8e-4) is certified against R0 = 0.002 separately in
  S3/G5. G3 bounds only the propagated TURN delta above it.

## Zero-perturbation identity

Enforced by the native CTest `rfc0047_turn_solver`: the independent
telescoping exact-best-response walker reproduces the turn solver's own
summed NashConv to machine epsilon (3.6e-16 BB). Identity turn gap in this
campaign: 5.1e-4..8.5e-4 BB on all six strata.

## Gate status

**G3 error-propagation certificate: PASS** (UCB 2.07e-3 <= 0.008 BB, six
strata, joint two-norm envelope). The online R0 term remains an explicit S3/G5
acceptance item and is not claimed here.
