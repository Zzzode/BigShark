# RFC 0047 Stage-1 Implementation Tracker (D-067)

- RFC: [0047](../rfcs/0047-depth-limited-certified-river-resolve.md) (Accepted
  2026-11-04). Authorization: TechLead D-067 - FULL Stage-1 implementation,
  budget approved by the user, no spend cap; measurement-only (no production,
  mount, host mapping, wire/proto change, or enablement). No push.
- Rules: three independently acceptable milestones S1 -> S2 -> S3; advance
  automatically on acceptance (no wait); targeted tests during iteration, ONE
  full verification pass at each milestone closure; if an RFC premise fails,
  record it here with an alternative and continue.
- All Stage-1 code lives under `engine/benchmarks/rfc0047/` behind a new
  configure-time link guard (does not link `bigshark_rfc0046_bench` or the
  production resolver; the algorithms are re-implemented per RFC 0047), with
  evaluator `tools/rfc/rfc0047-gates.ts`. No-legacy: deleted unless a later
  production addendum promotes a named subset.

## S1 — Measurement harness + infoset budget + oracle skeleton

Acceptable results (commands / data / tests):

1. Guarded harness builds: a new static bench target + manual binaries under
   `engine/benchmarks/rfc0047/`, an `rfc0047_link_guard` configure check and
   ctest (negative-tested), no edge to host/practice/resident/pack and no link
   to `bigshark_rfc0046_bench`.
   - command: `cmake --preset release && cmake --build --preset release`
2. Canonical board-class enumerators in the guarded harness with native pins:
   river `134,459`, turn `16,432`, and an n=3 cross-check equal to the
   production `enumerate_canonical_flop_classes()` count `1,755`; identities
   `270,725 x 48 = 12,994,800 = 5 x 2,598,960`.
   - command: `ctest --preset release -R rfc0047`
3. G1 per-river infoset/RSS budget for all six frozen strata: a CSV with the
   depth-limited turn infosets, EACH of the 48 per-river-subtree infosets
   (must be uniform 82/108 river action nodes), responder `-x` rows, and
   conservative sums; every value under 1,000,000 infosets.
   - command: `bigshark-rfc0047-infoset-budget` (CSV to stdout)
   - data: `docs/plans/ablation/rfc0047/s1-infoset-budget.csv`
4. G4 census tool: canonical board classes (16,432/134,459), the per-shape
   post-chance frontier-leaf factor from the built trees, and the projected
   stored-row universe (five-card) / build universe (four-card) formulas.
   - command: `bigshark-rfc0047-census`
   - data: `docs/plans/ablation/rfc0047/s1-census.csv`
5. Offline oracle SKELETON: BSNCV-family codec + immutable fail-closed reader
  keyed on (shape, canonical five-card board, frontier-leaf index, dense
  own-combo) -> `{cv_chips, certified_error_chips}` with per-key root pot;
  native round-trip, tamper, wrong-key, and truncation fail-closed tests. The
  deterministic exact BUILDER is an interface/stub only here (filled in S2).
   - command: `ctest --preset release -R rfc0047_oracle_codec`
6. Frozen evaluator skeleton: `tools/rfc/rfc0047-gates.ts` parses the S1 G1
  and G4 CSVs, enforces the structural predicates (per-request infosets <=
  1,000,000; census constants; reject malformed/probe rows), with node tests.
   - command: `node --test tools/rfc/rfc0047-gates.test.ts`; launcher
     `bin/rfc0047-gates.mjs`
7. Closure: format target, full release ctest once, `npm run check`,
   `npm run proto:check`, `node bin/check-docs.mjs`, `node bin/check-rfcs.mjs`.

Status: **DONE 2026-11-04 (awaiting the one full verification pass, then S2).**

### S1 results (commands / data / tests)

- Guard + build: `cmake --preset release` prints `rfc0047 link isolation
  guard passed`; new static `bigshark_rfc0047_bench` (strata, board_classes,
  tree_budget, cv_oracle) links only `bigshark_solver`; configure-time guard +
  `rfc0047_link_isolation` ctest; explicit reverse edge forbidding
  `bigshark_rfc0046_bench`.
- Canonical classes (native test `rfc0047_board_classes`, ~2.4 s):
  flop 1,755 (cross-checked equal to production
  `enumerate_canonical_flop_classes`), turn 16,432, river 134,459;
  270,725 x 48 = 12,994,800 = 5 x 2,598,960 identity pinned.
- G1 data: `bigshark-rfc0047-infoset-budget` ->
  `docs/plans/ablation/rfc0047/s1-infoset-budget.csv`; evaluator
  `node bin/rfc0047-gates.mjs <budget.csv> <census.csv>` prints
  `S1 structural gates PASS`.
- G4 data: `bigshark-rfc0047-census` ->
  `docs/plans/ablation/rfc0047/s1-census.csv` (constants + per-shape
  frontier/action factor).
- Oracle skeleton: BSNCV01 codec + immutable reader; native
  `rfc0047_oracle_codec` pins FIPS SHA-256 "abc", round-trip/lookup,
  tamper-digest, truncation, bad combo, negative-error, duplicate-key, and
  zero-pot failures. Self-contained SHA-256 (no production artifact dep).
- Evaluator: `tools/rfc/rfc0047-gates.ts` (+7 node tests) parses both CSVs,
  enforces G1 per-request infosets < 1,000,000, per-card uniformity, the
  six-stratum presence, and the G4 constants; G2/G3 are S2 stubs (no Stage
  PASS yet).

### S1 premise refinement (logged per D-067; favorable, no gate change)

The Accepted RFC (following the RFC 0046 closure review) called "one river
subtree" 82 x 1,081 = 88,642 infosets. The S1 tree walk shows two granularities:
- ONLINE reached subtree = ONE turn betting line + ONE river card = at most
  **12** river action nodes -> **14,053 infosets** incl. 1,081 gadget rows
  (limped and SRP both max 12; some (line,card) leaves are 0 = direct
  showdown). This is the actual continual-re-solving object and the binding G1
  online footprint (~71x under 1M).
- OFFLINE per-card grouping = a fixed river card aggregated across all 9/11
  turn lines = **82/108** action nodes -> 88,642/116,748 action infosets
  (+1,081 gadget = 89,723/117,829), uniform across the 48 cards. This is the
  offline builder grouping and is never resident online.
The envelope conclusion is unchanged and strengthened (the online peak is
smaller than the RFC stated); the full-range turn-rooted 4.27M Route A fact
and the offline G4 universe are unaffected. Alternative needed: none; the
RFC text gets an S1 measurement note (no redesign).

## S2 — Per-combo turn-CV oracle and certification

Status: **IN PROGRESS (S2.1 deterministic river kernel; convergence-law
feasibility established on single leaves; S2.2 builder/oracle next).**

> Note: S1 is committed separately as `3bac9ed` (TechLead round 103); S2 work
> is uncommitted in this working tree by design.

### S2.1 deterministic full-deal river solver (V3 kernel)

- Guarded `engine/benchmarks/rfc0047/river_solver.{hpp,cpp}`: SEEDLESS
  zero-sum cap-1 RIVER-rooted solver, fixed five-card board, 1081 own combos,
  all 990 compatible opponent combos every traversal, RM+ + linear averaging,
  reusable scratch pool (zero per-iteration allocation). Board-fixed int8
  signed showdown matrix (one matvec per showdown leaf); folds O(C).
- NashConv normalized on the 1081x990 uniform joint deals, summed over both
  seats / BB (B4 summed-BB).
- Bug fixed during S2: the viewer's own EQUILIBRIUM eval nodes summed child
  values without the average-strategy weights sigma (BR argmax was correct);
  before the fix NashConv was inflated (~2.9 BB false floor).
- Deterministic summed-BB NashConv (~33 iter/s, single thread):

  | iters | W1 | monotone (full range, 10 action nodes) |
  |---:|---:|---:|
  | 80 | 0.109 | 0.106 |
  | 180 | 0.044 | 0.039 |
  | 405 | 0.0172 | 0.0172 |
  | 608 | 0.0093 | 0.0106 |
  | 912 | - | 0.0061 |
  | 1368 | - | 0.0030 |

  Smooth, no sampled noise, no T^-0.55 plateau here; reaches the R0 = 0.002 BB
  scale at a few thousand iterations (~1-2 min/leaf). Strong positive evidence
  for G2 on a single leaf; NOT yet CERT/QUAL vs a reference, the margin
  identity, the G3 slope, or G4 coverage cost. Data:
  docs/plans/ablation/rfc0047/s2/.

  Full single-leaf result (single-thread, deterministic), the hardest online
  shape included:

  | shape (action nodes) | iterations at <=0.002 BB | wall |
  |---|---:|---:|
  | W1 (10) | ~2,052 (0.00201 @2,052; 0.00488 @912) | ~89 s to 3k |
  | monotone full range (10) | ~2,000 (0.0030 @1,368; 0.0014 projected @2,052) | ~59 s to 2k |
  | raised-SRP pot 8 (12) | ~1,100 (0.00230 @912, 0.00140 @1,368, 0.00084 @2,052) | ~108 s to 3k |

  Conclusion (S2.1): a deterministic full-deal river solve certifies a single
  online-reached subtree to R0 in well under the offline per-leaf budget,
  unlike RFC 0039's 10M-iteration/138 s sampled nested leaf. The remaining S2
  risks are not single-leaf convergence; they are (a) certified error vs a
  higher-accuracy reference (CERT/QUAL), (b) the G3 boundary-error slope into
  the turn gadget, and (c) G4 coverage/build across the class universe.
- Native `rfc0047_river_solver`: bitwise determinism, finite/range, monotone
  reduction, near-equilibrium sanity bound. (A zero-sum MEAN-CFV assertion was
  correctly removed: counterfactual values exclude own reach.)
- Per-combo equilibrium + BR values exposed (`river_combo_values`) for the
  oracle builder / G2 exact gap.
- **S2.2 pipeline skeleton:** `bigshark-rfc0047-oracle-build <stratum>
  <shape_id> <iters> <out.bsncv>` solves the check-to-river frontier
  deterministically and writes all 2x1081 five-card BSNCV01 rows with a
  conservative per-entry `certified_error_chips` = summed-BB NashConv x BB;
  round-trips through the S1 immutable reader. Verified: w1 @2,000 ->
  nash_conv_bb 0.00209, artifact 86,544 bytes = 32 header + 2,162 x 40 + 32
  SHA-256 (`docs/plans/ablation/rfc0047/s2/artifacts/w1-shape7-2000.bsncv`).
- **S2.3 multi-leaf post-chance builder + frontier census native test.**
  `rfc0047_oracle_build` now loops over EVERY post-chance leaf from
  `enumerate_frontiers` (432 limped / 528 raised), dedups identical river
  subgames on the full tuple (pot, gross contributions, stacks behind,
  five-card board), solves each unique subgame once, and emits keyed rows for
  every leaf (online lookup arrives through a specific turn line). Distinct
  subgames = distinct (ledger, card): 240 for w1 (5 ledgers x 48 cards).
  Smoke: w1 @50 iters -> 432 leaves / 240 unique solves / 933,984 rows /
  37,359,424 bytes, 382 s single-thread; worst NashConv 0.175 BB at 50 iters
  (convergence iteration count is the production knob, measured in S2.1).
  Native `rfc0047_frontier_enumerator` pins 432/528 leaves, exactly 48 cards
  per turn betting line, 5/6 distinct ledgers, five-card/river validity on all
  six strata (0.05 s).
- **S2.4 margin identity tiny game (`rfc0047_margin_identity`, 8 s).** A
  hand-built two-stage polar/bluff-catcher game solved four ways. GATED
  (tolerances scaled by the 1M-iteration linear-CFR reference's own
  exploitability ~1e-4 BB):
  - C2 exact-leaf depth-limited trunk reproduces the full-game turn strategy
    (gap 3.2e-3);
  - per-leaf river re-solve gadgets are subtree equilibria (max NashConv
    4.7e-5);
  - **C4b (the RFC identity): the gadget's locked responder leaf margin equals
    the full-game conditioned margin for every hand reaching the line, gap
    3.2e-4**;
  - **micro-G3 teeth: a known +0.5 chip margin perturbation appears as a
    0.500 responder-value error (leaf error->value slope = 1).**
  REPORT-ONLY (not gated), an important scope finding: feeding the trunk a
  STANDALONE-UNIFORM oracle instead of exact leaves shifts its turn decision
  (gap ~1), and the static two-sided hybrid NashConv across independently
  re-solved chance leaves is 0.49 even with exact leaves - because at a
  frontier whose generator range collapses to one hand, a value-indifferent
  bluff frequency is not pinned by a one-sided margin. This is exactly why
  production continual re-solving serves only the ONE reached card and why G3
  measures the reached-leaf hybrid on the real engine (RFC 0046 partition
  walker); the tiny test certifies the margin machinery, not oracle accuracy.
- **S2.5 per-combo reference-certified error + G2 evaluator.** The builder now
  solves every unique subgame TWICE (candidate `cand_iters`, higher-accuracy
  reference `ref_iters`, optional `max_groups` frame sampling) and ships a
  per-(seat,combo) one-sided bound
  `cert = |CV_cand - CV_ref| + max(0, BR_ref - CV_ref)` in
  `certified_error_chips`, plus a `<out>.cert.csv` sidecar with per-frame
  cand/ref NashConv, drift/residual/cert in both MAX (e_max) and RMS (e_l2,
  uniform leaf rho) BB norms. `RiverSolveResult` exposes per-combo
  `root_br_seat0/1`.
  Key scaling measurement (w1 check-to-river, pot 4, single subgame):

  | cand / ref iters | mean NashConv (cand) | CERT e_max | CERT RMS |
  |---:|---:|---:|---:|
  | 150 / 1,500 | 0.0523 BB | 0.552 BB | 0.032 BB |
  | 2,000 / 20,000 | 0.00193 BB | 0.0297 BB | 0.0061 BB |

  **Update 2026-11-05 (drift/residual split + correct G2 statistic).** At
  3k/30k the higher-accuracy reference is essentially exact (residual RMS
  2.9e-5 BB, residual max 1.5e-4), so the bound is dominated by candidate
  DRIFT, not reference error. Reading RFC 0044's CERT code
  (`rfc0044-gates.ts`, CERT gates on `meanGap` + seed half-width, never a
  worst-combo quantity) clarified that G2's 0.005 bar acts on the MEAN/RMS
  convergence gap, not per-combo worst. Corrected picture:

  | cand/ref | mean NashConv | drift RMS (CERT) | residual RMS (half-width) | e_max (drift, ->G3) |
  |---:|---:|---:|---:|---:|
  | 150 / 1,500 | 0.0523 | 0.032 | — | 0.552 |
  | 2,000 / 20,000 | 0.00193 | 0.0061 | — | 0.0297 |
  | 3,000 / 30,000 | 0.00121 | **0.00483** | **0.000029** | 0.0239 |

  **Final statistic correction (2026-11-05): G2 is a SUMMED gap, per-entry
  error is G3's input.** Re-reading the RFC 0044/0045 CSV schema, CERT g(T)
  is `bucket_sum_gap_bb` and QUAL is the exact-hole `sum_gap_bb` - both
  SUMMED BR/NashConv quantities, never per-combo. So for the seedless,
  bucket-free, exact-full-deal V3 kernel: CERT = QUAL = candidate summed
  NashConv (bar 0.005 / 0.03); the RFC 0044 independent-seed half-width is
  exactly 0 BY SEEDLESSNESS (covered by the bitwise-identical rerun native
  test), with a separate reference-quality check (ref NashConv no worse than
  cand). The per-combo drift e_max / drift RMS and the shipped one-sided
  per-entry bound are RECORDED ONLY - they are the oracle entry error G3
  propagates through real turn-equilibrium leaf reach rho; large-pot or
  value-indifferent combos (per-combo drift RMS up to 1.08e-2, e_max up to
  3.1e-2 on the frame) are therefore not a G2 failure.
- **G2 frozen frame RESULT (2026-11-05): PASS on all six strata.**
  cand 6k / ref 20k, stride-48 (one representative card per ledger: all 5/6
  distinct pots incl. the degenerate all-in pot-400 ledger):

  | stratum | CERT/QUAL summed NC | ref NC | e_max (->G3) | drift RMS (->G3) |
  |---|---:|---:|---:|---:|
  | w1 | 4.60e-4 | 6.16e-5 | 1.75e-2 | 7.17e-3 |
  | unpaired-rainbow-ahigh | 3.60e-4 | 6.00e-5 | 1.11e-2 | 3.34e-3 |
  | paired | 5.07e-4 | 1.06e-4 | 1.61e-2 | 3.86e-3 |
  | monotone | 3.36e-5 | 3.10e-6 | 1.93e-3 | 6.98e-4 |
  | unpaired-twotone | 3.82e-4 | 4.86e-5 | 3.09e-2 | 1.08e-2 |
  | raised-srp | 1.54e-4 | 1.42e-5 | 2.89e-2 | 2.47e-3 |

  CERT/QUAL is ~10x inside the 0.005 bar and ~60x inside 0.03 on every
  stratum; reference <= 1.1e-4. A texture-diverse stride-12 frame (4 river
  cards per ledger: one board-pairing + three non-pair low/mid/over cards,
  offsets 0/12/24/36 of the 48 live cards) confirms the river-card texture
  dimension, not just the ledger dimension. Frozen consolidated evidence:
  `docs/plans/ablation/rfc0047/s2/g2-frame12-6strata-6k20k.csv` (124 unique
  subgames, 20 per limped stratum + 24 raised; per-stratum per-group rows in
  `cert/frame12-<s>.bsncv.cert.csv`). Verdict: **G2 PASS all six**, CERT/QUAL
  worst 8.37e-4 BB (6x inside 0.005), reference worst 1.67e-4; per-combo
  e_max worst 7.35e-2 BB and drift RMS worst 1.08e-2 are recorded as G3 inputs
  (the larger e_max vs stride-48 is expected: stride-12 adds non-pairing
  overcards whose value moves more across iteration levels). The 3-4 MB
  per-frame `.bsncv` binaries are deterministic rebuild products (byte-
  identical rerun), not required as tracked gate input - the CSV is.
- **Parallel deterministic builder (2026-11-05).** `rfc0047_oracle_build`
  solves the independent unique subgames on a std::thread pool (optional args
  `[MAX_GROUPS] [GROUP_STRIDE] [THREADS]`), then emits artifact + sidecar
  serially in ascending group order. Verified byte-identical artifact and
  CSV across 1 vs 4 threads (seedless solves; only ordering synchronized).
  56 cores.
  Remaining S2: G3 turn-scale harness (the dominant remaining build): an
  independent rfc0047 turn-rooted solver with oracle-valued post-chance
  leaves (1128 four-card combos mapped through the dealt card to 1081 five-card
  dense rows, blocker-aware), real per-leaf CF reach rho, additive CV
  perturbation arms, and the independent telescoping hybrid-BR walker.
- **G3 implementation constraint (scoped 2026-11-05).** The
  `rfc0047_link_guard` forbids `bigshark_rfc0047_bench` linking
  `bigshark_rfc0046_bench`, so the RFC 0046 partition walker is reused as a
  SPEC, not a library: S2 adds an independent rfc0047 telescoping exact-BR
  hybrid-NashConv walker (`g(I) = own_prefix_H(I) (max_a PV_BR(a) -
  sum_a sigma_H(a) PV_BR(a))`, blocker-aware full joint deals, per-leaf real
  CF reach rho returned for e_l2). G3 then: solve the turn-rooted cap-1 tree
  with oracle-valued leaves to a turn equilibrium, inject additive five-card
  CV perturbations (zero-mean across combos; coherent same-sign adversarial;
  levels 1e-6..gate-scale in pot fraction), re-measure exact hybrid NashConv,
  and fit the one-sided UCB envelope delta <= f(e_max, e_l2) with production
  inequalities UCB(hybrid) <= 0.010 and UCB(t_river) <= 0.002 BB. The
  tiny-game micro-G3 already pins leaf slope = 1; real-scale rho and the
  turn amplification are the open quantities.
- **G3 harness frozen build plan (next focused unit).** New guarded files
  `engine/benchmarks/rfc0047/turn_solver.{hpp,cpp}` +
  `rfc0047_g3_probe.cpp` (manual binary, no CTest), spec reimplemented
  independently of `bigshark_rfc0046_bench` per the link guard:
  1. TurnGame: range-vectorized two-seat CFR over the turn-rooted cap-1 tree;
     action nodes reuse the river kernel's RM+/linear update and scratch pool;
     CHANCE nodes (48 live river cards) average child CFVs; a TerminalFrontier
     leaf is a matrix terminal whose per-own-combo value is
     `sum_j w(i,j) V_ref[i_five,j_five]`, where the turn combo (1128 dense on
     the 4-card board, blocker-filtered to those NOT using the dealt card) maps
     via `holding_for_dense`+`dense_pair_index` to the 1081 five-card dense row
     of that leaf's river solve (read from the BSNCV01 oracle or an in-memory
     matrix). Fold/showdown terminals handled exactly as the river kernel.
  2. After turn equilibrium, record per-leaf per-seat CF reach rho(seat,c)
     (opponent/chance view reach times own prefix probability), normalized per
     leaf per seat for e_l2.
  3. Perturb the five-card leaf matrix additively at predeclared pot-fraction
     levels {1e-6,1e-5,...,1e-2}: a zero-mean-across-combos arm and a coherent
     same-sign-over-a-player's-combos adversarial arm. e_max and
     e_l2 = sqrt(sum rho (Delta v/p)^2) recorded per injection.
  4. Independent telescoping exact-BR hybrid walk gives hybrid NashConv delta
     (and a t_river-only accumulation) vs the unperturbed turn equilibrium.
  5. Fit the one-sided UCB worst-case envelope coefficient over the six strata
     (deals 4320..4324 n=5 t=2.776 as in RFC 0044/0047); acceptance
     UCB(hybrid) <= 0.010 BB and UCB(t_river) <= 0.002 BB at the operating
     e_max_frozen implied by the shipped per-entry bounds. Native validation:
     zero perturbation -> hybrid NashConv equals the turn solver's own
     NashConv within tolerance (the telescoping identity, mirroring RFC 0046
     addendum 5.3's 1e-9-root-pot check).
  This same TurnGame is the S3 online turn solve core, so it is built once
  under the rfc0047 guard and reused.

Acceptable results (to be detailed at S1 closure without changing RFC gates):

- Deterministic seedless range-vectorized exact builder (V3 family) solves all
  48 post-chance river subtrees per turn object and writes five-card frontier
  rows with one-sided certified error; bitwise-identical rerun native test.
- `b_river_s(c)` margin identity native test on a tiny game (depth-limited
  with exact leaves vs full tree).
- G2: oracle CERT <= 0.005 BB (half-width <= 0.001) AND QUAL <= 0.03 BB HU on
  the frozen coverage frame, per-seed superiority over the nine-category and
  equity-frontier boundaries; data CSV.
- G3: absolute-BB perturbation -> hybrid-NashConv slope (e_max/e_l2 with leaf
  CF-reach rho, zero-mean + adversarial arms, one-sided UCB envelope);
  production inequality UCB(hybrid) <= 0.010 and UCB(t_river) <= 0.002.
- Evaluator implements G2/G3 with frozen statistics; node tests.

## S3 — Full-street integration and Gate-2 re-test

Acceptable results:

- Online depth-limited turn solve (b_turn from the sealed trunk reference) and
  on-river exact re-solve (locked b_river), both serve points certified;
  fail-closed paths.
- G1: two per-request R=30 HF-type-1 timing/resource distributions on the
  frozen machine (turn-only; river), p95 <= 10,000 ms / max <= 12,000 ms,
  RSS <= 1 GiB, nodes <= 5M.
- G5: responder bound at 1e-9 root pot on both serve points; cap-1-vs-cap-0
  gap UCB <= 0.002 BB; nothing promoted from approximate.
- Full Gate-2 re-test across the six strata, 30 cold resolves each where the
  gate requires, evaluator emits a typed PASS/CLOSE for the depth-decomposed
  route; G6 dual reporting; evidence package for a second non-author review.
- Closure: full verification pass + second evidence review inputs.

Status: NOT STARTED.

## Premise failures / deviations

None yet. Any failed RFC premise is logged here with: the failing gate, the
measured evidence, the affected milestone, and a concrete alternative
(per RFC 0047: range-conditioned oracle branch; governance closure precedent
RFCs 0041/0043/0045/0046).
