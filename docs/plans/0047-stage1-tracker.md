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
- **G3 step-1 RESULT (2026-11-05): two-street pipeline runs and converges.**
  `turn_solver.{hpp,cpp}` + `rfc0047_g3_step1.cpp` (uncommitted, separate from
  the S2 commit d8593c7). The river kernel gained an optional per-joint-deal
  seat-0 continuation matrix (`compute_deal_matrix`, added to the S2
  `solve_river_subgame` result), built by a fixed-profile probability walk;
  a native invariant cross-checks its row sums against the independently
  computed equilibrium CFV (they match to the bit at 1.2k iters once the
  blocker convention is matched). Two real bugs were found and fixed before
  the turn solve was correct:
  1. the turn leaf fold initially double-counted incompatible joint deals:
     it excluded combos that block the RIVER card but not combos whose two
     HOLE hands share a card (not a legal deal). The river matrix carries the
     river kernel's fold-reach convention (fold terminal sums reach over all
     1081 opponent hands, so blocker pairs are non-zero/non-zero-sum); the
     turn expectation MUST skip shared-hole pairs via a precomputed 52-bit
     combo mask (applied at matrix leaves AND fold terminals). Without this,
     turn NashConv was ~126 BB (non-zero-sum junk entries);
  2. turn NashConv normalization double-multiplied by kTurnCompat (1035 =
     C(46,2)): the vectorized evaluate returns RAW CFV summed over compatible
     deals (unlike the river kernel's per-deal-normalized combo_values), so
     the summed gap divides by kTurnC*kTurnCompat once, no extra factor.
  After both fixes, w1 zero-perturbation turn NashConv (river 300 / turn 100)
  is 0.366 BB @50 -> 0.187 BB @100 turn iters, monotonically ~N^-1, i.e. the
  depth-limited turn solve with oracle-matrix leaves is mechanically correct;
  gate scale (0.010/0.002) needs more turn iterations. Cost: 240 unique river
  leaves + 100 turn iterations in ~460 s wall (48 threads); the per-iteration
  cost is 432 full 1081x1081 matrix folds (range-vectorized, every chance leaf
  is reached) - the next optimization before the perturbation campaign is to
  parallelize/vectorize the chance-child matrix folds (GEMV-shaped). Not yet
  built: perturbation arms, real rho e_l2 export, the independent telescoping
  hybrid-BR walker, native tests, and the UCB fit.
- **G3 step-1 perf + RM+ (2026-11-05).** The 48-child river chance fold is
  now parallelized with a persistent fork-join thread pool in `turn_solver`
  (reused across CFR iterations and the BR evaluations; no per-job thread
  create), leaf/fold computations refactored to pure caller-buffer functions.
  Parallel output is bit-identical to serial (0.36612975/0.18681381 BB
  unchanged), wall for river-300 + turn-100 dropped 458 s -> 115 s on 24
  threads. Added the missing RM+ post-sweep regret clip (the river kernel
  clips; turn was reading positive regret for the policy but never clipping
  the accumulated table). A river-2000 / turn-4000 convergence curve (24
  threads) is running to find the turn iteration count at the B0/R0 scale.
- **G3 step-1 third bug (chance divisor; 2026-11-05).** The public-card
  chance average divided summed child CFVs by 46, correct only if the opponent
  holding blocking the card were not masked. The leaf fold masks BOTH sides
  (i5<0 for the own blocked card, j5<0 or shared-hole for the opponent), so a
  concrete joint deal (ti,tj) has exactly 44 live river cards (52 - 4 board
  - 4 holes): the uniform per-deal divisor is **44**. With /46 the
  continuation value was attenuated by 44/46 = 4.35% against fold terminals,
  adding a spurious fold-vs-continue exploitability floor (the pre-clip
  4000-iter floor of 0.017 BB). After /44 + the RM+ clip, the w1 river-600 /
  turn-4000 curve is perfectly monotone and smooth:
  `50=.2238 100=.1357 200=.0996 400=.0849 800=.0759 1600=.0726
  3200=.0712 final(4000)=.0704 BB`, wall 1181 s, 3610 turn action nodes.
  The remaining ~0.070 BB plateau at river 600 is NOT turn solver error:
  it is the finite-river-solve leaf error (600 CFR iters) propagated through
  real leaf reach - exactly the quantity G3 exists to bound. A river-2000
  curve is running to measure the floor shift; the G3 probe injects known
  errors around this operating point.
- **G3 step-1 floor is NOT leaf error (2026-11-05).** The river-2000 /
  turn-4000 /44 curve plateaus at essentially the SAME 0.0716 BB as the
  river-600 run (0.0704; r600 end even marginally lower) - tripling river
  solve accuracy does not move the floor. This is decisive: with FIXED leaf
  matrices the matrix-terminated turn tree is a finite zero-sum game, so its
  best-response gap must converge to ~0 regardless of whether the fixed
  leaves are an exact river equilibrium (the t_river term, not the turn gap,
  carries river error). The ~0.07 BB residual is therefore a TURN-CFR
  convergence-tail defect, not propagated leaf error. Prime suspect is RM+:
  the margin tiny game already needed VANILLA linear regret matching because
  RM+ clipped-and-froze persistent mixed strategies, and the two-street turn
  game is far more mixing-heavy (3,610 action nodes, matrix-valued leaves).
  Added `TurnSolveConfig.rm_plus` (default true) and a step1 `rmp|van` arg;
  an identical river-300/turn-4000 A/B is running to pick the variant. (The
  average-strategy opponent-reach weight `ow` is a node-level scalar that
  cancels in the per-node sigma normalization, so it is not the floor.)
- **G3 step-1 RM A/B result (2026-11-05).** Identical river-300/turn-4000:
  RM+ ends 0.07038 BB (smooth); VANILLA ends 0.19360 BB and stays noisy
  (400=0.418 rebound). RM+ is decisively better and is retained. So the
  ~0.07 floor is NOT the regret variant; it is shared by both.
- **G3 step-1 fourth bug: wrong average-strategy weight (2026-11-05).**
  Contrary to the earlier note, the average-strategy accumulation DID carry
  a reach factor: the turn traverser node weighted each contribution by
  `ow = sum_j pi[trav^1][j]`, the OPPONENT reach pi_-i (which is the REGRET
  factor), instead of the traverser's OWN reach pi_i. In this codebase's
  traversal convention the traverser's own reach pi[trav] is not scaled at
  own nodes and is identically 1 (the proven river kernel writes the weight
  as `avg_weight * pi[trav][i]` = linear weight). Weighting the time-averaged
  mix by a varying opponent reach biases it and explains the permanent floor
  while leaving regret-matched instantaneous play sound. Fixed to
  `sum += avg_weight * rm(...)`, exactly matching the river kernel. A
  river-300/turn-4000 RM+ rerun is measuring the new tail; if correct the
  turn gap must now keep falling toward zero (fixed-leaf finite game) instead
  of flattening at 0.07. The telescoping identity is unaffected (it evaluates
  whatever average strategy is built) and must remain at machine epsilon.
- **G3 step-1 RESOLVED (2026-11-05): average-weight fix removes the floor.**
  The river-300/turn-4000 RM+ curve after the fix is a clean ~1/T decay with
  NO plateau: `50=.1728 100=.0730 200=.0330 400=.01512 800=.005451
  1600=.001902 3200=.000721 final(4000)=.000515 BB`, wall 956 s. This is with
  deliberately inaccurate river-300 leaves, confirming the turn gap
  (fixed-leaf finite zero-sum game) is independent of leaf accuracy and that
  leaf error enters ONLY through t_river; turn 4000 reaches 5.2e-4 BB, well
  inside B0 = 0.010. The native identity test stays at machine epsilon
  (3.6e-16) and teeth remain positive (coh 1e-2 -> delta 0.0030 BB) after the
  fix; turn-120 solver NC dropped 0.128 -> 0.064 BB (faster convergence).
  Four real bugs total were found and fixed in the turn pipeline:
  (1) shared-hole deals not masked at leaves/folds (126 BB junk);
  (2) NashConv double-divided by kTurnCompat;
  (3) public-card chance divisor 46 -> 44 (4.35% value attenuation floor);
  (4) average-strategy accumulation weighted by opponent reach pi_-i instead
      of the traverser own reach pi_i = 1 in this convention (the 0.07 BB
      floor; now pure linear weight matching the proven river kernel).
  Missing RM+ clip was also added (smooths but was not the floor).
- **G3 w1 gate-scale RESULT (2026-11-05, r600/t3200/ref2400, 24 injections;
  `g3/g3-gate-w1.csv`).** Baseline turn NC 8.46e-4 BB, telescoping identity
  4.7e-18, offline t_river (loose r600 leaves) 4.09e-3. Injected
  delta_hybrid BB at the two gate-bracketing levels:
  | arm | e_l2@2e-2 | delta@1e-2 | delta@2e-2 | delta@E=0.0184 (interp) |
  | zm  | .0220 | .001172 | .003563 | .00318 |
  | coh | .0156 | .004852 | .013129 | **.01181** |
  | pat | .0218 | .002064 | .006539 | .0058 |
  | act | .00545 | .000345 | .001047 | **.00094** |
  The act arm imports the REAL signed per-combo candidate-minus-reference CV
  drift of each leaf (normalized to e_max); it is 12.5x milder than the
  coherent arm at gate scale and ~8.5x INSIDE the B0-R0 = 0.008 budget at the
  operating point e_max_frozen = 0.0184. The distinguishing coordinate is the
  RFC-defined second norm e_l2: the adversarial coherent shift (every combo of
  one seat same-sign at the max per-entry bound) carries e_l2 = .0156, whereas
  production oracle errors are diffuse mixed-sign with the measured global
  G2 drift-RMS/pot = 0.0027 (act@E has e_l2 ~ .005, still conservative).
  **Finding (to be confirmed over six strata): a NAIVE e_max-only worst-case
  certificate FAILS** (coh interpolates .0118 > .008 at E=0.0184; such a
  gross coherent bias is not a realizable deterministic-exact-solver residual)
  **while the joint (e_max,e_l2) envelope over the actual signed-drift shape
  PASSES with wide margin (~.0009).** G3 therefore must certify a mounted
  node in the two-norm plane, not on e_max alone. Small levels <=1e-4 sit in
  the +-3.5e-5 t3200 residual noise band (reported, not used at the gate).
  Probe now flushes the campaign CSV per injection (earlier run buffered until
  exit, causing a false "no output" monitor reading; no data was lost).
- **G3 mechanism committed; six-stratum campaign running (2026-11-05).** The
  turn solver, CF-reach export, independent hybrid walker, perturbation probe,
  native tests, and TS evaluator are committed as `7b46071` (11 files,
  isolated from the unrelated dirty set; engine/CMakeLists carries only the
  three G3 hunks). Campaign frame: w1 gate CSV already complete; the other
  five strata run at r600/t3200/ref2400 with levels {1e-6..2e-2} to
  `g3/g3-campaign-<stratum>.csv` (paired+monotone, then ahigh+twotone, then
  raised-srp). Final evaluator concatenates w1 + the five campaign CSVs and
  certifies the joint (e_max,e_l2) envelope via evaluateS2G3; tracker verdict
  and evidence are the next G3 commit after UCB.
- **G3 step-2 design (frozen; next implementation).** Perturb the per-leaf
- **G3 step-2 implemented (2026-11-05, uncommitted).**
  1. `TurnSolveResult.leaf_reaches`: per-config-leaf, per-seat, per-five-card
     combo exported counterfactual reach
     `rho(p,i5) = (1/kTurnNorm) sum_(ti->i5) P_p(ti)/44 *
       sum_(tj live, compatible) P_q(tj)`, accumulated by a parallel
     average-strategy probability walk (`reach_walk`/`reach_chance_into`).
  2. Independent `turn_hybrid_nash_conv` (new `HybridWalker` class, separate
     tree build, leaf mapping, matrix folds, recursion from TurnGame):
     telescoping exact-BR turn gap plus the t_river term =
     sum_k W_k * riverNC_k / kTurnNorm with W_k the per-leaf arrival mass
     (p0*p1 joint deals /44). Identity target: walker turn gap ==
     TurnSolveResult.nash_conv_chips at zero perturbation.
  3. Manual binary `bigshark-rfc0047-g3-probe` (NOT a CTest): solves the
     unperturbed equilibrium, prints the identity, then for arms
     {zm zero-mean alternating, coh coherent same-sign seat-0 only, pat
     modulo-7 zero-mean} x levels {1e-6..1e-2}: perturbs every unique group
     matrix `M'[i,j]=M+d0[i]-d1[j]`, re-solves the turn, evaluates the fixed
     perturbed strategy against TRUE leaves with the independent walker, and
     records e_max, e_l2 (frozen rho norms), turn_gap, river_term, delta.
     NOTE the earlier draft applied +amp to both seats identically, which
     cancels algebraically (d0-d1=0); coh is now a one-seat coherent shift
     per the frozen design.
  4. Native test `test_rfc0047_turn_solver` (registered CTest): pins the
     telescoping identity to <=1e-7 BB, rho non-negative/seat-symmetric,
     total river arrival mass in (0,1], and coherent-shift teeth
     (delta_hybrid > 1e-3 BB at level 1e-2). Both cases share one 240-group
     river fixture. **First full run PASSED (river 80 / turn 120):**
     solver NC 0.12849932 vs independent walker 0.12849932, diff = 1.67e-16
     (machine epsilon - two independently written evaluators agree to the
     bit); river t_river term 0.05244 BB at this deliberately tiny river
     solve; total river arrival mass 0.6010 (the remaining 0.399 of joint
     deals end at turn folds); coh level 1e-2 teeth delta_hybrid = 0.00492
     BB (> 1e-3 gate), an empirical leaf-error->exploitability slope ~0.49.
- **G3 statistics predeclared (2026-11-05, before campaign data).** RFC 0047
  section G3(d) anticipated seed variance from a SAMPLED NashConv deal-batch
  estimator (full seeds 4320..4324, n=5, t=2.776). The built telescoping
  walker is instead an EXACT, SEEDLESS full-deal evaluator (it enumerates
  every compatible joint deal; the zero-perturbation identity holds to 1.67e-16
  BB), so a per-node NashConv draw has no sampling distribution and inventing
  seeds 4320..4324 would be the pseudo-replication the RFC forbids. The honest
  variability to bound a NON-probed mounted node is ACROSS THE SIX FROZEN
  STRATA (texture/betting/ledger). Predeclared statistic per (arm, level):
  from the six deterministic stratum deltas compute mean, SD s, and the
  one-sided 95% UCB `mean + t_0.95(df=5)=2.015 * s/sqrt(6)` for BOTH
  delta_hybrid (turn-gap component) and the river term; the gate is evaluated
  at the production river solve count and the operating e_max_frozen from the
  G2 per-entry bounds (worst arm / first level >= e_max_frozen), requiring
  UCB(hybrid) <= 0.010 and UCB(t_river) <= 0.002 summed BB. The campaign frame
  is one append-mode CSV per probe run
  (`stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb`
  plus an `identity` row per stratum).
- **G3 step-2 design (frozen; next implementation).** Perturb the per-leaf
  five-card matrix additively at the (seat,combo) level, re-solve the turn
  equilibrium, and measure its hybrid NashConv against the UNPERTURBED
  leaves:
  - arms: zero-mean across a seat's combos (sum delta = 0); coherent
    same-sign adversarial (all combos of one seat shifted the same sign);
    and a scaled-actual-error arm using the measured G2 drift pattern;
  - levels: additive amplitude as a fraction of the leaf root pot p,
    {1e-6, 1e-5, 1e-4, 1e-3, 1e-2};
  - per injection record e_max = max|delta|/p and
    e_l2 = sqrt(sum_(seat,c) rho(seat,c) (delta/p)^2), with rho the leaf
    counterfactual reach from the unperturbed turn equilibrium, normalized
    per leaf per seat (exported by the turn solver alongside the strategy);
  - the independent telescoping exact-BR walker evaluates the perturbed-
    strategy vs true-leaves hybrid NashConv (whole game) and a t_river-only
    accumulation, so both UCB(hybrid) <= 0.010 and UCB(t_river) <= 0.002 can
    be certified against the operating e_max_frozen;
  - six strata x deals 4320..4324 n=5 t=2.776 one-sided UCB. Native test:
    zero perturbation hybrid NashConv == turn solver own NashConv.

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
