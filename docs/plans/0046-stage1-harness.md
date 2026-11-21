# RFC 0046 Stage 1 — Measurement Harness + Benchmark Prototype (awaiting joint doc+code review)

- RFC: [0046](../rfcs/0046-m2-fullstreet-hu-blueprint.md)
- Freeze: [Stage-0 gate addendum](0046-addendum-stage0-gates.md)
- Status: **DESIGN APPROVED 2026-11-04 (four non-author review rounds);
  PROTOTYPE IMPLEMENTED 2026-11-04, awaiting the JOINT doc+code review.** The
  design verdict authorizes construction; it does NOT authorize any gate run
  or any commit. Gates 1-3 remain unrun until the joint review (which must
  explicitly diff the two seam hunks) approves, and the mandatory second
  non-author evidence review follows the gates. The formal design decision
  record is section 10.
- Implemented prototype files (all benchmark/test-only):
  - `engine/benchmarks/rfc0046/`: `concrete_keyer.{hpp,cpp}`,
    `turn_gadget.{hpp,cpp}`, `certifier.{hpp,cpp}`,
    `partition_walker.{hpp,cpp}`, `margin_extract.{hpp,cpp}`,
    `strata.{hpp,cpp}`, and the manual runners `rfc0046_cell.cpp`,
    `rfc0046_handpair.cpp`.
  - Seam: `engine/include/bs/nseat_trainer.hpp` (decl) and
    `engine/src/gto/nseat_trainer.cpp` (two-site branch) - the ONLY production
    engine edits.
  - Isolation: `engine/cmake/rfc0046_link_guard.cmake` +
    `rfc0046_link_guard_check.cmake`, wired into the root CMakeLists.
  - Native ctests: `engine/tests/test_rfc0046_concrete_keyer.cpp`,
    `test_rfc0046_keyer_seam.cpp`, `test_rfc0046_gadget.cpp`.
  - Evaluator: `tools/rfc/rfc0046-gates.ts` (+ `-test.ts`, 54 tests) and
    `bin/rfc0046-gates.mjs`.
- One implementation refinement versus design prose, for joint review: the
  gadget average-strategy walk ENUMERATES the 48-card river chance instead of
  sampling one card per iteration (the river deck is a fixed public set; exact
  enumeration converges every river row the certifier visits and removes a
  coverage lottery; it matches the RFC 0005 full-traversal gadget). The regret
  walk remains external-sampling.
- Author: Claude (zzzode agent session).
- Scope: benchmark/tests ONLY. Nothing here links into
  apps/engine-host, pack/BSFLIB, serve/mmap, or practice. No-legacy: the
  Stage-1 TU, the trainer seam, and the gate evaluator are deleted after
  the gates unless a separately reviewed production addendum names a
  promoted subset (addendum section 11).
- This document resolves the Stage-0 addendum section-8 open point: it
  supplies the concrete additive decomposition, CSV schema, behavioral
  hand pair, and exact-key bucket/infoset definitions that the gate
  evaluator implements.

## 1. What exists vs what the prototype must add

Verified current state (source facts, re-checked against source):

- RFC 0011 subgame resolver rebuilds a TURN-rooted River-terminal
  subtree (`engine/src/resolver/subgame_resolver.cpp:766-767`) and runs
  ordinary MCCFR (`train_nseat`) with per-hand prior RANGES, but has NO
  resolving gadget (no opponent terminate/continue, no margin) and NO
  certifier; it serves `Approximate`. Its trainer keys infosets on
  `kNSeatCardKind` (`engine/include/bs/nseat_trainer.hpp:34-35`,
  Preflop169) -> postflop nine-category merge
  `(score>>20)&0xF` (`engine/src/abstraction/abstraction.cpp:417-422`).
- RFC 0005 terminal resolver (`resolver.cpp` + `gadget_cfr.cpp` +
  `certifier.cpp`) HAS the `-x` terminate/continue gadget with locked
  per-combo centered margins and an independent best-response certifier
  (`engine/src/resolver/certifier.cpp:128-159`, max(baseline, candidate)
  per positive-mass `(seat, cards)` infoset), but it covers only one
  terminal hero decision and the certifier throws on an action node
  (`certifier.cpp:38`).
- `CardBucketKind::Identity` is NOT exact keying postflop: it returns
  `strength_bucket` (made-hand score), which still merges equal-strength
  distinct hands and is not value-lossless on the turn
  (`abstraction.hpp` card-family doc).
- The enum contains only ordinals 0..3 (`abstraction.hpp:173-182`); 4/5
  are permanently retired comments. The sealed-checkpoint codec already
  rejects EVERY raw kind byte outside 0..3 through its default case
  (`engine/src/artifacts/sealed_checkpoint.cpp:364-384`); the existing
  native test `rejects_retired_or_unknown_kind`
  (`engine/tests/test_sealed_checkpoint.cpp:502-537`) already loops
  bytes {4,5,6,255}, so a prototype checkpoint tagged 6 fails closed
  TODAY, with zero codec edits and zero new tests needed there.
- `ResolveLimits` defaults: 10,000 ms, 5 M nodes, 1 M information sets,
  1 GiB (`engine/include/bs/resolver.hpp:183-195`).
- Production certification tolerance is `kCertPotTolerance = 1e-9`
  (`resolver.hpp:94`).
- Trainer card-key sites verified in source. Exactly TWO training-sweep
  sites compute the infoset key in `train_nseat`: the regret walk
  (`engine/src/gto/nseat_trainer.cpp:497-498`) and the average-strategy
  walk (`nseat_trainer.cpp:699-700`), both calling
  `abstraction::card_bucket(kNSeatCardKind, ...)` (a plain non-constexpr
  function, `abstraction.hpp:322-323`). The third match in that file,
  `NSeatPolicy::lookup` (`nseat_trainer.cpp:1173-1254`, bucket at
  1186-1190), is NOT a training site: it is the const READ-side method
  on the published codec-constructable policy, and it is a PRODUCTION
  serving path — `NSeatBehaviorPolicy::distribution` calls it for every
  served row (`engine/src/stage6/measured_policy.cpp:68`). It must keep
  keying on `kNSeatCardKind` and is outside the seam.
- Benchmark walkers do not need lookup: they self-compute the bucket and
  address `rows()` directly — the existing precedent is
  `engine/benchmarks/subgame_nash_conv.cpp:429-432` and `:839-842`
  (`rows().find({bucket, node_index})`), likewise
  `engine/src/resolver/subgame_resolver.cpp:766-769`.

## 2. Exact-key card representation and the benchmark-only seam

### 2.1 ConcreteCards key (measurement only)

A measurement card family, working name `ConcreteCards`, exists ONLY in
benchmark translation units. It is NEVER added to the production
`CardBucketKind` enum and has no persistent kind byte:

- `concrete_bucket(hole[2], board4) -> uint32 dense id` is a function of
  the two concrete hole cards and the four turn board cards only. It
  enumerates the 52-card canonical two-card hands in the fixed total
  order (card id = rank*4+suit, `c0 < c1`, rank-major with suit
  tie-break), skips any hand sharing a card with the board, and ranks
  survivors densely 0..K_b-1 where
  K_b = C(52-4,2) = C(48,2) = 1,128 at a four-card board. The same hand
  maps to the same id at every node of the subtree.
- Injectivity: on a fixed four-card board the live holdings are exactly
  the unordered pairs of the 48 non-board cards; dense enumeration is a
  bijection, so two distinct live holdings of one seat always get
  distinct ids. Keying depends on board4 only; the river card changes
  reach and subtree values, NEVER the bucket (river blockers act through
  zero-reach pruning).
- The endgame infoset key is `(concrete_bucket, node_index)`; the
  certified endgame traversal makes no strength/category `card_bucket`
  call on a postflop hand. The pre-river TRUNK keeps its chosen abstract
  family (T0/T1); only the turn-boundary and below is concrete.
- Infoset-count model (reported, Gate-2 head-line unknown): sum over
  turn+river action nodes of live concrete holdings, bounded above by
  (#action nodes)*1,128 and reduced by reach/blocker pruning. The
  harness reports nodes, concrete infosets, and the ratio to the
  nine-category build side by side.

### 2.2 Seam mechanism (committed; zero production-enum/codec edits)

The hook is a nullable defaulted trailing parameter on the multi-seat
trainer entry point, following the DIRECT signature precedent of
`train_nseat`'s existing defaulted trailing parameters
`frontier`/`initial_state`/`continuation`
(`nseat_trainer.hpp:271-277`):

- `engine/include/bs/nseat_trainer.hpp`: add a small
  benchmark/test-documented struct `NSeatCardKeyerSeam`
  (`bucket(span<const int> hole, span<const int> board) -> uint32`,
  `bucket_cap() -> uint32`) and a trailing
  `const NSeatCardKeyerSeam* card_keyer = nullptr` on `train_nseat`
  only (not `train_nseat_parallel`, not any production options struct,
  never stored in `NSeatPolicy`).
- `engine/src/gto/nseat_trainer.cpp`: the seam branches EXACTLY the two
  training-sweep keying sites — the regret walk (497-498) and the
  average walk (699-700). The nullptr path keeps the identical
  production call `abstraction::card_bucket(kNSeatCardKind, ...)`
  bit-for-bit; no production caller passes the seam.
- `NSeatPolicy::lookup` (1186-1190) is explicitly EXCLUDED: it is the
  post-training read-side method behind the production serving path
  `NSeatBehaviorPolicy::distribution` (`measured_policy.cpp:68`), it
  runs after `train_nseat` returns and is unreachable from a training
  parameter, and it keeps keying on `kNSeatCardKind` unchanged. Storing
  a seam pointer in the serialized `NSeatPolicy` is forbidden (dangling
  hazard and production-type pollution).
- The benchmark's gadget, certifier, and NashConv walkers never serve
  through lookup either: they self-compute `concrete_bucket` and
  address `rows().find({bucket, node_index})` directly, exactly as the
  existing walkers do (`subgame_nash_conv.cpp:429-432, 839-842`).
- New benchmark TU (working name
  `engine/benchmarks/rfc0046/concrete_keyer.{hpp,cpp}`) defines the
  ConcreteCards order/blocker logic and the `NSeatCardKeyerSeam`
  implementation, compiles into a `benchmark-only` static library, and
  is never installed.
- NO other engine edits: not `abstraction.hpp` (enum),
  `abstraction.cpp`, `sealed_checkpoint.*`, codec, BSFLIB, mmap, host,
  practice, measured_policy, or `NSeatPolicy`. Byte 6 rejection needs
  no change (section 1).

Exact production touch points are therefore TWO FILES: one declaration
hunk in `nseat_trainer.hpp` and the two-site branch in
`nseat_trainer.cpp` (plus benchmark-only CMake). The joint doc+code
review diffs both hunks explicitly.

### 2.3 Native zero-merge and isolation tests (never text grep)

Three ctests carry the guarantee:

1. **Behavioral merge detector (operative regression):** the section-6
   frozen hand pair. The concrete-keyed endgame returns different argmax
   actions for the two same-nine-category combos, while a policy trained
   through the nullptr production path returns ONE merged row for both.
   This fails if an equivalent of the `subgame_resolver.cpp:766` merge
   appears in a certified endgame path.
2. **Negative construction test:** constructing the certified endgame
   solve without the seam object cannot produce a ConcreteCards result
   (type-level: only the seam supplies dense 0..1127 keys; the
   production kind's postflop range is the nine-category merge).
3. **Link-graph test (CMake target dependency traversal):** a native
   ctest walks the CMake target dependency graph and asserts the
   benchmark-only lib is absent from engine-host, practice, the
   resident/mmap library, and pack targets.

## 3. Turn-boundary resolving gadget and per-combo margins

The prototype generalizes the RFC 0005 gadget from "one terminal hero
decision" to "the full turn+river subtree":

- Gadget root at the TURN boundary. Each non-hero seat gets one `-x`
  infoset choosing TERMINATE (pay the centered locked margin b_s(c)) or
  CONTINUE (enter the real cap-1 turn+river subtree; the 48-card public
  river chance then leads to exact showdown). At two seats this is the
  exact responder/safe-subgame construction.
- Margins are PER CONCRETE COMBO, not per bucket: for each seat and each
  concrete holding c at the turn boundary,
  `b_s(c) = (sum_d w(d) * C_s(d) over deals where seat s holds c) /
  m_s(c)`, with the same counterfactual weighting and blocker-aware
  joint deals the terminal gadget uses (`counterfactual_reach.*`), but
  C_s(d) is the locked TRUNK continuation value through the subtree.
  Zero-mass combos contribute zero (existing convention).
- The trunk supplies the locked prefix probabilities and continuation
  values; it may be card-abstracted pre-river, but the margin
  evaluation enumerates concrete combos. The boundary-margin error
  (T_boundary in section 5) is validated by the per-bucket-margin
  variant policy; serving always uses per-combo.
- The subtree CFR keys through the ConcreteCards seam (section 2) and
  uses the fixed 48-card public river chance and blocker-aware joint
  deals.

## 4. Subgame-capable certifier

Generalize `certifier.cpp` (which currently throws on an action node)
into a benchmark-only exact best-response walker over the full
turn+river subtree:

- For each non-hero seat and each positive-mass concrete combo c, walk
  every opponent action sequence through turn AND river to exact
  settlement, evaluating the RFC 0005 inequality across both streets:
  `BR_s(c) = max(b_s(c), candidate_continuation(c))`.
- **Certified iff, for every seat s and every positive-mass concrete
  combo c, `BR_s(c) <= b_s(c) + 1e-6 * root_pot`.** The per-resolve
  maximum observed slack is recorded in `max_cert_slack`.
- The 1e-6 root-pot slack is MEASUREMENT-ONLY for RFC 0046 gates. It is
  ~1000x the production `kCertPotTolerance = 1e-9`; it is acceptable
  here because R0 = 0.002 BB independently gates real endgame
  exactness. Nothing inherits 1e-6 into production without a new RFC.
- Determinism (addendum 6.1): cold-vs-cold and cold-vs-warm-hint
  profiles give bitwise-identical served probabilities and responder
  margin within 1e-6 root-pot; a hint that changes the verdict
  invalidates the T2t/T2h configuration. Outcomes are booleans per
  resolve in the CSV (`coldcold_bitwise`, `warmhint_bitwise`,
  `max_margin_diff`).
- Resource and time outcomes use the typed mapping frozen in Stage-0
  section 6.2. The certifier is independently written code from the
  solver (a separate BR enumeration, as today).

## 5. Per-street exploitability decomposition (the additive construction)

Stage-0 section 8 freezes `Total = T_trunk + T_boundary + T_action +
T_river` as four NON-NEGATIVE terms on one identical deal sample with an
exact identity test `|Total - sum(terms)| <= 1e-9 root-pot`, and defers
"the exact swap definitions" to this harness. Round-1 review cleared
the stop on this construction (independent derivation confirmed the
telescoping identity); this revision tightens the label rule, the
cap-0 convention, and the Gate-3 object definitions per its findings.

### 5.1 Why isolated full-policy swap deltas do NOT define the terms

Replacing one component of the served hybrid by an abstract counterpart
and taking the NashConv DELTA does not yield an additive partition: a
best response also re-optimizes through the other boundaries, so
isolated swap deltas overlap. Four isolated deltas do not sum to Total
in general, which is why the frozen addendum itself defers the swap
definitions here and includes its stop clause. The exact construction
is 5.2-5.3; the component comparisons the addendum names are retained
as mandatory full-variant walks in 5.4.

### 5.2 Evaluated policy and the game Total is measured in

The served hybrid `H` is: locked card-abstract cap-1 trunk prefix,
gadget-produced per-combo turn policy, concrete-key river policy. Total
is exploitability in the SERVED ABSTRACTION'S game, not literal
exploitability in the unrestricted legal poker game: pre-turn trunk
menus stay cap-1, while the turn-rooted endgame faces an UNRESTRICTED
opponent. Concretely Total is measured in a game tree that is cap-1 on
trunk nodes and cap-0 (unrestricted menu) inside the turn-rooted
subtree, matching Gate 3's turn-rooted scope and its 20 M-node outer
budget. `H` is extended into subtree regions that exist only under
cap-0 by the frozen Gate-3 cap-0 reference policy `H0`.

- `Total = NC(H-extended in that tree)`: chance-free summed BB, full
  1,326-combo range, identical per-seed independent deal batches,
  addendum section-1 universe (turn root, 48-card public river chance,
  blocker-aware C(48,2) x C(46,2) joint pairs, x44 fixed runouts).
- **T_trunk is zero by construction, and this is intentional.** The
  measurement tree is turn-rooted (addendum section 1: the harness
  builds board_size-4 trees, never the flop-rooted G3 object), so the
  traversed game has NO preflop/flop action infosets. Rule 3's
  pre-turn label set is therefore empty and `T_trunk = 0` on every
  cell; the evaluator pins this structurally (node test: the turn-rooted
  tree contains no pre-turn action node) and reports the column as the
  exact constant 0, never as accumulated values. The locked trunk's
  error is not absent - it enters the measurement through (a) the
  locked per-combo margin VALUES, which primarily land in `T_boundary`
  through the turn-infoset values (margin-driven turn-policy shifts can
  also move river reach distributions and hence `T_river`, which is
  independently gated by R0), and (b) the candidate-vs-
  candidate Total differences that Gate 1 selects on. The frozen
  addendum section-8 term "residual locked-prefix regret" has no
  separate infoset set in a turn-rooted game and cannot be split out of
  a per-infoset g(I) without an invalid non-telescoping swap chain;
  recording it as the identically-zero label term is the exact reading.
  No frozen gate binds T_trunk (B0 binds Total, R0 binds T_river, M
  binds the Gate-3 gap), so this relaxes nothing. The genuinely pre-turn
  exploitability of the offline trunk cannot appear in a turn-rooted
  measure at all; this is a stated residual risk carried into the later
  production design addendum, where any full-street validation is a
  separate decision.
- The walker computes Total and the four bucket sums in ONE fixed
  ordered accumulation pass over the identical per-(deal, player,
  infoset) value stream (single deterministic reduction order; no
  independent OpenMP reductions for identity values), so the 1e-9
  root-pot test compares one summation, not two roundings.
- Residual reporting: `telescope_resid` is the per-player MAX absolute
  identity residual in root-pot units (two players, worst of the two),
  with the BB value also carried for context.

### 5.3 Exact per-infoset telescoping partition and the TOTAL label rule

Fix one player and the opponent/chance at `H`. Switch the player's
decisions from `H` to a pure best response one infoset at a time in
POSTORDER (leaves first); descendants then play BR and ancestors still
play `H`. By multilinearity the increment at infoset I is

```
g(I) = reach_H(I) * ( max_a V(I, a; tail = BR)
                    - sum_a H(I,a) * V(I, a; tail = BR) )
```

with `reach_H(I)` the FULL H-reach (opponent/chance AND own prefix; the
own prefix still equals H-reach in postorder because switched
descendants/cousins lie on disjoint own action sequences). Every
`g(I) >= 0`; the increments telescope EXACTLY to `u(BR) - u(H)` per
fixed deal, hence after chance-free summation and pooling opponent
holdings at true infosets, and after summing both players.

Every traverser action infoset gets exactly one label under this total
ordered rule (evaluated top to bottom; this is the F2 precedence fix):

1. Infoset reachable only in a cap-0-only EXTENSION region (the served
   `H` places zero probability on every action leading there):
   **T_action**. Its `g(I)` is IDENTICALLY ZERO because `reach_H(I)=0`
   for any extension `H0` (F3 fix); the evaluator pins this mass to
   zero (`extension_mass` column) and fails closed on any nonzero
   value. H0 imperfection is NOT measured here: it enters (a) through
   the continuation values `V(I, a; tail=BR)` at rule-2 ancestors whose
   BR takes a cap-0-only action, and (b) through NC0 in Gate 3.
2. Otherwise, a cap-1 endgame infoset whose BR argmax action lies
   OUTSIDE the cap-1 menu: **T_action** (the missing-menu regret; the
   cap-0-only subtree is evaluated under H0). Argmax ties resolve
   deterministically BEFORE labeling: prefer an action inside the
   cap-1 menu; among still-tied actions take the lowest frozen
   identity-schedule action index.
3. Otherwise, by street: pre-turn (preflop/flop) trunk infoset ->
   **T_trunk** (this set is EMPTY in the turn-rooted measurement tree,
   so the term is the pinned zero of 5.2); TURN action infoset of the
   resolved subtree -> **T_boundary**; RIVER action infoset ->
   **T_river**.

Exact identity, asserted per (stratum, seed) on the player-max residual
(5.2), on identical deal draws:

```
Total - (T_trunk + T_boundary + T_river + T_action) = 0
```

Fail closed on residual beyond 1e-9 root-pot or any term below
-1e-6 root-pot.

### 5.4 Required component-swap variant walks (bucket validation)

Three full variant policies are ALSO walked on the same deal batches
(joined by stratum/root/seed/resolve_id). Their NashConv totals and the
g(I) masses in the relevant bucket are diagnostics; isolated full-policy
deltas are NOT identity terms:

- `rivermerge`: hybrid with the merged nine-category/k_river policy at
  river infosets. Check: its river-bucket mass >= `T_river` minus
  tol; STRICTLY larger (gap > 1e-9 root-pot, the predeclared tolerance)
  is REQUIRED on the monotone stratum, where the section-6 behavioral
  pair proves the merge discards a real action distinction; other
  strata require the non-strict bound.
- `bucketmargin`: per-bucket turn margins (the gadget re-solves with
  bucket-averaged margins; turn policy changes, river stays concrete).
  Check: its TURN-bucket mass >= `T_boundary - tol`.
- `statictrunk`: serve the locked card-abstract trunk profile's TURN
  policy directly (no gadget refinement; concrete river retained).
  Check: its TURN-bucket mass >= `T_boundary - tol`; its T_trunk label
  set is empty as in every turn-rooted walk.

The evaluator fails closed on any violated directional check.

### 5.5 Gate-3 endgame objects (pinned; F4)

`nc_endgame_cap0` (NC0) and `nc_endgame_cap1` (NC1) are measured in ONE
common tree: the REAL turn-rooted cap-0 game (NO gadget `-x` actions in
the measurement tree; the gadget is solely the solving device that
produces the profile), with fixed per-combo trunk ranges and boundary
priors, the 48-card river chance, exact showdown settlement, and the
frozen joint-deal universe. Same per-seed deal batches as Total.

- NC0 = NashConv of `H0`, the cap-0 reference profile solved inside the
  outer budget (20,000,000 nodes / 8 GiB / 600 s), measured in that
  tree. If the reference cannot be built within the outer budget, Gate 3
  -> CLOSE exactly as addendum section 7 freezes.
- NC1 = NashConv of the cap-1 exact endgame profile (the same
  per-combo-margin/concrete-river profile used by the served hybrid
  endgame), with cap-0-only regions filled by H0, in the SAME tree.
- Gate-3 gap = NC1 - NC0 (expect >= 0; LCB < -1e-6 root-pot aborts the
  gate); M = 0.002 on its UCB; the single cap-2 escalation is
  addendum section 7's only pre-authorized branch.
- NC0's magnitude is prominently reported per stratum. KNOWN BIAS: an
  underconverged H0 inflates NC0 and Total and SHRINKS the Gate-3 gap
  (dangerous direction: makes cap-1 look cheaper than it is). Each H0
  solve records its termination, iterations, nodes/bytes, and the
  solver's final internal regret/convergence diagnostic where one
  exists; setting a convergence FLOOR on H0 is not pre-authorized and
  would need section-9 approval — the harness surfaces the evidence, it
  does not invent a bar.
- Full-game `T_action` and the Gate-3 gap are deliberately NOT asserted
  equal: the first is full-game reach-weighted regret through the
  abstract trunk, the second is endgame-level. Both are reported.

Gate bindings overall: B0 = 0.010 on the six-stratum-max UCB of Total;
R0 = 0.002 on the six-stratum-max UCB of `T_river`; M = 0.002 on the
Gate-3 gap UCB.

## 6. Behavioral discriminator hand pair (pre-frozen rule and reference)

Everything in this section is pinned BEFORE any timed gate resolve
(addendum 6.1):

- Board: monotone turn board `Qs Ks As Js`, river `9c`, cap-1 menu.
  Full 1,326-combo range with board blockers removed. The shared bucket
  is `CategoryTiersV1` evaluated on the FIVE-CARD board (the board the
  merged production path passes at `subgame_resolver.cpp:766`).
- Selecting reference (frozen identity): a cap-1 exact-key endgame
  solve on that board, seed **4320**, **2,000,000 iterations**, full
  live range, run once during harness construction (never a timed
  resolve; no wall cap enters the selection). Its artifact/digest is
  committed with the pair before Gate 2.
- Pair rule: enumerate live combos in the ConcreteCards dense order
  (`c0 < c1`, section 2); take the FIRST pair of live combos in the
  SAME five-card CategoryTiersV1 bucket whose reference argmax actions
  differ STRICTLY (`V(best) - V(second) > 0` in the reference, with the
  per-action deterministic tie-break: among equal-valued actions pick
  the lowest frozen identity-schedule action index). Record the chosen
  pair's action-value gap in the hand-pair manifest as evidence (no
  robustness margin may be added post hoc; a near-tied pair that later
  fails the served assertion fails Gate 2 fail-closed).
- Non-vacuity: both combos live, same category id, distinct argmax
  (asserted in the native ctest). If no strict pair exists on monotone,
  the predeclared fallback is the unpaired-twotone stratum board
  `7s Kh As Js / 9c` with the identical reference and rule.
- Manifest `docs/plans/ablation/rfc0046/rfc0046-handpair.json`
  (retained evidence): board, river, seed, iterations, reference
  digest, both combos, category id, action indices, value gap,
  fallback-used flag. The evaluator loads the committed constants; it
  never derives the pair from gate rows.
- Criterion: concrete endgame argmax differs for the two combos; the
  production-path merged policy returns one identical row for both;
  the ctest asserts both directions.

## 7. CSV schema and evaluator of record

`tools/rfc/rfc0046-gates.ts` (node-test convention of rfc0045-gates.ts)
ingests per-walk rows and emits only PASS / REDIRECT / CLOSE per the
addendum section-10 table; no free-text verdict. One row per evaluated
policy walk, header order fixed:

```
stratum,root,turn_board,river_card,range,scheme,trunk,cap,seed,resolve_id,policy,
trunk_rows,trunk_bytes,
infosets,nodes,peak_bytes,train_s,certify_s,resolve_wall_s,walk_s,termination,
coldcold_bitwise,warmhint_bitwise,max_margin_diff,max_cert_slack,
total_nc,t_trunk,t_boundary,t_river,t_action,extension_mass,telescope_resid,
nc_endgame_cap1,nc_endgame_cap0,river_argmax_diff,note
```

Column units (finding M2 from the joint review, now part of the contract):
- `nodes` = materialized `tree.size()` (NOT walk visits); `peak_bytes` = peak
  process RSS (`getrusage`), not a cumulative byte charge.
- `resolve_wall_s` = receipt-to-result cold wall INCLUDING online model build,
  train, and certify, EXCLUDING locked-margin precomputation and the offline
  NashConv walk; `walk_s` is offline-only.
- `max_cert_slack`, `max_margin_diff` = root-POT FRACTIONS (tolerance
  1e-6 pot).
- `total_nc`, `t_trunk`, `t_boundary`, `t_river`, `t_action`,
  `nc_endgame_cap1`, `nc_endgame_cap0`, and the Gate-3 gap = BB per hand.
- `extension_mass`, `telescope_resid` = root-POT FRACTIONS (identity
  1e-9 pot, structural zeros).
The evaluator converts every root-pot-frozen threshold (identity 1e-9,
directional 1e-9, term floor -1e-6, cap-gap anomaly -1e-6) to BB per stratum
using the frozen pot/BB (limped pot 4 / raised pot 8, BB 2) before comparing
BB columns; a node test pins the conversion (1 BB = 0.5 pot limped, 0.25
raised).
- A non-Certified timing-slot row (`note=truncated`) leaves the partition,
  cert-slack, gap, and determinism numeric columns EMPTY and carries the true
  typed `termination` (SolveDeadline / CertifyDeadline / ResourceExhausted /
  CertificationRejected / DiagnosticInconclusive); it occupies one of the 30
  slots. CertificationRejected is reserved for a true responder-bound failure,
  never for a missing-row underconvergence (which maps SolveDeadline).

- `range` in {full, w1}; `policy` in {hybrid, rivermerge, bucketmargin,
  statictrunk, cap0ref}; variant/reference rows join by
  (stratum, root, seed, resolve_id).
- `trunk_rows`,`trunk_bytes`: frozen per (`scheme`,`trunk`) candidate
  constants repeated on that candidate's rows; the evaluator verifies
  consistency and they drive Gate-1's frozen total order
  (fewest rows -> smallest bytes -> smallest cold-resolve p95 ->
  lexicographically smaller scheme name). A node test pins the ordering
  on synthetic candidates (F1).
- `resolve_wall_s` is the ONLY timing column gated: receipt-to-result
  cold resolve wall = model build + train + certify, EXCLUDING the
  offline evaluation walk (`walk_s` is reported, never timed into the
  gate). `train_s`/`certify_s` are diagnostic components.
- Determinism/slack columns per section 4; `extension_mass` pinned zero
  per 5.3 rule 1; `telescope_resid` per 5.2; `nc_endgame_*` per 5.5.
- `termination` in {Certified, CertificationRejected, SolveDeadline,
  CertifyDeadline, ResourceExhausted, DiagnosticInconclusive}; `note`
  final/truncated/diagnostic.

Hard predicates (fail closed; incomplete strata, missing required
variant/reference/cap0ref rows, any non-Certified row in a gate input,
identity residual beyond tolerance, any negative term beyond
-1e-6 root-pot, a cap-gap LCB below -1e-6 root-pot, or a failed variant
directional check all abort):

- Six strata exactly, boards/roots per addendum section 1. The five
  full-range strata require `range=full`, n=3 seeds 4320/4321/4322,
  t=4.303; w1 requires `range=w1`, n=5 seeds 4320..4324, t=2.7764. The
  six-stratum-max UCB for B0/R0 includes w1 under its own stats; a
  full-range stratum may NEVER be populated or substituted from w1
  rows (evaluator rejects the input). W1 anchors diagnostics; it cannot
  replace a full-range stratum.
- Gate 2 timing/feasibility per stratum: exactly 30 cold resolves;
  nearest-rank HF type 1 p95 (29th ordered) <= 10,000 ms AND max
  (30th) <= 12,000 ms; ALL 30 Certified and individually inside
  5,000,000 nodes / 1,000,000 infosets / 1 GiB; exactly ONE diagnostic
  profile row per stratum maximum (count enforced; a diagnostic can
  never yield PASS); typed mapping and CLOSE/REDIRECT precedence per
  addendum sections 6.2 and 10.
- Gate 1: ELIGIBLE iff B0 and R0 UCB pass on EVERY stratum; selection
  by the frozen total order; none eligible -> the section-10 CLOSE.
- Gate 3: per-stratum gap UCB <= M with the sign anomaly abort;
  cap-2 escalation only as frozen.
- Hand-pair constants come exclusively from the committed manifest.

Node tests pin at minimum: t multipliers (4.303 / 2.7764); HF type-1
indices at n=30 (p95=29th, p50=15th); the partition identity on
synthetic per-infoset g(I) streams including a cap-0-only ARGMAX tie
case (tie prefers cap-1) and an extension-region node (zero mass
required); T_trunk structural zero (the turn-rooted tree has no
pre-turn action node, term is the constant 0); the label precedence of
5.3; six-stratum-max UCB; the
cap-gap sign anomaly; the three variant directional checks; the Gate-1
total order; the W1/full-range substitution rejection; and the
one-diagnostic-per-stratum limit.

### 7.1 Evaluator interpretations frozen in code (for joint adjudication)

The implemented `rfc0046-gates.ts` resolves four wording points literally;
the joint review must confirm or redirect them:

1. **Two-phase evaluation.** The 30 timing slots always exist; a typed
   failure (`note=truncated`) OCCUPIES its slot (no 31st resolve), the
   one diagnostic classifies it, and variant/cap0ref/UCB joins plus the
   partition checks run only in Phase B after Gate 2 globally PASSes
   (REDIRECT/CLOSE emits empty Gate-1 and null Gate-3). Otherwise a
   bound-unreachable stratum could never coexist with the required
   30-Certified gate inputs.
2. **Cold-resolve sample granularity.** Timing and certification are
   evaluated per (candidate, stratum), because Gate-1's total-order
   level 3 compares candidate p95; each of {T0,T1,T2t,T2h} carries its
   own 30 cold resolves. Gate-2 CLOSE/REDIRECT remains a property of
   strata: any (candidate, stratum) bound-unreachable maps the stratum
   CLOSE with the frozen total precedence.
3. **Diagnostic quota is GLOBAL per stratum.** Addendum section 3
   freezes one diagnostic per stratum IN TOTAL; the evaluator enforces
   at most one `note=diagnostic` row per stratum across ALL candidate
   blocks (not per candidate), with a dedicated node test.
4. **Gate 3 includes w1** under its own n=5/t=2.7764 statistics, because
   M binds the six-stratum-max UCB (sections 2/5); w1 rows can never
   substitute a full-range stratum.

## 8. Build/link discipline, artifacts, deletion set

- CMake: the ConcreteCards TU + gadget/certifier/partition/margin/strata
  units compile into a `bigshark_rfc0046_bench` STATIC library linked ONLY by
  the two manual runners (`bigshark-rfc0046-cell`,
  `bigshark-rfc0046-handpair`) and the measurement ctests. Isolation is
  enforced by a native ctest traversing the CMake TARGET DEPENDENCY GRAPH
  (configure-time `engine/cmake/rfc0046_link_guard.cmake` plus the
  `rfc0046_link_isolation` ctest against a generated closure manifest, not a
  symbol grep): engine-host, practice, resident/mmap, and flop-library
  build/pack targets must not reach the library, and the library must not
  reach any of them. The guard was negative-tested (temporarily linking the
  bench lib into bigshark-practice fails configure). The stage-6 offline
  configure guard stays green.
- CLI arms: `bigshark-rfc0046-cell` (one frozen cell -> one exact-schema CSV
  row; `--stratum/--seed/--iters/--cap/--extract-margins/--margin-iters`) and
  `bigshark-rfc0046-handpair` (the pinned 2,000,000-iteration reference pair
  selector). No production dispatch references either.
- Evidence of record retained under
  `docs/plans/ablation/rfc0046/`: aggregated per-stratum CSV/JSON,
  verdict JSON, the hand-pair manifest, H0 diagnostics. Raw scratch is
  gitignored under `/rfc0046-run/`.
- Deletion set after the gates (no-legacy, addendum section 11) unless
  an independently reviewed production addendum names a promoted
  subset: the ConcreteCards TU and benchmark-only lib; the
  `NSeatCardKeyerSeam` hook (nseat_trainer.hpp/.cpp hunks reverted);
  benchmark gadget/certifier extensions and the CLI arm; the measurement
  ctests; **`tools/rfc/rfc0046-gates.ts` and its `bin/` launcher**;
  raw scratch. Aggregated evidence documents are retained.
- Full project verification after the harness builds: release ctest,
  affected suites under ASan, npm check (incl. the new node tests),
  proto check, docs/RFC checkers.

### 9.1 Prototype status and known limitations for the joint review

- Green locally: 3 new native ctests (concrete keyer bijection/blockers;
  null-vs-delegating seam bitwise identity + cap refusal; gadget
  determinism, partition identity <=1e-9, T_trunk=0/extension=0, certifier
  mechanics, locked-margin safety direction), 55 evaluator node tests, the
  link-isolation ctest (negative-tested), full `npm run check` (112 node
  tests), docs/RFC checkers, full release ctest.
- Validated end to end: a W1 cell emits one exact 36-column CSV row; the
  gadget's NashConv tracks plain `train_nseat` through the seam (the
  earlier diagnostic pairs, restated in SUMMED BB - nash_conv_chip / BB,
  no live-seat divisor - are ~4.8/4.7, ~4.0/3.8, ~2.8/2.6 at 2k/10k/40k;
  exact figures re-measured at the campaign since the normalization
  changed), so the mechanism and the seam are numerically sound.
- KNOWN LIMITATION (the actual Gate-2 question, not a prototype bug): under
  the 10 s wall the gadget's external-sampling regret walk has not, in quick
  probes, converged to responder-certifiable margins on even the W1 object
  (certifier slack remains positive/large). The exact enumerated average
  walk fixed row COVERAGE (slack went from +inf to finite) but not regret
  convergence. Whether a 10 s / 5 M-node / 1 M-infoset cold solve reaches
  the 1e-6-pot responder bound is precisely what Gate 2 measures
  (PASS / REDIRECT-to-Route-C / CLOSE); no gate has been run and no tuning
  iteration count is pre-declared here.
- Not yet wired (left to the campaign driver, itself reviewable): the
  multi-cell orchestration (30 cold resolves/stratum/candidate, variant
  joins, cap-0 reference solve with the 20M/8GiB/600s outer bound, cap-2
  escalation, warm-hint T2t/T2h profiles, deterministic cold-vs-cold and
  cold-vs-warm bitwise columns), and the committed hand-pair manifest
  produced by running the pinned 2,000,000-iteration selector. The single
  cell and the evaluator are the reviewable units.
- Certifier timing note (finding n5): the prototype certifier has no
  independent Budget/deadline; an overlong exact BR walk classifies through
  the resolve wall (SolveDeadline/ResourceExhausted), which is an acceptable
  measurement fail-close; the campaign keeps `certify_s` inside
  `resolve_wall_s`.
- Margin-reference coverage (finding D1, resolved for the cell; the driver
  inherits it): the locked-margin reference is a finite-iteration MCCFR
  profile evaluated by an exact all-action/all-river fan-out, so it serves
  the CURRENT strategy at every reached infoset - average where accumulated,
  RM+ over clipped regrets on a visited-but-un-averaged row, uniform only for
  a never-visited infoset (the standard MCCFR current strategy). The SERVED
  gadget policy stays strict (`StrictPolicy` throws on a missing row), so this
  fallback cannot mask a Gate-2 coverage miss. All cell exceptions are caught
  in `main` (stderr + exit 4, no row, no abort). Verified: W1
  `--extract-margins` no longer aborts; a full-range zero-margin monotone
  cell exits 0 with 177k concrete infosets. The full-range
  exact-enumeration margin extraction itself is O(combos x tree x river
  cards) and dominates wall time; making it tractable inside the Gate
  envelope (or certifying a per-stratum margin-iters floor) is a driver
  deliverable.

## 9. Review gate before any measurement

This document AND the prototype code are reviewed together by a
separate non-author agent (the round-1 reviewer continues; findings are
not bypassed by switching reviewers). The review must explicitly
approve: (i) the ConcreteCards key, the committed one-hook/two-site
seam, and the three native tests; (ii) the turn gadget/margin and multi-street
certifier, including the measurement-only 1e-6 slack; (iii) the
additive partition — identity math, the 5.3 total label rule, the
served-abstraction-game Total convention, and the pinned Gate-3
objects/H0-bias reporting; (iv) the frozen hand-pair rule, reference
identity, and manifest; (v) the CSV schema and every hard predicate in
`rfc0046-gates.ts`; (vi) link-graph isolation, the stage-6 guard, and
the deletion set. ONLY after that approval do Gates 1-3 run, followed
by the mandatory second non-author evidence review.

## 10. Independent review decision record

- Author agent: Claude (zzzode agent session).
- Approved by: independent non-author approval agent
  `a7331a3d8f57387ae` (`auto_model/alwaysday1_max`), the same separate
  reviewer across every round; no author self-approval.
- Decision date: 2026-11-04.
- Review outcome: **Approved**.
- Reviewed scope: this harness design against the APPROVED Stage-0
  pre-frozen gate addendum and RFC 0046, with every load-bearing source
  citation independently verified in the engine.
- Round history: round 1 Changes Requested (one blocker F1, six major
  F2-F7, four minor F8-F11; the section-8 stop was CLEARED on the
  reviewer's own derivation); round 2 Changes Requested (F1-F11
  verified resolved; new major R1 on the seam site list plus m1-m3
  citation nits); round 3 Approved after R1/m1-m3 corrections (the
  reviewer's own test-only-caller claim was independently corrected by
  the author in source: `NSeatPolicy::lookup` is the production serving
  read path via `NSeatBehaviorPolicy::distribution`); v3.1 targeted
  clarification Approved (T_trunk structurally zero in the turn-rooted
  game; n1 prose tightened; pre-turn exploitability recorded as a
  residual risk for the production design addendum).
- What the approval authorizes: building the benchmark-only prototype
  (ConcreteCards keyer TU, the two-file trainer seam, the turn-boundary
  gadget generalization, the multi-street certifier, the per-infoset
  partition walker, `tools/rfc/rfc0046-gates.ts` with node tests, and
  the CMake isolation guard) and bringing the joint doc+code review.
- What it does NOT authorize: any Gate 1-3 run; any commit of the
  harness or prototype before the joint review approves; any production
  solver/schema/BSFLIB/mount/host/cache/serving/enablement change.
- Carried residual risks (re-checked at the joint review): (1) the two
  seam hunks must diff as pure branch-on-pointer with the nullptr path
  bit-for-bit identical (golden before/after on a seed-fixed run);
  (2) underconverged H0 biases the Gate-3 gap in the dangerous
  direction, surfaced as evidence with no privately invented floor;
  (3) identity totals must use one deterministic single-pass
  accumulation; (4) the behavioral hand pair must exist at the pinned
  2,000,000-iteration monotone reference, evidenced by the committed
  manifest with the predeclared fallback; (5) the CMake target-graph
  isolation ctest must cover engine-host, practice, resident/mmap, and
  pack transitive edges, and the stage-6 offline guard must stay green;
  (6) genuine pre-turn trunk exploitability is invisible to a
  turn-rooted measure and is deferred to the production design addendum.
