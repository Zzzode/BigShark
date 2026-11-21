# RFC 0046 Structured Exact-Solver Probe — Design Note (measurement-only)

Date: 2026-11-04. Status: **APPROVED by an independent non-author architect
agent (id a3a6b28a2ec6fbd15) after three review rounds (DISSENT -> CHANGES
REQUESTED -> APPROVE, 2026-11-04), then RE-SCOPED after the exact full-range
infoset measurement closed turn-rooted Route A at governance level.** V1/V3
is NO LONGER a prerequisite for closing Route A and is downgraded to parallel
research with exactly two residual uses: (a) the convergence-law record on
the W1-scale object (does any structured solver escape N^-0.5 where it
fits), and (b) a prototype of the offline certified per-combo margin oracle
the Route-C successor needs. A dramatic V3 result on W1 would reopen ONLY the
depth-decomposed per-river architecture, never turn-rooted Route A (its
full-range footprint is 4.27-5.62x over the frozen 1M-infoset envelope for
every algorithm - see "Two distinct objects"). Benchmark-only under
`engine/benchmarks/rfc0046/`; not linked to host/pack/serve/practice; no
production, schema, BSFLIB, or mount change; every new TU is deleted under
no-legacy if the surviving Route-C successor does not reuse it; no feature
flags.

## Question

Does an exact-key, turn-rooted, cap-1 HU endgame admit a **structured**
deterministic / variance-reduced CFR that reaches the responder bound inside
the Gate-2 envelope, where the current external-sampling MCCFR does not?
This tests the algorithm the Stage-0 addendum did NOT freeze (it froze the
envelope and predicates, not the solver), so it is inside Gate 2's
hypothesis, not a budget increase.

## Two distinct objects (the review's blocking correction)

- **W1 = the ten-hand anchor** (`strata.cpp`): ten premium combos only. Its
  measured 37,830 concrete infosets / 10,923 nodes / ~27 MB RSS are facts
  about THIS 10-combo range, NOT the full-range object. V0/V1/V2 screen on
  frozen W1.
- **Full-range stratum = 1,128 live turn combos (all strata except w1).**
  The footprint is now MEASURED EXACTLY (no solve) by the manual
  `bigshark-rfc0046-infoset-count` target: every live combo reaches every
  action node of its street under any fully-mixed reach (betting actions are
  public hole-card-independent information), so
  `infosets = turn_action_nodes x C(48,2) + river_action_nodes x C(47,2)`
  = `turn x 1128 + river x 1081` (a river node sits under one dealt card that
  blocks the 47 pairs containing it).

  | stratum | turn act | river act | exact concrete infosets | vs 1,000,000 |
  |---|---:|---:|---:|---:|
  | four limped (w1/rainbow/paired/monotone/twotone tree) | 10 | 3,936 | **4,266,096** | 4.27x over |
  | raised-srp | 12 | 5,184 | **5,617,440** | 5.62x over |

  Five of the six frozen strata are full range; only w1 fits. A full-range
  exact-key resolve exceeds the frozen infoset envelope by 4.3-5.6x EVEN IF
  the solver were instant and perfectly converged - allocation alone misses.
  Reach-sparse allocation does not rescue the exact hypothesis: RM+ starts
  fully mixed, the certifier requires every reached row (`StrictPolicy`
  fail-closes on a missing row), and a combo's reach to a node goes to zero
  only if every inbound action for that combo is zeroed, which positive-support
  CFR essentially never does on first visit. Material reduction needs card
  abstraction (forbidden by the exact-key hypothesis), a coarser/depth-limited
  boundary, or offline per-combo value nesting (Route C). This is a
  standalone envelope REDIRECT, distinct from the N^-0.5 convergence wall;
  V3 cannot pass Gate 2 on the five full-range strata regardless of speed.
  The infoset axis of the branch-(i) condition is therefore already settled;
  V3's remaining scientific value is the convergence law on the w1-scale
  object and as the engine that could produce an offline certified margin
  oracle (reference-floor branch).

## Variance structure to attack (verified in turn_gadget.cpp)

1. **River chance.** The resolve root is pinned on the turn (`board_size=4`,
   `Street::Turn`); the tree emits exactly one Chance level, the river. A
   dealt pair has **44 legal rivers** (board + 4 holes removed); the tree's
   public chance node has **48 children built from board-only removal**, and
   `board_ordinal` indexes it while the 4 blocked rivers carry zero weight
   (`abstract_tree.cpp` chance build; `certifier.cpp` 1/avail convention).
   The regret walk currently samples ONE river (`walk_regret` chance site);
   enumerating all 44 legal / 48 public-masked removes chance variance.
2. **Joint deal (the larger axis).** Each traverser iteration samples one
   blocker-aware hero x responder pair (and a third sampled deal feeds the
   average traversal). Removing this needs public-tree range-vectorized CFR
   (V3): per public node carry reach/value vectors over all live combos and
   integrate every deal by vector arithmetic. Only V3 changes the N^-0.5
   law; chance/deal sampling variance is removed only by V2/V3 respectively
   (CFR+/DCFR/linear change iteration efficiency, not sampling variance).

## Avoidable per-iteration cost (verified; implement in every variant)

- Full-table `clip_positive()` once per outer iteration (O(rows+gadget)
  ~O(38k)); clip only rows touched this traversal / lazily at read.
- `std::map<NSeatInformationKey,SubRow>` with three heap vectors per subtree
  row (the gadget `GRow` is fixed `std::array<double,2>`, not a heap vector).
  Move subtree rows to a dense/flat pooled table keyed by (node, dense
  combo); cap-1 menus are an order-preserving prefix (asserted in
  partition_walker).
- Per-call heap allocations: `regret_match` returns a vector; `gadget_sigma`
  additionally allocates a vector `r`; the traverser `child` vector; the
  opponent cumulative; `runout_cards`; the uniform chance cumulative rebuilt
  every visit (a plain uniform draw suffices); `conditional_resp_cum`
  rebuilds a vector three times per iteration. Reuse scratch buffers.
- A `steady_clock` read on every `tick()`/visit — gate the deadline on a
  cheap visit counter.
- **Dominant visit cost (missed in v1 of this note):** `walk_average` is a
  SEPARATE full traversal once per outer iteration that fans out EVERY action
  at EVERY node (not just the river) x 44 river cards with its own sampled
  deal. Conventional sampled CFR accumulates the average strategy inline in
  the regret traversal. V1 must eliminate/cheapen this traversal and report
  it as its own change, or the constant-factor attribution is conflated.

## Variants (each scored by the existing cell + certifier + partition walker)

- **V0** current external-sampling RM+ (baseline; corrected-wiring ladder).
- **V1** V0 + ALL engineering fixes above (incl. average-traversal removal).
  Sampling structure unchanged -> fixed N^-0.5 law; isolates constant factor
  only. Must pass a numeric-equivalence control: same-seed strategy
  trajectory vs V0 within a predeclared tolerance (dense tables/scratch reuse
  change summation order). Report it/s AT EACH wall budget, not a single
  constant: measured V0 throughput degrades 484 it/s at 100 k to 441 at 400 k
  to 430 at 1 M as the table fills (exactly what the full-table clip sweep
  predicts), so constant-rate K/year arithmetic is slightly optimistic and
  V1's per-budget rate is part of the constant-factor attribution.
- **V2** V1 + deterministic 44-legal-river enumeration IN the regret walk.
  With joint deals still sampled this is **chance-enumerated external-sampling
  MCCFR**, NOT vanilla CFR (deals remain Monte Carlo). Measure whether river
  chance variance dominates before spending here.
- **V3** V2 + range-vectorized public-tree CFR over all live combos (full
  deal integration). This is the only deterministic vanilla-CFR variant and
  the only one with a plausible non-sqrt law; it is also where the DeepStack
  precedent applies. CFR+ and linear/DCFR weighting are reported per table,
  never silently mixed with the frozen weighting split (linear t at -x gadget
  root rows, vanilla RM+ in the subtree).
- **W** best-of with a T1 cheap-abstraction warm start (constant-factor only;
  reported separately).

## V3 correctness specification (all five are required)

1. **Own-hand-only strategies.** At a hero node, own-combo values pool over
   responder combos by marginalizing opponent reach; the policy is a function
   of (public history, own combo) only. A per-(i,j) policy lookup would
   illegally condition on the opponent's cards.
2. **Blocker mask at every coupling point.** A validity matrix M[i,j] zeroes
   colliding pairs; the per-river mask additionally zeroes pairs containing
   the river card and that river outcome. Every range-straddling sum
   (terminal payoff, opponent pooling) must use it; inconsistent masking
   silently changes payoffs.
3. **Exact chance normalization.** Every legal pair has exactly 44 legal
   rivers. Summing 48 public children at 1/48 with a zero mask equals
   44/48 x the exact conditional mean (a strategy-invariant constant that
   does not distort regrets), but the implementation SHOULD use the
   certifier's exact 1/avail convention to stay auditable.
4. **River-dimension showdown matrix.** A single 1,128 x 1,128 matrix cannot
   encode the winner: the object is 48 x 1,128^2 outcome codes = 61,074,432 B
   ~61 MB (58 MiB) as int8 win/loss/tie (~489 MB / 466 MiB as double — too
   big), multiplied by the
   public, line-only pot at terminals. This footprint is part of the
   envelope budget and lives ONLY in the benchmark TU.
5. **Preserve the augmented game.** Per-responder-combo -x root with the
   locked margin vector b(j) (vectorizes directly), same weighting split,
   same served row schema.

## Instrumentation (unchanged 36-column contract)

Each variant emits the existing cell row through the SAME certifier and
partition walker, so `max_cert_slack`, `total_nc`, `t_river`, `t_boundary`,
wall, RSS, infosets are directly comparable. The certifier/partition walker
need no modification: they consume the same
`std::map<NSeatInformationKey,GadgetRow>` and `StrictPolicy` throws on a
missing reached row. Budget the full-range map materialization + certification
INSIDE the resolve wall (the harness puts them in the cold-resolve wall);
zero-reach rows are omitted via the existing fail-close semantics, never
synthesized. `--probe-nc` rows carry `note=probe`, which
`rfc0046-gates.ts` HARD-REJECTS AT PARSE (`NOTES` enum), so this path cannot
emit a gate-accepted row. Margins stay a locked external reference swept on
its own axis.

## Two-tier kill gates (the review's blocking correction)

Frozen Gate-2 bars: p95 <= 10 s (max 12 s), max cert slack <= 1e-6 pot =
4e-6 chips (pot = 4), R0 t_river <= 0.002 summed BB, envelope 1 M infosets /
5 M nodes / 1 GiB.

- **Tier 1 — cheap sound-KILL floor (necessary, NOT sufficient), W1.**
  Require BOTH max cert slack <= 1e-4 pot (= 4e-4 chips) AND
  t_river <= 0.02 summed BB at a 30 s wall. Failing legitimately kills the
  variant: at the shorter 10 s budget the error is no smaller, so
  s(10) >= s(30) > 4e-4 (up to 1.2e-3 under a pure 1/T law where
  s(10) = 3·s(30)), vs the 4e-6 gate; likewise t_river(10) is 0.02-0.06 vs
  the 0.002 gate. Curve protocol (predeclared): log-log OLS on max slack and
  separately t_river over at least four wall budgets that MUST include
  2.5 / 5 / 10 / 30 s; report slope CI and R^2 >= 0.9; STOP if the fitted
  exponent is consistent with -0.5.
- **Tier 2 — COMMITMENT gate (before monotone campaign / any "viable"
  claim).** Measured AT the 10 s budget: slack <= 1e-6 pot AND
  t_river <= 0.002 BB. Deterministic V3: bitwise-identical rerun (V3 is
  seedless); V2 and V1 keep sampled joint deals, so both use the multi-seed
  10-run p95 convention. On monotone additionally require
  infosets <= 1,000,000 and RSS <= 1 GiB (the dense-V3 risk). A conservative
  30 s-only shortcut equivalent under a 1/T law (error(10 s) = 3·error(30 s)):
  s(30) <= 1.3e-6 chips (~3e-7 pot) and t_river(30) <= 6.7e-4 BB (use
  6e-4 for round-direction conservatism). Passing Tier 1 but not Tier 2 is
  not a success.

## Determinism and margin-reference protocols

- V1 retains SplitMix seeding (sampled deals + sampled river). V2 enumerates
  the river but STILL SAMPLES the joint deal, so it is NOT seedless: it keeps
  deal-RNG seed variance (reduced, not eliminated) and uses the same
  multi-seed p95 protocol as V1. Only V3 (full deal integration) is
  deterministic and must be bitwise-identical across reruns (no hidden
  nondeterminism, no parallel reduction order dependence).
- Margin floor: fix a margin-iters ladder (200k / 400k / 1M / 2M) at one
  gadget budget and record max-combo |delta b| as the explicit reference-floor
  estimate. If slack at the long point TRACKS margin-iters (a b-dominated
  floor), the 1e-6-pot target is unmeasurable until margins carry a certified
  error bound; the prerequisite is then an exact / BR-safe offline margin
  oracle (which an offline V3 could itself produce) — a trunk-instrument
  finding, not a solver-speed finding.

## Scope guardrails

New TUs link ONLY into `bigshark_rfc0046_bench` (configure-time target-graph
guard + `rfc0046_link_isolation` ctest already fail if it reaches
engine-host/practice/resident/pack). The blocker-aware showdown matrix and
vectorized eval stay OUT of `engine/`; do not reuse the nseat trainer's
storage for dense tables; no production/schema/36-column-contract change;
no flags; delete everything under no-legacy if Route A closes.

## Decision rule driven by the corrected 5 M cell

(pot = 4 chips; Tier-1 floor = 4e-4 chips; gate = 4e-6 chips.)

- **(i) Governance-level Route-A closure + start the Route C RFC** — only if
  s(5M) >= ~0.1 chips (2.5e-2 pot) with no continuing curve AND V1 misses the
  Tier-1 30 s floor as arithmetic predicts (needs >=1e4x effective speedup at
  sqrt) AND V3 then fails Tier-1 on W1 or fails Tier-2 / the infoset envelope
  on monotone. Do NOT skip V3 on the 5 M datum alone: V3 changes the law, and
  the dissent's ordering stands.
- **(ii) V1 can suffice alone** — only if the V0 curve is already at gate
  scale. The constant factor to clear **Tier 1 at 30 s** is
  K1 = 5e6·(s/4e-4)²/(484·30) = 344·(s/4e-4)²; a plausible 5-20x engineering
  gain therefore clears Tier 1 only when s(5M) ≲ ~1e-4 chips. But clearing
  **Tier 2 at 10 s** under the fixed sqrt law needs
  K2 = 1033·(s/4e-6)²; even at K2 = 20, V1 alone suffices only if
  s(5M) ≲ 5.6e-7 chips (i.e. the 5 M V0 curve is already inside the gate).
  For s(5M) in [1e-6, 4e-4], V1 may pass the Tier-1 screen but is
  arithmetically guaranteed to miss Tier 2: K2 ranges from ~3x to ~6 orders
  beyond a plausible 20x engineering gain across this window (the 4-order
  point starts at s ~ 5.6e-5), so V1 alone cannot reach Tier 2 anywhere in
  [1e-6, 4e-4] and a law-changing V2/V3 remains mandatory. Run the cheap
- **(iii) Commit V1 -> V3 (V2 optional)** — if s(5M) lies between ~4e-4 and
  0.1 chips with sqrt continuing (the measured 11.33@1M / flattening N^-0.21
  trajectory projects here): V1 is a confirmed arithmetic dead end for the
  real gate; build it as the engineering baseline, attempt V2 only if
  diagnostics show river-chance variance dominates, and spend the budget on
  V3 gated by the full-range infoset/RSS measurement and Tier 2.
- **Reference-floor branch** — if slack moves with margin-iters at 5 M, do not
  call the solver hypothesis falsified; Gate 2 is unmeasurable at 1e-6 pot
  until a certified-error margin oracle exists.

In every branch there is no typed Gate-2 CLOSE without the frozen 720-slot
Phase A; the research exit is the governance-level closure path the close
report describes (precedented by RFCs 0041/0043/0045).
