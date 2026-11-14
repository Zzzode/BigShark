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
- **G3 VERDICT: PASS (2026-11-06), joint two-norm certificate.** All six
  strata completed (150 injections; `g3/g3-frame6.csv`). Operating point from
  the shipped G2 bounds: E = 1.8370e-2 (max cert_max/pot), L = 2.6974e-3
  (max cert_rms/pot). The evaluator (`evaluateS2G3`, 22 node tests) certifies
  the propagated TURN delta along the REAL signed-drift ray (the `act` arm),
  interpolated at E per stratum, with one-sided 95% t=2.015 UCB across six:
  **mean 1.3211e-3 / UCB 2.0694e-3 BB <= B0-R0 = 0.008 (headroom 5.93e-3,
  ~3.9x).** Per-stratum joint delta: w1 .93e-3, ahigh .86e-3, paired .68e-3,
  monotone 3.01e-3 (worst), twotone .73e-3, raised-srp 1.71e-3. The act ray
  e_l2 at E (4.98e-3..7.78e-3) is >= L on every stratum, so the certified
  point is no less correlated than any mounted node (conservative). The
  single-norm coherent stress arm UCB is 1.5135e-2 and is RECORDED as
  over-budget, not gated: a whole-range same-sign bias at the max per-entry
  bound is not a realizable deterministic-exact-solver residual (residuals
  are diffuse mixed-sign, captured by e_l2) - this is exactly why RFC 0047
  G3(a) defines both norms. Full numbers and conservatism notes in
  `g3/G3-VERDICT.md`. The online R0 = 0.002 river term remains an explicit
  S3/G5 item (campaign used loose r600 leaves; its t_river is a diagnostic,
  not the production R0); G3 bounds only the propagated turn delta above it.
- **G3 independent review CHANGES-REQUESTED + full verification (2026-11-06).**
  A separately launched non-author reviewer reproduced all arithmetic
  (act-ray UCB 2.07e-3, coh 1.51e-2, 22 node tests) and confirmed the
  instrument (independent walker, machine-epsilon identity) but rejected the
  claim that the act ray certifies an arbitrary mounted node. Accepted
  findings:
  1. BLOCKING: a 1-D ray cannot bound the 2-D box {e_max<=E,e_l2<=L};
     exploitability is NOT monotone in e_l2 (frame itself: w1 1e-2 zm
     e_l2 .0110/delta .00117 vs coh e_l2 .0078/delta .00485). Missing worst
     shape = partially coherent BR-weight-ranked SPIKE on the ~(L/E)^2 ~3%
     combos carrying most telescoping BR-deviation weight.
  2. BLOCKING: shipped L is UNWEIGHTED per-group cert RMS; probe e_l2 is
     reach-weighted and pooled - different norms; must recompute L* reach
     weighted from per-combo cert vectors and exported rho.
  3. MAJOR: joint framing is RFC-supported, but no mount-time per-node e_l2
     contract exists yet (future S3 production RFC must enforce it); coherent
     arm is RFC-required adversarial and fails, reported not hidden.
  4. MAJOR: t-UCb 2.07e-3 is a MEAN bound below the observed worst stratum
     (monotone 3.01e-3); the non-probed-node envelope is the frozen-frame MAX
     (3.01e-3, still < 0.008) or a tolerance factor on a census-sampled
     frame, not a mean CI.
  5. MAJOR labeling: full G3 acceptance also needs R0<=0.002 (S3/G5);
     relabeled act-ray result to PROVISIONAL turn-propagation component.
  6. MINOR: act per-group/per-seat max normalization overstates cross-leaf
     scale (conservative); nominal d vs effective d0-mean(d1) (conservative);
     signed 6k/20k drift never exported - required before production auth.
  Full milestone verification on commit aaaa89d was GREEN: release build RC=0,
  ctest 91/91, npm run check RC=0, npm run proto:check RC=0.
  Closure under way: probe gained (a) independent-walker BR-minus-sigma leaf
  gap weights `turn_leaf_gap_weights` (per-leaf/seat/combo w = BR own-reach
  minus average own-reach), (b) deterministic `spike(q)` arms that water-fill
  a +E payoff shift onto the top-q rho mass ranked by w/rho
  (q=.02/.033/.05/.1; smoke confirms e_l2 scales .00195/.00248/.00304/.0135 at
  E, q=.033 ~ L), and (c) a reach-weighted real-norm sidecar L* from the
  exported rho and the measured signed drift. Six-stratum production
  convergence spike campaign (r600/t3200/r2400, task bqcxegknd) is running;
  the evaluator then gates the frozen-frame MAX stratum box-corner delta at
  e_l2=L* against 0.008 (no mean-CI-as-envelope). Verdict stays provisional
  until that passes and R0 is measured in S3/G5.
- **G3 spike box result on the r600 PROXY: borderline FAIL; production L*
  measured next (2026-11-06).** Six-stratum production-convergence spike
  campaign (r600/t3200/r2400, 7 injections/stratum, `g3-spike-frame6.csv`)
  traces delta vs reach-weighted e_l2 at amplitude E=0.0184. The reach-weighted
  norm of the LOOSE r600/r2400 drift (`g3-realnorms.csv`) is much larger than
  the earlier unweighted RMS guess: L*(r600) = w1 .00785, paired .00606,
  monotone .00441, ahigh .00581, twotone .00512, raised .00393 (emax_real
  r600 up to .206 pot vs the shipped 6k cert_max .0184 - the r600 leaves are a
  deliberately sloppy proxy). evaluateS2G3Spike box-corner deltas at this
  proxy L*: ahigh .00237, twotone .00230, paired .00362, monotone .00563,
  w1 .00746 (chord; concave so possibly higher), **raised-srp .00872 > 0.008**
  -> frozen-frame MAX 8.72e-3, FAILS by 9% on the proxy. The spike DELTA
  frontier is a fixed-leaf-game quantity and leaf-accuracy independent (proven
  earlier), but the L* coordinate is not: the correct production object is the
  reach-weighted e_l2 of the 6k/20k (not r600/r2400) drift, joined with the
  production-leaf turn-equilibrium rho. A norms-only campaign at r6000/ref20000
  (task bjx0h0to4, per-stratum files g3-norms6k-*.csv) is measuring it; with
  r600 errors ~an order of magnitude larger than shipped, L*(6k) is expected
  to fall well into the measured low-q region (q=.02 e_l2 .0024-.0028,
  raised delta .0044) but this is MEASURED, not assumed. The G3 box verdict is
  held until L*(6k) returns; if the production corner still exceeds 0.008 the
  response is tighter G2 per-entry bounds (more river iterations) or an S3
  mount-time shape/e_l2 contract, never a relaxed gate.
- **G3 production L* measured (2026-11-07): far below the proxy.** The
  r6000/ref20000 reach-weighted real norms (`g3-norms6k-*.csv`, per-stratum
  single-writer files; emax_real is now 0.0061-0.0232 pot, i.e. the shipped
  accuracy regime) give L*(6k) = w1 **.000801**, paired **.001294**, monotone
  **.000993**, ahigh **.000984**, twotone **.001136**, raised **.000431** —
  5-6x smaller than the r600 proxy (.0039-.0079) and well below the finest
  measured spike concentration q=.02 (e_l2 .00236-.00279 at E). Since delta
  at E decreases monotonically as q shrinks on every stratum (q=.02 is the
  smallest/adversarial-most-efficient point), the production box corner at
  L*(6k) is bounded above by the r600-leaf q=.02 spike: raised .00439,
  monotone .00304, paired .00130, ahigh .00044, twotone .00083, w1 .00142 —
  frozen MAX .00439 < 0.008. To close the residual leaf-substitution
  objection directly (the spike frontier used r600 leaves/rho), a
  candidate-only r6000 prod box campaign measures q=.01/.02 at E with
  production leaves and the production equilibrium rho (2 injections/stratum,
  task btguooss4); the box verdict is finalized from that.
- **G3 production box FINAL (2026-11-07): turn-propagation component PASS.**
  q=.01/.02 with production r6000 leaves gave frame MAX 2.27e-3 (3.5x); the
  ultra-fine q=.002/.005 production points (`g3-prodfine-*`, combined
  `g3-prodbox-frame6.csv`, 4 measured points/stratum) then bracket EVERY
  operating L* with measured points (raised L*=.00043 < q=.002 e_l2=.00080,
  clamped) - zero extrapolation. Final box-corner propagated turn delta:
  w1 1.47e-4, ahigh 1.24e-4, paired 7.14e-4, monotone **8.19e-4** (max),
  twotone 2.87e-5, raised 5.35e-4. **Frozen-frame ENVELOPE MAX 8.19e-4 BB <=
  B0-R0 0.008, ~9.8x headroom; evaluateS2G3Spike PASS.** Independent review
  Findings 1 (ray vs box), 2 (reach-weighted production L*), 4 (frame MAX not
  mean CI) are closed; Finding 5 (labeling) applied (this is the
  turn-propagation component, not full G3 acceptance); Finding 3's mount-time
  per-node e_max/e_l2 contract and the R0<=0.002 term remain explicit S3/G5
  items. Committed through eca44bb; prodfine evidence and this final number
  follow in the next commit with a full milestone re-verification.
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
- **G3 round-2 closure FINAL (2026-11-08): shape-free analytic box PASS.**
  Definitive `full` campaign (candidate r6000 + reference r20000 + four fine
  spikes q=.002/.005/.01/.02 at E per stratum, `g3-full-frame6.csv`,
  `g3-norms6k-full.csv`). The analytic global-pooled two-tail water-fill bound
  (commit 22a9441) DOMINATES the measured delta at all 24 production spike
  points (3-15x margin), empirically validating the first-order bound despite
  the telescoping gap's convexity. Each stratum is certified at its OWN
  measured full-census envelope (a mounted node's error <= that stratum's
  e_max_real and reach-weighted L*); applying the cross-stratum amplitude max
  E_env=.02321 to low-emax strata was an incorrect over-rescale and is
  reported only. Own-envelope analytic box bounds BB: w1 2.08e-3, paired
  5.66e-3 (MAX), monotone 4.82e-3, ahigh 2.15e-3, twotone 5.53e-3, raised
  2.61e-3. **Frozen-frame analytic ENVELOPE MAX 5.66e-3 <= B0-R0 0.008 (1.41x);
  G3 turn-propagation component PASSES on the shape-free bound** (measured
  corners 1.5e-4..8.2e-4, far below). 27 gate node tests. Review F1 (global
  pooling), F2 (negative tail), F3 (full-census amplitude) closed; R0<=0.002
  and the mount-time e_max/e_l2 contract remain explicit S3/G5 items.
- **G3 turn-propagation component CLOSED + independent APPROVAL (2026-11-08).**
  Four rounds of non-author review converged to APPROVED. Final gates: shape-
  free analytic global-pooled two-tail box envelope MAX 5.66e-3 BB <= B0-R0
  0.008 (per-stratum own full-census envelope); exact box-corner optimizer
  measured <= analytic on all six strata (max ratio .793, largest measured
  2.84e-3 BB); automated evaluateG3Corner fail-closed node gate (29 G3 node
  tests). Final full verification green: release build RC=0, ctest 91/91,
  npm check RC=0 (141 node tests), proto:check RC=0. B0 ledger 1.06e-3 finite
  turn gap + 5.66e-3 propagated + R0<=2.0e-3 = 8.72e-3 <= B0 0.010.
  Deferred to S3/G5 (not claimed): online one-card R0<=0.002 (G5), the
  mount-time per-node (e_max,e_l2) contract, and six-fixture -> G4 census
  generalization. Commits 7b46071..73250c4 (14 G3 commits), local only.
- **G5 cap-1-vs-cap-0 menu-materiality premise FAIL (2026-11-13, component
  committed 7ea92b3).**
  - Failing gate: the S3 acceptance criterion "cap-1-vs-cap-0 gap UCB <= 0.002
    BB" (this section, G5). Measured seedlessly via the RFC 0046 partition
    walker (cap-1 average policy embedded in the cap-0 tree with zero mass on
    cap-0-only actions, one exact full-menu BR walk, no cap-0 equilibrium);
    the seedless enumeration has no sampling half-width, so this is an exact
    value, not a UCB.
  - Measured evidence: pure menu term (e_embed minus the cap-1 policy's own
    NashConv) over each stratum's deep check-to-river anchor root, 300-iter
    cap-1, Uniform primary arm / BrFavorable upper bound, in BB:
    w1 19.46/152.90; ahigh 19.42/154.05; paired 19.56/154.24; monotone
    20.81/169.76; twotone 19.50/153.76; raised-srp 29.43/188.75. The cap-1
    policy's own NashConv is 0.0098-0.0255 BB everywhere, an order of
    magnitude below the menu term, and the menu term is iteration-stable from
    300 to 2,000 cap-1 iterations (w1 19.46 -> 20.69) while cap-1 NashConv
    converges to 0.0019, so the gap is the action abstraction, not solver
    residual. Overrun is ~10^4x (Uniform) / ~10^5x (BrFavorable) versus
    M = 0.002 BB; cap-0 trees carry 7,824-11,232 action nodes vs 10-12 under
    cap-1 at 198 BB behind. Each stratum's full-leaf MAX includes this anchor,
    so the per-stratum gate verdict is FAIL without the exhaustive batch.
  - Affected milestone: M2. The RFC 0046 production blueprint (cap-1 turn trunk
    + exact per-combo turn-rooted river endgame) assumes the bounded online
    menu is immaterial; the G3 B0 ledger already allocates 8.72e-3 of the
    0.010 BB B0 (turn gap 1.06e-3 + propagated envelope 5.66e-3 + R0 <= 2e-3),
    so a 20+ BB menu term cannot be absorbed as an additional B0 line item -
    the budget would be exceeded by ~3 orders of magnitude.
  - Concrete alternative (requires a new RFC before any production change; not
    authorized inside this bench-only task): (a) PRIMARY - widen the online
    action menu (raise the per-street raise cap toward cap-0 on the binding
    deep roots), re-run this same embedding measurement to locate the smallest
    cap whose pure menu term <= 0.002 BB, and re-certify the G1 timing/RSS/
    node envelope for the larger tree; (b) re-open the M2 quality contract to
    carry an explicit, separately-bounded menu-error term with a larger total
    B0, which changes the certified error guarantee and is therefore a contract
    decision, not an implementation tweak. Option (b) is infeasible under the
    current 0.010 BB B0, so menu widening (a) is the only path that preserves
    the existing quality bar. The bench component, frozen formula, and 0.002 BB
    gate remain in place to score whichever menu the follow-up RFC selects.
  - Resource feasibility of widening (reconnaissance, public AbstractTree only,
    reached-river anchor root; the online re-solve object, not the turn-rooted
    48-way fanout): tree size grows smoothly with the raise cap and stays far
    inside the G1 5M-node / 1 GiB envelope at every cap - cap-1 27 nodes /
    19 KB, cap-2 99 / 77 KB, cap-3 315 / 187 KB, cap-4 957 / 622 KB, cap-5
    2,631 / 1.5 MB, cap-6 5,769 / 3.0 MB, and the full cap-0 menu only 33,693
    nodes / 23.7 MB (identical for the five limped strata; raised-srp is
    smaller, 23,469 / 12.1 MB at cap-0). Memory is therefore NOT the binding
    constraint; the open question is the smallest cap whose embedded pure menu
    term falls to <= 0.002 BB and its CFR convergence/timing cost, which the
    next cap-k embedding sweep must measure before a widening RFC can be
    scoped. Next critical-path work item: parameterize the embedding entry over
    the online cap, run the cap = 1..6 (toward cap-0) e_embed curve on the
    binding anchors, and use the crossing point to draft the menu-widening RFC
    (no production menu or contract change before that RFC is independently
    approved).

## S3 task 1 — online one-card river re-solve envelope (G5 serve point B)

- Probe `rfc0047_online_river` (manual, measurement-only): per unique reachable
  river subgame (frontier dedup, w1=240) x iteration checkpoint, records summed
  NashConv BB (R0 candidate), max per-combo CFR residual |BR-CFV|/pot, wall ms,
  action_nodes, getrusage peak RSS. Seedless.
- w1 stride-20 smoke (~12 subgames x 7 checkpoints, single-thread),
  `s3/online-river-w1-smoke.csv`. Findings:
  1. R0 summed NashConv is REACHABLE: worst across subgames 4.56e-3 BB @1000,
     2.01e-3 @2000, 8.77e-4 @4000, 4.18e-4 @8000 -> b_river ~3000-4000 gives
     comfortable R0<0.002 margin.
  2. G1 TIMING BLOCKS single-threaded: wall is ~28.9 ms/iter (250->7.4s,
     2000->58-71s, 4000->142s); 10s p95 / 12s max cannot hold at the iterations
     R0 needs. The river kernel is single-threaded; its O(kC^2) per-own-combo
     terminal showdown fold (river_solver.cpp terminal_value) is the same
     independent-row shape turn_solver already parallelizes -> S3 task 2 is
     river-kernel fork-join parallelization with a bitwise-identity cross-
     thread check (machine has 56 cores; target >=8-16x).
  3. 1e-9 GATE CLARIFICATION: the RFC 1e-9 root-pot responder gate is NOT the
     CFR per-combo convergence residual. resolver.hpp kCertPotTolerance=1e-9 is
     a certifier NON-REGRESSION inequality BR_s(c) <= locked baseline
     b_s(c) + 1e-9*pot (rfc0046/resolver component). The probe column was
     renamed combo_resid_pot and is a convergence diagnostic only; the 1e-9
     certifier belongs to the S3 certifier-integration task, not this probe.
  4. RSS tiny (peak ~14 MB single subgame), action_nodes <= ~small; G1 RSS/nodes
     envelopes are not at risk; only wall time is.

## S3 task 2 — river kernel fork-join parallelization (G1 timing blocker)

- Extracted the persistent worker Pool into shared `thread_pool.hpp`; turn_solver
  now includes it (local copy removed; turn identity test still 3.6e-16).
- RiverGame owns a Pool; the O(kC^2) per-own-combo terminal showdown fold
  (terminal_value) and equilibrium_deal_matrix run as fork-join parallel_for
  over own-combo rows (serial inner j-reduction) -> bitwise identical across
  thread counts by construction. RiverSolveConfig gains `threads`.
- Native ctest test_threading_bitwise_identical: serial vs 8-thread solve (60
  iters, deal matrix on) must match NashConv, both root CFV vectors, and the
  full deal matrix exactly. PASS.
- online_river probe takes a THREADS arg; a serial-vs-16-thread speedup run on
  w1 (stride 40, to 4000 iters) measures whether parallelization brings the
  R0-feasible iteration count inside the G1 10s p95 envelope. Result pending.
- T2 RESULT (w1 stride-40, `s3/online-river-t1.csv`, `-t16.csv`): 16-thread
  speedup 8.4-9.1x (250 12.17s->1.16s, 1000 36.15->4.28s, 2000 72.15->8.63s,
  4000 162.9->17.88s). Amdahl serial fraction ~5.2%; 24/32 threads cap ~11-12x.
  TENSION (16 threads only): 2000 iters fits the 10s p95 (8.63s) but its
  worst sampled subgame R0 is 2.007e-3 (no margin, effectively AT the 0.002
  gate); the R0-margin point 4000 iters (8.77e-4) takes 17.9s.
- T2 RFC-GROUNDING CORRECTION (warm-start rejected before building): the G1
  gate is explicitly R=30 COLD resolves per stratum (RFC Goals G1) and the
  Non-Goals forbid relaxing the frozen 10s envelope. The 2026-11-04 S1 infoset
  note reconciles the 88,642/116,748 figure in the G1 text: that is the OFFLINE
  per-card aggregate across all 9/11 turn lines, "never resident online"; the
  actual online object is the at-most-12-action-node x 1081 = 14,053-infoset
  reached subtree (this probe measures exactly that: action_nodes 0/10/12).
  RFC design is therefore a COLD exact solve of the small subtree via tree
  size + parallelism, NOT a warm start. Warm-start would (a) not be credited by
  the cold-resolve timing gate and (b) require the oracle to ship per-combo
  river average-strategy tables, which is outside RFC 0047's turn-CV oracle and
  is an architecture change needing a new RFC. Only 16 of 56 cores were used;
  a 24/32/48/56-thread sweep (with 2500/3000/3500-iter operating points) decides
  cold-start feasibility before any heavier change.
- T2 KERNEL NOTE: cost is dominated by one kC^2=1.17M showdown matvec per
  showdown leaf per traversal (tree itself is only ~12 nodes). terminal_value
  opens a SEPARATE fork-join region (condvar barrier + per-task atomics) on
  every leaf call, so the ~5% Amdahl serial floor is largely barrier/regret-
  update overhead. A module-local fusion of the per-leaf parallel regions is the
  fallback if the 56-core sweep still misses 10s; it preserves the serial
  per-row reduction so bitwise identity holds. No shared/contract change.
- T2 THREAD SWEEP (w1 stride-40, 6 sampled subgames, cold cumulative wall ms,
  `s3/online-river-w1-t{24,32,48,56}.csv`; checkpoints now include 2500/3000/
  3500). t=24 FINAL worst: 2000=8,628 / 2500=10,390 / 3000=12,283 / 3500=14,028 /
  4000=16,497 ms. Effective Amdahl serial fraction DRIFTS UP with thread count:
  fit s=0.050 at n=16 (sp 9.11x) but s=0.062 at n=24 (sp 9.87x) - not a fixed
  serial fraction, evidence of per-region scheduling/sync overhead growing with
  threads. Root cause in thread_pool.hpp: every parallel_for region dispatches
  kC=1081 single-row tasks, each with two contended atomic fetch_add ops (next_
  and done_) on shared cache lines, plus a condvar barrier; river opens a region
  per showdown leaf per traversal (~tens of regions/iteration). Extrapolation
  with s~0.06 caps speedup near 13x -> 4000 iters ~12.5-13s even at 56 cores; the
  10s budget tops out near ~3200 iters, where the 12-sample smoke R0 (~1.25e-3)
  leaves no robust margin once the full 240-subgame census worst is counted.
  PLANNED MINIMAL FIX (module-local, bitwise-preserving): chunked dispatch in
  thread_pool.hpp - partition [0,n) into ~4*workers contiguous ranges so atomic
  ops fall ~5x while each row still runs once with its serial inner reduction;
  guarded by the existing turn 3.6e-16 identity and river threading bitwise
  tests. Region fusion is the second step only if chunking is insufficient.
- T2 TWO-CLASS SUBGAME STRUCTURE (t24 full 6-subgame sample, decides the
  operating point). Reached river subgames split by action_node count:
  * nodes=0 (direct showdown): NashConv exactly 0, trivial, ~1.1s/4000.
  * nodes=10 (groups 0,40): R0-BINDING, slow to converge but fewer showdown
    leaves -> shorter wall. Worst NashConv 1.93e-3 @2000, 1.21e-3 @3000,
    8.61e-4 @4000; wall 13.6-14.1s @4000 (t24).
  * nodes=12 (groups 80,120,160): TIMING-BINDING, more showdown leaves -> more
    fork-join regions -> 16.0-16.5s @4000 (t24), but converges easily (worst
    1.06e-3 @2000, 2.58e-4 @4000).
  A single iteration budget must clear R0 on the nodes=10 class AND the 10s
  wall on the nodes=12 class. Sampled worst NashConv rises with sample size
  (stride-20 12-subgame 2.007e-3 @2000 > stride-40 6-subgame 1.93e-3), so the
  full 240-subgame census order statistic will be higher; 2000 iters likely
  breaches 0.002 census-wide, 3000-4000 is the robust range. A stride-1 census
  at the CHOSEN single iteration count (not all checkpoints) is the R0 evidence
  and is queued behind the thread sweep (no CPU oversubscription).
  t24->t32 barely speeds the slowest nodes=12 subgame (its many small regions
  pay the most per-region condvar/atomic cost), confirming more threads alone
  cannot close the 12-node 10s gap; the chunked-dispatch Pool fix targets it.

- T2 POOL DEADLOCK INCIDENT + CHUNKED EPOCH-BARRIER REWRITE (2026-11-09).
  The 48-thread sweep probe (PID 2889514) hung at startup: all 49 threads in Sl
  sleep, 48 workers at 0:00 CPU, main only 18s, /proc/io wchar=8, CSV 0 bytes
  for ~8h; t56 never launched. Killed scan shell 2840971 first (no t56 spawn)
  then the probe; t24/t32 CSVs and tracker evidence preserved.
  ROOT CAUSE (old thread_pool.hpp): workers had NO epoch boundary sync. A
  worker that finished drain but had not yet re-entered work_cv_.wait could see
  the NEXT epoch (main only waited on done_==total_, not on workers parking),
  then claim chunks against a reset next_/stale fn and range - a cross-epoch
  generation race that permanently loses the done count at high region turnover
  (48 threads). Single-index dispatch also paid 2 contended atomic ops per one
  of 1081 rows per region.
  FIX (module-local, thread_pool.hpp only; public parallel_for signature and
  fork-join semantics unchanged): (a) contiguous CHUNK dispatch, chunks =
  min(n, 4*workers), so atomic ops drop from 2n to ~2*chunks (~8*workers),
  every index still runs exactly once with its serial inner reduction; (b) an
  epoch BARRIER counter arrived_ - parallel_for returns only when
  remaining_==0 AND arrived_==worker_count_, so no worker can touch the next
  epoch's fn/range; main thread still helps drain and is excluded from arrived_.
  Bitwise guards GREEN: test_rfc0047_river_solver (threads 1 vs 8 exact) RC=0;
  test_rfc0047_turn_solver identity diff=3.608e-16 (unchanged), OK RC=0.
  Same-caliber chunked sweep (w1 stride-40 to 4000, t24/32/48/56,
  *-chunk.csv) re-measures wall and must reproduce the baseline NashConv column
  bit-for-bit (deterministic); t48/t56 additionally confirm the hang is gone.

- T2 CHUNKED-POOL SAME-CALIBER RESULT (2026-11-09, w1 stride-40, 6 subgames,
  *-chunk.csv vs baseline; NashConv/pot/resid bitwise-identical on ALL 48 rows
  at every thread count, RSS ~65 MiB << 1 GiB).
  * t=24 (within one NUMA node, CPUs 0-27): chunk is 1.056-1.104x faster across
    all checkpoints (4000: 16,497 -> 15,202 ms; 3000: 12,357 -> 11,266; 2000:
    8,628 -> 7,972). This is the socket-local SWEET SPOT.
  * t=32 (spans both sockets, >28 cores): long runs ~1.00x (neutral after
    amortization); short runs (250-1000) show a one-time first-epoch barrier +
    cross-NUMA wakeup penalty (0.45-0.56x on the ~1-2s runs), sensitive to
    instantaneous tenant contention.
  * t=48/56: REGRESS (4000: 24.1s / 28.8s). Machine is 2 sockets x 28 physical
    cores, NO hyperthreading, dual NUMA (node0 0-27 / node1 28-55); the a0_
    showdown matrix and scratch are allocated socket-local, so >28 threads pay
    remote-memory latency on every matvec, plus oversubscription. A separate
    tenant's `zomc` job (14+ processes at 100% CPU across both sockets, load
    avg 18-21) and a Loom-repo agent further contend; absolute t48/t56 numbers
    are contaminated and these configs are non-viable regardless.
  VERDICT: chunked dispatch is NECESSARY (fixes the cross-epoch deadlock; +8-10%
  at the viable socket-local point; no viable-config regression) but NOT
  SUFFICIENT for G1. Even the best config (t24) cannot run the R0-robust counts
  inside 10s: 2000=7.97s (6-sample R0 1.93e-3, no census margin), 3000=11.27s,
  4000=15.2s. R0 curve on the 6-sample passes >=2000 (3000=1.21e-3, 4000=8.61e-4)
  but the full 240-subgame census order statistic is still pending and will
  raise the worst. Next lever is river_solver REGION FUSION (one fork-join per
  traversal batching all showdown-leaf matvecs instead of one barrier per leaf)
  to remove the strict-barrier straggler cost and lift parallel efficiency
  toward the ~16-24x needed; module-local, per-row serial reduction retained so
  bitwise identity holds. BLOCKER for absolute G1 timing: the frozen-machine
  R=30 distribution cannot be validly measured while tenant zomc load holds
  ~18-21 cores; timing evidence must wait for a quiescent box. No warm-start,
  no CFR-kernel change, nothing pushed.

- T2 REGION FUSION + CONTROLLED-WINDOW NOTE (2026-11-10). Implemented river
  region fusion (river_solver.cpp only): each CFR traversal is now explicit
  reach-propagation -> ONE fused fork-join over all showdown-leaf matvecs ->
  bottom-up RM+/linear-average backprop; old recursive cfr() deleted; eval()
  retained (its terminal_value made serial; only post-solve walks use it).
  Public solver/Pool interfaces unchanged; per-row serial reductions and exact
  operation order preserved for bitwise identity. Clean idle chunk baseline
  captured at load ~1 (*-chunk-idle.csv): t24 4000=14,857ms / 3000=11,144 /
  2000=7,433; t32 idle is slower than t24 (16,445 @4000), confirming the
  cross-NUMA penalty persists on an idle box and t24 (one socket) is the sweet
  spot. Required fusion gain to clear 10s: >=1.49x at 4000 or >=1.11x at 3000.
  Mid-sweep the co-tenant zomc job restarted (7+ procs at 100% CPU plus
  blender/python; load back to ~32), so t48/t56 idle-baseline rows and the
  fusion timing A/B are annotated with load; the clean chunk-idle t24/t32
  anchor is preserved. Bitwise tests are load-independent and gate first.

- T2 FUSION A/B RESULT (2026-11-10; fusion vs chunk-idle baseline; NashConv
  bitwise-identical, re-verified at probe level). Bitwise gates GREEN:
  test_rfc0047_river_solver all checks RC=0; test_rfc0047_turn_solver identity
  diff=3.608e-16 (identical), OK RC=0.
  FUSION FLIPS THE SWEET SPOT. With one barrier per traversal instead of one
  per showdown leaf, cross-NUMA per-barrier cost becomes negligible and each
  fused row task does S matvecs reusing one a0_ row (cache-friendly), so more
  cores now help: fusion t32 is ~17% FASTER than fusion t24 (ratio 0.83), the
  reverse of the chunk kernel. Loaded (~9-14 busy cores/28) fusion worst wall:
  * t24: 3000=8,762 / 3500=10,203 / 4000=11,704 ms (1.26-1.31x vs chunk-IDLE).
  * t32: 2000=4,928 / 3000=7,246 / 3500=8,447 / 4000=9,727 ms.
  So even under contention t32-fusion puts 4000 iters (6-sample R0 8.61e-4,
  comfortable margin) at ~9.73s and 3500 (R0 1.01e-3) at 8.45s. These are
  loaded LOWER bounds, not the G1 evidence: authoritative p95 needs the idle
  R=30 (stride-8, 30 subgames, HF type-1 nearest-rank = 29th ordered) run,
  followed by the full 240-subgame R0 census in the same idle window.

- T2 FUSION R=30 MILESTONE (2026-11-10, `s3/online-river-w1-r30-t48-fusion.csv`,
  30 stride-8 subgames x 8 cold-resolve checkpoints, t48 fusion; HF type-1
  nearest-rank p50=15th / p95=29th / max=30th). RUN UNDER LOAD (load 8->22,
  co-tenant zomc returned mid-run), so timing is a CONSERVATIVE LOWER-HEADROOM
  bound, not the clean G1 evidence; the idle re-run is still required.
  TIMING (ms), all under 10s p95 / 12s max even loaded:
    iter   p50    p95    max
    2000  4171   4273   4288
    2500  5167   5267   5307
    3000  6135   6240   6360
    3500  7139   7318   7339
    4000  8115   8232   8263
  R0 (30-subgame WORST, stronger than the 6-sample): 2000=2.2478e-03 FAIL
  (order statistic breaches 0.002, as predicted), 2500=1.9142e-03,
  3000=1.7829e-03, 3500=1.4949e-03, 4000=1.2252e-03 PASS. R0 binds on the
  nodes=10 class (4000: 1.225e-3); nodes=12 is timing-binding (max 8,263ms)
  but easy to converge (R0 5.72e-4); nodes=0 R0=0. RSS peak 297.7 MiB << 1 GiB;
  action_nodes max 12 << 5M.
  OPERATING POINT: 4000 iters @ t48 fusion clears BOTH gates with margin even
  under contention (R0 1.23e-3 vs 0.002; p95 8.23s/max 8.26s vs 10s/12s). t32 is
  the single-socket fallback. Remaining for G1/G5 closure: (1) clean IDLE R=30
  re-run (wait for quiescent box) for formal p95; (2) full 240-subgame R0
  census at 4000 iters in the same idle window (R30->R240 will raise worst,
  expect still <0.002 given the 0.77e-3 headroom); (3) local commit with CMake
  hunk isolation; nothing pushed; no warm-start/CFR-kernel change.

- T2 CLEAN IDLE R=30 + LOCAL COMMIT (2026-11-10). Isolated S3 commit 1d768f8
  (7 files, CMake +6 lines only via single-hunk index patch; 9 cross-RFC CMake
  hunks and all other dirty files excluded; not pushed). Bitwise gates re-green
  pre-commit (river exact; turn identity 3.608e-16 unchanged).
  CLEAN R=30 (stride-8, 30 subgames, t48 fusion, idle box, HF type-1 nearest-
  rank p50=15th/p95=29th/max=30th; NashConv byte-identical to the loaded run,
  confirming determinism). Wall ms:
    iter   p50    p95    max    mean | worst R0 (30)
    2000  4167  4999  9723   3695  | 2.2478e-3 FAIL
    3000  6137  7287  7410   5136  | 1.7829e-3 PASS
    3500  7145  8556  8559   5959  | 1.4949e-3 PASS
    4000  8148  9748  9792   6801  | 1.2252e-3 PASS
  FAT-TAIL FINDING: idle p95/max are HIGHER than the earlier loaded run (4000:
  9748 vs 8232) - not load, but transient OS scheduler jitter. The 12-node wall
  is bimodal: a tight ~8.3s steady cluster plus occasional ~9.7s stragglers; the
  straggler subgame changes between checkpoints (group 48 spikes at 2000,
  groups 176/184 at 4000), so it is not subgame-specific. CV ~0.38. The fused
  single barrier per traversal (8000 barriers over a 4000-iter solve) means one
  descheduled thread stalls all 48 workers; on a shared box this inflates the
  tail. G1 as defined still PASSES at 4000 (p95 9748 < 10000, max 9792 < 12000)
  but with only ~2.5% p95 headroom. 3500 iters has robust margin on BOTH gates
  (p95 8556 / R0 1.49e-3) and is the fallback operating point; the full 240-
  subgame census (running) supplies both the R240 R0 worst and a 240-sample
  tail to decide 3500 vs 4000 and whether thread pinning / a lower thread count
  is needed for tail stability. RSS 297.7 MiB, max action_nodes 12.

- T2 R240 CENSUS PAUSED ON TENANT CONTENTION (2026-11-10, per D-127 >8 rule).
  14/240 subgames completed before co-tenant zomc (15+ procs) + blender pushed
  load to ~32 (oversubscription: 48 probe threads + ~16 tenant cores on 56).
  Probe stopped; 14 rows preserved as *-PARTIAL-LOAD.csv with timing quarantined
  and R0 retained (NashConv is deterministic and load-independent, so the R0
  values are valid; only wall_ms is contaminated). Partial R0 worst rises with
  sample size as the order statistic predicts: 8.61e-4 -> 9.72e-4 -> 1.1556e-3
  across the 14 subgames (all nodes=10, the R0-binding class). This is consistent
  with the R=30 worst 1.2252e-3 but does NOT establish the R240 bound; the full
  stride-1 census must finish in a clean window to certify worst R0 < 0.002.
  NEXT-IDLE-WINDOW ACTIONS: (1) re-run full R240 at 4000 (single-checkpoint arg
  added, ~30 min); (2) re-measure a clean tail and choose the operating point -
  candidates to stabilize the fat tail are 3500 iters (p95 8.56s / R0 1.49e-3,
  margin on both), a single-socket t24 run, or CPU affinity to cut migration;
  (3) only then finalize G1 p95. Nothing pushed; commit 1d768f8 is the sole S3
  commit and stays local.

- T2 R240 RESUMABLE CHECKPOINT ORCHESTRATOR (2026-11-11, D-127 shard). The 21.5h
  premise of one long idle window was falsified (136 starts, 0 completes, median
  probe life ~80s, longest 260s; rm-restarts only ever covered groups 0-37).
  Replaced with /tmp/bigshark-orch/s3_resume.sh (local tooling, no solver/R0/
  shared-area change):
  * Durable store s3/r240-done.csv, one row per completed group; 137 salvaged
    PARTIALs deduped into groups 0-37 (repeats bit-identical -> safe), archived
    under s3/salvaged-partials/. Probe gained GROUP_START/GROUP_END window args.
  * BATCH=8 sized from measured ~6.7s median / 7.5s p95 per-group wall => ~56-60s
    clean, inside the ~80s median survival window.
  * Monotonic: always resume at smallest missing group index; salvage() appends
    only groups not already in the store; merged result equals one stride-1 run
    (NashConv deterministic). load>8 (5-min avg) watchdog unchanged: kill,
    salvage fflush'd rows, wait, resume.
  RESUME SELF-CHECK PASS: static next_group=38 (skips 0-37); dynamic first batch
  [38,46) completed clean in ~60s, store 38->46 contiguous no gaps; a later
  load>8 watchdog interrupt salvaged +1 partial group correctly (6 watchdog
  events handled, no lost/duplicate groups). Running to 240; interim worst R0
  over groups 0-54 = 1.2252e-3 PASS. Final 240-group worst R0 recorded on
  completion in s3/r240-summary.txt.

- T2 R240 FULL CENSUS CLOSED (2026-11-11, s3/r240-done.csv + r240-summary.txt).
  All 240 unique reached river subgames (stride-1, 4000 iters, t48 fusion,
  single-checkpoint): 240 rows / 240 unique groups / 0 missing / 0 duplicate,
  contiguous 0..239. Worst summed NashConv R0 = 1.2252e-3 BB (group 29),
  GATE 0.002 PASS with 0.7748e-3 (~39%) headroom. The R240 worst equals the
  earlier R=30 worst, so the 30-sample had already captured the global binding
  subgame. Determinism cross-validated: the 30 stride-8 groups match the clean
  R30 run bit-for-bit in a separate process invocation. Resource envelope over
  all 240: peak RSS 56.2 MiB << 1 GiB, max action_nodes 12 << 5M.
  Run endured a contended box (85 min wall, 78 load>8 watchdog pauses, only 5
  fully clean batches, 83 salvage events) yet lost no progress because each
  completed group is checkpointed; 0/240 completed rows exceeded 10s, but this
  mixed-condition wall is NOT used to certify p95. Timing/fat-tail operating-
  point calibration (3500 vs 4000, t24 vs t48, taskset pinning) still needs a
  contiguous idle window; existing clean R30 already gives 3500-iter p95=8.56s
  (R0 1.49e-3) vs 4000-iter p95=9.75s (R0 1.2252e-3).

## S3 task 3 — native G5 serve-point-B certified river entry (1e-9 + fail-closed)

- Goal (D-136 next native gate): a real in-process TYPED call entry that runs
  the exact one-card river re-solve and certifies the per-combo responder
  non-regression inequality BR_s(c) <= locked b_s(c) + 1e-9*root_pot on BOTH
  seats, returning Certified only on full pass and failing closed otherwise
  (never promoting an uncertified strategy). "Online" = in-runtime call, not
  deployment. Native CTest, minimal fixture RED then GREEN.
- Owning module: engine/benchmarks/rfc0047 (bigshark_rfc0047_bench), the same
  Stage-1 scope as every RFC 0047 component. Does NOT cross the rfc0047 link
  guard into bigshark_resolver; composition-root promotion is a separate later
  gate needing its own authorization (no new product direction here).
- Certification mirrors resolver.hpp MarginRecord: slack = BR - baseline -
  tol*pot, pass iff slack <= 0. BR is the kernel's independently-walked
  eval(br=true) value (reads only the average strategy, never regret/sum
  tables), the rfc0047 analogue of the independently-written certifier.
  Production tolerance constant kServeCertPotTolerance = 1e-9 (the deliberate
  predeclared kill gate; the 1e-6 instrument tolerance must NOT be the default).
- New files: river_serve.hpp/.cpp + tests/test_rfc0047_river_serve.cpp. CMake
  adds one lib-source hunk and one test-target hunk, isolated from the 9
  cross-RFC hunks. Fixtures: invalid input; certified against own locked BR;
  fail-closed on a tightened baseline; 1e-9-vs-1e-6 binding-gate proof;
  bitwise determinism; action_nodes <= 12 envelope.
- Remaining G5 after this: turn serve point A, cap-1/cap-0 UCB <= 0.002,
  six-strata Gate-2 re-test, G6 dual reporting, composition-root promotion.

- T3 G5 SERVE-POINT-B NATIVE ENTRY CLOSED (2026-11-12, commit 2968640).
  New typed in-process entry bs::rfc0047::serve_river_certified
  (river_serve.hpp/.cpp, bigshark_rfc0047_bench; stays inside the rfc0047 link
  guard, no bigshark_resolver change). Contract: exact seedless river re-solve
  then per-combo responder non-regression BR_s(c) <= b_s(c) + tol*root_pot on
  BOTH seats, BR from the independent eval(br=true) walk (no CFR-table reads),
  slack form identical to resolver.hpp MarginRecord. Production tolerance
  kServeCertPotTolerance=1e-9 (the predeclared kill gate; 1e-6 is an explicit
  instrument-only override). Certified only on full pass; any violation ->
  CertificationRejected with margin records (never promoted from approximate);
  bad baseline size/negative tolerance/non-positive pot -> InvalidInput.
  RED-first native CTest test_rfc0047_river_serve (5 fixtures): invalid input;
  own-BR certified + bitwise determinism; tightened-baseline fail-closed;
  1e-9 rejects a 5e-8*pot gap that 1e-6 accepts (binding gate); action_nodes
  <=12 envelope. RED = undefined reference pre-impl; GREEN all checks RC=0.
  Full rfc0047 ctest subset 9/9 PASS (link_isolation, board/tree/oracle codec,
  river_solver bitwise, river_serve, frontier, margin_identity 8.6s, turn
  identity 3.608e-16). CMake change isolated to one lib-source hunk + one
  test-target hunk; the 9 cross-RFC hunks stay uncommitted.
  G5 REMAINING (not claimed here): turn serve point A (-x root with oracle-
  bounded frontier leaves); cap-1/cap-0 menu materiality UCB <= 0.002; six-
  strata Gate-2 re-test (30 cold resolves each); G6 dual reporting; the idle-
  window fat-tail operating-point calibration; and the separately-authorized
  composition-root promotion of this entry into bigshark_resolver.

- T3 INDEPENDENT REVIEW ROUND 1 -> CHANGES REQUESTED (2026-11-12, non-author
  agent). Certification math (slack sign, seat pairing, chip units, independent
  BR walk, binding 1e-9 gate, link guard, determinism, no scope creep) verified
  correct. Four hardening findings, all local (no math/architecture change):
  Major-1 NaN/Inf could fail-open to Certified (NaN compares false in every
  inequality); Major-2 kernel std::invalid_argument on a malformed GameDef
  escaped the documented no-throw API and validation ran after the solve;
  Minor-1 worst_slack seeded at 0 so never reported the true negative margin;
  Minor-2 InvalidInput fixtures covered only baseline size.
  FIXES (working tree, re-review open): isfinite guards on tolerance/pot/both
  baselines/both BR vectors, all validation moved before the solve, kernel
  throw caught and mapped to InvalidInput, worst_slack seeded at -infinity
  (certified fixture now asserts the exact -1e-9*pot margin), three new
  fixtures (non-finite rejection, request bounds, malformed-def no-throw).
  Native test now 8 fixtures, all checks RC=0; link_isolation + river_solver
  bitwise regression 2/2 PASS. Commit deferred until the re-review verdict.

- T3 REVIEW ROUND 2 -> APPROVED (2026-11-12). All four round-1 findings closed
  and one optional residual (tolerance-product overflow) hardened; the reviewer
  re-verified binary-vs-source freshness, the fail-closed exhaustiveness, exact
  -1e-9*pot margin assertion, no new fail-open path, unchanged slack math and
  1e-9 default, link guard, and no scope creep. Verdict APPROVED with no
  blocking/major items. Hardening committed as 2c1bfc6 (3 files, +174/-13;
  native test 5 -> 9 fixtures). Final full rfc0047 ctest on the hardened bytes:
  9/9 PASS (link_isolation, board/tree/oracle-codec, river_solver bitwise
  117s, river_serve 166s, frontier, margin_identity 8.2s, turn identity
  3.608e-16 at 536s). check-docs 490 files pass. Serve-point-B G5 native gate is
  CLOSED and independently approved; only the separately-authorized G5 items in
  the prior entry (turn serve point A, cap-1/cap-0 UCB, six-strata Gate-2, G6,
  fat-tail calibration, composition-root promotion) remain open.

## S3 task 4 — native G5 serve-point-A certified turn entry (1e-9 + B0 + fail-closed)

- Goal (D-140): typed in-process turn serve entry certifying the depth-limited
  turn gadget at its responder root, RFC 0047 G5 lines 158-174. Two independent
  predicates, both required: (A) per-combo responder non-regression
  BR_s(c) <= b_turn_s(c) + 1e-9*root_pot on BOTH seats at the TURN root, with
  b_turn from a SEALED reference (never the same solve as the oracle);
  (B) whole-strategy B0 hybrid NashConv, local bound = finite turn gap + G3
  reach-propagated oracle error (an external BOUNDED input, not self-computed)
  + river R0 term <= B0=0.010 BB, alongside the empirical independent-walker
  hybrid (turn_hybrid_nash_conv). Frontier leaves stay bounded inputs.
- Reuse only: solve_turn_subgame, turn_hybrid_nash_conv, turn_leaf_gap_weights,
  the existing independent evaluate(br=true) walker (newly exported as
  per-combo root BR/CFV, mirroring RiverSolveResult). No new shared abstraction,
  no bigshark_resolver/composition-root change, no public contract change.
- Fail closed: missing/non-finite/out-of-bounds baseline or leaf river-NC,
  missing/negative G3 propagated bound (CoverageMiss), malformed GameDef, or a
  B0 overshoot -> never promoted from approximate. Existing identity <=1e-7,
  reach seat diff <=1e-10, mass <=1+1e-9 stay in test_rfc0047_turn_solver and
  are NOT re-claimed as this new gate.
- New files turn_serve.hpp/.cpp + tests/test_rfc0047_turn_serve.cpp; two
  isolated CMake hunks. Bounded RED->GREEN native fixture; independent review
  in-session. Remaining G5 after: cap-1/cap-0 UCB<=0.002, six-strata Gate-2,
  G6 reporting, fat-tail calibration, composition-root promotion.

- T4 RED/GREEN EVIDENCE BOUNDARY (honest, per D-140 correction). The
  serve_turn_certified entry is new in this milestone and absent from every
  prior commit, so a behavioral RED "on the baseline" is not definable (the
  symbol did not exist); no source was temporarily removed and no compile/link
  failure is presented as a behavioral RED. The first REAL runtime failure was
  an incorrect test assumption, not an entry bug: the B0-threshold fixture
  expected a 40-iteration turn solve to fit B0=0.010 BB, but the measured turn
  gap at short iters is ~0.07 BB (turn-120 ~0.064 / turn-4000 ~5.2e-4 per the
  S2 curve), so the entry correctly REJECTS it. Fixed the fixture to size a
  custom budget to the measured bound for the two-way threshold test and to
  assert the production-default verdict as measured, never assumed. The sealed
  b_turn is a separate solve object supplied externally (the entry cannot
  derive it from its own solve); the independent empirical HybridNashConv is
  reported alongside the local B0 bound (G6 dual). Consolidated the test to one
  shared sealed solve plus 6 entry calls (from ~17 turn solves) to bound ctest
  time. Runtime PASS/FAIL and exact component magnitudes are recorded below.

- T4 POINT-A GREEN + NATIVE EVIDENCE (2026-11-12). Entry serve_turn_certified
  (turn_serve.hpp/.cpp) plus additive TurnSolveResult root per-combo BR/CFV
  exports (turn_solver +41 lines, reusing the independent evaluate walker; no
  existing CFR/hybrid math changed). Native test_rfc0047_turn_serve (5 fixture
  groups, 4-thread bitwise-identical solves) all checks RC=0, wall 444.8s under
  ctest. Observed B0 components on the short 40-iteration solve (printed as
  evidence, not hidden): turn_gap=2.290114e-01, river=2.311124e-01,
  local_bound=4.601238e-01 BB vs B0=1.0e-02 -> production verdict correctly
  REJECT (honest: a short non-converged solve is not B0-certifiable); the
  two-way threshold is proven with a custom budget sized to the measured bound
  (g3=0 passes, g3=2e-6 over the 1e-6 room fails). Responder gate certifies the
  externally supplied sealed root BR at exact -1e-9*pot margin and is
  deterministic; tightened margin fails closed with records; negative G3 ->
  CoverageMiss; NaN/Inf/wrong-size/zero-pot/malformed-def -> InvalidInput
  without throwing. Full rfc0047 ctest 10/10 PASS (link_isolation, river_solver
  bitwise 128.8s, river_serve 178.4s, turn_serve 444.8s, frontier,
  margin_identity 17.6s, turn_solver identity 3.608e-16 at 553.7s). Independent
  non-author review launched in-session; verdict and any hardening recorded on
  return; commit deferred until APPROVED. Remaining G5 unchanged: cap-1/cap-0
  UCB<=0.002, six-strata Gate-2, G6, fat-tail calibration, composition-root.

- T4 INDEPENDENT REVIEW ROUND 1 -> CHANGES REQUESTED (2026-11-12, non-author
  agent; no blocking). Contracts 1-4 math/units/non-self-oracle/link-guard all
  PASS and the evidence-boundary treatment judged honest/acceptable. Findings:
  Major-1 a negative leaf river NashConv (an out-of-range bounded input) was
  only finite-checked, so it could shrink the B0 river term into a false
  Certified; Minor-1 the no-throw header promise was broader than the
  invalid_argument-only catch (resource/overflow errors should propagate);
  Minor-2 exact-span length checks ran after the solve; Minor-3 missing
  negative-NC / tolerance-overflow / b0<=0 fixtures.
  FIXES (no certification math changed): pre-solve non-negativity guard on every
  leaf river NC -> InvalidInput; exact kTurnC/leaves length and tol*pot overflow
  checks moved before the solve; header narrowed to state invalid_argument maps
  to InvalidInput while tree_resource_exhausted/bad_alloc propagate; added the
  three fixtures. Re-review open; commit deferred until APPROVED.

- T4 POINT-A CLOSED + REVIEW ROUND 2 APPROVED (2026-11-12, commit 5f0c409).
  Re-review confirmed all four round-1 findings closed with no new fail-open
  path and zero certification-math change; the negative-river-NC forged-Cert
  exploit is structurally unreachable (pre-solve reject), turn_solver export
  stays +41/-0 additive, link guard and G5 contracts 1-4 hold, evidence
  boundary accepted. Hardened scoped test all checks RC=0 (~6 min, 4-thread
  bitwise-identical; B0 components unchanged). Committed 6 files (turn_serve
  x2, turn_solver additive x2, native test, two isolated CMake hunks); the 9
  cross-RFC CMake hunks and all other dirty files stay uncommitted. G5 serve
  points A and B now both have native in-process certified entries with
  independent approval. REMAINING G5/M2 KRs (not claimed): cap-1/cap-0 menu
  materiality UCB <= 0.002 BB; six-strata Gate-2 re-test (30 cold resolves
  each); G6 dual-reporting wire-up; fat-tail operating-point calibration (idle
  window); production-budget end-to-end Certified convergence evidence (needs
  convergence-depth turn solves, not the short control-flow fixtures here);
  composition-root promotion into bigshark_resolver (separate authorization).

## S3 task 5 — G6 dual quality reporting bench wiring (RFC 0039 D1)

- Source audit first: both serve entries were consumed only by native tests; no
  bench harness produced a G6 record, and no C++ dual-report type existed (real
  gap, no duplicate). Reused vocabulary already in tools/rfc/rfc0047-gates.ts G2
  and rfc0047_oracle_build: within-instrument CERT summed-NashConv bar 0.005 BB,
  exact-hole QUAL bar 0.03 BB; seedless exact kernel has within==exact by
  construction; composed M2 uses the serve-point gate (river R0 0.002 / turn
  B0 0.010).
- Minimal additive layer, no new serve API/math: quality_report.hpp/.cpp
  QualityDualReport + river_quality_report/turn_quality_report pure assemblers
  over the existing RiverServeResult/TurnServeResult (within-instrument NC,
  independent exact/hybrid gap, composed M2 delta, gate, status). Always fully
  populated even on a rejected serve (anti-laundering); non-finite ->
  invalid_argument. Added TurnServeResult::b0_bb so the report records the
  effective budget (custom-budget tests do not leak the production constant).
- Fast native CTest test_rfc0047_quality_report (synthetic results, no solver):
  river within==exact identity and all three bars; turn three distinct measures
  with a non-Certified anti-laundering row; non-finite rejection; constant
  agreement. Manual bench bigshark-rfc0047-g6-report wires a real river serve to
  a per-subgame G6 CSV (both columns every row). Review in-session.

- T5 G6 INDEPENDENT REVIEW -> APPROVED (2026-11-12, non-author; no blocking/major).
  All G6 dual-report contract checks pass: both predicates always populated
  (anti-laundering), bars/units match rfc0047_gates.ts (CERT 0.005/QUAL 0.03/
  R0 0.002/B0 0.010, *_bb only), river within==exact and turn three distinct
  measures, pure assembly with no serve API/math change (only additive
  TurnServeResult::b0_bb), manual probe does not self-oracle (sealed margin
  supplied externally), link guard and scope hold. Reviewer ran full rfc0047
  ctest 11/11 (1091s). Three non-blocking minors fixed: turn_serve.hpp comment
  alignment (clang-format), explicit <string> IWYU in quality_report.cpp, and a
  symmetric river-side non-Certified anti-laundering fixture (now both seats
  cover the rejected-still-populated case). g6 probe CSV scratch git-ignored.
  Post-fix final rfc0047 ctest re-run is the closure gate; commit follows on
  11/11 green with three isolated CMake hunks (lib source, manual g6 target,
  quality CTest), the 9 cross-RFC hunks and other dirty files untouched.

- T5 G6 CLOSED (2026-11-13, commit e95e4aa; 8 files, CMake 3 isolated hunks).
  Post-fix full rfc0047 ctest 11/11 PASS (quality_report 0.00s, river_solver
  124.9s, river_serve 169.4s, turn_serve 351.5s, frontier, margin_identity
  8.4s, turn_solver identity 3.608e-16 at 497.8s, link_isolation). check-docs
  497 files pass. The 9 cross-RFC CMake hunks and all 27 modified / 191
  untracked unrelated files remain uncommitted. Serve points A and B plus G6
  dual reporting now have native in-process entries with independent approval.
  REMAINING G5/M2 KRs: cap-1/cap-0 menu materiality UCB <= 0.002 BB; six-strata
  Gate-2 (30 cold resolves each); fat-tail operating-point calibration;
  production-budget end-to-end Certified convergence (needs convergence-depth
  turn solves); composition-root promotion into bigshark_resolver (separate
  authorization); a turn-side manual G6 probe (turn assembler currently covered
  by the synthetic CTest only).

## S3 task 6 — cap-1 vs cap-0 menu materiality (G5), native source + feasibility

- Semantics from source (engine/include/bs/abstraction.hpp:274): cap = max full
  raises per street; 0 means NO cap (exact full menu = cap-0 reference), 1 is
  the bounded online menu. GameDef/strata already accept cap 0/1/2; the two
  solvers hard-coded identity(1). rfc0046 G3 census established the FLOP-rooted
  cap-0 tree is >100M nodes / 55 GiB (infeasible); rfc0047 is turn/reached-river.
- Additive change only: RiverSolveConfig::action_cap (default 1) threaded into
  RiverGame's ActionAbstraction::identity(cap); all existing call sites keep
  cap 1 so bitwise output is unchanged. test_rfc0047_river_solver still all
  checks pass after the change (default-cap regression green).
- Native tree feasibility (bigshark-rfc0047-cap-materiality treesize, w1):
    turn-rooted cap0 = RESOURCE_EXHAUSTED at the 1 GiB TreeLimits byte cap;
    turn-rooted cap1 = 10,923 nodes;
    reached-river (one five-card board, the actual online re-solve object)
      cap0 = 33,693 total / 11,232 action nodes (FEASIBLE, under the 1M
      infoset / 5M-node envelope), cap1 = 27 nodes / 10 action nodes.
  => only the reached-river cap-0 reference is a candidate; turn-rooted cap0 is
     structurally unavailable, consistent with RFC 0046.
- Solve check (w1 first reached subgame, 100 iters t16, ~9.4 min wall): cap0
  NC=0.4900 BB, cap1 NC=0.0862 BB — both far from converged at 100 iters; the
  cap-0 exact reference needs a convergence curve before any 0.002 BB claim
  (running). Action has default operator== (heads_up.hpp), so a public action-
  path key for cap1->cap0 policy embedding is constructible.
- Method note (RFC 0046 Gate 3 / partition_walker, read for pattern only, no
  dependency on the untracked rfc0046 module): the binding measure is the cap-1
  policy EMBEDDED in the cap-0 game (cap-0-only branches take zero probability,
  exact BR still allowed) -> its exploitability minus the cap-0 equilibrium's
  own NashConv, UCB <= 0.002 BB. Comparing the two separate equilibria's own
  NashConv is NOT the gate (both converge toward zero trivially). The
  embedding-BR evaluator is the next native component; no cap result is being
  hard-claimed before convergence + that walk exist.

- T6 EMBEDDING DESIGN (pre-implementation review gate). cap0 equilibrium cost
  curve (w1 reached river): NC 0.655/0.490/0.211 BB at 50/100/200 iters over
  11,232 action nodes (~T^-0.55); reaching 0.002 needs thousands of iters/hours,
  so a converged cap0 equilibrium cannot be a prerequisite. Adopt RFC 0046
  partition-walker method instead: embed cap1 average policy into the cap0 tree
  (public action-path map via Action operator==; cap0-only raises get prob 0),
  then ONE exact BR traversal over the full cap0 menu (no cap0 CFR). Proposed
  conservative binding e_embed = summed-seat (BR-V) in cap0 game; since
  e_embed = menu_error + cap1 residual and the RFC-literal gap is e_embed -
  NC_cap0 with NC_cap0 >= 0, certifying e_embed <= 0.002 suffices without a
  cap0 equilibrium. Design in cap_embedding.hpp; independent math/architecture
  review opened before implementation. No duplicate jobs: cap curve processes
  confirmed zero residue; curve kept as cost evidence only.

- T6 PRE-IMPL MATH/ARCH REVIEW -> CHANGES NEEDED then formula FROZEN (2026-11-13).
  Review CONFIRMED the conservative binding and the single-walk BR approach;
  two spec gaps now closed in cap_embedding.hpp:
  (1) Opponent completion on cap-0-only extension subtrees: primary Uniform
      (rfc0046 precedent, non-anticipatory/combo-independent) + a BrFavorable
      robustness arm that bounds e_embed over ANY completion; unmapped count is
      EV-walk-only (BR entering extensions is expected, not a failure).
  (2) Dedicated lightweight fixed-policy evaluator over the cap-0 AbstractTree
      (board a0_/fold/stake + paired-DFS map + depth scratch, tens of MB); a
      cap-0 RiverGame is forbidden (full regret layout ~1.4 GB, breaks the 1 GiB
      envelope). Mapping is paired DFS from roots (flat Action lookup banned:
      Check/Call/Fold share target_total 0), with ordered-prefix + suffix-all-
      aggressive invariants that throw on violation.
  Frozen gate: e_embed = summed-seat exact (BR-V) of the embedded cap-1 policy
  in the cap-0 game, per-stratum MAX over all reached river roots, <= 0.002 BB;
  seedless so it is an exact value not a UCB; no cap-0 equilibrium / NC_cap0
  needed (subtracting NC_cap0 would be anti-conservative). Formula fixed before
  gate data. Native tests T1-T6 per review (mapping micro-fixture, fail-closed,
  cap0==cap1 normalization identity, distribution/BR invariants, determinism,
  completion/gate direction).

- T6 IMPLEMENTED + native gate green (2026-11-13). Components, all bench-only:
  cap_embedding.{hpp,cpp} (paired-DFS map_cap1_to_cap0; dedicated lightweight
  fixed-policy EmbeddedGame over the cap-0 AbstractTree, no cap-0 RiverGame/CFR;
  measure_cap_materiality / measure_stratum_cap_materiality; diagnostic
  embed_root_values), river_solver additive RiverSolveResult.avg_sigma snapshot
  (build_sigma already ran post-solve; no kernel numeric path changed), probe
  modes embed / embed-gate in rfc0047_cap_materiality.cpp, and
  test_rfc0047_cap_embedding (8 fixtures). Two implementation defects caught in
  RED: (a) arm dispatch hard-coded the BR-favorable max for BOTH completions so
  Uniform == BrFavorable bit-for-bit - fixed so Uniform plays 1/A via the
  kernel-identical snapshot reach scaling inside extension subtrees;
  (b) evaluator correctness proven independently by embed_root_values: the EV
  walk never enters a zero-mass extension, and its raw root values match the
  cap-1 kernel root CFV x 990 to < 1e-6 for every combo/seat (both arms).
  Determinism serial vs t8 bitwise; rfc0047_river_solver stays bitwise green.

- T6 FIRST GATE DATA -> cap-1 menu FAILS the 0.002 BB materiality bar on the
  deep w1 check-to-river root (single root, both arms; NOT yet the per-stratum
  max over all 432-528 leaves). Frozen decomposition e_embed = e_G1 + pure menu
  term, measured:
    300 iters: cap1 NC 0.02395 BB; Uniform e_embed 19.4822 (menu 19.4582);
               BrFavorable e_embed 152.9223 (menu 152.8983).
   2000 iters: cap1 NC 0.00193 BB; Uniform e_embed 20.6878 (menu 20.6859);
               BrFavorable e_embed 156.8507 (menu 156.8488).
  The pure menu term is STABLE as the cap-1 policy converges (e_G1 -> 0.0019),
  so the failure is the menu restriction itself, not solver residual: ~20 BB
  under the non-anticipatory Uniform completion and ~157 BB under the
  BrFavorable upper bound, 4-5 orders of magnitude over M = 0.002 BB with
  11,232 cap-0 vs 10 cap-1 action nodes (198 BB behind, pot 4). Gate was never
  relaxed. Remaining evidence: the full per-stratum MAX batch (all frontier
  leaves; shallower post-bet roots have smaller cap0 trees and may be less
  severe, but this root alone fails by ~10^4x) and independent non-author
  review before the isolated local commit.

- T6 INDEPENDENT REVIEW APPROVED + hardened, committed locally 7ea92b3
  (2026-11-13). Non-author review returned APPROVED with no blocking findings;
  three Low hardening items were applied before commit:
  (1) pair_dfs now throws invalid_argument on a paired node-kind divergence
      (covered by a new turn-rooted vs river-rooted cross-shape fixture);
  (3) the EV walk throws logic_error if it ever reaches an unmapped extension
      node (fail-closed, replacing the prior silent 1/A completion); the
      unmapped_ev_reachable_nodes=0 field is thus runtime-enforced, not merely
      asserted;
  (4) EmbeddedGame borrows the cap-0 AbstractTree built once in prepare_embed
      instead of rebuilding it (probe output is bit-identical after the change).
  Informational items (hand-built micro-game oracle, direct negative
  distribution-throw tests) recorded as future hardening; not required because
  the public bench API does not inject avg_sigma and the EV<->kernel-CFV oracle
  already pins terminal/map correctness. Test count rose to 9 fixtures, all
  green; rfc0047_river_solver bitwise regression green; check-docs green.
  Commit is strictly bench-only (7 files; engine/CMakeLists.txt carries only
  the three isolated cap hunks, the nine cross-RFC hunks and all other
  worktree changes untouched); no upstream, not pushed.

- T6 SIX-STRATUM ANCHOR SCOPE decision. The full per-stratum MAX batch
  (embed-gate over all 432-528 frontier leaves per stratum) is a multi-hour
  offline run whose marginal value is low once the gate is already a decisive
  FAIL: every stratum's MAX includes its own deep check-to-river anchor root,
  and the w1 anchor alone exceeds M by ~10^4x. A proportionate six-stratum
  anchor sweep (one embed probe per stratum, ~300-iter cap1, both arms) is run
  instead to establish the FAIL verdict across every frozen stratum; the
  exhaustive MAX remains available via embed-gate if an exact binding worst
  value is later needed for the M2 ledger.

- T6 SIX-STRATUM ANCHOR SCOPE RESULT -> all six strata FAIL M (2026-11-13).
  One embed probe per frozen stratum (300-iter cap1, both completion arms);
  pure menu term = e_embed - cap1 NashConv, all values in BB, gate M = 0.002:
    stratum                 cap0/cap1 nodes  cap1 NC   Uniform e  BrFav e
    w1                      11232/10         0.02395   19.4822    152.9223
    unpaired-rainbow-ahigh 11232/10         0.02554   19.4486    154.0793
    paired                  11232/10         0.02412   19.5817    154.2606
    monotone                11232/10         0.00975   20.8220    169.7682
    unpaired-twotone        11232/10         0.02360   19.5265    153.7816
    raised-srp               7824/12         0.01437   29.4414    188.7606
  Every stratum exceeds M by roughly 10^4x under Uniform (the primary arm) and
  ~10^5x under the BrFavorable upper bound; the cap-1 policy's own NashConv is
  already an order of magnitude below the menu term everywhere, so the binding
  error is the action abstraction, not solver residual. raised-srp is the worst
  anchor despite the smaller cap-0 tree. Because each stratum's full-leaf MAX
  includes its own anchor root, the per-stratum gate verdict is FAIL without
  needing the exhaustive multi-hour batch. The bounded cap-1 menu therefore
  cannot be certified as menu-immaterial at 100 BB; this is a Stage-1 finding
  for the M2 design (a wider online menu or an explicit menu-error term in the
  B0 ledger), not something the bench component can relax. Component, formula,
  and native gate remain as committed in 7ea92b3; the 0.002 BB bar is unchanged.

- T6 CLOSURE VERIFICATION (2026-11-13). Full rfc0047 ctest suite 12/12 green
  under the release preset (1,260 s total), including rfc0047_link_isolation,
  the rfc0047_river_solver bitwise regression (confirming the additive
  avg_sigma/action_cap changes move no kernel numerics), all five serve/quality
  fixtures, and the new 9-fixture rfc0047_cap_embedding. check-docs green.
  Format applied per-file with clang-format (the repository-wide format target
  would touch the many unrelated cross-RFC worktree changes and is intentionally
  not run). No production resolver, composition root, public include, protocol,
  or persisted format changed; no push (branch has no upstream).

## S3 task 6b — D164 bounded cap-k parameterization and widening curve

- D164 SCOPE (TechLead round 367, handoff msg_d711d8c6cc9c). Bench-only, same
  scope guards as T6: engine/benchmarks/rfc0047 + native tests/CMake + this
  tracker. Parameterize the online raise cap k (identity(k), k >= 1) and measure
  the SAME frozen e_embed(k) curve on a single bounded anchor root under a
  comparable solve config, both completion arms, with online NC / pure menu
  term / nodes / wall / CPU / RSS. Production default stays cap-1; the 0.002 BB
  bar, B0/G1, independent gates, external oracle/ABI/resolver contracts, and the
  CPU/timeout/training budgets are unchanged. Single-instance bounded probes
  only; no full-stratum or 432-528-root blind sweep; a single-root crossing is a
  candidate, never a global MAX/Certified/M2 pass.

- D164 IMPLEMENTED (uncommitted working change). cap_embedding.{hpp,cpp}
  generalized from hard-coded cap-1 to online_cap: map_bounded_to_cap0 (renamed
  from map_cap1_to_cap0), measure_cap_materiality / measure_stratum /
  embed_root_values now take std::uint8_t online_cap; cap 0 is rejected with
  invalid_argument (the cap-0 equilibrium stays out of scope). CapMateriality
  gains online_cap and renames cap1_* -> online_* (bench-only, no production
  field). Probe gains capcurve and per-cap wall/cpu/rss_peak/rss_current
  columns; embed/embed-gate take a CAP positional. Native test expanded to 15
  fixtures (cap-1 AND cap-2 sigma/map/dominance/EV-oracle/determinism, a new
  cap-widening monotonicity fixture c2.online_nodes > c1 and e_embed(c2) <
  e_embed(c1), and the cap-0 fail-closed case); all green (~558 s). Also fixed a
  pre-existing bench-only memory leak so in-process cap sweeps stay bounded:
  RiverGame now frees reg_/sum_/sigma_ in a destructor and build_sigma releases
  the previous snapshot (reg_/sum_ allocated once; no double-free); the
  rfc0047_river_solver bitwise regression stays green, confirming no numeric
  change. No CMake hunk needed (no new files).

- D164 cap1..6 CURVE, w1 deep check-to-river anchor, 300 cap-k iterations,
  t16, Uniform primary arm (e_embed = online NC + pure menu term, BB):
    cap online_nodes online_NC e_embed  pure_menu wall_ms  rss_peak_kb
    1   10           0.02395    19.4822   19.4582    ~40000   30460
    2   34           0.12661    3.4023    3.2757     22054    30460
    3   106          0.09159    0.9970    0.9054     29938    34876
    4   320          0.11082    0.2596    0.1488     53674    63428
    5   878          0.07066    0.1089    0.0383    113493   163608
    6   1924         0.12671    0.1289    0.00217   304047   351316
  BrFavorable stays a large upper bound throughout (cap-6 0.7414 BB). The pure
  menu term falls ~4-6x per added raise and only REACHES the 0.002 bar at cap-6
  (0.00217, 8% over), but cap-6 already violates the G1 online envelope by
  ~30x on wall (304 s for 300 iters vs the 10 s p95) and its online NC is 0.127
  (unconverged; the 300-iter NC floor dominates e_embed for cap >= 3).

- D164 cap-6 1000-ITER CONVERGENCE PROBE -> the 300-iter near-crossing was a
  non-converged transient, NOT a pass (w1 anchor, t16):
    cap-6 iters  online_NC  e_embed  pure_menu  wall_ms   rss_peak_kb
    300          0.12671    0.1289   0.00217    304047    351316
    1000         0.06821    0.0743   0.00610    700438    351236
  As the policy converges the online NC falls but the pure-menu term RISES
  (0.0022 -> 0.0061, now 3x over M): the frozen identity holds for any policy,
  but its two terms re-split as sigma_k moves, and the 300-iter value mixed a
  weak policy's own NC into almost the entire gap. At 1,000 iters cap-6 still
  has e_embed 0.0743 BB (37x over M), an online NC 34x over M converging only at
  ~T^-0.52 (300->1000 gave just 1.86x NC reduction), and a 700 s wall (70x the
  G1 10 s p95) at 351 MB. Extrapolating the T^-0.52 slope, driving the cap-6 NC
  to 0.002 needs order 10^5-10^6 iterations - days of solve - so no bounded cap
  reaches a CERTIFIED e_embed <= 0.002 inside any online-feasible budget on this
  deep root.

- D164 DECISIVE VERDICT (for RFC 0048). Simple uniform menu widening cannot
  satisfy G5 inside G1 at the deep 198-chip (99 BB, big_blind=2) check-to-river
  anchor: the caps whose structural menu restriction approaches M (cap >= 5-6)
  have trees too large to converge within the 10 s/1 GiB/5M-node online
  envelope, while the caps that converge fast enough online (cap 1-2) carry a
  3-19 BB menu error. This is a
  measured quality-vs-latency conflict, not a solver bug (the EV<->kernel-CFV
  oracle is <1e-6 at both cap-1 and cap-2; seedless determinism holds). Options
  for the follow-up RFC, none authorized as a production change by D164:
  (a) move the wider-menu equilibrium OFFLINE (pre-solve/blueprint the deep
      roots instead of online re-solving) - changes the online/ offline split;
  (b) selective/reach-aware widening (widen only high-reach betting nodes) -
      requires a new reach-weighted gate, the current one is exact per-root MAX;
  (c) re-open the M2 quality contract with an explicit bounded menu-error term -
      explicitly NOT chosen, D164 forbids lowering/re-opening the quality bar,
      and B0=0.010 BB cannot absorb a >=0.006 BB term alongside the existing
      8.72e-3 G3+R0 allocation. RFC 0048 must present this evidence and select a
  path under independent approval; the bench component and 0.002 BB bar remain
  unchanged to score it. Single-root results stay candidates, not a global MAX.

- D164 CLOSURE + RFC 0048 ACCEPTED (2026-11-13). Two independent non-author
  reviews, both Approved by distinct agents: the D164 code review
  (parameterization + leak fix; three Low items applied) and the RFC 0048
  approval-agent review (four Low findings F1-F4 all resolved). Local commits
  on the unpushed branch: f74f4e4 (parameterized cap-k measurement + bounded
  leak fix + 15-fixture native test), 1955f4d (RFC 0048 draft), 0cc463c
  (RFC 0048 Accepted decision record). F1 corrected the depth descriptor to
  198 chips = 99 BB (big_blind=2); no BB gate value changes. Full rfc0047
  ctest 12/12 green under release (1,723 s), including rfc0047_link_isolation,
  the rfc0047_river_solver bitwise regression (leak fix numerics-neutral), and
  rfc0047_cap_embedding (576 s, 15 fixtures). check-rfcs (48) and check-docs
  (502) green. No production menu/resolver/contract/artifact change; the 0.002
  BB bar, B0, G1, and all external contracts are unchanged; no CMake hunk was
  needed. The G5 cap-1/cap-0 materiality KR is therefore closed as a measured
  premise FAIL routed to an Accepted direction RFC, not a passed gate. The
  next independent reachable KR is the turn-side manual G6 probe (the turn
  quality assembler currently has only the synthetic CTest, not a real
  serve->dual-report end-to-end run); the remaining M2 certification KRs
  (Gate-2, fat-tail, end-to-end Certified, composition-root promotion) stay
  blocked on a future Path A/B addendum and are not unblocked by D164.

- T7 TURN-SIDE MANUAL G6 PROBE closed (2026-11-13, commit adf5984; independent
  non-author review APPROVED, one Low + two Info all dispositioned). New
  bench-only manual binary bigshark-rfc0047-turn-g6-report (NOT a CTest) drives
  the real frontier -> 240 grouped exact river continuations -> sealed turn
  reference -> external responder margin -> serve_turn_certified ->
  turn_quality_report pipeline, emitting one always-populated 22-column G6 row
  (within-instrument CERT + exact-hand QUAL + composed M2/B0 ledger, no
  laundering). Review Low (nested N x N thread oversubscription) fixed: per-
  group river solves now run serial inside the outer group-worker pool, matching
  the native turn_serve fixture; numerics are thread-count invariant. No
  production/contract change; one isolated CMake hunk; link isolation intact.
  Evidence (w1, sealed margin = same-policy root BR, external G3 input):
    100/100 iters, G3=0:       status CertificationRejected; responder PASS;
      within NC 0.0769 (CERT fail), exact hybrid 0.1172 (QUAL fail),
      local_bound 0.1172 > B0 0.010; 85.7 s; peak RSS 2.95 GB.
    1000/1000 iters, G3=0.00566: responder PASS; within NC 0.00373 (CERT PASS),
      exact hybrid 0.00571 (QUAL PASS), but B0 ledger turn_gap 0.00373 +
      river_term 0.00198 + G3 0.00566 = local_bound 0.01137 > B0 0.010, so the
      composed M2 gate still REJECTS (13.7% over) even though both quality
      predicates pass - the G3 propagated envelope is the dominant term and the
      anti-laundering dual report makes that visible in one row; 711 s;
      2.96 GB. The 1000-iter wall/RSS came from the pre-oversubscription-fix
      binary; the numeric fields are unaffected and a post-fix 60/60 smoke
      re-confirmed the full pipeline (exit 0). The ~2.95 GB RSS is the 240
      retained river deal matrices (~2.24 GB at 1081^2 x 8 B), not a leak.
  REMAINING reachable KR after this: none that is independently completable
  inside the current measurement authorization. Gate-2 (30 cold resolves x six
  strata), fat-tail calibration, end-to-end Certified convergence, and
  composition-root promotion all depend on a future RFC 0048 Path A/B addendum
  (the G5 menu gate is a measured premise FAIL, so a cap-1 end-to-end
  Certified cannot be produced); composition-root promotion additionally
  requires its own explicit authorization.

## RFC 0048 Path A Stage 1 — offline wide-menu / bet-sizing grid feasibility (D176)

- D176 AUTHORIZATION (TechLead round 386, handoff msg_d1fa3d593b63). Bench-only
  reversible spike; no production resolver/composition-root/oracle/API/contract
  and no quality/budget gate change; no push/merge/publish/deploy/spend/delete
  or worker start-stop; keep the 217 cross-RFC dirty items. Bounded single-
  instance fail-fast runs; NO extra training/equilibrium long curve or full-tree
  scan budget. Deliver exact node/bytes and build CPU/RSS/wall plus a feasibility
  upper bound, using EXISTING RFC grid enumeration representative instances.
  In-session self-review + independent math/arch review before implementation.

- STAGE-1 ADDENDUM DESIGN (self-reviewed, pre-implementation). Two independent
  widening levers, both already expressible through the existing
  ActionAbstraction API (no new abstraction invented):
  Lever 1 RAISE CAP: identity(k) for k=1..6 and k=0 (uncapped), default pot
  fractions (bets 1/3,3/4,3/2; raises 1/2,1) with MinAndCap cover seeds.
  Lever 2 BET-SIZING GRID: ActionAbstraction::declared(schedule, MinAndCap,
  cap=1) over three representative schedules built only from existing Fraction/
  StreetSizes primitives - coarse (bets {1/2}, raises {1/1}; the stage6
  coarse_action precedent), identity (default schedule), wide (bets
  {1/4,1/2,3/4,1,3/2}, raises {1/2,1,2}).
  Roots: w1 turn-rooted (make_stratum_def; the offline trunk, known to EXHAUST
  the 1 GiB TreeLimits at cap-0) and one w1 reached-river frontier leaf (the
  per-card offline/online object; cap-0 is only 33,693 nodes / 23.7 MB).
  Per instance record total/action nodes (turn vs river split),
  accounted_bytes, dense infoset estimate with the SAME convention as
  tree_budget (river action*1081+1081; turn action*1128+1128 via
  live_combo_count), build wall/cpu ms and getrusage peak/current RSS, and a
  typed feasibility verdict against TreeLimits (2,000,000 nodes / 1 GiB) and
  the 1,000,000-infoset artifact envelope. tree_resource_exhausted is caught
  and emitted as a BOUNDARY row so one oversized config never aborts the sweep
  (negative size/cost conclusions are first-class outputs).
  NEW bench probe rfc0047_patha_grid_budget (manual, NOT a CTest) + small native
  test test_rfc0047_patha_grid_budget (tree builds only, no solve, fast):
  (T1) cross-construction size identity identity(1) == declared(default,
  MinAndCap, 1); (T2) determinism - repeat build gives identical nodes/bytes;
  (T3) grid ordering coarse <= identity <= wide on the same root/cap;
  (T4) cap monotonicity - identity(k) nodes non-decreasing in k, cap-0 largest;
  (T5) resource verdict - a known small instance is under both envelopes and an
  exhausted config yields tree_resource_exhausted not a crash; (T6) fail-close -
  an invalid Fraction (zero denominator) throws at abstraction construction.
  No equilibrium is run in stage 1; build cost is the tree-construction cost,
  which is the actual artifact-envelope feasibility gate.

- STAGE-1 INDEPENDENT DESIGN REVIEW -> APPROVED with required conditions
  (2026-11-13, agent a3f1a288ff083710b; no redesign). Dispositions absorbed into
  the implementation:
  F1 Major: make_stratum_def/make_river_root_def reject cap>2, so each GameDef
    is built ONCE with fixed literal cap 1 and the real cap k is carried only by
    ActionAbstraction::identity(k)/declared(...,k) (the D164 pattern); the
    GameDef does not gate tree building.
  F2: grid node ordering uses WEAK <= only (fraction sets are NOT nested;
    monotone-clamp refinement, not subset), with >=1 strict inequality per
    fixture so it cannot pass vacuously.
  F3: only tree_resource_exhausted is caught -> BOUNDARY row recording what();
    invalid_argument/bad_alloc/overflow_error propagate (config/env/arithmetic
    faults must never masquerade as an envelope boundary).
  F4: the ctest forces exhaustion deterministically with a tiny
    TreeLimits{max_nodes=100}, never a 1 GiB build (precedent
    test_abstract_tree_fidelity).
  F5: T1 compares tree_shape_digest + size + accounted_bytes, NOT action_id
    (identity vs declared ids legitimately differ while trees are identical).
  F8: add mean/max actions-per-action-node columns; the 1M-infoset figure is
    documented as a structural proxy (menu-width-blind), with turn/river
    infoset components reported separately and the identity(1) row pinned to
    the existing measure_tree_budget w1 convention.
  Plus: SizeSchedule is default-constructed then only the Flop/Turn/River
  streets overridden (reduced() eagerly validates all four); ru_maxrss is a
  process high-water mark, so the feasibility verdict uses deterministic
  nodes/bytes/infosets only; raised-srp pot-8 relative inflation is a recorded
  unmeasured residual. Matrix bounded at 20 solve-free builds.

- STAGE-1 CENSUS RESULT (D176, w1, solve-free, bigshark-rfc0047-patha-grid-
  budget; probe rc=0, 20 builds fail-fast). Structural counts are the
  deterministic gate; wall/RSS are environmental. F8 convention pin: turn
  infosets 12,408 matches measure_tree_budget(w1,1) exactly; the whole-trunk
  conservative sum 4,268,305 reproduces RFC 0046's documented 4.27-5.62M
  full-range footprint (different aggregate from tree_budget's per-card 89,723).
  >> D179 UNIT-CORRECTION (independent review aee31581abed02b2f): the "OVER 1M"
     infoset labels in the table and verdicts (1)/(3) below are RETRACTED. The
     4.27M/1.15M/7.07M figures are U3 whole-trunk BUILD grand totals (all 48
     river fanouts, never co-resident) and must not be compared to the online
     ResolveLimits 1,000,000-infoset peak, which gates U1 = turn trunk + ONE
     reached subtree = 26,461 infosets at cap1 (well inside 1M). U3 is still
     bounded by the TreeLimits node/byte build caps. The byte-cap and the
     reached-river cap6/cap0 over-1M rows below remain valid (correct units).
     Read the table's infoset columns as U3 build totals, not envelope verdicts.
  RAISE-CAP lever, TURN-ROOTED full trunk (nodes / MB / U3 build infosets):
    cap1  10,923 nodes,  5.98 MB,  4.27M U3 (U1 online resident only 26,461)
    cap2  153,267,      95.2 MB,  56.4M U3
    cap3  1,152,843,   760 MB,    419M U3 (near byte cap)
    cap4  RESOURCE_EXHAUSTED byte cap (1 GiB); cap5/cap6/cap0 same boundary,
          each fail-fast ~1.4 s then sweep continues, peak RSS 758 MB.
  RAISE-CAP lever, REACHED-RIVER single subtree (the online/ per-card object,
  here U1 == the subtree's own infosets):
    cap1..5 fit 1 GiB AND 1M infoset (cap5 2,631 nodes / 1.5 MB / 0.95M);
    cap6 5,769 nodes / 3.0 MB / 2.08M infosets OVER 1M;
    cap0 33,693 / 23.7 MB / 12.1M infosets OVER 1M.
  SIZING-GRID lever at cap=1, turn trunk (nodes / MB / U3 build infosets):
    coarse   2,847 / 1.49 MB / 1.15M U3 (NOT an envelope overrun post-D179)
    identity 10,923 / 5.98 MB / 4.27M U3
    wide     18,417 / 11.8 MB / 7.07M U3 (1.66x identity; still byte-feasible)
    reached-river grids all tiny (7.6K-14K U1 infosets), all envelopes pass.
  FEASIBILITY VERDICTS (revised under the corrected units):
  (1) The 1 GiB TreeLimits BYTE cap binds the full TURN trunk build at raise
      cap >= 4 (valid U3 build boundary). The prior "1M infoset exceeded at
      cap-1 full trunk" claim is retracted: online U1 is only 26,461. A full
      turn-rooted equilibrium is still not the chosen design, but on build
      cost/depth grounds, not the (misapplied) online infoset gate.
  (2) On the reached-river object only caps <= 5 fit 1M infoset, but D164
      already showed caps <= 5 miss the 0.002 quality bar and cap-6 (which
      approaches it) exceeds 1M even on the single subtree; the envelope and the
      quality bar therefore do not intersect for raise-cap widening.
  (3) Bet-sizing widening is structurally cheaper than raise-cap widening, BUT
      the D179 prefix canary proved coarse/wide declared grids are not ordered
      prefixes of identity(0), so their e_embed against identity(0) is undefined
      under the frozen formula (map fails closed). It remains a lead requiring a
      new union-menu reference formula (separate addendum), not a measured pass.
      Build cost is negligible for every feasible instance (turn cap1 7 ms;
      reached-river
      sub-30 ms); only boundary builds reach ~1.4 s / 758 MB.
  Residual (bounded, not expanded): raised-srp pot-8 relative grid inflation
  unmeasured; infoset count is menu-width-blind (mean/max actions/node columns
  added for stage-2 byte costing); no equilibrium was run by design.

## RFC 0048 Path A Stage 2 — in-budget candidate metering fix and quality chain (D179)

- D179 AUTHORIZATION (TechLead round 389, handoff msg_be3bd1ab78d9). Bench-only;
  keep cap1 / e_embed<=0.002 / B0=0.010 / G1 and all existing resource/time
  budgets; reuse only the already-authorized consumable offline measurement
  budget and existing bounded native entries; single-instance fail-fast; no new
  training/equilibrium/full-tree-scan budget, no parallel duplicate curves, no
  gate lowering or synthetic/old-menu pass; protect the 217 cross-RFC dirty
  items; no push/merge/publish/deploy/delete/worker start-stop.

- STAGE-2 ADDENDUM DESIGN (self-reviewed, pre-implementation). Source audit of
  the actual budget/count definitions found a stage-1 UNIT CONFLATION that must
  be corrected before any candidate is called feasible:
  Unit U1 ONLINE RESIDENT (gated by ResolveLimits max_information_sets=1e6,
    engine/include/bs/resolver.hpp:185; ContinuationBudget mirrors it for ONE
    inner nested solve): the turn trunk plus ONE reached river subtree, i.e.
    tree_budget's conservative_online_sum = turn_infosets +
    reached_river_infosets_max (w1 identity cap1 = 12,408 + 14,053 = 26,461).
  Unit U2 OFFLINE BSNCV ARTIFACT ROWS (cv_oracle.hpp OracleRow; builder emits
    frontier_leaves x 2 seats x 1081 combos, one CV double + error per row;
    rfc0047_oracle_build.cpp:294-313): a per-combo value table whose row count
    is menu-WIDTH-independent (width changes only the number of turn betting
    lines/frontier leaves, not the per-combo row). w1 cap1 = 432x2x1081.
  Unit U3 WHOLE-TRUNK BUILD GRAND TOTAL (all 48 river fanouts summed): never
    resident together; RFC 0047 solves per-river, so it is not an online-peak
    envelope unit but IS bounded by the build TreeLimits node/byte caps. The
    stage-1 patha conservative_infoset_sum (4.27M at cap1) is U3 and must not be
    compared to the 1M online gate.
  Consequence: against the correct U1 unit every turn trunk in the stage-1
  matrix is far inside 1M (the online river subtree is the binding part, and
  D164's reached-river cap6=2.08M over-1M conclusion already uses the correct
  single-subtree unit and stands). The stage-1 "coarse turn trunk 1.15M over
  1M" verdict was a U3-vs-U1 mislabel and is withdrawn.
  IMPLEMENTATION QUEUE (bounded, directed; no 20-instance rerun):
  Q1 Correct the meter: patha_grid_budget emits U1 (turn + reached-max), U2
     (leaf x 2 x 1081 row estimate), and U3 (labeled build grand total) as
     distinct fields, each mapped to its own envelope; boundary rows keep
     bytes=0 and are structurally incapable of a feasible verdict (native
     fixture pins this so a placeholder can never read as a success).
  Q2 Native fixtures (no solve): meter-caliber identity against
     measure_tree_budget(w1,1) for U1; determinism; grid weak ordering with a
     strict step; tight-limit resource rejection; U1/U2/U3 non-equivalence.
  Q3 Generalize the embedding quality entry to accept an explicit
     ActionAbstraction (not just identity(k)), then run bounded single-instance
     e_embed on the REACHED-RIVER coarse/identity/wide grid candidates at cap1
     under the same D164 iteration budget - the one promising combination
     (sizing grid) never quality-measured. Report e_embed/pure-menu/NC honestly;
     a reached-river grid is <15K U1 infosets so it is in-budget to measure.
  STOP/REPORT conditions: if a candidate cannot reach a runnable in-budget
  quality entry, emit exact cost/gap and release; never claim M2 from counts,
  normal probe exit, or near-1M proximity. New-candidate all-root/production-G1
  results are explicitly UNVALIDATED unless measured.

- STAGE-2 INDEPENDENT REVIEW -> CHANGES REQUESTED (agent aee31581abed02b2f;
  F1 independently reproduced in-session). Dispositions:
  F1 BLOCKING confirmed: declared coarse/wide grids are NOT ordered prefixes of
    identity(0). On the w1 reached-river root, cap0 menu is
    Check/Bet2/Bet3/Bet6/Bet198; coarse = Check/Bet2/Bet198 (interior sizes
    dropped -> pair_dfs prefix mismatch at index 2); wide adds Bet4 (6 vs 5
    actions -> bounded menu exceeds cap-0). map_bounded_to_cap0 correctly throws
    invalid_argument for both; only the identity grid embeds. Therefore grid
    e_embed against identity(0) is structurally undefined under the frozen
    formula, NOT a failed measurement. Q3 is RESCOPED: (a) add a native canary
    pinning identity-accepted / coarse+wide-rejected as a typed result; (b) do
    NOT build a union-menu reference or re-derive the embedding identity - that
    is a new frozen formula requiring its own RFC 0048 addendum, outside D179.
    The bench-local explicit-Abstraction entry on the solver remains a valid
    mechanical change but has no embeddable non-identity consumer yet, so it is
    deferred until such an addendum (no speculative abstraction added).
  F2 retract the mislabel in BOTH the accepted RFC 0048 Stage-1 Evidence and
    the tracker stage-1 table inline; keep the valid cap>=4 1 GiB byte boundary
    (U3 build) and the reached-river cap6/cap0 over-1M U1 rejections.
  F3 retire GridBudget.under_1m_infoset (the U3-vs-1M conflated boolean); map
    U1 -> ResolveLimits 1M infoset/5M nodes/1 GiB online peak; U3 -> TreeLimits
    2M nodes/1 GiB build only; U2 -> G4 census as rows x 40 B/row (no 1M-row
    artifact gate exists).
  F4 reword: U3 IS bounded by build byte/node limits, just not an online-peak
    infoset unit.
  F5 pin U2 by root kind (turn: chance_nodes x48 x2 x1081; reached-river no
    chance: 2x1081=2162) and port tree_budget's chance-child walk to compute the
    reached-max single subtree for U1; pin constants U1 w1=26,461 and
    analytic U2=933,984 in the native fixture.
  F6 add the F1 prefix canary and a pin that every boundary row has bytes==0 and
    all envelope verdicts false.
  F7 coarser menus remove options, so a larger restriction term is the expected
    direction against any valid reference (no ordering theorem for non-nested
    menus); even a favorable coarse result is a different AbstractionId and
    cannot replace cap-1 without re-running G2/G3/B0 - menu-reshaping evidence,
    not a Path A pass.
  REVISED stage-2 deliverable (in-scope, bounded, no new formula/training):
  meter rewrite to distinct U1/U2/U3 with correct envelopes + native calibration
  + the embedding prefix canary + doc retractions. The grid-quality question is
  closed for D179 as "not expressible under the frozen identity(0) reference",
  with the union-reference formula named as the next addendum's explicit input.

- STAGE-2 IMPLEMENTED + native gate green (D179, bench-only). Meter rewritten
  into distinct U1/U2/U3 with correct envelopes; the conflated
  under_1m_infoset/conservative_infoset_sum fields are retired.
  patha_grid_budget now: U1 = turn infosets + largest single reached-river
  subtree (chance-child walk ported from tree_budget), gated by the 1M online
  peak; U2 = BSNCV OracleRow count (turn: chance_nodes*48*2*1081; reached-river:
  2*1081) x sizeof(OracleRow), G4 storage unit, no 1M-row gate; U3 = all-48
  build grand total gated only by TreeLimits node/byte. Boundary rows keep
  bytes=0 and all envelope verdicts false (regression-pinned).
  Native test expanded to 8 fixtures incl. F1 embedding prefix canary
  (identity grid maps to cap0; coarse/wide throw invalid_argument), F5
  calibration pin (U1 w1=26,461 == tree_budget conservative_sum; U2=933,984),
  F6 boundary all-false pin, and U1/U2/U3 non-equivalence. Probe re-run confirms
  corrected rows: turn cap1 U1 26,461 (under_1m=true), U2 933,984/37,359,360 B,
  U3 4,268,305 (labeled build total); reached-river cap1 U1 11,891, U2 2,162.
  Q3 grid-quality chain is closed for D179 as structurally undefined under the
  frozen identity(0) reference (F1), NOT a failed measurement; the union-menu
  reference formula is the explicit next-addendum input. No solver/abstraction
  generalization was added (no speculative consumer); no training budget used.

## RFC 0048 Path A Stage 2b — union-menu reference semantics (D184)

- D184 AUTHORIZATION (TechLead, handoff 6867). The D179 freeze is lifted ONLY for
  the math definition/implementation of a bench candidate union-menu reference,
  plus minimal addendum + bench/native fixtures + isolated CMake + this tracker.
  Production resolver/composition-root/solver/oracle/ABI/proto/public CLI, the
  old certification results, and the original cap-0 identity reference stay.
  cap1 / e_embed<=0.002 / B0=0.010 / G1 and all budgets unchanged; no
  push/merge/publish/deploy/delete/worker/extra-training; protect the 217 dirty.
  The new reference must mark its candidate domain and preconditions; if it is
  not provably equivalent-or-conservative to the original certification
  definition, its numbers are exploration/rejection, never an M2 pass.

- REAL NON-PREFIX COUNTEREXAMPLES (source-verified, w1 reached-river root):
  cap0 identity(0) root menu = Check/Bet2/Bet3/Bet6/Bet198.
  coarse declared = Check/Bet2/Bet198 (subset of cap0 but NOT an ordered prefix:
    its index-2 action Bet198 equals cap0 index 4 -> pair_dfs prefix mismatch).
  wide declared   = Check/Bet2/Bet3/Bet4/Bet6/Bet198 (Bet4 is foreign to cap0 ->
    bounded menu exceeds cap0; no embedding into cap0 exists).
  Two distinct cases: (a) subset-but-interleaved (coarse) needs a generalized
  subset map against cap0; (b) reference-foreign target (wide) needs a union.

- ADDENDUM DESIGN (self-reviewed, pre-implementation).
  SEMANTICS. Define the union reference G_u = cap0 identity(0) menu UNION the
  bounded grid menu at every public node. build_menu_with_cover clamps each
  fraction to [min,cap], sorts, uniques with shared MinAndCap seeds, so a
  declared abstraction over the union SizeSchedule (default fractions union grid
  fractions, MinAndCap, raise cap 0) builds exactly G_u node-for-node. Then the
  bounded grid Gg is a true SUBSET of G_u at every node, so embedding sigma_g
  with zero mass on G_u\\Gg is well defined.
  CONSERVATISM / M2 RELEVANCE. G_u superset-of G0, hence BR over G_u weakly
  dominates BR over G0 wherever both are defined; the union e_embed is an UPPER
  bound on the cap0-relative restriction, i.e. a conservative (stricter)
  certificate. It is a DIFFERENT reference domain, so a union number never
  overwrites the G5 cap0/.002 verdict; it is admissible as conservative
  supporting evidence only, explicitly labeled.
  IDENTITY DEGENERATION. For bounded = identity(1), union(default,default)=
  default -> G_u == G0 and the measure must equal the committed
  measure_cap_materiality(cap1) bit-for-bit (native consistency gate).
  GENERALIZED MAP. Replace the prefix-only pair_dfs for this path with a subset
  embedding map: per paired node, edge_map[ref_action] -> bounded_action index
  or -1 (extension). Pair children recursively through exact Action equality
  (type+target_total); fail closed with invalid_argument if any bounded action
  has no equal reference action (bounded-not-subset). Prefix identity yields the
  identity edge map [0..m-1,-1..], leaving the certified path unchanged.
  EVALUATOR. The same frozen structure: EV walk follows only mapped edges and
  must reproduce the bounded kernel's own root CFV x 990 to <1e-6 (independent
  EV oracle); BR walk maximizes over the full union menu (Uniform 1/A and
  BrFavorable per-combo completion on extension subtrees unchanged). Identity
  e_embed = bounded NashConv + non-negative pure-menu term carries over because
  bounded Gg is a subset of reference G_u (superset-BR proof).
  SOLVER ENTRY. Add an OPTIONAL explicit ActionAbstraction to bench
  RiverSolveConfig/RiverGame (default empty -> today's identity(action_cap), so
  no existing numeric path changes) so a grid policy can be solved seedlessly.
  NATIVE GATES (solve-free tiny/real roots, no text matching):
   N1 identity degeneration: union(default,default) tree == identity(0) and the
      union measure equals measure_cap_materiality(cap1);
   N2 subset-map: coarse maps to cap0 (interleaved, every bounded action matched,
      extensions are exactly cap0\\coarse); wide against cap0 rejects;
   N3 union map: wide maps into its union with no extension action equal to a
      bounded one missed; union menu == cap0 menu UNION wide menu element-wise;
   N4 tiny enumerable root: hand-asserted menu set membership, EV<->kernel CFV
      1e-6, and per-combo BR_union >= BR_bounded (superset max monotonicity);
   N5 determinism and fail-close (invalid bounded-not-subset, malformed fraction).
  COST/UNITS. Every run emits U1 resident / U2 artifact / U3 build with source
  and artifact identity (abstraction id digest + tree shape digest), nodes /
  infosets / bytes / wall-cpu / explicit RSS (ru_maxrss high-water + statm
  current), boundary rows fail-fast and never read feasible.
  BOUNDED REAL MEASUREMENT only after N1-N5 green: single-instance reached-river
  coarse and wide at cap1 under the existing D164 iteration budget; if the
  precondition or budget fails, emit the precise gap/counterproof and keep going
  on solver-free items rather than claim a pass.

- INDEPENDENT REVIEW a56fda80e49e62818 (non-author, separate launch): verdict
  CHANGES REQUESTED, core math sound (set-union, subset completeness,
  superset-BR all hold against source). Blocking F1 = frozen evaluator's
  prefix-index logic (a >= menu_b / direct sigma index) is false for interleaved
  menus -> new path must route every mass/zero-mass/sigma decision through an
  explicit per-node edge_map; N2/N4 need an end-to-end SOLVED interleaved
  fixture. Blocking F2 = formula must be recorded as an RFC 0048 addendum, not
  only in this tracker -> DONE, new "Stage-2b Addendum (D184)" section in
  docs/rfcs/0048, sections A-I. F3 = enforce same_game_def before DFS and retain
  the extension-must-be-aggressive invariant by Action type (not suffix
  position). F4 = fixture holes closed in N4/N5: cap-2 Raise-node
  raise-fraction merge, short-stack clamp collision, 32-action overflow throw.
  F5 = N1 structural digest + 1e-12 tolerance gate with operation-order
  argument, EV 1e-6 oracle retained. F6 = union built via declared(...,
  MinAndCap, 0) with its own digest; every emitted row mechanically carries
  union-candidate / supporting-evidence-not-m2-g5-pass labels. F7 = optional
  RiverSolveConfig action_abstraction, nullopt single identity construction
  site, same abstraction to solver and bounded tree. Component boundary
  decision: frozen cap_embedding.* stays unmodified; new union translation
  units carry the generalized mapper/evaluator (distinct reference domains,
  not old/new versions). Re-review by the same agent required before
  implementation; the addendum's section I records the final verdict.

- RE-REVIEW APPROVED 2026-11-13 (same agent a56fda80e49e62818): no blocking
  findings; subsection H boundary accepted (parallel instruments, N1 as the
  permanent drift gate); L1 per-pair kind guard and L2 permanent CTest +
  derivation comment adopted. Verdict recorded in RFC 0048 subsection I;
  check-rfcs (48) and check-docs (507) green; docs/rfcs/README.md index
  updated.

- D184 IMPLEMENTATION (bench-only; frozen cap_embedding.* byte-stable).
  New TUs union_menu.{hpp,cpp} (make_union_abstraction: per-street reduced-
  fraction merge, declared(MinAndCap, 0) with its own digest;
  map_bounded_subset: same_game_def precondition, per-node edge_map by exact
  Action equality, fail-closed kind/not-subset/passive-extension guards, L1
  kind guard retained) and union_embedding.{hpp,cpp} (edge_map-only
  UnionGame evaluator, Uniform/BrFavorable arms, distribution invariants,
  negative pure-menu-term logic_error, mechanical union-candidate /
  supporting-evidence-not-m2-g5-pass fields, abstraction/shape digests).
  river_solver gains the optional RiverSolveConfig::action_abstraction
  (nullopt single identity construction site; both free functions honor it).
  Native test test_rfc0047_union_embedding (N1-N5, all behavioral) green:
  N1 structural digest + 1e-12 measure equality vs the frozen cap1 measure +
  1e-6 EV oracle; N2 w1 interleaved Bet198 idx2->ref idx4 end-to-end solved,
  wide-vs-cap0 reject; N3 wide union menu element-wise set union incl. Bet4;
  N4 hand-audited shallow root (Check/Bet2/Bet3/Bet6/Bet8), clamp collision
  (2/1 and 5/2 -> single Bet8), cap-2 Raise node, per-combo BR superset
  monotonicity; N5 determinism, same_game_def/foreign/malformed/32-action/
  DeclaredOnly fail-close. Neighbor regressions green: river_solver,
  cap_embedding, patha_grid_budget (solver option numerics-neutral).
  clang-format clean on the eight touched C++ files. Manual probe
  bigshark-rfc0047-union-materiality (NOT a CTest), single-instance, emits
  U1/U2/U3, both RSS figures, wall/CPU, and the mechanical labels.

- D184 FORMAL EVIDENCE (w1 reached-river anchor, 300 iters, serial, one
  grid per process; docs/plans/ablation/rfc0047/s3/stage2b-union/).
  COARSE (interleaved subset; maps both to cap0 and to the union; the two
  reference trees share shape digest 14919905793871934331 and give BITWISE
  identical e_embed though their abstraction digests differ): bounded NC
  0.02761552 BB; e_embed Uniform 31.24489667 / BrFavorable 215.34663379 BB;
  pure menu 31.21728115 / 215.31901826. Bounded tree 15 nodes/9,664 B;
  reference 33,693 nodes/23.7 MiB; U1 reference 12,142,873 infosets ->
  under_1m=0; U2 2,162 rows/86,480 B; node+byte build limits pass;
  wall ~23.1 s, RSS peak ~55 MiB.
  WIDE (cap0 direct map REJECTED, union-only rows): bounded NC 0.02730796;
  e_embed Uniform 22.74050406 / BrFavorable 151.31873978 BB; pure menu
  22.71319610 / 151.29143183. Bounded 33 nodes; reference 45,789 nodes/
  24.1 MiB; U1 16,501,465 -> under_1m=0; wall ~35.5 s, RSS peak 51 MiB.
  VERDICT (candidate domain, NOT M2/G5): both grids candidate_gate_pass=0,
  missing M=0.002 BB by ~1.1e4x (Uniform). The pure menu term dominates
  (22.7-215 BB) over the policies' own ~0.027 BB NashConv, so on the deep
  99 BB anchor bet-sizing-only widening does not recover the missing menu
  value at cap 1; this is the legally measured negative answer the D179
  canary said was undefined under the frozen prefix formula. U1 over 1M on
  the reference trees is reported, never used to negate U3 build
  feasibility (both fit node/byte), and no number overwrites G5.
  Remaining: full rfc0047 ctest closure, independent implementation review,
  isolated local commits; no production/CLI/proto/ABI change.

- D184 IMPLEMENTATION REVIEW APPROVED 2026-11-13 (same non-author agent
  a56fda80e49e62818, third review round): no blocking/major findings;
  reviewer independently reran the N1-N5 binary and a 5-iter probe,
  confirmed the cap0-vs-union bitwise equality at another iteration count,
  edge_map-only lookups, evaluator branch-by-branch fidelity to the frozen
  EmbeddedGame, solver neutrality, frozen cap_embedding.* byte-stable, and
  the four-item CMake scope. Non-blocking advisories: L1 stage only the
  four D184 CMake hunks (pre-existing unrelated hunks stay unstaged; this
  is the established --recount patch workflow); L2 reference walks dominate
  wall time (~18-35 s/row), so a future all-root sweep must not assume
  per-row costs; L3 the 32-action pin is root-level only (adequate because
  the builder throw is path-independent). Residual risk stays the
  parallel-instrument drift surface, permanently bounded by the N1 CTest.

## RFC 0048 Path B — reach-weighted certified bound (D192)

- D192 AUTHORIZATION (TechLead, handoff 6867 successor). Bench-only native
  measurability of a reach-weighted certified bound that CONSERVATIVELY
  constrains the frozen G5 target; the long-term goal stays the original
  G5/M2 certification, no reference/weight substitution. D184's coarse/wide
  candidate_gate_pass=0 and 22.7-215 BB failures stand and are not repackaged.
  Same boundaries: production cap1, cap0 reference domain, 0.002 BB, B0, G1,
  resource/training budgets frozen; no full-root/long scans, no iteration
  inflation, no push/merge/publish/deploy/delete/Worker; bounded tiny/native
  first; U1/U2/U3 separate, cumulative peak never reported as per-instance
  delta; no heuristic patches if the proof fails.

- TARGET QUANTITY (what G5 actually is). The frozen per-root target is
  e(sigma) = sum_s sum_{compatible i,j} (BR_s(i,j) - V_s(i,j)) /
  (1081*990) / BB, an exact per-ROOT measure; the stratum gate is the MAX
  over reached roots (no root weighting exists in the frozen contract).

- REACH SOURCE AND POLICY DEPENDENCE. The only reach with a proven
  relation to e(sigma) is the EVALUATED PROFILE'S OWN per-combo forward
  reach pi_s(n,i), the product of sigma's own action masses root->n (the
  evaluator already tracks pi[seat][combo] at every node). Reach from any
  other policy tau != sigma (blueprint, prior, cap1 policy used to score a
  hybrid sigma_h) defines a different weighted sum with NO ordering
  relation to e(sigma): a tau that avoids a high-gap node makes the score
  shrink while e stays large. FOREIGN REACH IS EXPLORATORY ONLY; the
  component validates supplied reach by recomputing the forward pass from
  sigma and rejects a mismatch.

- DIMENSIONS. pi dimensionless (root 1.0 per combo); local gap delta in
  CHIPS (counterfactual expectation over opponent reach already inside);
  raw_s = sum_n sum_i pi_s(n,i) delta_s(n,i) chips; BB conversion divides
  by 1081*990*big_blind, identical to the D184 normalization. Per-combo
  dense rows, not public-node scalars (blocker structure lives per combo).

- NODE/ROOT AGGREGATION. Within one root: exact sum over the seat's mapped
  action nodes. Across roots: the frozen MAX is retained; a reach-weighted
  root MEAN satisfies mean <= max and can therefore under-report, so it is
  rejected as a G5 certificate (native two-root numeric control). Reach
  weighting is legal ONLY within a root across infosets, never across
  roots.

- LEMMA 1 (reach-weighted decomposition is an IDENTITY). At every mapped
  s-action node n define delta_s(n,i) = max_{a in A_ref(n)} C(n,i,a) -
  sum_{a in A_b(n)} sigma(n,i,a) C(n,i,a), where C is the BR-subtree
  continuation value (opponent fixed to sigma; reference EXTENSION actions
  stay inside the max even though sigma has zero mass on them). Telescoping
  the tree recurrence BR(n)-V(n) = local gap + sum_a sigma(a)(BR-V)(n_a)
  at own nodes and linear expansion at opponent nodes gives
  sum_i (BR_root(i)-EV_root(i)) = sum_n sum_i pi_s(n,i) delta_s(n,i).
  Therefore the weighted sum EQUALS the frozen e_embed(sigma) to FP order:
  conservative by equality, not by deletion. Nodes with zero own reach for
  every combo contribute exactly zero BY THE IDENTITY (they remain
  enumerated; nothing is assigned or removed). Native pin: node-accumulated
  raw vs UnionGame root BR-EV per seat, tolerance 1e-9 chips.

- LEMMA 2 (selective-widening sufficient certificate; CORRECTED after
  independent review round 1). W = nodes whose bounded menu equals the
  full reference menu (edge_map has no -1); their menu-restriction
  component is zero. In the RAW evaluator representation a continuation
  is C(n,i,a) = sum_j pi_opp(n,j) E[payoff|a] over ALL 1,081 opponent
  combos (fold terminals count blocker-incompatible reaches too).
  Define R(n) = max-min payoff over n's descendant terminals and
  S(n) = sum_j pi_opp(n,j) (all 1,081). Any two continuations differ by
  at most R(n)*S(n), so B(n) = R(n)*S(n) dominates delta nodewise for
  every combo: delta = sum_a sigma(a)(C(a*)-C(a)) <= R*S. The round-1
  draft B(n)=max|terminal payoff| was INVALID (missing S ~ 1,081 at the
  root and the range factor 2; fold-to-jam swings span the full payoff
  interval). T4 includes a RED fixture pinning the naive bound
  undercutting a measured positive-reach delta. Then
  C = sum_{n in W} pi delta + sum_{n not in W} pi B >= e(sigma).
  C <= 0.002 is SUFFICIENT for the frozen gate; C > 0.002 is
  inconclusive, never a policy fail. EXPECTED LOOSENESS (review F2,
  recorded honestly): root alone contributes up to R(root)/(990*BB) per
  seat ~0.2 BB on the deep 99 BB anchor vs 0.002 (~0.01 BB even on the
  shallow root), so a metadata-only structural certificate is expected
  to read inconclusive, not pass; Path B's durable value is the exact
  Lemma-1 node-level diagnostic allocation plus a conservative loose
  screen. A tight bound is the Lemma-1 identity itself, not a cheaper
  certificate. The forward reach pass yields both pi_s and S; no cap0
  CFR and, in production shape, no reference CFV walk; the reference
  tree validates B >= exact delta in bench only.

- LEMMA 3 (foreign reach / zeroing is not conservative; the required
  counterexample). Tampered weights rho != pi_sigma make
  sum rho delta deviate without bound in either direction. Tiny control:
  take exact pi, zero the reach of a positive-reach high-gap node -> the
  weighted sum drops below exact e; the reach validator (recompute from
  sigma, exact compare) must throw invalid_argument. A second control pins
  the laundering trap: delta computed against the bounded menu alone is
  residual (zero only at a within-game equilibrium); the component always maximizes over
  the full reference menu and a gap table missing extension actions is
  rejected.

- STRICT APPLICABILITY CONDITIONS. (1) same D184 domain: 2-seat five-card
  river root, MinAndCap, valid SubsetMap; (2) reach = evaluated sigma's own
  per-combo forward reach, validator-recomputed with the evaluator's exact
  snapshot multiplicative update (pi_child = saved*sigma, same op order,
  bitwise match; no tolerance, which would admit weight-scaling attacks);
  (3) gap max over the FULL reference menu incl. zero-mass extensions;
  (4) a widened node is claimed only when edge_map has no -1, else reject;
  (5) B >= delta nodewise for every positive-reach combo, checked against
  exact delta in bench; (6) per-root outputs only, MAX stays external;
  (7) identity holds algebraically for any fixed finite-iteration sigma
  but certifies that policy's exploitability, not equilibrium quality;
  (8) SNAPSHOT IMMUTABILITY: one avg_sigma table threads both the reach
  pass and the gap pass (review F4; reach v1 + gaps v2 passes shape
  checks but breaks the identity); (9) sum over MAPPED OWN-SEAT action
  nodes only - extension nodes get no gap term because their option value
  is already inside the anchor's full-menu max (adding one double counts);
  a hybrid policy is admissible with its own reach while its support
  stays inside the bounded menu.

- PLANNED NATIVE TINY GATES (new bench TU reach_bound.{hpp,cpp} +
  test_rfc0047_reach_bound.cpp; shallow 8-chip root from the D184 N4
  fixture, real solves, no text matching): T1 cross-instrument identity
  vs D184 union measure (both completion arms, 1e-12 BB); T2 per-node
  delta >= 0, widened nodes have no extension edge; T3 per-seat telescope
  against root BR-EV (1e-9 chips); T4 corrected R*S structural B >= exact
  delta nodewise under BOTH completion arms (L1; bound arm-independent by
  interval containment, both empirically pinned), C >= e, W=all ->
  policy-only residual, W=none -> exact,
  PLUS a RED fixture proving naive max-|payoff| undercuts a measured
  positive-reach delta (review F1); R uses evaluator-consistent signed
  payoffs (L2; negation swaps max/min, width invariant); T5 tampered/foreign reach rejected;
  T6 widening claim with missing action rejected; T7 malformed
  vectors/NaN/B<delta rejected; T8 zero-reach node enumerated with zero
  contribution; T9 extension laundering trap via EXISTENCE SCAN (find a
  zero-sigma-mass extension-anchored node with measured delta > 0, pin
  bounded-only fake gap == 0 on the same node; explicit diagnostic if no
  such node exists, deeper-stack root as parameterized fallback; review
  F6); T10 determinism; T11 two-root weighted mean < MAX, component
  refuses root weights; T12 node-level full-joint-reach cross-check:
  independent forward walk over pi_s(n,i)*pi_opp(n,j) per combo pair,
  equality asserted PER NODE not per seat (review F3, catches node-local
  opponent-reach drops). RED first (test against declared API, observed
  failing), then GREEN, then adjacent rfc0047 gates and the full
  background ctest. U1/U2/U3 emitted via the existing
  measure_grid_budget conventions; no manual long probe unless a bounded
  need appears.

- D192 IMPLEMENTATION (bench-only; D184 components byte-stable). New TU
  reach_bound.{hpp,cpp}: ReachInstrument copies the Stage-2b walk
  conventions (pool/showdown/fold/stake/snapshot scaling/completion arms)
  and records per-mapped-own-seat-node continuation values, full-menu
  delta, bounded-only gap, edge_map/widened, opponent reach sum S(n),
  payoff interval R(n), and B=R*S. Public entries: measure_reach_bound,
  reach_bound_from_snapshot (single immutable snapshot threaded through
  both passes), recompute_forward_reach + validate_supplied_reach
  (bitwise, same snapshot op order), verify_custom_bounds,
  selective_certificate (claims fail on non-mapped/extension nodes),
  exact_diagnostic_certificate (the tight Lemma-1 allocation),
  aggregate_roots_max / refuse_root_weights, and the retained-invalid
  naive_max_abs_payoff_bounds RED helper. Native test
  test_rfc0047_reach_bound T1-T12 all behavioral; RED observed first
  (stub runtime_error, exit 134), then GREEN.
- KEY CORRECTION DURING GREEN: the BR-walk pi[viewer] vector is
  COUNTERFACTUAL (own action masses are never applied, stays 1 along own
  paths) and is correct only inside continuation values. The telescope's
  outer weight must be the seat's FULL forward reach (product of own
  action masses); records therefore keep the walk-time opponent reach for
  S(n) but fill own_reach from the independent forward propagation that
  T12 also uses. Before the fix seat-1 terms double-counted own policy
  paths (per-seat root BR-EV matched, node allocation did not); T1 now
  closes to 1e-12 BB and T3 to a scale-relative ~1e-13 chip tolerance
  (two independently coded FP walks). T8 uses a valid deterministic
  snapshot row rather than perturbing the equilibrium policy, and node 0
  selection uses a boolean flag (node index 0 collides with a zero
  sentinel). T4 RED fixture confirmed: naive max-|payoff| undercuts a
  measured positive-reach delta and verify_custom_bounds rejects it.
  clang-format clean; union gate stays green. Full 15-test rfc0047
  ctest closure and the independent implementation review run next.

- D192 IMPLEMENTATION REVIEW APPROVED 2026-11-13 (agent
  a56fda80e49e62818, third Path B round): no blocking/major/medium
  findings. Counterfactual-vs-forward reach correction verified at
  every entry point; L1 adopted (T12 now bitwise-pins opponent reach
  and S per node, suite still green); L2 adopted (bounded_gap is the
  local within-game NashConv residual, zero only at equilibrium, not
  "tautologically zero"; RFC/tracker wording corrected); L3 verdict
  recorded in RFC subsection H. Scope confirmed: two isolated bench
  CMake hunks, no production surface, kIters=60 tiny root only, no
  long scan. Residual risk stays the disclosed structural looseness.

- PATH B REVIEW ROUND 1 (2026-11-13, agent a56fda80e49e62818): CHANGES
  REQUESTED. Lemma 1 telescope and Lemma 3 foreign-reach/root-MAX
  arguments sound; F1 BLOCKING structural bound invalid as drafted
  (resolved by R*S above with all-1081 reach sum); F2 expected-looseness
  honesty paragraph added; F3 T12 added; F4 preconditions 8-9 added;
  F5 validator op-order specified; F6 T9 made an existence scan.
  Re-review required before implementation.
- PATH B REVIEW ROUND 2 (2026-11-13, same agent): APPROVED, no blocking/
  major findings. Probe (a) R*S dominance holds under BrFavorable too:
  completion arms only pick policies over existing edges; every subtree
  policy payoff is a convex combination of descendant terminal payoffs,
  so per-deal action differences stay within interval width R(n).
  Probe (b) all-1081 S(n) well-defined everywhere: reach propagation is
  dense/blocker-blind (blockers only in the showdown matrix); S=0 ->
  delta=0 and B=0 with no division. Advisories adopted: L1 T4 runs both
  arms; L2 signed-payoff convention documented; L3 verdict recorded in
  RFC subsection H and here. Approval gates reach_bound.{hpp,cpp} +
  T1-T12 implementation, RED first; no production surface.

- D192 REAL-ANCHOR DIAGNOSTIC (manual probe rfc0047_reach_diagnostic,
  NOT a CTest; w1 reached-river, 300 iters, single instance;
  docs/plans/ablation/rfc0047/s3/pathb-reach/). Exact Lemma-1 values
  reproduce the D184 union CSV bitwise (coarse 31.24489667 /
  215.34663379 BB; wide 22.74050406 / 151.31873978 BB), a second
  real-anchor cross-instrument confirmation. Structural Lemma-2
  certificates read 493.65 BB (coarse, both arms — arm-independent)
  and 206.49 Uniform / 251.51 BrFav BB (wide): ~9-16x looser than the
  exact totals and five orders of magnitude over the 0.002 bar,
  empirically confirming the RFC's expected-looseness statement; no
  metadata-only structural certificate can pass on this anchor.
  NODE ALLOCATION (the durable diagnostic output). Coarse: the three
  restricted (non-widened) reach-1.0 nodes contribute 13.7603 +
  10.0134 + 6.3461 = 30.12 BB of 31.24 (~96%); the two widened nodes
  contribute 0.00086 BB total, so selective widening of deeper nodes
  cannot recover the error — the menu restriction bites at root-level
  raise decisions, not deep low-reach lines. Wide: widened nodes carry
  ~8.11 BB of local policy NashConv residual (not menu error); the
  remaining ~14.63 BB sits at restricted nodes. Cost: U1 reference
  12.1M/16.5M over 1M flagged honestly; U3 node/byte feasible;
  23.0-35.5 s wall, RSS peak 51-58 MiB; U2 2,162 rows/86,480 B. All
  rows mechanically labeled; nothing overwrites G5. U1/U2/U3, wall/CPU,
  and both RSS figures are emitted per row.

## RFC 0048 Path A root-menu feasibility closure (D195)

- D195 AUTHORIZATION (TechLead, handoff 6867 successor). Close the
  root-level-menu / offline-wide-blueprint feasibility loop with an
  executable bench prototype, not another plan. Frozen: cap0 reference
  domain, production cap1, 0.002 BB, B0=0.010, G1, training/resource
  budgets, the coarse/wide FAILs. No full-root scan, no iteration
  increase, no new production/certification contract, no push/merge/
  publish/spend/delete/Worker. Any claim needs an equivalent-or-
  conservative proof against the frozen target or stays exploratory.
  Reuse D192 evidence; state coverage gaps; no duplicate half-hour
  acceptance run.

- COVERAGE GAP OF THE D192 DIAGNOSTIC. The node allocation proved WHERE
  the menu error sits (three reach-1.0 restricted nodes, ~96% of the
  coarse total) but did not split each node's delta into (a) the pure
  menu-restriction component removable by widening and (b) the bounded
  policy residual that survives widening. Without that split, "widen the
  root nodes" is a hypothesis, not a feasibility verdict.

- NODE-LEVEL ERROR DECOMPOSITION (REVISED after review round 1 F1). The
  existing bounded_gap column uses FULL-walk continuations C_full, so it
  is a one-level restriction and telescopes only to an UPPER bound on
  e_Gg; the exact identity needs a new self-consistent RESTRICTED BR walk
  over the bounded tree (max only over A_b at every own node, no
  extension subtree; paired back through SubsetMap). With restricted
  continuation C_g and local gap g_g = max_{A_b} C_g - sigma.C_g:
    menu_gap(n,i) = delta(n,i) - g_g(n,i) >= 0 by set inclusion; the local
      widened flag is NOT a zero condition (deeper subtree extensions can
      keep menu_gap positive), zero only when the whole mapped subtree is
      extension-free; cross-walk sign uses a chip tolerance (independent
      walk summation), bit-exact retained only for single-walk columns
    sum reach*g_g      = e_Gg = solver nash_conv_bb (exact)
    sum reach*menu_gap = D184 pure_menu_term (exact)
  Requires a small bench-only walk extension; the 'no TU change' premise
  is withdrawn. Exact for any fixed finite-iteration policy.

- TWO-TIER FLOOR (review F2). TIER 1 EXACT fixed-policy: embedding the
  measured sigma (zero mass on new actions) after any node widening
  keeps exploitability >= measured e_Gg = 0.0276 BB (14x over M), and
  removes exactly the selected nodes' reach-weighted menu_gap. TIER 2
  EMPIRICAL re-solve: no theorem orders finite-iteration RM+ NashConv
  across different game shapes (D164 NC sequence 0.0240..0.1267 at 300
  iters is non-monotonic; cap6 300->1000 iters NC 0.127->0.068 while
  pure menu RISES 0.0022->0.0061). Refusal rests on the measured
  T^-0.52 law (1e5-1e6 iters; 700 s at 1000) and the 0.0240-0.0276 BB
  residual band across all grids at 300 iters, not a universal floor.

- EXPRESSIBILITY GAP (stated, not patched). ActionAbstraction is
  schedule+cap driven with no per-node menu override; a node-targeted
  hybrid (full menu only at the three reach-1.0 nodes) cannot be built
  without modifying the production abstraction class, which D195 does
  not authorize. The node-level split is the rigorous bench substitute:
  it gives the exact removable quantity and the exact residual floor
  for EVERY possible node-widening selection, including the perfect
  root-only selection, without a production change.

- PROTOTYPE (bench-only, minimal). New restricted BR walk in
  reach_bound (bounded tree only, SubsetMap pairing) producing exact
  per-node g_g and menu_gap; manual probe gains the split columns;
  native T13 in test_rfc0047_reach_bound:
   (i) menu_gap >= 0 with chip-scale tolerance; subtree-extension-free
       nodes measured at zero; local widened flag reported as diagnostic
       only, never asserted equivalent to zero (review round-2 F1);
   (ii) restricted telescope == bounded_nash_conv_bb, chip-scale
        scale-relative tolerance (independent walk summation order);
   (iii) exact menu-gap telescope == D184 pure_menu_term cross-
        instrument on the same fixture, both completion arms, 1e-12 BB,
        each paired with the ONE arm-free g_g (bounded tree has no
        extension nodes, so completion arms are vacuous there);
   (iv) round-3 correction: bounded_gap >= g_g is NOT nodewise true
        (differences of ordered functions are unordered); pin the
        AGGREGATE sandwich e_Gg <= sum reach*bounded_gap <= e_embed at
        chip tolerance, plus the bit-exact single-walk nodewise
        delta >= bounded_gap (one continuation vector).
  T13(i) existence clause: scan for at least one extension-free mapped
  node or fail with a diagnostic (no vacuous zero clause; T9 pattern).
  Restricted walk records pair via the inverse of SubsetMap::node_map
  (bijection onto mapped nodes); reuse existing terminal/opponent/pool
  conventions. Permanent CTest; RED first.

- BLUEPRINT ARTIFACT FEASIBILITY (existing solve-free evidence, no new
  run needed for the reached-river object). Wide bounded tree U1 =
  14,053 infosets (inside 1M), U2 = 2,162 rows/86,480 B, reference
  24.1 MiB and node/byte limits pass; stage-1 turn-trunk census
  (RFC 0048 Stage-1 Evidence): wide 7.07M infosets / 11.8 MB vs
  cap-2 56.4M / 95 MB. Storage/build is FEASIBLE; the wall is quality
  convergence and raise depth, not artifact size.

- VERDICT CRITERIA (falsifiable). FEASIBLE only if a measured menu
  configuration simultaneously gives (a) removable menu term leaving a
  residual floor <= 0.002 BB within the existing iteration budget and
  (b) U1/U2/U3 inside envelope. Otherwise INFEASIBLE with the exact
  node-level numbers; no gate is lowered. Expected outcome from the
  existing three menu points (coarse 31.24, wide 22.74, identity cap1
  19.48 BB; none within 4 orders of M) plus the 0.0276 residual floor:
  INFEASIBLE on every MEASURED configuration within frozen budgets with
  no plausible within-budget candidate identified; the continuous
  fraction space makes universal exclusion impossible (residual risk =
  an unmeasured grid with materially faster same-budget convergence),
  to be confirmed by the T13-gated split rather than assumed.

- D195 REVIEW ROUNDS 3-4 (agent a56fda80e49e62818). Round 3 CR:
  bounded_gap >= g_g is not nodewise true (differences of ordered
  functions unordered; counterexample h=(10,0), f=(10,10)) -> aggregate
  sandwich only + bit-exact single-walk delta >= bounded_gap; T13 zero
  clause gained existence scan. Round 4 (2026-11-14) APPROVED, no
  findings; sandwich endpoints independently re-derived, T13 fully
  falsifiable, implementable as recorded. Four rounds total before code.

- D195 IMPLEMENTATION (bench-only; reach_bound extended, no production
  change). RestrictedInstrument = self-consistent BR walk on the bounded
  tree (arm-free, no extension nodes), recording exact g_g;
  subtree_is_extension_free static DFS over mapped edges; T13 permanent
  CTest (RED stub exit 134 observed, then GREEN first build). T13
  reconciles restricted telescope to solver nash_conv_bb, exact menu
  telescope to D184 pure-menu under both arms with one arm-free g_g,
  bit-exact single-walk delta>=bounded_gap, aggregate sandwich
  e_Gg <= sum reach*bounded_gap <= e_embed, and an extension-free
  existence scan (2 such nodes on the tiny fixture; strict interior
  sandwich verified non-vacuous). Probe rfc0047_reach_diagnostic now
  emits the per-node residual/removable-menu split.

- D195 FORMAL EVIDENCE (w1 reached-river, 300 iters, single instance;
  docs/plans/ablation/rfc0047/s3/patha-rootmenu/). COARSE split columns
  reconcile bitwise: bounded_residual sum = 0.02761552 BB == solver
  bounded NC; removable_menu sum = 31.21728115 BB == D184 pure-menu
  term; total 31.24489667 == e_embed. The four restricted root-level
  nodes (three at reach 1.0 and node 3 at mean reach 0.4188) hold
  31.2173/31.2173 = ~100% of the removable menu
  value; the two extension-free nodes carry zero removable menu
  exactly. WIDE: removable menu 22.7132 BB spread across nine
  restricted nodes; of the four LOCALLY-widened nodes, the three
  subtree-extension-free ones carry only policy residual (0.00734 BB
  total, zero removable menu), while the fourth (seat 1 node 0) is
  locally widened but NOT subtree-extension-free and still carries
  8.087 BB removable menu — the round-2 local-vs-subtree distinction
  observed on real data. VERDICT (falsifiable,
  measured configurations only): INFEASIBLE within frozen budgets.
  Even PERFECT root-only widening (unattainable without a production
  per-node abstraction change) leaves the exact fixed-policy floor
  e_Gg = 0.0276 BB, 13.8x over M=0.002; the T^-0.52 convergence law
  requires 1e5-1e6 iterations to move the floor (D164 measured
  700 s/1000-iter wall). Artifact storage is non-binding (U1 bounded
  7,567/14,053 inside 1M; reference 24 MiB; U2 86,480 B). The menu
  restriction is removable in principle but the residual policy tier,
  not the menu, is the binding wall on the deep 99 BB anchor. No G5/M2
  verdict replaced; all rows union-candidate labeled.

## Averaging-policy re-solve within frozen budgets (D198)

- D198 AUTHORIZATION. Test the existing but uncovered linear_averaging
  solver switch as a new policy candidate under the SAME menu/domain/
  300-iter budget; no new menu builder, per-node abstraction, generic
  solver framework, external dependency, production/schema change, full
  root sweep, or training grid. Fixed-policy certificates are never
  transferred to a new policy; without an equivalence/conservatism proof
  every new-policy number stays exploratory.

- BUDGET ACCOUNTING (real authorization basis; revised after review
  round-2 F1). The authorization unit is one SINGLE-INSTANCE PROCESS at
  300 iterations, never a per-process solve count (D164 capcurve ran
  many solves per job); no cumulative training pool was ever defined.
  D198's anchor binary performs BOTH averaging modes in one process:
  one new uniform-candidate solve plus one in-process linear CONTROL
  solve, same menu/domain/threads=1/300 iters, no new grid/root/pool.
  The in-process linear tables make A2 hash/L1, NC comparison, and A4
  realizable; historical linear CSVs remain the cross-run reference but
  no golden policy table is persisted. Tiny distinctness failure still
  cancels before the anchor process is spent on w1.

- MECHANISM (river_solver.cpp:559). Each iteration t accumulates the
  average strategy with weight w = t when linear_averaging=true (CFR+
  linear average, recent iterations dominate) or w = 1.0 when false
  (uniform time average); build_sigma normalizes by the per-combo action
  sum, falling back to uniform only when the sum is zero. Both outputs
  are legal distributions by construction; the switch changes ONLY the
  averaging weights, never regret matching, tree, or evaluator.

- PRE-SELECTED CANDIDATE (one, fixed before any anchor run).
  sigma_uni = solve(coarse cap1 menu, w1 reached-river, 300 iters,
  threads=1, linear_averaging=false). Hypothesis, not a promise: uniform
  averaging weights early iterations equally and can yield a lower
  finite-iteration exploitability than linear averaging on this small
  tree; it can equally be worse. No current floor is assumed to carry
  over.

- TINY NATIVE GATES FIRST (new test_rfc0047_averaging.cpp, real solves,
  no text matching; tiny 8-chip root). MODE-INDEPENDENCE fact: regret/
  reach/RM+ path never reads avg_weight, so both runs share bitwise
  identical per-iteration RM+ and zero-tot fallback rows.
  A1 legality: every node/combo row of both policies finite, non-
     negative, sums to 1 within 1e-12; uniform 1/A fallback rows legal
     and fallback trigger node/combo SETS identical between modes;
  A2 distinctness metric (review F1): row L1 d=sum_a|p_lin(a)-p_uni(a)|;
     distinct iff some action node has d>=1e-12 AND content hashes
     differ; evaluated at small tiny-root count (pre-anchor) AND again
     at the 300-iter w1 anchor in-process at zero extra solve;
     coincidence is a legitimate 'no new information' closure, never a
     forced difference;
  A3 independent external evaluation: BOTH policies reproduce solver
     root CFV x990 below 1e-6 via the union/EV walk; reach validator
     bitwise;
  A4 default stability: in-process same-binary linear-vs-linear repeat
     bitwise identical (the zero solver-TU-diff check is review/CI
     hygiene, not a runtime CTest; no golden policy file exists);
  A5 fail-closed: malformed (negative/non-normalized/NaN) and missing/
     zero-reach snapshots rejected by the existing validator, including
     the tot==0 fallback rows;
  A6 (reported, not asserted): tiny-root NC(linear) vs NC(uniform) as an
     early pre-anchor signal on the uniform-may-be-lower hypothesis.
  RED-first where a new assertion is added.
- HASH/A4/A6 SPECS (review round 2). Content hash = FNV-1a 64-bit over
  flat avg_sigma vectors in DFS node order, combo-row-major per action,
  reproducible across processes; supporting evidence, not historical
  authentication. Tiny-root A2 count pinned at 60 iterations. A6 reuses
  RiverSolveResult::nash_conv_bb for both modes (signed diff + root id
  + iteration count + both hashes), non-asserted. A4 = in-process
  same-binary linear-vs-linear bitwise repeat; zero solver-TU diff is a
  review/CI hygiene item, not a runtime CTest.

- ANCHOR EVALUATION (only if A2 passes). One manual non-CTest binary
  performing, in one single-instance process, one uniform-candidate
  solve plus one in-process linear control solve (both 300 iters) on
  the exact D195 coarse cap1 fixture, each evaluated in the frozen
  cap0 reference domain with the existing union/reach instrument (no
  new math). Emits abstraction/shape/policy-hash identity, bounded
  NashConv, e_embed, pure menu term, U1/U2/U3, wall/CPU, RSS.
  Acceptance is strictly exploratory:
  report whether bounded_NC(sigma_uni) is below/above the
  linear=true baseline 0.02761552 BB and whether e_embed moves; no G5
  pass is claimed unless the frozen 0.002 bar is met under the original
  target definition, which the evidence will settle.

- D198 REVIEW ROUNDS 1-3 (agent a56fda80e49e62818). R1 CR: undefined
  distinctness threshold, policy-hash overclaim, missing fallback/A6.
  R2 CR: one-solve budget contradicted anchor-level in-process check ->
  adopted two-solves-in-one-process (per-run unit is a process, D164
  capcurve precedent); FNV-1a DFS-order hash specified; 60-iter tiny
  count pinned; A4 TU-diff reclassified as hygiene. R3 (2026-11-14)
  APPROVED, no design findings; L1-L2 stale tracker lines harmonized,
  verdict recorded in RFC subsection D. Implementation sequence:
  test_rfc0047_averaging (A1-A6, tiny 8-chip root, 60 iters) RED-first,
  bench FNV hash helper, then one-process two-mode w1 300-iter probe.

- D198 IMPLEMENTATION (bench-only). policy_hash.{hpp,cpp}: FNV-1a 64-bit
  DFS-ordered snapshot hash + row/max L1; root_values_from_snapshot added
  to reach_bound (snapshot-based per-combo root EV/BR, no re-solve; D184
  union component untouched). test_rfc0047_averaging A1-A6 native on the
  8-chip root at 60 iters, all behavioral: legality + fallback trigger
  parity, L1/hash distinctness, external CFV x990 oracle, in-process
  linear bitwise stability, malformed/NaN/non-normalized fail-close,
  non-asserted NC signal. Manual probe rfc0047_averaging_probe runs BOTH
  modes in one single-instance process at 300 iters.

- D198 FORMAL EVIDENCE (w1 coarse cap1, 300 iters, one process;
  docs/plans/ablation/rfc0047/s3/averaging/w1-coarse-300.txt).
  Distinctness confirmed: hashes 1922761369290757593 (linear) vs
  285468303300579094 (uniform), per-combo max row L1 0.40069 (review L1
  corrected the helper from a node-vector aggregate to the RFC's
  per-combo row metric bounded in [0,2]; hashes unchanged, so the policy
  tables themselves were unaffected). The in-process linear
  control reproduces the historical D184/D195 values bitwise
  (bounded NC 0.02761552; e_embed 31.24489667 Uniform / 215.34663379
  BrFav; pure menu 31.21728115), validating same-binary reuse.
  VERDICT: uniform-average candidate REJECTED on measured evidence.
  Its bounded NashConv is 0.07459669 BB, 2.70x the linear control's
  0.02761552 and 37.3x over M=0.002; e_embed moves only slightly
  (31.09872530 / 214.92172480) because a marginally lower pure-menu
  term (31.02412861) is offset by the worse residual. The existing
  linear CFR+ average is the better averaging mode at this budget;
  the uncovered switch adds no value toward M2. Both policies remain
  orders of magnitude over the bar, no certificate transferred, all
  rows union-candidate labeled. Cost: two 300-iter solves in one
  process, ~18 s evaluation per row, RSS peak 46.9 MiB; U1/U2/U3
  unchanged from D195 (storage non-binding).

- D198 IMPLEMENTATION REVIEW APPROVED 2026-11-14 (agent
  a56fda80e49e62818): no blocking/medium findings. L1 (LOW) metric unit
  mismatch fixed in code to the RFC per-combo row L1 (bounded [0,2]);
  L2 fallback-row inference via output equality accepted with the
  mode-independence proof (a bench solver-side tot==0 flag accessor is a
  future-exact option, deferred); L3 verdict recorded here and in RFC
  subsection D. Frozen solver/union/cap TUs zero diff; reach_bound change
  purely additive; three isolated CMake hunks.

## Per-node mixed-menu tiny mechanism (D202)

- D202 AUTHORIZATION. TL extends bench owning scope to the previously
  forbidden per-node heterogeneous menu / selective widening, built ONLY
  from existing tree/solver/game primitives. Production
  ActionAbstraction public API, persisted schemas, external protocols,
  production flags are NOT changed; cap0 reference, cap1 default,
  0.002/B0/G1, the 300-iter/single-instance resource basis, and root
  weights/target/conservatism are frozen. One pre-selected candidate, no
  full-root scan, no grid sweep, no iteration increase, no cumulative
  pool. Any non-cap1 candidate menu stays bench/exploratory labeled.

- STRUCTURAL FACT (pre-implementation survey; review round 1 corrected).
  AbstractTree is immutable and constructible only from (GameDef,
  ActionAbstraction); the private no_build ctor and builder internals are
  not bench-reachable, and ActionAbstraction carries only per-street
  uniform schedule+cap+seeds, so a pruned-tree or per-node abstraction
  cannot be materialized without a production change. The minimal
  bench-correct composition is therefore NOT a new tree but an EDGE MASK
  over an existing reference tree: all nodes/terminals/edges stay, and
  only the candidate policy's per-node allowed action set changes.
  Honesty correction from independent review: the terminal/stake
  ledgers, the a0 showdown matrix, snapshot reach scaling, and the
  vector pool are reused as DATA and conventions, but the CFR traversal
  cannot be injected into the anonymous-namespace RiverGame (its
  layout/traversal/snapshot loops are TU-private) — the new TU contains
  a self-contained parallel MixedMenuSolver with compact allowed-slot
  storage and a per-node reference-edge-to-slot map, the same precedent
  as UnionGame / ReachInstrument / RestrictedInstrument. No production,
  ABI, or public-API edit is required or forced.

- MECHANISM (new bench TU mixed_menu.{hpp,cpp}, no production edit).
  MixedMenuMask: one vector<bool> per action node over the REFERENCE
  tree's edge indices; allowed(n,a) => the candidate may use reference
  edge a at node n. Validation (fail-closed invalid_argument):
   (1) only removal: every allowed edge is an existing reference edge,
       no menu ever grows beyond the frozen cap0 reference;
   (2) non-empty: every mapped action node keeps >=1 allowed edge;
   (3) public-information consistency: the mask is a function of public
       node identity only, never of hole cards/combos — the structure
       holds one mask row per public node and has no combo dimension
       (tree nodes ARE the engine infoset units: distinct public DFS
       histories get distinct nodes, and the engine has no infoset
       equivalence class across nodes);
   (4) reference integrity: opponent deviation edges are NEVER removed
       from the reference — the mask constrains only the candidate
       policy, while BR continues to maximize over the full reference
       menu, so opponent exploitation stays measurable;
   (5) passive-edge ban (review F3): legality-derived passive edges
       (Fold/Check/Call, i.e. non-aggressive actions) are never
       removable; a -1 entry on a passive edge is rejected as state
       drift, identical to the D184 map_bounded_subset precondition;
   (6) hard caps: node/action/byte TreeLimits checked before allocation,
       tree_resource_exhausted propagated, never an OOM wait.
  Because no second AbstractTree exists, map_bounded_subset cannot be
  called directly; the TU instead provides standalone
  validate_mask_subset_map(reference, map) running the IDENTICAL
  kind/duplicate/passive-extension/non-empty checks, and M2 exercises it
  including each rejection branch. Single-tree-specific checks are
  named explicitly (review L3): (a) every edge_map index is in range
  and node_map is the identity bijection over the mask-row node set
  (the analog of the impossible same_game_def check); (b)
  allowed-edge child closedness — every allowed edge's child node
  carries a mask row, throwing if an allowed edge reaches a row-less
  child (the analog of pair_subset_dfs recursive pairing).
  MixedMenuSolver: CFR+ over the reference tree STRUCTURE with regret/
  sum/sigma allocated per node over the ALLOWED slot subset only (slot
  storage plus a per-node reference-edge->slot map). Every policy
  operation ranges over exactly the allowed slots: RM+ uniform fallback
  is 1/|allowed(n)| (public, identical across all 1,081 combos),
  regret accumulation and negative clipping, average-sum accumulation,
  and traversal never descend a disallowed edge. The fixed node actor
  means the mask restricts only that seat's choices; both seats'
  self-play policies normalize over the node's allowed set, so no
  probability mass can leak through a removed edge and the RM+ regret
  bounds apply unchanged. Disallowed subtrees are retained solely as
  reference-BR territory. The solver owns its own BR walk OVER ALLOWED
  EDGES (RestrictedInstrument pattern) to produce the masked game's
  bounded_nash_conv_bb; the reference evaluator is never reused for the
  masked game's own NashConv.
  Re-expansion contract (review F3): emitted snapshot rows are full
  REFERENCE-width with exact zero bits on disallowed edges; compact slot
  storage is expanded BEFORE any union/reach evaluator call (those
  index avg_sigma through edge_map), and zero-reach nodes retain
  normalized fallback rows so validate_snapshot passes over the
  identity-mapped node set. The mask-derived SubsetMap is
  edge_map[n][a] = a when allowed else -1, node_map identity. No new
  math: the D192 Lemma-1 and D195 restricted/menu decomposition apply
  unchanged. CORRECTED zero-gap condition (review F1): menu_gap(n)==0
  is a SUBTREE property — it holds only when no reachable same-seat
  mapped action node in n's whole subtree carries a -1 edge
  (subtree_is_extension_free); a node whose LOCAL mask equals the full
  reference menu can still have positive menu_gap, and the root-only
  candidate is exactly such a node.

- PRE-SELECTED CANDIDATE (one). On the tiny 8-chip root: ROOT-ONLY
  widening — the root node gets the full cap0 menu (5 edges), every
  deeper node keeps the original coarse cap1 menu. This directly tests
  the D195 diagnostic that ~96-100% of removable menu value sits at
  root-level restricted nodes, at the smallest enumerable scale.

- NATIVE TINY GATES (new test_rfc0047_mixed_menu.cpp, RED first, real
  solves, no text matching):
  M1 mask validation: over-size/empty/public-violation/unknown-edge/
     passive-removal masks rejected; a valid mask builds;
  M2 structural nesting: validate_mask_subset_map runs the identical
     D184 kind/duplicate/passive-extension/non-empty checks on the
     mask-derived SubsetMap, including every rejection branch;
  M3 policy legality: snapshot rows finite, non-negative, normalized to
     1 over ALLOWED edges with exact zeros on disallowed reference edges
     (reference-width re-expanded rows; zero-reach fallback rows
     normalized);
  M4 support locality and distinctness (review F2/L1; numeric policy
     identity and empirical positivity equality are NOT asserted — the
     widened root changes descendant reach flows and RM+ trajectories,
     so an edge may carry exactly zero average mass in one policy and
     positive mass in the other): (a) a PRE-SOLVE structural check that
     the mask's allowed reference-edge set at each non-root node equals
     the coarse tree node's action set 1:1 (mask equivalence; no
     playable action appears or disappears), and both policies carry
     exact zeros on disallowed reference edges; (b) the mixed policy is
     distinct from BOTH coarse and full-cap0 overall, via content hash
     plus per-combo L1 — all numeric comparison lives here;
  M5 external EV/BR oracle: existing reach_bound/root-values entry
     reproduces solver root CFV x990 <=1e-6 for the mixed policy.
     Evaluator adapter wiring is pinned (review L2) — with no second
     tree, root_values_from_snapshot / validate_snapshot receive the
     REFERENCE tree as BOTH the reference and bounded argument, the
     identity node_map, reference-width re-expanded rows, and the mask
     edge_map; then nb==n, the bounded menu width equals the reference
     width, and disallowed edges return zero before indexing. No
     parallel evaluator may be invented, and M5 asserts this exact
     wiring yields the 1e-6 CFV match;
  M6 full-menu BR preservation: the evaluator's BR still maximizes over
     disallowed reference edges (a disallowed edge carrying positive BR
     option value yields a positive measured menu_gap — the laundering
     trap stays closed); the solver's own masked-game BR walk, by
     contrast, maximizes only over allowed edges and its telescope
     equals the masked bounded NashConv;
  M7 info-set/reach conservation: structural assertion that the mask is
     indexed by public node only with no combo dimension; bitwise
     forward-reach validation, zero-reach nodes enumerated not deleted,
     root total reach 1.0;
  M8 subtree gap identity (review F1/L4; replaces the false local-menu
     identity): (i) at the fully-widened root the edge_map contains no
     -1 locally AND subtree_is_extension_free(root, seat) is FALSE for
     EACH seat evaluated separately (actors alternate; one seat's
     subtree could conceivably be extension-free even if the other is
     not) — deeper same-seat cap-1 nodes keep extension edges; (ii) an
     existence scan finds a POSITIVE root menu_gap (diagnostic with the
     T9 no-extension fallback convention); (iii) zero menu_gap is
     pinned only at a genuinely extension-free node, reusing the T13
     fixture's known extension-free nodes near all-in terminals, under
     the established cross-walk CHIP tolerance 1e-9 (not bit-exact:
     delta and the restricted gap come from independent walks);
  M9 determinism: repeat solves bitwise identical;
  M10 fail-closed resource (review F4 reword): a mask is a trivial
     vector<bool> and cannot itself exhaust TreeLimits; the REFERENCE
     tree construction propagates tree_resource_exhausted as a typed
     error, and masked storage is bounded nodewise by the reference menu
     widths so it cannot exceed the reference envelope — the gate
     asserts typed propagation of reference-construction failure plus
     the nodewise storage bound, not mask-driven exhaustion.
  The w1 300-iter anchor is consumed ONLY if M1-M10 pass; otherwise the
  tiny failure is retained and no anchor budget is spent.

- ANCHOR DECISION RULE (pre-declared). If tiny gates pass, run the same
  root-only mask on the w1 reached-river fixture at the existing 300
  iter/single-instance basis via the existing probe shape, emitting
  identity/hash, bounded NC, e_embed under BOTH completion arms, exact
  residual/menu split, U1/U2/U3, wall/CPU, RSS. Pre-declared resource
  class (review F4): the anchor runs CFR over the FULL cap0 reference
  tree (w1: 11,232 action / 33,693 total nodes, 23.7 MiB) for 300
  iterations — a new wall-time class versus prior bounded solves;
  extrapolating D164's cap-6 304 s/300 iters at 1,924 action nodes gives
  an order-30-minute expectation, NOT the ~20 s bounded-anchor class;
  RSS stays bounded by the reference envelope (~tens of MiB plus
  combo-sized ledgers). The run remains covered by the per-run
  single-instance PROCESS authorization (D164 capcurve precedent), and
  is not started until tiny gates pass. Acceptance remains exploratory:
  a G5-quality claim requires the frozen 0.002 bar under the original
  target definition; anything else is mechanism evidence. The old
  sigma's reach/residual certificates are never applied to the mixed
  policy — it is re-solved and re-evaluated from scratch.

- ANCHOR RESULT (measured 2026-11-14; supersedes the pre-declared
  resource class above). One single-thread 300-iteration solve of the
  root-only mask on w1, both completion arms evaluated on the immutable
  snapshot (docs/plans/ablation/rfc0047/s3/mixed-menu/w1-rootonly-300.*;
  probe rfc0047_mixed_menu_probe). Reachable closure: 2,430 action
  nodes, 7,286 compact allowed slots vs 33,692 reference edges (21.6%);
  candidate policy hash 969471175395458718 (genuinely distinct from the
  coarse and cap-0 baselines).
    bounded NashConv ............ 0.03661936 BB (coarse 0.02761552; 1.33x worse)
    e_embed Uniform ............. 23.34239272 BB (coarse 31.24489667)
    pure menu term Uniform ..... 23.30577336 BB (coarse 31.21728115; -25.3%, -7.91 BB)
    e_embed BrFavorable ......... 159.63194387 BB (coarse 215.34663379)
    solve wall/CPU .............. 3010.5 s / 3002.1 s (~50.2 min; coarse 18.0 s, 167x)
    peak RSS .................... 730,508 KiB = 713.4 MiB (coarse 46.8 MiB, 15.3x)
    evaluation .................. ~29.8 s/arm over the full reference tree (by design)
  VERDICT: the per-node mixed-menu mechanism is REAL and honestly
  measurable — root widening removes 7.91 BB of Uniform pure menu term
  — but it is not an M2/G5 path. Residual e_embed 23.34 BB Uniform
  misses the frozen 0.002 BB bar by ~1.17x10^4, and the bounded NashConv
  worsens because the 2,430-node restricted game converges more slowly
  than the 6-node coarse game at the same 300-iteration budget (the
  T^-0.52 policy-convergence wall). Root-only widening leaves the
  dominant menu mass in still-restricted deeper subtrees, empirically
  confirming the D195 subtree conclusion on an actually-widened tree.
  The pre-declared "tens of MiB / ~30 min" class was wrong on both
  counts and is corrected here; 713.4 MiB stays inside G1's 1 GiB with
  ~30% headroom but never engages the 10 s online half of G1, so the
  construction is offline evidence only. All rows are mechanically
  labeled union-candidate / supporting-evidence-not-m2-g5-pass; no
  certificate transfers and no M2/G5 pass is claimed.

- REVIEW RECORD. Round 1 (2026-11-14, independent session agent
  a56fda80e49e62818): CHANGES REQUESTED. F1 blocking — M8/local
  full-menu zero-gap claim repeated the D195 local-vs-subtree error;
  F2 — M4 numeric policy identity across games is false; F3 —
  hand-built mask map bypassed passive/duplicate/kind checks and lacked
  a re-expansion contract; F4 — masked game needs its own allowed-edge
  BR walk, anchor wall/RSS class undeclared, M10 asserted impossible
  mask-driven exhaustion. All four dispositions are incorporated above
  (parallel-solver honesty correction, passive ban +
  validate_mask_subset_map + reference-width re-expansion, subtree
  M8, support-locality M4, internal BR walk, pre-declared ~30 min
  anchor, typed-propagation M10).
  Round 2 (2026-11-14, same reviewer): APPROVED — no further design
  review round required. Four LOW precision items, all incorporated
  before RED work: L1 — M4(a) reframed as a pre-solve structural
  allowed-set check rather than empirical zero-pattern equality;
  L2 — M5 pins the single-tree evaluator wiring (reference tree passed
  as both arguments, identity node_map, reference-width rows, mask
  edge_map) and forbids a parallel evaluator; L3 — the standalone
  validator explicitly enumerates identity-bijection/index-range and
  allowed-edge child-closedness rejection branches; L4 — M8 evaluates
  extension-freedom per seat and states the 1e-9 cross-walk chip
  tolerance. Implementation proceeds RED-first on
  mixed_menu.{hpp,cpp} and test_rfc0047_mixed_menu.cpp.
  Implementation review (2026-11-14, same reviewer): APPROVED with no
  blocking/major/medium findings. Confirmed the corrected gap walk is
  the standard NashConv BR recurrence, slot RM+ has no reach-leak, the
  closure/kNoNode semantics preserve the D184 invariant, the single-tree
  evaluator wiring needs no parallel evaluator, and the scope is two new
  bench TUs plus two additive CMake hunks with zero production or
  frozen-TU diff. Two LOW observations addressed before closure: L1 —
  off-closure reach rows documented as shape-only, never consumed;
  L2 — M3b kernel-equivalence gate now runs on both the 8-chip and
  20-chip fixtures. Residual risk is empirical game outcome at the
  optional w1 anchor, not mechanism correctness.

- IMPLEMENTATION RECORD (2026-11-14). Built bench-only
  engine/benchmarks/rfc0047/mixed_menu.{hpp,cpp} and native
  test_rfc0047_mixed_menu.cpp behind the rfc0047 guard; two additive
  engine/CMakeLists.txt hunks (one library source, one ctest target).
  No production, ABI, proto, schema, CLI, or ActionAbstraction change.
  MixedMenuGame is a self-contained slot-based CFR+ sibling of RiverGame
  (terminal/stake/a0/pool conventions reused; traversal copied and
  adapted), with compact allowed-slot reg/sum/sigma, per-node
  reference-edge->slot map, RM+ 1/|allowed| fallback, linear t average,
  and reference-width re-expanded snapshot rows with exact zero bits on
  suppressed edges. RED first (logic_error stub), then GREEN.
  One real defect found and fixed during bring-up: the initial
  allowed-only gap walk RETURNED the sigma-policy value at own nodes, so
  deeper own-node deviations were played by policy instead of best
  response and the NashConv telescope undercounted (all-true mask diff
  ~3.8e2 chips vs the kernel root total). The walk now returns the
  best-response value at every own node while accumulating
  full_forward_reach*(best-policy) locally — the standard NashConv BR
  form. Permanent gate M3b proves an all-allowed mask reproduces the
  identity(0) river kernel's average policy and NashConv BITWISE, which
  independently certifies both the CFR dynamics and the corrected
  telescope. Gates M1-M10 + M3b all pass on the 8-chip tiny root (M8 uses
  the T9 8->20 chip deeper-stack fallback convention). Independent
  implementation review APPROVED (no blocking/major/medium; two LOW
  addressed: shape-only off-closure reach comment, M3b second fixture);
  full rfc0047 ctest regression result recorded below.

- CLOSURE-SCOPING FIX AND RE-REVIEW (2026-11-14). Anchor smoke runs exposed
  that MixedMenuGame::layout() allocated the CFR hot storage (two-seat
  reach_, value_, and the showdown/fold work lists) over the FULL cap-0
  reference tree even though a selective mask reaches only a subset, so
  solver cost scaled with the reference tree rather than the restricted
  game (5-iter w1: RSS peak 1.40 GB, solve 258.7 s). Fix scopes reach_/
  value_ and the terminal work lists to the mask-reachable closure while
  keeping original tree-node-id indexing (off-closure nodes hold empty
  vectors; no index math, header, or external-wiring change). Numerics are
  re-pinned: 5-iter policy hash, bounded NashConv, and both-arm
  exploitability are byte-identical before/after, and M1-M10 plus the
  dual-fixture M3b bitwise kernel-equivalence gate stay green. After:
  RSS peak 713 MiB (inside G1's 1 GiB, ~30% headroom; offline evidence, no
  10 s online claim), 5-iter solve 88.2 s; the ~29.8 s/arm evaluation phase
  is unchanged by design because the embedding BR walk must still traverse
  the full reference tree (M6). Independent targeted re-review APPROVED
  (no code changes; one doc condition to correct the stale "tens of MiB"
  resource prose with these measured figures; one optional debug-assert
  hardening not taken because the gating is structural and the failure
  mode is fail-loud). The empty tree-sized outer layer costs ~2.4 MiB
  (~0.3%) and avoids a node-id remap that would touch every child/snapshot
  boundary. The single layout hunk is held uncommitted until the formal
  300-iteration anchor returns numeric parity, then lands as its own
  isolated perf commit with the resource-prose correction.

## Sparse multi-layer menu isolation (D208)

- D208 OBJECTIVE. Isolate the DEEP abstraction (menu) error from the
  solver UPDATE error on a real tiny native instance, and deliver a
  reproducible bench-only offline SPARSE MULTI-LAYER menu/action
  selection mechanism — not a survey. Reuse the D202 MixedMenuGame
  solver unchanged: it already solves any legal mask, so D208 adds a
  mask-SELECTION primitive plus native gates, owning only
  engine/benchmarks/rfc0047 primitives. No new long anchor (the 50.2 min
  w1 run stays a one-off), no full-root sweep, no grid matrix, no 1e5
  solve; the feedback loop stays on the 8/20-chip tiny fixtures at
  kIters=60.

- WHY D202 ROOT-ONLY WAS INSUFFICIENT (measured). Root widening removed
  only 25.3% of the Uniform pure menu term because widening the root
  OPENS reference-only subtrees, and D202 gave every opened node the
  full cap-0 menu, so fresh restricted nodes appear one layer down; the
  dominant menu mass stays in still-restricted deeper subtrees (the D195
  subtree conclusion on an actually-widened tree). D208 attacks those
  deeper nodes directly but sparsely.

- MECHANISM (new builder in mixed_menu.{hpp,cpp}; solver untouched).
  make_sparse_multilayer_mask(reference, coarse, widened_budget B):
   (1) selection rule W is public/deterministic and deployment
       observable: among coarse-mapped action nodes that still carry a
       suppressed edge, take up to B in (public action depth asc, DFS
       index asc) order, always including the root. The semantic
       deployment key is the parent-chain tuple of incoming_action
       values (a pure public-history path, trivially reconstructible at
       serve time); the DFS node index is only a reproducible bench
       tie-break inside one public depth, never an infoset key, and the
       menu never depends on hole cards, combo, or unobservable reach;
   (2) a node in W gets the FULL reference cap-0 menu (the paid
       widening); every other coarse-mapped node keeps its coarse edge
       subset;
   (3) a reference-only node reached through an opened edge gets the
       SPARSE minimal legal menu = all passive (legality) edges plus the
       single first aggressive edge when one exists — replacing D202's
       full-menu cascade, so growth is bounded and passive edges are
       never removed (the D184 invariant holds). This minimal set is
       always non-empty at source (due==0 gives Check; due>0 gives
       Fold+Call; aggressive rights are simply absent on the no-raise
       early returns), and "first aggressive" is the sorted smallest
       legal target;
   (4) the mapped set is the root DFS closure; mask_to_subset_map and
       validate_mask_subset_map run unchanged, giving a kNoNode
       suppressed subtree and an exact D192/D195 telescope.
  Builder preconditions (review F3), fail-closed invalid_argument:
   - B >= 1; B==0 (or any non-positive budget) throws;
   - when B exceeds the count of eligible suppressed coarse nodes, the
       budget clamps to that count rather than over-reading or padding;
   - if the root itself has no suppressed edge (an already-full-menu
       fixture) the "always include root" precondition cannot be met, so
       the builder throws rather than silently emitting an identity mask.
  Eligible-count zero is logically equivalent to this root case: if the
  root has a suppressed edge it is itself eligible, so the count is >=1;
  no separate zero-eligible branch exists (review F2).
  B maps directly to an action-node/infoset budget; fail-closed typing
  stays with tree_resource_exhausted (M10). Note (review F4): a sparse
  B=1 game is NOT identical to the D202 root-only game — only the
  WIDENED SET is root-only; the opened reference-only subtrees use the
  sparse minimal menu instead of D202's full cascade.

- ERROR ISOLATION. Against the SAME frozen cap0 reference,
  reach_bound_from_snapshot splits exact exploitability into
  bounded_nash_conv_bb (solver UPDATE error, the restricted game's own
  g_g telescope) and pure_menu_term_bb (abstraction/menu error). The
  improvement claim is made ONLY on the pure menu term (the comparable
  fixed-reference quantity); bounded NC across different restricted
  games has no monotonicity theorem (D195) and is reported separately,
  never as "better NC". No NC is compared across different reference
  domains, and no reference deviation action is deleted (the BR walk
  keeps the full menu). There is likewise NO cross-mask monotonicity
  theorem for the pure menu term: widening re-solves the policy and the
  D164 evidence shows the term can move the "wrong" way as mass
  re-splits. A smaller B>=2 pure menu term is therefore an EMPIRICAL
  hypothesis on the fixed fixture, asserted as a loud observed-outcome
  gate (not a structural invariant); a refutation is retained as red
  evidence and triggers the FAILURE DISCIPLINE mechanism switch rather
  than test erosion.

- NATIVE TINY GATES (new test_rfc0047_sparse_multilayer.cpp, RED first).
  S1 RED existence: with budget B=1 (widened set root-only) a deeper
      restricted node n* has a positive menu_gap (the observable deep
      deviation under the full reference BR).
  S2 selection legality and preconditions: W is deterministic,
      root-first, never selects by private state, and fits the declared
      tiny envelope; the builder rejects non-positive budgets (B < 1)
      explicitly — B==0 or a negative budget throws invalid_argument, an
      over-large B clamps to the eligible count without padding, and a
      root with no suppressed edge throws (review F3). The "spans >=2
      public depths when B>=2" clause is an OBSERVED property with the
      8->20-chip fallback (multiple eligible nodes may share one depth on
      a small fixture), not a structural precondition.
  S3 sparse cascade, split by guarantee strength (review F2):
      structurally assert subset inclusion — every edge the sparse mask
      reaches is reachable under the D202 full-cascade mask on the same
      fixture (the minimal menu is a subset of the full cap-0 menu); and
      empirically pin a STRICTLY smaller reachable action-node count
      than the explicitly-constructed D202 mask, with the 8->20-chip
      fallback, since strictness needs a reachable node with >=2
      aggressive edges and does not hold on a degenerate all-in-only
      tree. Opened reference-only nodes carry the minimal legal menu
      (all passive + <=1 aggressive).
  S4 policy legality + exact zeros (reuse M3 conditions).
  S5 full-menu BR preservation + positive option still measurable (M6);
      no reference deviation action is deleted and the BR keeps the full
      cap-0 menu.
  S6 bitwise reach conservation + root mass 1 (M7).
  S7 ERROR ISOLATION (empirical, review F1): on the fixed fixture the
      B>=2 sparse mask's pure_menu_term_bb against the SAME frozen cap-0
      reference is observed strictly below the B=1 mask's term (loud
      observed-outcome gate, not a monotonicity theorem — the term can
      move either way when the policy re-splits); each mask's
      bounded_nash_conv_bb is reported independently with no cross-game
      NC comparison; the subtree extension-free zero-gap pin (M8) still
      holds. A refutation is kept as red evidence and triggers the
      FAILURE DISCIPLINE switch.
  S8 determinism and all-true kernel equivalence stay green (M3b/M9).
  S9 typed resource fail-close and nodewise storage bound (M10).

- FAILURE DISCIPLINE. If a counterexample refutes sparse multi-layer
  widening on the tiny fixture, retain the red evidence and reason and
  switch to another authorized reachable small mechanism (Path A/B or
  grid/turn-CV) rather than parking on an unreachable report. The
  frozen 0.002/B0/G1/cap0/cap1/budgets are unchanged; every row stays
  exploratory/supporting evidence and can never be reported as M2/G5.

- D208 IMPLEMENTATION RECORD (2026-11-14). Added
  make_sparse_multilayer_mask to mixed_menu.{hpp,cpp} (the D202 solver,
  evaluator, and validators are unchanged) and native
  test_rfc0047_sparse_multilayer.cpp with gates S1-S9; one additive
  CMake ctest target. RED first (logic_error stub), then GREEN.
  RED->GREEN corrected one test-design flaw (the builder itself was
  always correct, confirmed by an isolated per-budget probe): the first
  S3 tried to assert the sparse closure was nested in the D202 root
  mask and built an invalid "full menu over the sparse region" mask
  (marking sparse-reachable nodes all-true opens edges to nodes left
  all-false, so validate_mask's closure saw a reachable empty node).
  The corrected S3 uses the always-legal all-true cap-0 reference mask
  and asserts edge-wise subset inclusion (every sparse-allowed edge is a
  real reference edge), plus minimal-menu legality at opened
  reference-only nodes (all passive retained, <=1 aggressive) and the
  nodewise storage bound. The sparse B=2 mask is not nested in D202's
  root mask: it reaches 16 action nodes vs 12 because it widens a deeper
  node D202 leaves coarse (observed, not a guarantee).
  TINY RESULT (8-chip root, 60 iterations, Uniform arm, same cap-0
  reference): pure menu term B=1 1.49051376 BB -> B=2 1.15447102 BB, a
  22.5% reduction in the MENU/abstraction error from widening one
  deeper public node; bounded NashConv (the solver UPDATE error) is
  reported independently per mask (B=1 0.01567429, B=2 0.01824050 BB)
  with no cross-game ordering. This is the requested native isolation:
  a small, reproducible selection rule measurably reduces the menu
  component on a real fixture while the two error terms stay separated.
  All outputs stay exploratory; no long anchor, sweep, or matrix was run
  and nothing is reported as an M2/G5 pass.

- D208 IMPLEMENTATION REVIEW (2026-11-14, same independent reviewer):
  APPROVED with no blocking/major/medium findings. Confirmed the builder
  is correct and minimal (one appended free function; the D202 solver,
  evaluators, and validators are unchanged), the corrected edge-wise S3
  is the right structural assertion, passive/non-empty/determinism are
  guaranteed at source and re-pinned by the validator, and the S7
  error-isolation framing has no erosion path. Four LOW items all closed
  before commit: F1 — replaced the tautological clamp test with an
  independent eligible-count check proving std::min clamps to exactly
  the eligible node count; F2 — added the root-with-no-suppressed-edge
  rejection case (reference-as-coarse identity map); F3 — turned S1's
  fallback message into a real parameterized 8->20-chip retry matching
  T9/M8; F4 — restored the trailing newline and ran the format target.
  Hardened S1-S9 stay green; full rfc0047 regression is the separate
  cross-component check. All D208 output is bench-only exploratory
  evidence with no M2/G5 implication.

## Pot-fraction menu selection within the reference action domain (D217)

- D217 OBJECTIVE. A pre-registered canonical POT-FRACTION action selector
  that picks an existing frozen cap-0 aggressive action at the restricted
  reference-only nodes, replacing D208's fixed first-aggressive choice,
  while reusing the D208 MixedMenuGame builder/solver, the same W rule,
  the frozen cap-0 full BR, and the D192/D195 error decomposition.
  Bench-only; no new public ActionAbstraction API, no synthesized bet/
  raise amount, no reference game-tree/root/stake change, no sweep. One
  fixed fraction and one tiny budget this milestone.

- PRE-REGISTERED RULES (fixed before any run; reviewed independently).
  Canonical fraction f = 1.0 (pot-sized). All quantities are
  deployment-observable public state reconstructed from the parent-chain
  incoming_action history and the GameDef root — no hole cards, opponent
  reach, or hidden history.
  CORRECTION 2026-11-14 (post review round 1, before any valid ledger):
  the first formulation credited the gross root blind contributions
  (def.contributions, which are already dead money inside def.pot) as the
  per-seat starting commitment. On a river-rooted tree the betting street
  starts fresh, so the correct state is street-cumulative: street_committed
  starts at zero and pot starts at def.pot. No run result informed this
  correction; the fraction, budget, ordering, and every gate are unchanged.
   (1) Pot reconstruction (single river street; TerminalDepth::River,
       five board cards, so street commitments never reset and
       close_street/refund_unmatched cannot occur inside a replayed river
       path). Start pot = def.pot and street_committed[seat] = 0. Replay
       each public action root->node with the mover taken from the PARENT
       node's actor (the seat that acted to create the child; node.actor is
       the next player and is kNoNode on terminals — review F1):
       Fold/Check change nothing; Call adds call_paid = min(due,
       def.stacks[actor] - street_committed[actor]) where due =
       max(street_committed) - street_committed[actor], and the min-clamp
       makes a short all-in call exact for arbitrary starting stacks;
       Bet/Raise with street-cumulative target_total T adds
       (T - street_committed[actor]) to pot and sets
       street_committed[actor] = T, matching HeadsUpState
       paid = target_total - street_committed (heads_up.cpp:220). After
       EVERY replay step check the invariant
       pot == def.pot + street_committed[0] + street_committed[1] via an
       NDEBUG-independent throw (review F3; a cassert disappears under
       -DNDEBUG). Fold rows keep the pre-refund gross pot: the production
       unmatched-bet refund is a terminal settlement and choosers consume
       action-node states only, where no refund has occurred. This uses
       only public history, not the solve-only detail projection header.
   (2) Due at the node: due = max(street_committed) -
       street_committed[actor]; zero means a betting (not calling) node.
       A structural cross-check (review F2, now executed against the real
       replay outputs at every mapped node) must agree with the real menu:
       due>0 implies Fold/Call present and no Check; due==0 implies no
       Fold/Call (the iff in the positive direction, since a zero-due node
       may check through with no Bet offered).
   (3) Pot-sized IDEAL ADDITIONAL PAYMENT (not a literal street
       target_total). due==0 (a bet): ideal additional = pot, and the
       actor's street commitment is zero on every river due==0 node (a
       call ends the river street, so no round reopens with money
       outstanding). due>0 (a raise): ideal additional = pot + 2*due (call
       due, then raise the post-call pot pot+due; facing bet B into pot P
       this is P+3B). The selector works in additional-payment space and
       never asserts equality with a literal target_total.
   (4) Selection: among the node's EXISTING legal aggressive reference
       actions (Bet when due==0, Raise when due>0) choose the one
       minimizing |additional_payment(action) - ideal_additional| where
       additional_payment = action.target_total - street_committed[actor];
       the street-base offset is a node-constant across the candidate
       actions, so the argmin matches comparing literal target_total.
       Equidistant tie-break is deterministic toward the SMALLER
       target_total (the menu is sorted ascending with distinct totals, so
       this is the lower index), pinned by a synthetic-node unit test on
       the production comparator itself (review F4; the earlier test
       exercised a re-implemented copy). The nearest-legal rule is
       self-bounding to the legal set — no amount is synthesized. When no
       aggressive action is legal, keep the legal passive edges only.
       Every passive legality edge is always retained (the D184/D202
       invariant), and non-selected aggressive edges map to suppressed (-1)
       reference edges.
  The menu depends only on the public node (pot/due/legal menu), so it is
  identical for every combo at that infoset.

- SINGLE-FACTOR ARM. The widened set W, its budget, and its (public depth,
  DFS index) rule are byte-identical to D208; W nodes keep the full
  reference menu and coarse-mapped non-W nodes keep their coarse subset.
  The ONLY changed factor is the aggressive choice at reference-only
  (kNoNode) restricted nodes: D208 first-aggressive -> D217
  pot-fraction-nearest. Any reach difference caused by the different
  chosen edge is a natural consequence and is reported as a separate
  observed quantity, not folded into the selection rule. Changing W would
  be a separately named arm, not done here. Review F4 enforces this by
  construction: the eligible enumeration, (depth,DFS) sort, W widening,
  and root-DFS closure are factored into ONE shared helper parameterized
  by a chooser(reference,node,pot,due) callable that returns the chosen
  aggressive edge index (or none); D208 passes the first-aggressive
  chooser and D217 the pot-nearest chooser, so the common machinery cannot
  drift and P7's same-input ledger has a structural basis.

- NATIVE TINY GATES (new test_rfc0047_pot_fraction.cpp, RED first).
  P1 RED distinctness: on the same real reference situation the
      pot-fraction selector returns a legal aggressive reference action
      whose target_total differs from the first aggressive edge at least
      once (8-chip, 20-chip fallback; same fixed fraction, no budget
      reset).
  P2 in-domain legality: every selected action is an existing reference
      node.actions entry (no synthesized amount), all passive edges
      retained, suppression is aggressive-only, rows non-empty; the
      derived mask passes validate_mask and validate_mask_subset_map.
      Consumes the actual replay_public_states outputs (review F2) at
      every mapped node: the pot invariant
      pot == def.pot + street_committed[0] + street_committed[1] holds
      including at terminals (the F1 OOB site), and the reconstructed due
      classification matches the materialized menu (due>0 implies
      Fold/Call with no Check; due==0 implies no Fold/Call), and every
      selected aggressive edge has the type the classification predicts.
      The at-most-one-aggressive rule is scoped to reference-only nodes
      (base kNoNode), matching D208 S3 (review round-2 Low-1).
  P2b street-start contract (review round-2 Low-2): a settlement-valid
      mid-street root whose GameDef carries nonzero street_committed is
      rejected by replay_public_states with std::invalid_argument before
      any replay step.
  P3 public-only/determinism: repeated builds are bitwise identical; the
      selector reads only pot/due/menu, never combo; same infoset -> same
      menu. Plus the tie-break unit test on the production comparator
      itself (review F4): feed nearest_aggressive_choice synthetic
      (ideal, legal-target) inputs including an exact equidistant pair
      and assert the lower target total wins, so the comparator is pinned
      even if no natural tiny node is equidistant.
  P4 policy legality with exact zero bits on non-selected edges (M3).
  P5 full cap-0 BR preservation with positive option still measurable
      (M6); the reference BR walk keeps every deviation action.
  P6 bitwise reach-EV conservation and root mass 1 (M7).
  P7 same-input error ledger vs the D208 first-aggressive baseline at the
      IDENTICAL W/budget/tie-break family: NC, pure_menu_term, e_embed
      under both arms, node/edge counts, wall and peak RSS; the only
      attributed difference is the restricted-node aggressive factor.
      No cross-game NC monotonicity claim; improvement is attributed to a
      specific component and any solver-error offset is reported.
  P8 determinism + all-true kernel equivalence inherited (M3b/M9).
  P9 typed resource fail-close and nodewise storage bound (M10).

- FAILURE DISCIPLINE. If the fixed pot-sized rule is refuted on both tiny
  fixtures (no distinct legal aggressive action, or no menu-term
  improvement), retain the red counterexample and switch to another
  reachable same-domain/same-budget small selector or a shallow frontier
  mechanism; do not lower the gate, change the fraction, reset the budget,
  or open a new action domain / Path-A blueprint / Path-B certificate.
  Frozen cap0/cap1, 0.002, B0=0.010, G1, root weights, threads, and the
  60-iteration tiny budget are unchanged; no 300-iter/50-min anchor.

- D217 IMPLEMENTATION RECORD (2026-11-14). Refactored the D208 builder into
  a chooser-parameterized shared build_sparse_mask (eligible enumeration,
  public-depth/DFS sort, W widening, root-DFS closure, and validation
  shared by construction per review F4); D208 now passes the
  first-aggressive chooser through that shared helper. The refactor itself
  is behavior-preserving: on an identical mask input the seedless solver
  produces identical outputs and the D208 S1-S9 suite stays green as the
  regression guard. Separately, the D208 arm differs from the D202
  root-only arm by construction (16 vs 12 reachable nodes; pure menu
  1.49051376 -> 1.15447102 BB; same bounded NC) — those figures compare the
  two arms, not the code before and after the refactor. Added
  replay_public_states (public-history street-cumulative pot/due, the
  short-all-in min-clamp, an explicit street-start root precondition, and
  an NDEBUG-independent per-step pot invariant),
  nearest_aggressive_choice (additional-payment argmin with
  lower-target equidistance, unit-tested on the production comparator
  itself per F4), pot_fraction_choice (canonical f=1.0), and
  make_pot_fraction_mask; new native
  test_rfc0047_pot_fraction.cpp P1-P9, RED stub first then GREEN.
  REVIEW ROUND 1 (agent a56fda80e49e62818) found a blocking chain:
  F1 the replay credited node.actor (the next player, kNoNode on
  terminals) as the mover instead of the parent node's actor, causing an
  out-of-bounds std::array write at every terminal whose corrupted state
  was simply never consumed; F2 the P2 gate never invoked the replay;
  F3 the invariant was a cassert compiled out under -DNDEBUG; F4 the
  tie-break test exercised a re-implemented copy of the comparator.
  Root-cause analysis past F1 exposed the deeper gross-vs-street
  conflation described in the correction block above: the replay seeded
  street commitments from def.contributions (dead money already inside
  def.pot), which on a river-rooted street misreads every
  street-cumulative target_total. All four findings plus the street model
  are fixed in this revision; the pre-fix ledger (20-chip node 132,
  pure menu 2.96632063 -> 2.88701315, bounded NC 0.02477526 ->
  0.02208551) is INVALID and superseded — it was produced by the broken
  replay and no node-132 split exists under the corrected model.
  VALID LEDGER (corrected replay; release; seedless; 60 iterations;
  budget 2; identical W by construction). The first mask split is on the
  8-chip fixture itself at node 27, no 20-chip fallback needed. At node
  27 the replay reports pot=7, due=3 (the actor faces a 3-chip bet into
  the 4-chip root pot), so the pot-sized ideal additional is
  7 + 2*3 = 13; the 8-chip stack behind caps the legal raise-to domain
  at targets {6, 8} (additional payments 6 and 8 from a zero street
  base), and the nearest-legal rule self-bounds to target 8
  (distances 7 vs 5) where the first-aggressive arm keeps target 6. No
  amount is synthesized: an ideal above the legal maximum simply resolves
  to the largest legal action by the pre-registered argmin. The
  same-input ledger at identical W shows the pot-fraction factor reduces
  the pure menu term 1.15447102 -> 1.14819095 BB (-0.54%, the abstraction
  component) while bounded NashConv moves 0.01824050 -> 0.03203123
  (worse, as expected for a different restricted game at the same tiny
  iteration budget; reported with no cross-game monotonicity claim — the
  two components are independently measured and the menu-term reduction
  is the single-factor attribution). Release P1-P9 plus the D208/D202
  suites are green; the ASan/UBSan instrument run is recorded separately
  below once complete (review round-2 Medium-1). The cap-0
  tree/oracle/domain are unchanged, and every gate stays exploratory
  with no M2/G5 claim; no long anchor, sweep, or fraction matrix was run.

- D217 REVIEW ROUND 2 (2026-11-14, independent non-author agent, separate
  launch): verdict Approved. F1-F4 were verified fixed at the code level
  and all seven math challenges passed re-derivation (pot-sized ideal
  P+2B; node-27 arithmetic; unsigned underflow ruled out by production
  legality; strict-less equidistance; single-factor integrity; P2
  directional implications and structural XOR; D208 behavior
  preservation). One evidence gate and three Low findings:
  Medium-1 (evidence, not a code defect): the F1 array OOB needs an
  ASan/UBSan clean run of the three mask suites for instrument-level
  closure; a release pass cannot substitute. The run is executed against
  the final source and its result recorded in the ASAN EVIDENCE block
  below before this milestone closes.
  Low-1 (fixed): P2's at-most-one-aggressive check was scoped by "edge_map
  contains -1", which also catches coarse-mapped nodes allowed several
  coarse aggressive edges. Now scoped to reference-only nodes
  (base.node_map[n] == kNoNode), aligned with the D208 S3 gate.
  Low-2 (fixed): the replay's street-start assumption was implicit. It is
  now an explicit fail-loud contract — replay_public_states throws
  std::invalid_argument when any def.street_committed is nonzero — with
  the header documenting the precondition; new native gate P2b builds a
  settlement-valid mid-street root (contributions {4,2}, pot 6, stacks
  {6,8}, street_committed {2,0}, live raise rights) and asserts the
  rejection. The guard's exception type distinguishes it from the later
  pot-invariant logic_error, so the gate exercises the new contract.
  Low-3 (fixed): the implementation record no longer brackets D202-vs-D208
  arm figures (16 vs 12 nodes; 1.4905 -> 1.1545) inside the
  refactor-preservation sentence; the refactor claim and the arm
  comparison are now stated separately.

- D217 ASAN EVIDENCE (2026-11-14; closes review round-2 Medium-1). Built
  with the asan preset (BIGSHARK_ENABLE_SANITIZERS=ON, AddressSanitizer
  plus UndefinedBehaviorSanitizer) from the final source including the
  Low-1/Low-2/P2b changes and the comment-only label corrections, then
  run through ctest --preset asan with
  ASAN_OPTIONS=halt_on_error=1 and
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:
    rfc0047_mixed_menu ....... Passed (1277.18 s)
    rfc0047_sparse_multilayer  Passed (405.38 s)
    rfc0047_pot_fraction ...... Passed (348.68 s)
  100% tests passed out of 3, no sanitizer diagnostic or abort. The
  instrument run traverses replay_public_states at every terminal (the
  original F1 out-of-bounds site) in all three suites via the shared
  build_sparse_mask path, and rfc0047_pot_fraction additionally
  exercises the new P2b mid-street invalid_argument contract, so the
  F1 fix has instrument-level, not merely code-level, closure.

## Counterexample-driven single-edge witness augmentation (D225)

- D225 OBJECTIVE. A bench-only deterministic selector that turns SEALED
  per-node best-response evidence (the ReachBoundResult produced from a
  frozen baseline snapshot) into exactly one public-node, one-edge menu
  augmentation over the immutable cap-0 reference, then builds a legal
  mask and — only inside the verified tiny cumulative budget — performs
  one fresh same-source re-solve with its own reach/BR/decomposition.
  It is the data-driven successor to the two fixed-rule arms: D208 picks
  the sorted first aggressive edge, D217 picks the pot-nearest edge by a
  pre-registered formula, while D225 picks the edge the sealed BR
  evidence says carries menu option value. No fixed rule, no sweep, one
  candidate per fixture.

- PRE-REGISTRATION (frozen before any implementation or run).
  Frozen inputs and identity.
   (F1) Fixture: the same 8-chip river-root tiny fixture as D208/D217
       (stacks {8,8}, pot 4, gross contributions {2,2} already dead
       money, board 2c 3d 7h Js 9c, coarse Coarse cap-1 grid, reference
       identity(0), 60 seedless CFR+ iterations, 1 thread).
   (F2) Baseline mask m0 = make_sparse_multilayer_mask(ref, coarse, 2)
       (the D208 first-aggressive arm). Frozen snapshot
       sigma0 = solve_mixed_menu(def, identity(0), m0, 60, 1). Sealed
       evidence E = reach_bound_from_snapshot(def, sigma0.river, ref,
       ref, mask_to_subset_map(ref, m0), Uniform). E carries four
       abstraction/shape digests that the selection result records for
       provenance; the selector never re-solves and never walks the BR
       tree itself.
  Public-node qualification. A mapped action node n is eligible iff
   (Q1) E contains a NodeGapRecord keyed by n for its acting seat (each
       action node has exactly one acting-seat record),
   (Q2) the record edge_map contains at least one -1 entry whose
       reference action is aggressive (a suppressed legal aggressive
       edge; passive edges can never be suppressed),
   (Q3) the node score defined below is strictly positive above a fixed
       1e-12-chip FP floor.
  Legal combo set and normalization.
   (L1) L(n) = the combos i with own_reach[i] > 0 in the sealed record,
       i.e. the combos for which sigma0 actually reaches n as the acting
       seat. Weight w_i = own_reach[i] taken verbatim from E; no
       recomputation, no renormalization (all rankings are invariant to
       a common positive scale, and the raw chip scale stays comparable
       to the D192/D195 telescope ledger). Opponent reach is not used:
       it belongs to the structural certificate, not menu option value.
  Single (n,a) scoring (policy residual excluded by construction).
   (S1) Per combo i let A be the node menu width and C(i,a) =
       continuation[i*A+a]. Bounded best
       C*_b(i) = max over edges with edge_map[a] != -1 of C(i,a).
       Option value v(i) = max(0, max_a C(i,a) - C*_b(i)). The selector
       computes C*_b, v, and every edge score directly from
       continuation; the stored identity
       delta[i] - bounded_gap[i] = max_a C(i,a) - C*_b(i) (the
       policy-weighted value cancels algebraically because record_node
       subtracts the same stored policy_value) is used only as a
       cross-check: |(delta-bounded_gap) - (max C - C*_b)| <= 1e-9
       chips per combo, matching the existing kChipTol scale, since
       floating reassociation preserves sign but not bit equality. The
       selector never treats total delta as removable menu benefit.
   (S2) Node score S(n) = canonical reduction of the terms
       {w_i * v(i) : i in L(n)}. CANONICAL REDUCTION (review round-1
       Major-1): materialize every product term, reject the record with
       std::invalid_argument if any term or weight is non-finite
       (an isfinite guard that also protects the later sort's strict
       weak order), then std::sort the terms by numeric value and sum
       them sequentially in that fixed order. Sorting makes the reduced
       multiset and hence the rounded sum identical under any combo-row
       permutation, so permutation invariance holds bitwise rather than
       probabilistically; cost is at most 1081 elements per node.
   (S3) Node pick via ONE global scan over every record across both
       seats with the key (S(n) descending, reference_node ascending);
       nested per-seat loops are forbidden so an exact score tie cannot
       be hijacked by seat ordering (review Low-1). n* is the first
       key; only nodes passing Q1-Q3 enter the scan.
   (S4) Edge score at n* for each suppressed aggressive edge a uses the
       same canonical reduction over
       {w_i * max(0, C(i,a) - C*_b(i)) : i in L(n*)}. Edge pick
       a* = argmax Q(a); exact ties resolve to the smallest edge index
       (menus are sorted ascending with distinct targets, so this is
       the smaller target_total). Witness requires Q(a*) > 1e-12;
       otherwise the result is kNoCandidate and the selector terminates
       immediately WITHOUT falling back to a second-best node or edge
       (review Low-3; one candidate per fixture per T1).
  Determinism and private-info boundary.
   (D1) All scores use the canonical sorted reduction with index-only
       node/edge tie-breaks; no RNG, no per-combo output. The result is
       one public (n*,a*), independent of hole-card order: permuting the
       combo rows of a copied evidence object produces the identical
       ordered term sequences and therefore the bitwise identical
       witness (this is structural under IEEE-754 default rounding, the
       same premise the existing bitwise reach contract already relies
       on). Runtime menus stay a function of public node identity alone
       (the existing MixedMenuMask has no combo dimension).
  Stopping condition.
   (T1) Exactly one refinement per fixture: one node, one edge, one
       candidate. No re-scoring after augmentation, no second node, no
       fraction/budget/root sweep.
  Augmentation and closure.
   (A1) m1 starts as a copy of m0 with allowed[n*][a*] flipped to true
       after validating n* is an action node, a* is in range,
       aggressive, and currently suppressed.
   (A2) Root-DFS closure: an action node newly made reachable whose row
       is still all-false receives the D208 sparse minimal legal menu
       (all passive edges plus the first aggressive edge via the shared
       first_aggressive_choice; passive-only when none exists). This is
       deterministic legality filling, not a second scored selection.
       The m0->m1 diff therefore has a mandatory three-layer structure
       (gated explicitly in W2/W4): (i) among nodes reachable in m0 the
       only flipped bit is the single witness (n*,a*); (ii) every other
       flipped bit sits on a node unreachable in m0; (iii) each such
       newly-reachable row equals exactly the sparse-minimal rule (all
       passive edges plus at most the first aggressive edge). The scored
       witness edge count is exactly one even though closure flips
       additional mechanical bits.
   (A3) m1 passes validate_mask, mask_to_subset_map, and
       validate_mask_subset_map unchanged.
  No-candidate and fail-close contracts.
   (E1) No suppressed aggressive edge, or no positive score:
       WitnessStatus::kNoCandidate; no augmentation is attempted.
   (E2) Invalid node/edge index, a passive witness edge, an
       already-allowed witness edge, or a sealed record row with zero
       non-suppressed (bounded) edges — the state that would make
       C*_b negative infinity and propagate inf into the scores —
       throws std::invalid_argument. A record whose edge_map disagrees
       with the mask at the chosen node also throws invalid_argument
       (sealed evidence/mask inconsistency). The zero-bounded-edge case
       is unreachable on genuine sealed evidence (validate_mask
       guarantees a non-empty reachable row) but can occur inside a
       hand-built Fb record, so it is an explicit enumerated guard
       (review Medium-3).
   (E3) A cumulative RefinementBudget charges only real solves;
       selection and augmentation cost zero tokens. A charge with no
       remaining token throws the typed witness_budget_exhausted
       BEFORE any solver call; the budget is monotone-decreasing and
       exposes no reset.
  Conditional re-solve path (only with verified tiny balance).
   (R1) Charge one token, then sigma1 = solve_mixed_menu(def,
       identity(0), m1, 60, 1): a fresh CFR+ run with its own regret,
       average, and forward reach — sigma0 reach or certificates are
       never reused. E1b = reach_bound_from_snapshot on sigma1 with the
       m1 map: fresh full cap-0 BR, bounded NC, pure menu term,
       e_embed, storage, wall, and RSS. Report m0 vs m1 side by side
       with no monotonicity claim; any worsening is retained as
       measured. The witness edge's average policy mass under sigma1 is
       observed and reported (legality gated, not value gated).
  Budget ledger (cumulative, no reset; unknown external balance = 0).
   (B1) Selection/augmentation need no solve: at zero external balance
       the selector, mask construction, and all legality/invariance
       gates are still fully deliverable from sealed records. The
       re-solve ledger is the explicit conditional path, charged
       against the same tiny 60-iteration fixture budget class as the
       D208/D217 native gates (one baseline solve + one candidate solve
       + two snapshot BR walks, all on the 8-chip root). No large-root
       BR, full-combo census, training, 1e5 solve, 300-iteration anchor,
       or root scan is authorized.
  Fallback if sealed records cannot locate an edge.
   (Fb) If the sealed evidence yields kNoCandidate, a minimal fixture
       may explicitly construct a verifiable counterexample record set;
       it is labeled mechanism-validation evidence, never a real-root
       witness, and no large-root witness is fabricated. The constructed
       counterexample has a field-level contract (review Medium-1):
        (a) a reference tree node whose menu contains the passive edges
            plus at least two aggressive edges;
        (b) a paired mask row in which the passive edges and the first
            aggressive edge are allowed and the target aggressive edge
            is suppressed;
        (c) a NodeGapRecord with a 1081-wide own_reach carrying at least
            one strictly positive entry, a 1081*A continuation in which
            for some positive-reach combo the suppressed edge strictly
            exceeds the bounded best, an edge_map bitwise identical to
            the mask row, delta/bounded_gap consistent with the S1
            identity, and widened == false;
        (d) the full W2/W3/W4 gate set runs against it under the
            mechanism-validation label, including the S1 identity
            cross-check (review Low-5): recomputing
            max_a C - C*_b from continuation must match
            delta - bounded_gap within 1e-9 chips per combo.

- NATIVE GATES (new engine/tests/test_rfc0047_witness_augment.cpp,
  RED stub first, real behavior only, no source-text/log matching).
  W1 witness existence RED: selection on the sealed 8-chip evidence
      returns kSelected and a suppressed aggressive legal edge.
  W2 behavioral distinction and diff classification: m1 differs from
      m0, and the diff obeys the A2 three-layer contract — among
      m0-reachable nodes exactly the one scored flip (n*,a*) changes,
      every other changed bit is on an m0-unreachable node, and each
      such row is exactly the sparse-minimal menu; m1 also differs from
      the D217 pot mask as a whole because n* is coarse-mapped and D217
      leaves its suppressed coarse edge untouched. The witness edge was
      suppressed in m0.
  W3 determinism and private-combo permutation invariance: repeated
      selections are bitwise identical, and a combo-row-permuted copy of
      the sealed records (own_reach, delta, bounded_gap, continuation
      permuted together) yields the same (n*,a*), guaranteed by the
      canonical sorted reduction.
  W4 legality: in-range aggressive previously-suppressed edge, every
      passive edge retained, validate_mask and subset-map validators
      green, scored witness edge count exactly one per the A2
      classification.
  W4b identity cross-check (review Low-5): for every scanned record the
      per-combo option value recomputed from continuation
      (max_a C - C*_b) matches stored delta - bounded_gap within 1e-9
      chips; the same check runs on any Fb constructed record.
  W5 no-candidate typed no-op: full-menu evidence (no suppressed edge)
      returns kNoCandidate and leaves the mask untouched.
  W6 fail-close: bad node/edge index, passive witness, already-allowed
      witness, evidence/mask disagreement throw invalid_argument;
      constructed-record rows also cover the S2 non-finite-term guard
      and the E2 zero-bounded-edge guard (each asserted with
      invalid_argument); an exhausted budget throws
      witness_budget_exhausted before a solve.
  W7 closure and kernel regression: the m1 mapped set is its allowed
      DFS closure; augmenting an all-true mask cannot produce a
      candidate (no-op path), preserving the M3b all-true kernel
      equivalence established by the mixed-menu suite.
  W8 conditional re-solve (balance present): sigma1 forward reach
      validates bitwise for both seats via validate_supplied_reach,
      all ledger quantities finite, witness-edge mass reported; the m1
      map and snapshot are consumed by the unchanged reach-bound
      instrument.
  W9 budget semantics: one shared monotone budget across
      baseline/select/validate/retry, zero-cost selection asserted (no
      token charged), exhaustion typed and pre-solve.

- FAILURE DISCIPLINE. If the selector is refuted (no positive-score
  witness on the tiny fixture and the constructed fallback is also
  rejected), retain the red evidence and stop the mechanism; do not
  lower the 1e-12 floor, change the scoring, sweep parameters, or expand
  the action domain. Frozen cap0/cap1, 0.002, B0=0.010, G1, root weights,
  threads, the 60-iteration tiny budget, and the no-long-anchor
  constraints are unchanged. Every row is labeled
  supporting-evidence-not-m2-g5-pass; heuristic scores and old evidence
  are never benefit or certification guarantees.

- D225 REVIEW ROUND 1 (independent non-author agent, separate launch,
  pre-implementation): verdict Changes Requested. The central
  delta-minus-bounded-gap identity, closure legality, one-record-per-node
  structure, zero-balance deliverable, and re-solve independence were all
  verified against the code; four blocking/major-or-medium findings
  plus five lows required pre-registration revision before
  implementation (failure discipline forbids changing the scoring after
  a run):
  Major-1 (fixed in text): plain sequential summation cannot
  structurally guarantee bitwise permutation invariance. Adopted the
  recommended canonical reduction (materialize terms, isfinite guard,
  std::sort by value, fixed-order sum) in S2/S4 and D1; W3 now asserts a
  structural property.
  Medium-1 (fixed): Fb now has a field-level constructed-record contract
  (a)-(d) including the identity cross-check.
  Medium-2 (fixed): W2/W4 now assert the A2 three-layer diff
  classification (one scored flip among m0-reachable nodes; other flips
  only on m0-unreachable rows; each exactly sparse-minimal).
  Medium-3 (fixed): a record row with zero bounded edges is an explicit
  invalid_argument case in E2.
  Low-1 (fixed): S3 is one global scan keyed (score desc, node asc), no
  nested per-seat loops. Low-2 (fixed): C*_b/v/Q are computed from
  continuation; delta-bounded_gap is a 1e-9 cross-check only. Low-3
  (fixed): S4 kNoCandidate terminates with no second-node fallback.
  Low-4 (fixed): RFC wording no longer claims tree/mask reads come from
  the records alone. Low-5 (fixed): new W4b identity cross-check gate
  for real and Fb records.
  Accepted residual risks (kept in the ledger): option values inherit
  the Uniform completion arm; single-edge greedy Q has no optimality
  claim; W1 remains empirical and may force the Fb path; cross-platform
  bitwise equality rests on the same IEEE-754 premise as the existing
  reach contract; re-solve worsening is retained as measured.

- D225 IMPLEMENTATION RECORD (2026-11-14). After review round-2 closure,
  implemented the witness selector in mixed_menu.{hpp,cpp} (same
  translation unit, reusing the anonymous-namespace is_aggressive and
  first_aggressive_choice helpers plus the existing validators) and the
  native suite engine/tests/test_rfc0047_witness_augment.cpp, with one
  BUILD_TESTING CMake hunk. RED was observed first: the stub selector
  returned kNoCandidate and W1 failed at its kSelected assertion; the
  real implementation then turned every gate GREEN. New bench-visible
  surface: RefinementBudget (monotone-only solve tokens, typed
  witness_budget_exhausted distinct from invalid_argument),
  WitnessSelection (with the four sealed digests for provenance),
  select_witness_augmentation, and apply_witness_augmentation. The
  selector performs no solve and no BR walk; canonical sorted
  reductions, the global (score desc, node asc) scan, the 1e-9
  identity cross-check, and the 1e-12 floor follow the frozen
  pre-registration exactly.
  VALID TINY LEDGER (8-chip river root, 60 seedless iterations, budget
  class identical to D208/D217; release; deterministic across repeated
  runs to the printed digits). The sealed D208 evidence selects public
  node 48, reference edge 2 (a suppressed aggressive action), with
  node_score 1176155.38345900 and edge_score 1147791.32305174 raw
  reach-weighted option units. After the single-edge augmentation and a
  FRESH re-solve (its own regret, average, forward reach, validated
  bitwise for both seats, and a fresh full cap-0 reach-bound pass):
  pure menu term 1.15447102 -> 0.74330806 BB (-35.6%, the abstraction
  component) versus only -0.54% for the D217 fixed pot rule on the same
  baseline, while bounded NashConv moves 0.01824050 -> 0.02718028
  (worse; no cross-game monotonicity claim, retained as measured). The
  witness edge gains real average-policy mass under sigma1
  (witness_mass 308.477329 summed over the 1081 combos), confirming the
  new edge is legal and reachable rather than a zero-probability
  placeholder. Suite wall about 7.5 s end to end, RSS peak about
  14.4 MiB; one baseline solve + one candidate solve + two snapshot BR
  walks, no extra solve for selection itself.
  Interpretation stays strictly exploratory: the score is a heuristic
  witness derived from sealed Uniform-completion evidence, not a benefit
  or certification guarantee; the mechanism validates that
  counterexample-driven per-edge selection can target menu error far
  more sharply than either fixed rule at the same W, while the bounded
  NC worsening re-confirms the D195/D198/D202/D217 conclusion that
  menu and solver-update errors move independently and the binding wall
  remains policy convergence. No M2/G5 claim; no long anchor, sweep,
  large-root BR, census, or training was run; cap0/cap1, 0.002,
  B0=0.010, G1, root weights, threads, and the 60-iteration tiny budget
  are unchanged. The Fb constructed-record path was not needed because
  genuine sealed evidence produced a positive witness; its field
  contract remains registered for a future no-candidate fixture.

- D225 IMPLEMENTATION REVIEW (round 3, same independent agent, static):
  verdict Approved, no blocking/major/medium. The implementation matches
  the frozen pre-registration on every rule (canonical reduction, global
  score-desc/node-asc scan, floor placement, no fallback, sparse-minimal
  closure, W4b identity as a cross-check only); ScoredRecord pointers
  stay within the unmutated const evidence; the permutation gate has
  real discriminating power (naive summation would break bitwise
  equality under the 137i+29 bijection on 1081=23*47 combos); selection
  has no hidden solve/BR; scope is bench-only with no M2/G5 overreach and
  the m0 ledger matches the D208 B=2 figures bit-for-bit. Five Low
  hardening findings, all adopted before closure:
  Low-1: the zero-bounded-edge guard is now evaluated from edge_map
  alone BEFORE the mask-agreement and passive-edge checks, so an
  all-suppressed constructed record reaches its own diagnosis instead
  of the agreement branch.
  Low-2: a W6 gate now corrupts delta at a positive-reach combo and
  asserts the W4b identity guard throws; the L(n) scope (zero-reach
  combos skipped, never scored) is documented at the check.
  Low-3: W2 layer (iii) now pins the single allowed aggressive edge of
  each newly-opened row to the exported first_aggressive_choice output
  (promoted to external linkage, same body as the D208 chooser) rather
  than re-implementing the comparison.
  Low-4: negative own_reach now throws invalid_argument explicitly
  (defense in depth; upstream reach validation already forbids it), with
  a W6 constructed-record gate.
  Low-5: the already-allowed-edge W6 case scans for a genuine allowed
  aggressive node and CHECK-fails rather than silently falling back to a
  passive index.
  After these changes the W1-W9 suite is green again and the happy-path
  ledger is byte-identical (node 48, edge 2, pure menu 0.74330806 BB),
  confirming the hardening changed only malformed-input and test paths.
