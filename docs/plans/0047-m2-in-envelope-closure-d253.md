# RFC 0047/0048 M2 In-Envelope Closure Argument and D253 Certification Design

Date: 2026-11-16. Status: Proposed closure argument; the D253
experiment has run with an INCONCLUSIVE slope-band verdict but a
supporting in-envelope G1 overshoot measurement (section 8).
Measurement-only; no production or frozen-gate change. All numbers are
sealed research-probe rows reproduced across
D164/D184/D195/D198/D249/D251/D252/D253.

## 1. Question

With the frozen gates unchanged — cap-0 reference / cap-1 bounded menu,
river term R0 = 0.002 BB, hybrid B0 = 0.010 BB, G1 online envelope
(p95 <= 10,000 ms, max <= 12,000 ms, <= 1,000,000 infosets (1M count),
<= 1 GiB, <= 5M nodes; RFC 0047 sections G1/G3) — does ANY candidate
remain that is both in-domain and distinguishable by a 300-iteration
single-process two-solve anchor?

## 2. The gate metric has two terms with different walls

The D195 T13 telescope certifies the bit-exact single-walk split on the
frozen w1 Coarse cap-1 root at 300 iterations (an independent
restricted walk reconciles within 1e-12; bit-exactness is per walk):

```
e_embed = bounded_nash_conv_bb + pure_menu_term_bb
31.24489667 = 0.02761552            + 31.21728115
```

- **Pure-menu term (structural, 99.91%).** Two distinct claims must not
  be conflated. The construction-level invariant is the zero-mass fact:
  the embedded cap-1 policy places zero mass on cap-0-only edges, and
  T13 certifies menu_gap >= 0 by set inclusion for every bounded
  policy, so no update rule, averaging weight, discount schedule, or
  iteration count can create mass on an action the menu does not
  contain. The exact pure-menu NUMBER is a different object: it is a
  sigma-weighted arithmetic residual (exact exploitability minus
  bounded NC), so it is not a pointwise constant. The sealed rows
  measure a band of 31.02412861 to 31.39289840 BB across the policies
  actually run (D198 uniform gives the minimum 31.02412861; D249 fine
  linear control the maximum 31.39289840), and D164 cap-6 shows the
  term can drift with iteration count on a different tree. The band
  minimum alone is 15,512x the 0.002 BB gate, so replacing the earlier
  pointwise-invariance overstatement with this measured band does not
  weaken the closure: every measured cap-1 policy is structurally
  tens of thousands of times over the gate before its policy tier is
  even considered.
- **Bounded NashConv (policy tier).** The only component an update rule
  can move; 0.0276 BB at 300 iterations, itself 13.8x over the gate and
  2.76x over B0 = 0.010.

## 3. The only conceivable escape (menu widening) is already closed

D195 measured even PERFECT, unattainable root-only widening: the
fixed-policy floor e_Gg = 0.0276 BB remains, 13.8x over M = 0.002, and
the binding wall changes from menu to policy convergence. D164 measured
that convergence on the widened cap-6 tree: 300 -> 1,000 iterations
reduced online NC only 1.86x (0.12671 -> 0.06821), a T^-0.52 law, at
700 s wall per 1,000 iterations (70x the G1 p95) and 351 MB.
Extrapolating that law to 0.002 needs order 10^5-10^6 iterations — days
of solve. Quality vs latency is a measured conflict, not a solver bug.

## 4. The four in-envelope update-side axes are exhausted

Every axis was tested on the same frozen anchor with the same
single-process two-solve discipline; the default RM+/linear/
undiscounted policy is empirically the BEST in-domain update, and every
non-default variant is strictly worse at 300 iterations:

| Axis | Milestone | Candidate vs control | Result |
|---|---|---|---|
| Sizing-grid density | D249 | Fine grid | negative; e_embed 31.27 BB, 1.56e4x over gate |
| Average weight | D198 | uniform vs linear t | 2.70x worse NC |
| Regret clipping | D251 | vanilla RM vs RM+ | 2.86x worse NC |
| Time discounting | D252 | Linear DCFR+ vs RM+ | 2.17x worse NC |

The numeric control row 0.02761552 BB reproduces bitwise across
D184/D198/D251/D252; the policy hash 1922761369290757593 reproduces
bitwise across the three hashing-era ledgers D198/D251/D252 (D184
predates the FNV hash instrument and carries no hash), so the
comparisons share one sealed baseline.

## 5. Verdict: no distinguishable in-domain candidate remains

**Judgment: NONE.** A 300-iteration two-solve anchor can only
re-rank update rules at one point. It cannot (a) put mass on absent
menu edges — the zero-mass/menu_gap invariant is construction-level and
the measured pure-menu band never comes within four orders of magnitude
of the gate — or (b) escape the T^-0.52 policy wall, which the four axes
show no in-domain variant accelerates (all degrade it). One factorial
cell is explicitly inherited as a documented non-goal rather than
overlooked: vanilla-RM regrets combined with uniform averaging was never
run (D251 non-goal), because both component arms were independently
measured 2.86x and 2.70x worse and the two-solve cap forbids the cell;
it is listed here as a residual cell, not a live candidate. The other
residual ideas (classic DCFR 1.5/0/2 schedules, other factor schedules,
exponential/optimistic regret matching, regret-side/average-side
discount splits) are new mechanism classes requiring separate
authorization, and even a hypothetical winning variant would still face
the structural menu band and the G1 latency wall. Running a fifth
in-place variant would repeat a negative result and spend budget without
changing either wall; it is therefore not authorized by reason, not
merely by precedent.

## 6. The single minimal supplementary experiment (D253)

The closure currently imports ONE out-of-envelope datum: the D164
1,000-iteration/700 s slope on a different (cap-6) tree that establishes
T^-0.52. D253 reproduces a convergence certification entirely INSIDE the
frozen envelope (300 iterations, 120 s wall, 1 GiB), on the best policy
on the exact w1 Coarse cap-1 root, against the gate metric.

Non-duplication with the existing bigshark-rfc0047-river-curve binary:
that probe emits only kernel-NC checkpoints on an automatic x1.5
ladder plus iter/s; it performs no reach-bound bounded-NC
reconciliation, no OLS fit, no R^2/T*/t*/G1 receipt, and none of the
TreeLimits/tripwire/CSV discipline. No existing binary produces the D253
receipt in one run, so a dedicated probe is warranted; it reuses the
solver checkpoint API and the D249-D252 probe scaffolding rather than
adding solver code.

- **What it measures.** One 300-iteration RM+ linear Coarse cap-1 w1
  solve (dcfr=false, rm_plus=true, linear_averaging=true, threads=1)
  using the existing checkpoint API at the fixed seven-point ladder
  {30, 60, 100, 150, 200, 250, 300}; the per-checkpoint
  history_nash_conv_bb is the kernel NashConv curve. All seven points
  are retained in the receipt unconditionally — no point may be dropped
  to improve the fit; any non-finite checkpoint value is a non-zero
  probe exit, and any local increase of average-strategy NashConv is
  printed as observed rather than smoothed. Only the T=300 endpoint
  snapshot exists (RiverSolveResult keeps no intermediate snapshots),
  so only that endpoint receives an independent reach-bound walk; the
  interior checkpoints are certified as bounded NC through the D195
  domain identity (bounded_residual == solver NC on this Coarse
  cap-0-mapped root) plus the T13 1e-12 restricted-walk reconciliation,
  not by per-point independent walks, and the receipt says so
  explicitly. Total wall and per-iteration throughput are recorded.
- **Computed receipt.** In-probe OLS fit log10(NC) = a + b*log10(T)
  over the seven points with R^2; the extrapolated iteration count T*
  and wall time t* at which bounded NC would reach 0.002 BB; the
  iterations feasible inside G1 (throughput x 10 s and x 12 s); and the
  factor by which t* exceeds the G1 max. The slope thresholds are
  conventional pre-registered bands around the out-of-tree D164
  two-point estimate, not calibrated constants. The band-edge figures
  below are PRE-RUN DESIGN PROJECTIONS anchored at the endpoint
  (300, 0.02761552) using the sealed prior-run throughput of
  ~16.3 ms/iteration, not receipt fields — the probe binary prints only
  the point-estimate fit, T*, t*, and G1 ceilings — and they exist to
  show the design conclusion is not point-estimate-sensitive: at
  b = -0.60, T* ~ 2.4e4 iterations and t* ~ 389 s (32x the 12 s max);
  at b = -0.45, T* ~ 1.0e5 and t* ~ 1,674 s (140x).
- **Expected values.** b near the sealed -0.52; T* of order 10^4-10^5
  iterations; t* of order 10^2-10^3 s, i.e. tens to hundreds of times
  the 12 s G1 max. The extrapolation spans about two decades from a
  one-decade fit, so it is recorded as an order-of-magnitude
  corroboration of D164, not a stand-alone proof.
- **Pre-registered decision rule.** Certification succeeds if the fit
  is decreasing (b < 0) with R^2 >= 0.90, b lies in [-0.60, -0.45], and
  t* exceeds 12 s (the actual factor is reported). Pre-registered
  falsifier that would force reopening: b steeper than -0.60 AND a T*
  whose extrapolated wall is <= 12 s. At the measured throughput the
  wall clause alone already requires b around -2.93, so the slope
  conjunct only gains independent force if the receipt's measured
  throughput differs materially from the sealed ~16.3 ms/iteration;
  this redundancy is stated rather than hidden. Anything else (weak
  fit, slope outside the band without the falsifier conjunction) is
  reported as inconclusive and never upgrades the closure.
- **Budget and scope.** One manual probe binary (not a CTest, consistent
  with river_curve and the D249-D252 probes, which are plain
  executables; ctests cover only changed mechanisms and D253 changes no
  solver code), one process, one solve, TreeLimits{2,000,000 nodes, 256
  depth, 1 GiB}, two-layer 120 s tripwire with exit 4, RSS sampled
  only. No solver, production interface, threshold, menu, or
  budget-category change; no retry, sweep, or second root.

## 7. What D253 can and cannot conclude

Can: make the policy-tier unreachability evidence self-contained inside
the frozen envelope and quantify the G1 overshoot factor for the best
in-domain policy. Cannot: overturn the structural menu argument
(section 2) — the construction-level zero-mass invariant plus the
measured 31.02-31.39 BB pure-menu band make e_embed <= 0.002 impossible
for every cap-1 policy observed, independent of convergence — or
authorize any production change. A surviving successor hypothesis
remains exactly what D195 and RFC 0046 named: offline-certified
continuation values / blueprints outside the online envelope, requiring
a new RFC rather than another in-envelope variant.

## 8. Measured D253 result (2026-11-16, single run, no retry)

The probe ran RC 0 under the 120 s outer timeout (solve 5.15 s, total
process 23.11 s including the ~18 s endpoint reach-bound walk, RSS
45.7 MiB, no tripwire). All seven checkpoints are finite and strictly
monotone: 0.31669569, 0.18365192, 0.11621217, 0.07685143, 0.05510212,
0.03827402, 0.02761552 BB; the endpoint equals the sealed control NC
bitwise and reconciles through reach_bound with pure menu
31.21728115 BB.

- **OLS receipt:** a = 1.08887660, b = -1.03655081, R^2 = 0.98003715.
  The slope is OUTSIDE the pre-registered [-0.60, -0.45] band, so the
  pre-registered verdict is **inconclusive**, not a certification; the
  falsifier conjunction is not met because t* = 77.38 s far exceeds the
  12 s G1 max. No band, threshold, checkpoint, or rerun was changed
  after observing the data.
- **Why the band missed:** adjacent-segment slopes steepen across the
  whole ladder (-0.79, -0.90, -1.02, -1.16, -1.63, -1.79); the small
  cap-1 tree is in an early steep regime inside 30-300 iterations,
  whereas the D164 -0.52 estimate came from a much larger cap-6 tree at
  300-1,000 iterations. The one-decade in-envelope fit cannot determine
  whether the law later flattens onto the D164 plateau.
- **What is nevertheless supported in-envelope:** T* = 4,511
  iterations extrapolates to t* = 77.38 s = 6.45x the G1 max; the G1
  iteration ceilings themselves (699 at max, 582 at p95, using the
  measured 17.153 ms/iteration) leave fitted bounded NC of 0.0138 BB
  (6.91x over gate) and 0.0167 BB (8.35x). With a steepening measured
  tail, any later flattening pushes t* further out, so 6.45x is an
  optimistic bound. The symmetric direction is also closed: for an
  endpoint-anchored power law to reach 0.002 BB inside the 699-iteration
  G1-max ceiling at the measured throughput, the slope would have to be
  about -3.10, far steeper than the steepest observed segment (-1.79),
  so even continued steepening cannot bring the target inside G1.
  These are direct in-envelope latency measurements
  on the exact anchor root; they corroborate the G1 wall without the
  D164 import, but per the pre-registered rule they do not certify the
  slope law.
- **Standing of the closure:** unchanged. The structural menu argument
  (section 2) never depended on D253. The inconclusive slope verdict
  neither strengthens nor weakens it; the experiment retires the
  specific in-envelope -0.52 reproduction hypothesis and replaces the
  imported latency datum with a directly measured, still-prohibitive
  G1 overshoot (at least 6.45x) for the best in-domain policy.
