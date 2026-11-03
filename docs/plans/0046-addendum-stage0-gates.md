# RFC 0046 Stage 0 — Pre-Frozen Gate Addendum (APPROVED)

- RFC: [0046 M2 Full-Street Heads-Up Blueprint](../rfcs/0046-m2-fullstreet-hu-blueprint.md)
- Status: **APPROVED 2026-11-04 (v3, three review rounds)** by a
  separate non-author independent approval agent. Approval authorizes
  RFC 0046 Stage 1 (benchmark/tests-only exact-key prototype and
  measurement harness), the evaluator `tools/rfc/rfc0046-gates.ts`
  (with swap definitions/CSV schema/hand-pair frozen at the Stage-1
  harness review before any gate runs), and running Gates 1-3 with a
  second non-author evidence review. It does NOT authorize a production
  solver, BSFLIB trunk schema, board-4/5 mount, host/cache serving
  change, or enablement. Author: Claude (zzzode agent session).
- Scope: FREEZES the measurement design for RFC 0046 Gates 1-3. It
  authorizes no production solver, artifact, mount, serving, or
  enablement.
- Governing rule: every predicate and value below is fixed BEFORE any
  measurement is examined. Section 9 makes any post-freeze change a new
  independent approval; there are no conditional pre-authorizations
  except the single Gate-3 cap-2 escalation spelled out in section 7.

## 1. Frozen strata, roots, and the turn-rooted enumeration

Endgame solving and Gate timing are at a TURN root (four public cards).
The hybrid NashConv metric is computed at the five-card river board
(section 6). Six frozen strata; the W1 anchor IS one of them (not seven):

| name | betting root | turn board (4) | river card |
|---|---|---|---|
| w1 / limped anchor | limped: contrib 2, pot 4, stacks 198, button 0 | 2c 3d 7h Js | 9c |
| unpaired-rainbow-ahigh | limped | 7s Kh Ad Js | 9c |
| paired | limped | 7s Kh Kd Js | 9c |
| monotone | limped | Qs Ks As Js | 9c |
| unpaired-twotone | limped | 7s Kh As Js | 9c |
| raised-srp | single-raised: contrib 4, pot 8, stacks 196, button 0 | 2c 3d 7h Js | 2s |

All roots: 2 seats, BB 2, `preflop=false`, `terminal=River`, identity
action schedule at the cap under test, built field-identical to the RFC
0045 campaign GameDef.

Card enumeration (each context is stated explicitly; mixing these was a
round-1 finding):

- At a FLOP root (3 public), the next-street chance is 49 public cards
  (`public_runout_cards` removes only public cards); deal-conditioned
  per-player after one's own 2 hole cards it is 47, and the fixed
  joint-deal universe begins C(49,2)xC(47,2).
- At the TURN root used here (4 public), the RIVER chance in the public
  tree is **48 cards**. Conditioned on the four public cards and one
  player's 2 hole cards, 46 cards remain, so the opponent holding
  universe is **C(46,2) = 1,035**; the RFC 0045 fixed turn fixed-runout
  universe is C(48,2)xC(46,2) = 1,167,480 joint two-hand pairs and
  chance-resolved x44 = 51,369,120 outcomes.
- Gate 2's exact-key turn-rooted subtree enumerates the **48-card public
  river chance** with blocker-aware concrete joint deals (the 44-outcome
  fixed-runout walk used inside a deal). Reported node and information-
  set counts MUST correspond to THIS enumeration; the Stage-1 harness
  builds board_size 4 turn-rooted trees (never the flop-rooted G3 object).

## 2. Card scheme, ranges, seeds, statistics

- Endgame CFR information sets are keyed on CONCRETE hole cards; the
  certified endgame path has no postflop card bucket. The TRUNK card
  abstraction is a Gate-1 selection variable only (section 5).
- Full range: the 1,326-combo RFC 0045 full range with uniform weights,
  board-blocking combos removed. W1 is the ten-hand anchor and is
  diagnostic only; it can never replace the full range.
- Seeds 4320/4321/4322 (full, n=3) and 4320..4324 (W1, n=5);
  independent per-seed deal batches. Half-widths use n-1 SD with
  t=4.303 (n=3) and t=2.7764 (n=5).
- Every mean compared to a frozen threshold in Gates 1-3 is compared on
  its UPPER confidence bound `UCB = mean + t*sd/sqrt(n)`. This applies to
  the six-stratum-max hybrid NashConv, the river decomposition term, and
  the Gate-3 cap gap. Point estimates are never accepted.

## 3. Frozen machine profile and timing sample (Gate 2)

Timing is reproducible; this section is mandatory, not advisory.

- Host: n37-100-184, release build, `OMP_NUM_THREADS=1`, otherwise-idle
  machine (same convention as RFC 0045 Stage-3 section 6). A run whose
  machine load materially distorts timing is discarded and rerun per
  that rule; the deterministic gap values are unaffected by load.
- Cold resolves: **R = 10 independent cold resolves per (stratum, seed)**
  = 30 per stratum (3 full-range seeds x 10), each a fresh
  process-equivalent cold cache (no reusing a prior public-state cache
  entry and no warm-hint run counted toward timing). Record every wall
  time.
- Statistics, frozen estimator: p50 and p95 use the nearest-rank /
  inverse-empirical-CDF method (Hyndman-Fan type 1) on the 30 samples
  for a stratum: p95 is the 29th ordered value and p50 the 15th (the
  p95 index coincides with Hyndman-Fan type 3 at n=30; the p50 tie
  convention is not relied on since type 1 is fixed). `max` is the
  single largest of those 30. Predicate: **p95 <= 10,000 ms AND
  max <= 12,000 ms** (12,000 is a new a-priori value introduced by this
  freeze; the production default is only the 10,000 ms p95). At n=30
  the two clauses are independent: one of thirty tail runs (the 30th)
  may land in 10,000-12,000 ms via the max clause, while p95 (29th)
  must still be <= 10,000 ms.
- Exactly ONE bounded diagnostic profile is allowed PER STRATUM IN TOTAL
  (no rerun under any outcome), solely to classify a deadline/resource
  failure as bound-unreachable vs budget-tight: nodes up to 10,000,000,
  bytes up to 2 GiB, wall up to 30,000 ms, information sets up to
  2,000,000, run once. It can never produce a PASS. If the diagnostic
  run itself is inconclusive or resource-exhausted (still cannot
  complete or certify inside those bounds), the stratum is classified
  BOUND-UNREACHABLE -> Gate 2 CLOSE.

## 4. Frozen resource envelope (Gate 2)

The production `ResolveLimits` defaults are the STARTING envelope and
are all gates (exceeding any one is a miss for that stratum):

- max_nodes 5,000,000;
- **max_information_sets 1,000,000** (production
  `resolver.hpp` default; previously omitted — now an explicit gate);
- max_bytes 1 GiB / peak RSS <= 1 GiB.

The exact-key information-set count is BOTH reported AND gated. A
candidate above 1 M infosets (even with nodes < 5 M / RSS < 1 GiB) is a
Gate-2 miss, because it cannot run under unmodified production limits.
Any proposal to change a production shared limit requires a new RFC /
addendum and can never be counted as a Gate-2 pass.

## 5. Gate 1 — trunk selection

Closed candidate set (no post-hoc additions; any new candidate needs a
new frozen addendum + independent approval per section 9). Four
explicit configurations:

- **T0**: tiers-only (CategoryTiersV1) pre-river trunk, turn rows only.
- **T1**: coarse EHS 32/64/64 pre-river trunk, turn rows only.
- **T2t**: T1 card scheme, turn rows plus a coarse RIVER WARM-START
  hint used only to initialize the endgame solve.
- **T2h**: T1 card scheme, turn rows plus a coarse river hint that is
  also available to the certifier as starting data. Both T2 variants
  must leave the Certified verdict identical to a cold start within the
  determinism tolerance (section 6.1); a hint that changes the verdict
  invalidates that configuration.

A-priori thresholds:

- **B0 = 0.010 BB**: provisional primary target for the worst-stratum
  (max over the six strata) full-range chance-free summed HYBRID
  NashConv UCB.
- **R0 = 0.002 BB**: the river term of the per-street decomposition
  UCB (the endgame must be effectively exact).

Predicate, mechanically:

1. For each configuration, measure the served hybrid strategy and the
   decomposition of section 8. A configuration is ELIGIBLE iff on EVERY
   stratum the hybrid NashConv UCB <= B0 AND the river-term UCB <= R0.
2. Among eligible configurations select by the total order:
   (1) fewest sealed trunk rows, (2) smallest trunk bytes, (3) smaller
   cold-resolve p95, (4) the lexicographically smaller scheme name.
3. If at least one configuration is eligible, Gate 1 PASSES and the
   acceptance bar is fixed at **B = B0 = 0.010** (the other max() term
   in the v1 draft was a dead branch and is removed). If NONE is
   eligible, Gate 1 returns the section-10 global decision — it has no
   REDIRECT of its own to an un-frozen "richer trunk".
4. All Gate-1 numbers MUST be produced with the exact-key endgame
   prototype paired with the candidate trunk, never the merged RFC 0011
   path.

## 6. Gate 2 — exact-key HU endgame feasibility and exactness

### 6.1 Exactness sub-conditions (required in addition to timing)

- Zero-merge NATIVE ctest (compiled/linked/runtime assertion, never a
  source-text grep, per the project test policy): the endgame
  translation units do not call `abstraction::card_bucket(...)` on a
  postflop hand; the known merged call site is
  `engine/src/resolver/subgame_resolver.cpp` (~line 767) and the test
  must fail if an equivalent call exists in a certified endgame path.
- Behavioral discriminator with a PRE-FROZEN hand pair, selected before
  any solve and shipped in the addendum evaluation harness: on the
  monotone stratum (Qs Ks As Js board), pick the deterministic pair of
  full-range combos that share the same `CategoryTiersV1` nine-category
  bucket but have distinct deterministic river-best actions in the
  cap-1 exact tree for river 9c (the pair is pinned by an ordering rule
  over the 1,326 combos at Stage-1 harness review, NOT chosen after
  seeing results). Served policy DIFFERENCE criterion: argmax action
  differs; if no such argmax-separating pair exists for that board, the
  rule falls back to the predeclared second stratum (unpaired-twotone),
  and the harness must prove the pair non-vacuous (both combos live,
  same category). The two served distributions' argmax MUST differ.
- Determinism: run cold-vs-cold and cold-vs-warm-hint profiles; served
  action probabilities bitwise identical and responder margin within
  1e-6 root-pot units.

### 6.2 Per-stratum status mapping and the global decision

Each (stratum, seed) resolves to exactly one typed outcome, aggregated
from its R=10 cold resolves by a frozen total rule (no tail deadline is
absorbed — the timing p95/max over the 30 samples separately governs
wall acceptance):

- A (stratum, seed) outcome is `Certified` IFF ALL TEN cold resolves
  return the certified bound AND each individual resolve stays inside
  the section-4 resource envelope (nodes <= 5,000,000, information sets
  <= 1,000,000, peak RSS <= 1 GiB). Equivalently a stratum requires all
  30 cold resolves (3 seeds x 10) to certify within the envelope on every
  resolve. A single resolve beyond any resource cap or non-`Certified`
  makes that seed (and hence the stratum) fail the envelope; the
  one-shot diagnostic profile then determines bound-unreachable vs
  budget-tight for the global mapping below.
- `CertificationRejected` — bounds fail even on the section-3 one-shot
  diagnostic profile -> BOUND UNREACHABLE.
- `SolveDeadline` / `CertifyDeadline` / resource-exhausted — a seed that
  did not certify within section-4; the section-3 diagnostic profile
  (strictly once per stratum) classifies it as budget-tight (certifies
  under the larger diagnostic profile) or bound-unreachable.

A stratum PASSES Gate 2 only if all three seeds are `Certified` under
the frozen envelope and 6.1 holds. Global decision (CLOSE precedence is
total, removing author discretion):

1. If ANY stratum is bound-unreachable (`CertificationRejected`, or
   deadline that the diagnostic profile shows still cannot certify with
   the bound failing), Gate 2 -> **CLOSE**.
2. Else if every stratum passes, Gate 2 -> **PASS**.
3. Else (all bound-reachable but some miss the 10s/resource envelope
   even on the diagnostic profile) Gate 2 -> **REDIRECT to Route C**
   (RFC 0039-style boundary value). There is no "increase the online
   resource cap" branch here; resource-limit changes need a separate
   approval (F1/F5).

## 7. Gate 3 — action-cap materiality

cap-0 here is an OFFLINE reference: it is exempt from the 10 s wall but
bounded by a predeclared outer budget (the G3 census cap-0 structural
limit >100 M nodes / 55 GiB; for the TURN-rooted reference the harness
uses a predeclared 20,000,000-node / 8 GiB / 600 s outer bound). If the
cap-0 turn-rooted reference cannot be built even within that outer
budget, Gate 3 -> **CLOSE** (no reference, no materiality verdict).

- **M = 0.002 BB** frozen. Measure, per stratum, the UCB of
  (cap-1 exact endgame NashConv) minus (cap-0 exact endgame NashConv)
  on the full range; the gap is the regret of the coarser menu relative
  to the unrestricted reference and is expected non-negative
  (NC(cap0) <= NC(cap1)). The evaluator fail-closes on an estimator
  anomaly: a significantly negative gap (lower confidence bound <
  -1e-6 root-pot, i.e. beyond numerical noise) aborts the gate rather
  than silently accepting. cap-1 is accepted for the online subtree iff
  every stratum gap UCB <= M.
- If any stratum exceeds M, the single pre-authorized escalation (no
  cap-3) is one remeasure of the ONLINE subtree at **cap 2**, under the
  UNCHANGED section-3/4 wall/node/infoset/byte envelope (Gate 3 changes
  only the action cap; it never relaxes time or memory). cap 2 is
  adopted only if, on every stratum that failed at cap 1, the
  (cap-2-minus-cap-0) gap UCB <= M and cap 2 fits the envelope;
  otherwise Gate 3 -> **CLOSE**.
- Gate interlock and global consequence: Gate 2's exact-key prototype
  must exist before Gate 1 hybrid numbers and Gate 3 gaps are measured.
  Gate 2 CLOSE stops Gates 1/3. ANY stratum CLOSE at any gate stops the
  RFC (no shipping the subset of strata that passed) and prevents entry
  to production design.

## 8. Per-street decomposition and the evaluator of record

The hybrid strategy's full-game chance-free summed NashConv is reported
as a single total plus four non-negative terms on the SAME deal sample,
estimated consistently so the decomposition is exact by construction:

- Total = T_trunk + T_boundary + T_action + T_river, where each term is
  the best-response regret contribution introduced at that boundary
  computed by swapping components one at a time against the full hybrid
  policy: T_river uses concrete endgame policy vs a merged-k_river
  policy; T_action uses cap-1 menu vs the cap-0 reference; T_boundary
  uses per-combo vs per-bucket turn margins; T_trunk is the residual
  locked-prefix regret. The exact swap definitions, the (identical)
  per-seed independent deal batches, the summation identity test
  (|Total - sum(terms)| <= 1e-9 root-pot), and the CSV schema are
  designed in Stage 1 and frozen in the Stage-1 harness review BEFORE
  any gate runs; they are not improvised in code.
- Verdict is emitted only by a frozen evaluator of record,
  `tools/rfc/rfc0046-gates.ts` (+ native node tests, convention copied
  from rfc0045-gates.ts), implementing the six-stratum-max, UCB, the
  decomposition totals, and the section-10 CLOSE/REDIRECT branches. It
  fails closed on incomplete strata/non-Certified rows. It is built and
  independently reviewed with the Stage-1 harness; a later change needs
  section-9 approval.
- The residual-risk note is explicit: if no additive decomposition
  satisfying the summation identity can be defined at Stage-1 review,
  Gate 1 cannot run and the RFC returns for redesign rather than
  redefining terms post hoc.

## 9. Re-approval clause (programmatic freeze)

Every frozen item is immutable once this addendum is Approved and until
all gates complete: B0/R0/M; the 10,000/12,000 ms and 5 M/1 M/1 GiB
envelope and the one-shot diagnostic profile; seeds and t multipliers;
the six strata and roots; the closed candidate set {T0,T1,T2t,T2h}; the
cap sequence {1,2}; R=10 and the machine/estimator profile; the frozen
behavioral hand-pair rule; and the decomposition/evaluator definitions.
Changing any item BEFORE a gate run requires a new independent
addendum approval. Changing any item AFTER a gate has run automatically
invalidates that gate's result and requires re-running it under a new
approval. The ONLY pre-authorized escalation is the Gate-3 cap-2
remeasure in section 7.

## 10. Global decision table

| Gate 2 | Gate 1 | Gate 3 | Outcome |
|---|---|---|---|
| PASS | >=1 eligible, pick by order | cap1 (or cap2) gap <= M | proceed to production design addendum |
| CLOSE (any stratum) | - | - | RFC CLOSED: exact certified HU endgame not reachable; Route C/no-go |
| REDIRECT (Route C) | - | - | stop; nested-boundary route needs its own RFC |
| PASS | none eligible | - | RFC CLOSED (bounded trunk cannot reach B0/R0) |
| PASS | eligible | any stratum CLOSE | RFC CLOSED (no partial-stratum shipment) |

## 11. Out of scope

Measurement only. No production solver beyond the benchmark prototype,
no BSFLIB trunk schema, no board-4/5 mount, no host/cache serving
change, no enablement. A later production design addendum and a separate
enablement RFC are required after a clean gate result and a second
independent evidence review. No-legacy applies to Stage 1: the
benchmark-only harness, seam, and gate evaluator are deleted after the
gates unless a later, independently reviewed production addendum
promotes a named subset.

## Decision

Approved 2026-11-04 after three independent review rounds (rounds 1 and
2 returned Changes Requested; all findings resolved). Review outcome:
Approved. Author agent: Claude (zzzode agent session). Approved by:
independent approval agent afd5e08d300cfc601 (separate, non-author).
Decision date: 2026-11-04.

Decision record of record (verbatim):

> 2026-11-04 independent approval agent (non-author), RFC 0046 Stage-0
> pre-frozen gate addendum v3 (B3-reconciled): Approved — R=10 per
> (stratum,seed) reconciles across sections 3/6.2/9 (3x10=30 cold-cache
> resolves; nearest-rank HF type 1 p95 29th <= 10,000 ms and max 30th
> <= 12,000 ms independently binding; certification requires all 30
> resolves within 5M-node/1M-infoset/1GiB); B1 cap-1-minus-cap-0
> non-vacuous gap with LCB<-1e-6 root-pot anomaly abort, N1/N2/N3 fixed,
> check-docs/check-rfcs green; authorization limited to Stage-1
> benchmark-only exact-key harness, evaluator tools/rfc/rfc0046-gates.ts,
> and Gates 1-3 with a second non-author evidence review — no production
> solver/schema/mount/serving/enablement authorized; frozen-item changes
> require new independent approval under section 9, sole pre-authorized
> escalation is Gate-3 cap 2.
