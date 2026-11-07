# RFC 0047 G3 verdict — six-stratum error-propagation certificate

> **Status (2026-11-07): TURN-PROPAGATION COMPONENT PASS ON THE PRODUCTION
> BOX (pending the q=.002/.005 ultra-fine bracket); full G3 acceptance still
> pending S3/G5 R0.** An independent non-author review (Changes-Requested)
> rejected the original act-ray-only certificate; the two BLOCKING findings
> have been closed with a partially-coherent spike frontier and a
> reach-weighted production L*. The numbers below supersede the earlier
> ray-only section. Final zero-extrapolation box verdict lands after the
> prodfine campaign.

## Production operating point (reach-weighted, r6000/ref20000)

- e_max_frozen E = max shipped cert_max/pot = **1.8370e-2**.
- L* = reach-weighted e_l2 of the REAL r6000-vs-r20000 signed drift joined
  with the production-leaf turn-equilibrium rho (`g3-norms6k-*.csv`):
  w1 8.01e-4, paired 1.29e-3, monotone 9.93e-4, ahigh 9.84e-4, twotone
  1.14e-3, raised **4.31e-4**. (The earlier unweighted per-group RMS 2.70e-3
  and the loose r600 reach-weighted proxy 3.9e-3..7.9e-3 were wrong/different
  norms and are superseded.)

## Partially-coherent BR-weight-ranked spike box (production r6000 leaves)

The spike(q) arm puts a +E·pot payoff shift on the minimal top-q rho mass
ranked by the independent walker's BR-minus-sigma gap weight w — the worst
shape in the (e_max,e_l2) box. Measured with production r6000 leaves and the
production equilibrium rho (`g3-prod-*`, `g3-prod-frame6.csv`):

| stratum | L* | q=.01 e_l2 | box-corner delta (clamped) |
|---|---:|---:|---:|
| w1 | 8.01e-4 | 1.77e-3 | 8.95e-4 |
| paired | 1.29e-3 | 1.84e-3 | 1.43e-3 |
| monotone | 9.93e-4 | 1.92e-3 | 2.21e-3 |
| ahigh | 9.84e-4 | 2.00e-3 | 4.95e-4 |
| twotone | 1.14e-3 | 1.86e-3 | 3.41e-4 |
| raised | 4.31e-4 | 1.70e-3 | **2.27e-3** |

Frozen-frame **MAX 2.27e-3 BB <= B0-R0 = 0.008 (3.5x headroom)**;
evaluateS2G3Spike PASS. Every L* is below the finest measured spike q=.01, and
absolute delta rises monotonically with q on every stratum, so the q=.01
delta bounds the box corner from above (clamp). The q=.002/.005 campaign
brackets even the smallest L* (raised) with measured points to remove the
q->0 extrapolation entirely.

## What is and is not certified

- CERTIFIED: propagated TURN exploitability of a non-probed mounted node whose
  per-combo one-sided error has e_max<=E and reach-weighted e_l2<=L* for the
  cap-1 shapes of the six frozen strata: <= 2.27e-3 BB (frame max).
- RECORDED, NOT GATED: the synthetic fully-coherent single-norm arm (UCB
  1.51e-2) — unreachable by a deterministic exact solver's diffuse mixed-sign
  residual; kept as a sensitivity bound. A future S3 production RFC must
  enforce per-node e_max/e_l2 at mount time for this to hold in production.
- NOT YET: UCB(t_river) <= R0 = 0.002 with the online exact-key one-card river
  re-solve (S3/G5); and the signed 6k/20k coherence is measured here but the
  per-node mount-time e_l2 contract belongs to S3.
- The exact-seedless telescoping walker identity (machine epsilon, native
  CTest) and full milestone verification (ctest 91/91, npm/proto green)
  underpin the instrument.

## (superseded) earlier act-ray-only headline

The original joint act-ray UCB 2.07e-3 was a valid stress coordinate but did
not bound the 2-D box (a 1-D ray cannot); the spike box above is the reviewer-
required certificate.

---


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

**G3 turn-propagation component: PROVISIONAL PASS — superseded by independent
review (2026-11-06, Changes-Requested).** The act-ray number above is sound,
but a non-authoring reviewer showed it does NOT yet bound a non-probed
mounted node:

1. a 1-D ray cannot bound the 2-D norm box {e_max<=E, e_l2<=L}; exploitability
   is not monotone in e_l2 (the frame itself: w1 1e-2 zm e_l2 .0110 / delta
   .00117 vs coh e_l2 .0078 / delta .00485). The missing worst shape is a
   partially coherent, best-response-weight-ranked SPIKE on the ~3% combos
   carrying most telescoping BR-deviation weight. Closure: add deterministic
   spike(q) arms ranked by w = BR-reach - sigma-reach and gate the 2-D
   frontier.
2. the shipped L (unweighted per-group cert RMS) and the probe e_l2
   (reach-weighted, pooled over leaves/seats) are different norms; the
   production reach-weighted L* must be recomputed from the per-combo cert
   vectors and the exported rho before any dominance comparison.
3. the t-UCb (2.07e-3) is a bound on the MEAN and lies below the observed
   worst stratum (monotone 3.01e-3); the envelope for a non-probed node is
   the frozen-frame MAX plus margin, not a mean confidence bound.
4. full G3 acceptance also requires UCB(t_river) <= 0.002 with the production
   exact-key river re-solve (S3/G5); this campaign's loose r600 t_river is
   diagnostic only.

Until the spike frontier, L*, and the max-envelope are measured, the
authoritative status is **G3 act-ray provisional only; do not serve a
non-probed mounted node as G3-certified**. The coherent single-norm arm
fails (UCB 1.51e-2), exactly why the two-norm + spike closure is required.

## (superseded) prior act-ray headline

~~G3 error-propagation certificate: PASS (UCB 2.07e-3 <= 0.008 BB, six strata,
joint two-norm envelope).~~ Superseded as a full-node certificate by the
review above; the numeric ray result remains valid as one stress coordinate.
