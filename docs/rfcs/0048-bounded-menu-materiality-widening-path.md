---
rfc: "0048"
subject: "Bounded Action-Menu Materiality and the Certified Menu-Widening Path"
status: "Accepted"
authors: "Claude (zzzode agent session)"
created: "2026-11-13"
updated: "2026-11-13"
owners: "Engine rfc0047 benchmark harness (measurement); future production resolver/solver path gated by a later addendum"
supersedes: ""
superseded-by: ""
---

# RFC 0048: Bounded Action-Menu Materiality and the Certified Menu-Widening Path

## Summary

RFC 0047's G5 gate requires the bounded online action menu (`identity(1)`, one
full raise per street) to be menu-immaterial relative to the uncapped
`identity(0)` menu within `M = 0.002` BB. The RFC 0047 S3/T6 embedding
measurement proved the cap-1 menu is highly material (a ~20 BB pure menu term
on the deep 100 BB anchor). This RFC records the D164 parameterized cap-k
widening curve that searches for the smallest bounded cap whose embedded
exploitability clears M, establishes that no online-feasible bounded cap does
so inside the G1 timing/resource envelope, and selects the quality-preserving
direction (offline and/or reach-aware widening) with explicit gates. It changes
no production behavior: the measurement component and the 0.002 BB bar stay as
committed, and no production menu, resolver, or contract is modified by this
RFC.

## Motivation

The M2 production blueprint (RFC 0046) builds a bounded cap-1 turn trunk plus an
exact per-combo turn-rooted river endgame solved online. Its certified error
budget B0 = 0.010 BB already allocates 8.72e-3 BB (RFC 0047 G3 turn gap
1.06e-3 + propagated envelope 5.66e-3 + river R0 <= 2.0e-3). T6 showed the
cap-1 menu restriction contributes a ~20 BB (Uniform) / ~153 BB (BrFavorable)
pure menu term on the deep check-to-river anchor — four to five orders of
magnitude over M and un-absorbable into B0. M2 therefore cannot be certified
until the menu restriction is resolved. D164 was scoped to determine, with
bounded same-caliber measurements, whether simply raising the online cap
recovers the quality bar without breaking the G1 online envelope.

## Goals

- Quantify the exact embedded exploitability `e_embed(k)` for every practical
  bounded cap k = 1..6 on the binding deep anchor, under a fixed comparable
  training configuration, both completion arms, with online NashConv, pure menu
  term, node count, wall/CPU time, and RSS.
- Determine whether any bounded cap achieves a certifiable `e_embed(k) <= 0.002`
  BB within the G1 online envelope (p95 <= 10 s, max <= 12 s, RSS <= 1 GiB,
  nodes <= 5 M).
- Identify the quality-vs-latency trade-off boundary and the viable
  quality-preserving path, without lowering the quality bar.

## Non-Goals

- No production menu, resolver, composition-root, ABI, protocol, or persisted
  format change.
- No re-opening or weakening of the M2 quality contract or the 0.002 BB bar.
- No full-stratum or 432-528-root exhaustive batch under this RFC's measurement
  phase (single bounded anchor roots only; a per-stratum MAX remains a separate
  gate).
- No cap-0 equilibrium solve (the uncapped multi-hour solve stays out of scope;
  the embedding method needs no cap-0 equilibrium).

## Current State and Evidence

All evidence is benchmark-only from `engine/benchmarks/rfc0047`
(`cap_embedding.{hpp,cpp}`, `rfc0047_cap_materiality.cpp`), seedless and
bitwise reproducible. The frozen identity (independently reviewed in T6) is

```
e_embed(k) = e_Gk(sigma_k) + pure_menu_term(k),   both terms >= 0
```

where `e_Gk` is the bounded policy's own exact NashConv and the pure menu term
is the additional best-response value unlocked only by cap-0-only actions.

D164 cap-k curve, w1 deep check-to-river anchor (198 chips = 99 BB behind at
big_blind = 2; pot 4 chips = 2 BB; all table values below are BB after the
kernel's big-blind normalization, so no gate number changes),
300 cap-k iterations, 16 threads, Uniform primary arm, BB:

| cap | online nodes | online NC | e_embed | pure menu | wall (ms) | RSS peak (KB) |
|----|--------------|-----------|---------|-----------|-----------|---------------|
| 1 | 10 | 0.02395 | 19.4822 | 19.4582 | ~40000 | 30460 |
| 2 | 34 | 0.12661 | 3.4023 | 3.2757 | 22054 | 30460 |
| 3 | 106 | 0.09159 | 0.9970 | 0.9054 | 29938 | 34876 |
| 4 | 320 | 0.11082 | 0.2596 | 0.1488 | 53674 | 63428 |
| 5 | 878 | 0.07066 | 0.1089 | 0.0383 | 113493 | 163608 |
| 6 | 1924 | 0.12671 | 0.1289 | 0.00217 | 304047 | 351316 |

Convergence probe at cap-6, 1000 iterations:

| cap-6 iters | online NC | e_embed | pure menu | wall (ms) | RSS peak (KB) |
|-------------|-----------|---------|-----------|-----------|---------------|
| 300 | 0.12671 | 0.1289 | 0.00217 | 304047 | 351316 |
| 1000 | 0.06821 | 0.0743 | 0.00610 | 700438 | 351236 |

Verified facts:

1. The pure menu term falls roughly 4-6x per added raise but only approaches M
   at cap-6, and the 300-iteration near-crossing (0.00217) is a non-converged
   transient: at 1000 iterations the pure menu term rises to 0.00610 (3x over M)
   as the policy's own NashConv falls and the structural restriction is exposed.
2. The online NashConv converges slowly (~T^-0.52; 300->1000 iterations gave
   only 1.86x reduction). At cap-6 the 1000-iteration NC is still 0.0682 BB,
   34x over M; reaching 0.002 by extrapolation needs order 10^5-10^6
   iterations. This iteration estimate is a NECESSARY-NOT-SUFFICIENT, if
   anything optimistic lower bound: even if the NC reached 0.002, the pure-menu
   structural floor observed at cap-6 (0.0061 and non-decreasing across the only
   two observations) would keep e_embed = NC + pure-menu above M on its own.
3. Cost already violates G1 at cap-6: 700 s wall for 1000 iterations (70x the
   10 s p95), 351 MB RSS. The binding G1 dimensions here are wall time and
   convergence, NOT memory or node count (351 MB << 1 GiB; 1,924 online nodes
   << 5 M). Note the capcurve rss_peak_kb is a process-level getrusage
   high-water mark across an in-process multi-cap sweep; clean per-cap RSS uses
   a single-cap `embed` process, and either way RSS is non-binding. The caps
   whose structural menu term approaches M (cap >= 5) cannot be converged
   online; the caps that converge fast enough (cap 1-2) carry a 3-19 BB menu
   error.
4. Correctness is independent of the convergence issue: the embedded EV root
   values match the bounded kernel's own CFV x 990 to < 1e-6 at both cap-1 and
   cap-2; serial/threaded results are bitwise identical; the 15-fixture native
   test is green.

Conclusion: no bounded cap achieves a certified `e_embed <= 0.002` inside any
online-feasible budget on the deep anchor. Uniform online menu widening is not
a viable standalone fix.

## Design Principles

- The certified quality bar (0.002 BB menu materiality, 0.010 BB B0) is
  immutable under this RFC; a design that cannot meet it is redirected, not
  excused by relaxing the bar.
- The offline boundary and seedless exact enumeration are preserved; any widened
  equilibrium must remain deterministic and independently certifiable.
- Blast radius stays inside the rfc0047 benchmark until a later addendum
  explicitly authorizes a production path.
- Occam's razor: prefer the smallest change that preserves the quality
  guarantee over a new abstraction.

## Proposed Design

This RFC proposes a direction and its gates, not a production implementation.
Two candidate paths preserve the quality bar; a third is rejected.

### Path A (primary candidate): move the widened equilibrium offline

Pre-solve the wide-menu (or cap-0) equilibrium for the binding deep roots as an
offline blueprint artifact, and have the online resolver consume certified
per-combo continuation values rather than re-solving a wide tree within the
request envelope. The online component retains a small bounded tree for
adaptation, but its menu error is bounded against the offline wide reference by
the same embedding measurement, now scored offline without a latency constraint.

- Ownership: offline solver/artifact builder (new addendum), existing BSNCV-style
  codec, online resolver read-only consumption.
- Open design item: artifact size and build cost for wide-menu deep roots, and
  how the offline wide reference composes with the RFC 0047 turn-CV oracle.
- The Path A stage-1 spike must not anchor on raise-count alone: it also
  evaluates an alternative BET-SIZING abstraction (a different pot-fraction
  grid rather than more raises per street), which could recover the missing
  strategic value at lower tree depth than a wider raise cap. Both raise-cap
  widening and sizing-grid redesign are scored with the same embedding
  measurement before either is selected.

### Path B (secondary candidate): reach-aware selective widening

Widen the menu only at high-reach betting nodes, keeping the full cap-1 menu on
low-reach deep multi-raise lines, so the aggregate certified error clears M at a
fraction of the full-wide tree cost. This requires a NEW reach-weighted gate
(the current G5 gate is an exact per-root MAX with no reach weighting), so it
cannot be claimed under the existing contract and needs its own certified
bound derivation.

### Path C (rejected): re-open the quality contract with a menu-error term

Adding an explicit >= 0.006 BB menu-error line to B0 is infeasible: B0 = 0.010
BB already has 8.72e-3 allocated, and D164 explicitly forbids lowering or
re-opening the quality bar.

## Dependency Rules

- Measurement code stays in `bigshark_rfc0047_bench`, which remains forbidden
  from linking any production resolver or composition root (the configure-time
  rfc0047 link guard).
- Any future production widening addendum must depend on this RFC's accepted
  evidence and may not be implemented before independent approval.
- No dependency from production engine code onto the benchmark component.

## Compatibility and Migration

No runtime, protocol, persisted-format, or CLI contract changes in this RFC; the
only CLI touched is a non-installed manual benchmark binary, whose optional CAP
positional defaults to the prior cap-1 behavior. A later Path A/B addendum owns
its own migration and artifact-versioning plan.

## Security and Operational Impact

None for this measurement RFC: no credentials, network I/O, or game-platform
contact (the offline boundary is unchanged). Operational impact appears only if a
later addendum adopts Path A (offline build/storage cost) or Path B (a new
reach-weighted certification gate); both are out of scope here.

## Alternatives Considered

- Uniformly raise the online cap and accept the latency: rejected by the
  measured 70x G1 wall overrun and the un-converged NashConv at cap-6.
- Certify against the cap-1 equilibrium's own NashConv while ignoring the menu
  restriction: rejected as quality laundering; T6 established the menu term is
  the dominant error and the embedding gate exists precisely to catch it.
- Do nothing and ship cap-1: rejected; leaves M2 uncertifiable against B0.

## Risks

- The single-anchor curve is not a per-stratum MAX; shallower post-bet roots may
  clear M at smaller caps. Mitigation: a later bounded per-stratum measurement
  before any production claim; a single-root crossing is never a pass.
- The T^-0.52 convergence extrapolation is an estimate, not a certified iteration
  count. Mitigation: treat Path A/B gates as empirical stop conditions.
- Path A artifact footprint is unmeasured for wide deep-root trees. Mitigation:
  a feasibility spike (node/byte census) is the first Path A stage.

## Verification Plan

- The D164 native test `rfc0047_cap_embedding` (15 fixtures) pins cap-k mapping,
  fail-closed validation, cap-widening monotonicity, the EV-to-kernel-CFV
  oracle at cap-1 and cap-2, and bitwise determinism.
- The cap-k CSV curve and cap-6 convergence probe are the recorded evidence;
  any future Path A/B claim must reproduce the embedding measurement under the
  new design and show `e_embed <= 0.002` with the EV-oracle still < 1e-6.
- Full rfc0047 ctest family plus `check-docs`/`check-rfcs` remain the closure
  gates.

## Rollout Plan

1. Accept this RFC on the measured evidence (no code promotion).
2. Path A stage 1: offline deep-root feasibility spike covering BOTH a wider
   raise cap and an alternative bet-sizing (pot-fraction) grid, measuring
   size/build cost; stop if both exceed the artifact envelope.
3. Path A stage 2 or Path B spike: produce a design whose embedding measurement
   clears M on the binding anchor; stop and re-plan if it cannot.
4. Only after a design clears M within its stated envelope does a separate
   production addendum reach review.

## Stage-1 Evidence (D176, implemented; D179 unit correction)

The rollout stage-1 spike is implemented as the benchmark-only
`rfc0047-patha-grid-budget` census (commit d87248c; independent design review
approved, native gate green). Solve-free public-tree measurements on the w1
turn-rooted full trunk and one reached-river root. A D179 independent review
found and corrected an infoset-unit conflation in the original wording; the
corrected three-unit reading (metered separately from stage 2, commit recorded
in the stage-2 tracker entry) is:

- U1 ONLINE RESIDENT (the unit the ResolveLimits 1,000,000-infoset peak gates):
  turn trunk plus ONE reached river subtree. At identity cap-1 this is only
  26,461 infosets on w1 (12,408 turn + 14,053 reached-river), ~38x INSIDE the
  gate, matching tree_budget's conservative_sum exactly.
- U2 OFFLINE BSNCV ARTIFACT ROWS: one per-combo value row per frontier leaf,
  seat, and 1,081 combos (933,984 rows / ~37 MB for w1 cap-1); menu-width does
  not change the per-row structure.
- U3 WHOLE-TRUNK BUILD GRAND TOTAL (all 48 river fanouts summed, never
  co-resident): the 4.27M figure is U3, bounded by the build TreeLimits
  (2,000,000 nodes / 1 GiB), NOT compared to the online 1M peak. The earlier
  text that called 4.27M a 1M-gate overrun and "coarse 1.15M 15% over" is
  hereby RETRACTED as a U3-vs-U1 mislabel.

Corrected stage-1 findings that stand:

- Raise-cap widening on the full turn trunk hits the 1 GiB TreeLimits BYTE cap
  at cap >= 4 (typed tree_resource_exhausted, fail-fast): a real U3 build
  boundary. On the reached-river subtree (the correct U1 object) only caps <= 5
  fit 1 M infosets, and D164 showed caps <= 5 miss the 0.002 quality bar while
  cap-6 exceeds 1 M even there (2.08 M), so the online envelope and the quality
  bar do not intersect for raise-cap widening.
- Bet-sizing grids are structurally small (reached-river 7.6K-14K U1
  infosets) and cheap to build, BUT a D179 prefix-compatibility canary proved
  declared coarse/wide grids are not ordered prefixes of identity(0), so their
  e_embed against the identity(0) reference is undefined under the frozen
  formula (the embedding entry fails closed). Sizing-grid quality therefore
  needs a new union-menu reference formula and its own addendum; it is a lead,
  not a measured pass. Feasible builds cost single-digit to ~20 ms, so build
  cost is not the constraint.
- The infoset count is a menu-width-blind structural proxy; raised-srp pot-8
  relative inflation remains an unmeasured bounded residual.

Stage 1 therefore closes with: a negative raise-cap verdict under the correct
units, a byte-cap build boundary, and a sizing-grid lead that requires a new
reference construction before quality can be measured. No production behavior
changes; stage 2/union-formula work requires its own independently-approved
addendum.

## Stage-2b Addendum (D184): Union-Menu Reference Semantics

This addendum defines the benchmark-only candidate reference that makes
non-prefix sizing-grid quality legally measurable. It changes no production
behavior and does not amend the frozen G5 cap-0 instrument: the committed
`cap_embedding` component, its formula, and every recorded G5 verdict stay
byte-stable. The new instrument is a distinct reference domain with its own
identity and labeling; its numbers are conservative supporting evidence, never
an M2 or G5 certification pass.

### A. Candidate domain and preconditions

The union reference is defined only for the same object the frozen instrument
accepts: a two-seat, five-card, river-rooted `GameDef`
(`TerminalDepth::River`). Every construction below fails closed with
`std::invalid_argument` rather than returning a partial result:

1. The reference and bounded trees must be built from the identical root. The
   mapper calls the project's single root-identity predicate
   `bs::poker::same_game_def(ref.def(), bounded.def())` before any DFS; the
   earlier prefix mapper's kind-only comparison is insufficient for a
   generalized subset map.
2. The bounded menu must be a true subset of the reference menu at every paired
   node (element-wise `Action` equality on `(type, target_total)`). Any bounded
   action without an equal reference action is a bounded-not-subset failure.
3. Every reference-only (extension) edge must be `Bet` or `Raise`. A passive
   extension (`Fold`/`Check`/`Call`) cannot exist at identical states because
   passive actions derive solely from legality; observing one proves a
   state-divergence bug, so the mapper throws instead of mapping it.
4. Both abstractions use `CoverSeeds::MinAndCap` and the reference is uncapped
   (`max_raises_per_street() == 0`). The union algebra relies on the shared
   minimum/cap seeds; a `DeclaredOnly` input is rejected.
5. A menu exceeding 32 actions surfaces as the builder's existing
   `invalid_argument` ("abstract action count exceeds 32"), never as a silently
   truncated feasible menu. Callers treat it as a precondition failure.

### B. Union construction and the set-union proof

At any node the builder drives both abstractions from the same `LegalActions`
and `MenuContext` (identical reached `GameState`). Under
`build_menu_with_cover` the target set is

```
sort(unique( {minimum, cap} U {clamp(base + ceil(pot_after_call, f), minimum, cap)
                               : f in F} ))
```

with a fixed clamp at the node and shared MinAndCap seeds. Clamping is a fixed
function, so `clamp(F1 U F2) = clamp(F1) U clamp(F2)`; the union schedule's
fraction set therefore produces exactly the set union of the two clamped
target sets, and `sort` plus `unique` yields the same ordered action vector.
Clamp collisions collapse through `unique`, which is set semantics: no target
is lost and none is duplicated.

The union abstraction is constructed as
`ActionAbstraction::declared(union_schedule, CoverSeeds::MinAndCap, 0)`, where
`union_schedule` merges the per-street `bets`/`raises` fraction lists of the
uncapped identity schedule and the bounded grid schedule (value-sorted,
deduplicated in reduced form). It deliberately mints its own declared-family
digest: a union artifact can never be mistaken for an `identity(0)` artifact,
and tree equality is always established by `tree_shape_digest`, never by id
string. Passive actions are legality-derived and therefore identical; they can
never be union-only. Cap 0 disables the cap-suppression branch, so the union
contains the uncapped identity(0) menu.

### C. Generalized subset embedding (replaces prefix indexing)

The frozen prefix mapper assumes bounded actions are the first `|A_b|`
reference edges in order. That assumption is false for real grids: on the w1
reached-river root the coarse menu is `Check/Bet2/Bet198` while identity(0) is
`Check/Bet2/Bet3/Bet6/Bet198`, so bounded action index 2 (`Bet198`) sits at
reference index 4. The new path therefore uses an explicit edge map:

- `node_map[n_ref]` is the paired bounded-tree node index, or `-1` for an
  extension subtree.
- At every paired action node, `edge_map[a_ref]` is the bounded action index
  whose `Action` equals reference action `a_ref`, or `-1` when the edge is
  reference-only.
- Children are paired recursively through exact `Action` equality; a bounded
  action with no reference match fails closed (precondition 2), and a
  reference-only passive edge fails closed (precondition 3).

All mass and zero-mass decisions and every policy lookup go through
`edge_map`. There is no prefix-index arithmetic anywhere in the new
evaluator: reference edge `a` has zero embedded mass exactly when
`edge_map[a] == -1`, and otherwise its probability is
`avg_sigma[node_b][combo * menu_b + edge_map[a]]`. The BR max loop and the
node-level unmapped-extension interception are unchanged from the frozen
evaluator. When the bounded menu is an ordered prefix, the constructed edge
map is the identity map `[0..m-1, -1..]`, so the certified path's semantics
are a special case of the generalized map, not a separate code path.

### D. Evaluator, embedding identity, and the superset-BR proof

The evaluator keeps the frozen structure: one EV walk and one exact BR walk
per seat over the reference tree, the same showdown/fold/stake tables, the
same opponent-reach snapshot scaling, and the same two completion arms on
extension subtrees (Uniform `1/A`; BrFavorable per-combo max). The EV walk
follows only positive-mass mapped edges and throws `logic_error` if it enters
an unmapped extension; the BR walk maximizes over the full reference menu.

For a fixed bounded profile `sigma_g` with zero mass on `G_u \ G_g`: the EV
walk never enters an extension subtree, so `V(sigma_g; G_u) = V(sigma_g;
G_g)`; at each best-responding node the BR maximizes over a weakly larger
per-combo action set, so `BR(G_u)(i) >= BR(G_g)(i)` elementwise for every
combo. The inequality is independent of the completion rule, which only
changes continuation values inside zero-opponent-mass extension subtrees;
terminals are chip-history-determined and identical on shared paths. Hence

```
e_embed^u = e_Gg(sigma_g) + pure_menu_term^u,   both terms >= 0
```

with the same normalization (1,081 combos, 990 compatible deals, big-blind
conversion) as the frozen instrument. Because `G_u` is a superset of the
identity(0) menu `G0`, the union measure is a conservative upper bound
wherever the cap0-relative restriction is defined. Labeling precision: this
is strictly a same-profile statement. A wide-grid profile is not playable in
`G0` at all (it carries reference-foreign targets such as `Bet4`), so no
cap0-relative number for that profile exists; the union run changes both the
profile and the reference domain and can never overwrite the G5 cap0/0.002
verdict.

### E. Identity degeneration and the native gates

When the bounded abstraction is `identity(1)`, the union schedule equals the
default schedule and the union tree is structurally identical to
`identity(0)`. Gate N1 pins exact structural equality
(`tree_shape_digest`, node count, accounted bytes; the digest excludes
terminal payloads, so terminal semantics are pinned separately by the EV
oracle) and numeric equality of the union measure against the committed
`measure_cap_materiality(cap1)`. Identical edge order implies identical
floating operation order in the seedless kernel, so bitwise equality is
expected in principle; the native gate uses a tolerance of `1e-12` (the same
noise allowance the frozen distribution invariant uses) and keeps the
existing bitwise determinism pin as supporting evidence, plus the
EV-to-kernel-CFV `1e-6` oracle on raw per-combo root values.

Native gates (all behavioral; no source-text or log-pattern assertions):

- N1 identity degeneration: union(default, default, cap0) structural equality
  with identity(0), and the union measure equal to the committed cap1 measure
  within `1e-12`; EV oracle below `1e-6`.
- N2 subset mapping: the coarse grid maps into identity(0) through the
  interleaved edge map (root pin: bounded `Bet198` index 2 maps to
  reference index 4), including an end-to-end SOLVED fixture on the real w1
  root so a prefix-index port cannot pass; the wide grid against identity(0)
  rejects.
- N3 union mapping: the wide grid maps into its union with every bounded
  action matched and no passive extension; the union menu equals the
  element-wise sorted set union of the identity(0) and wide menus.
- N4 enumerable controls on a hand-built shallow river root: hand-asserted
  menu-set membership, the EV-to-kernel-CFV `1e-6` oracle, and per-combo
  `BR_union >= BR_bounded` (the superset max monotonicity positive control).
  The shallow root also reaches a `Raise`-type node under cap 2 with
  divergent raise fractions (raise-fraction merge coverage), and a second
  short-stack schedule forces two distinct fractions to clamp to the same
  all-in target (the clamp-collision case at the center of the set-union
  algebra).
- N5 determinism and fail-close: repeated builds/solves are digest- and
  bitwise-stable; mismatched roots (`same_game_def` false),
  bounded-not-subset, malformed fractions, and the 32-action overflow all
  throw `invalid_argument`.

### F. Solver plumbing constraints

The bench river solver gains one optional field,
`RiverSolveConfig::action_abstraction` (`std::optional<ActionAbstraction>`).
A null option is the single construction site that builds
`ActionAbstraction::identity(action_cap)`, preserving every existing numeric
path: tree layout, the RM+ uniform fallback, and iteration order depend only
on the tree, and the kernel has no RNG. Both solver free functions honor the
field through the one constructor. A grid embedding passes the SAME
abstraction to the solver and to the separately built bounded `AbstractTree`
(reading back `tree.action_abstraction()` where useful), so
`avg_sigma[node]` indexing aligns with the mapper's node order.

### G. Evidence shape, units, and boundedness

Every emitted row carries, mechanically rather than in prose: the
`union-candidate` reference label and a `supporting-evidence-not-m2-g5-pass`
marking; the bounded and union abstraction id digests and both tree shape
digests; the U1 online-resident, U2 artifact-row, and U3 build-grand-total
units from the stage-1 taxonomy (never using U3 to negate U1); node,
infoset, and byte counts; solve wall and CPU time; and explicit RSS as both
the `getrusage` `ru_maxrss` high-water mark and the current
`/proc/self/statm` resident figure. Resource boundaries fail fast as typed
`tree_resource_exhausted` rows with every feasibility verdict false.

A real single-instance coarse/wide reached-river quality run is authorized
only after N1-N5 are green, under the existing D164 per-instance iteration
budget (no new training or equilibrium budget), one process at a time,
fail-fast on any overrun. If a precondition or budget fails, the run emits
the precise counterproof or gap and no quality claim is made. A union number
that clears 0.002 BB is a CANDIDATE conservative certificate for that root;
it is never recorded as an M2 or G5 pass and never replaces the cap0
identity reference or its verdicts.

### H. Component boundary

The frozen `cap_embedding.{hpp,cpp}` is not modified. The generalized mapper,
union factory, edge-map evaluator, and candidate measurement entry land in
new benchmark-only translation units under `engine/benchmarks/rfc0047/`,
inside `bigshark_rfc0047_bench` behind its configure-time link guard. The two
instruments are distinct reference domains rather than old and new versions
of one component; unifying them is deferred unless a later addendum adopts
the union path for production, at which point that addendum owns the
refactor and its bitwise-neutral verification. No production resolver,
composition root, solver public API, oracle, ABI, protobuf contract, or
public CLI changes.

### I. Independent review record

A separately launched non-author review agent challenged the union algebra,
subset completeness, the superset-BR proof, conservatism labeling, identity
degeneration, solver numerics, fixture validity, and boundedness. Its first
verdict was Changes Requested with two blocking findings (the prefix-index
translation trap and the requirement to record the formula in this RFC) and
five strengthening findings; the dispositions are recorded above
(precondition 1 and the passive-extension invariant; the edge-map
specification; the N1/N2/N4/N5 gate additions including raise-fraction,
clamp-collision, and overflow coverage; the tolerance argument; mechanical
candidate labeling; and the single-site solver plumbing). The final verdict
is recorded below after re-review.

Re-review verdict (2026-11-13): **Approved**, no blocking findings. The
reviewer independently re-ran both documentation gates (48 RFCs; 507 text
files) and accepted the subsection H component boundary (frozen
`cap_embedding.*` byte-stable; new benchmark-only translation units carry
the union instrument as a distinct reference domain; shared extraction is
not required and is deferred to any future production-adoption addendum).
Two non-blocking advisories are adopted into implementation: (L1) the
generalized recursive mapper retains a per-pair node-kind equality guard
that fails closed; (L2) N1 stays a permanent CTest, and the new header
documents its derivation from the frozen evaluator (shared
1,081-combo/990-compatible-deal normalization, opponent-reach snapshot
scaling, Uniform/BrFavorable completion arms) so cross-instrument
convention drift remains reviewable.

## Path B Addendum (D192): Reach-Weighted Certified Bound

This addendum defines the benchmark-only math and native instrument for
Path B (reach-aware selective widening) and states exactly when a
reach-weighted certificate conservatively constrains the frozen G5
target. It changes no production behavior and neither replaces nor
reweights the frozen contract: the G5 per-root exact measure, its 0.002 BB
bar, the cap-0 reference domain, cap-1 production default, B0, and G1 all
stand. The D184 coarse/wide candidate failures (22.7-215 BB) are retained
as measured evidence, not repackaged.

### A. Target quantity, reach source, and dimensions

The frozen per-root target for a fixed bounded profile `sigma` embedded in
a menu-superset reference game G is

```
e(sigma) = sum_s sum_{compatible (i,j)} (BR_s(i,j) - V_s(i,j))
           / (1081 * 990) / big_blind
```

a uniform-deal exact measure per river root; the stratum gate is the
unweighted MAX over reached roots. The only reach with a proven relation
to this target is the evaluated profile's OWN per-combo forward reach
`pi_s(n,i)`: the product of `sigma`'s own action masses on the root-to-n
path. Reach computed from any other policy `tau != sigma` (a blueprint,
prior, or the cap-1 policy used to score a distinct hybrid policy)
produces a different weighted sum with no ordering relation to
`e(sigma)`; such a number is exploratory and can never be recorded as a
certificate.

Dimensions: `pi` is dimensionless (root reach 1.0 per combo); the local
gap `delta` defined below is in chips and already contains the opponent's
sigma-reach in its counterfactual expectation; raw chip sums convert to
BB through the same `1081*990*big_blind` normalization as the frozen and
Stage-2b instruments. All reach and gap quantities are per dense combo,
never public-node scalars, so blocker structure is preserved.

### B. Lemma 1 — the reach-weighted decomposition is an identity

At every mapped action node n of seat s, with `C(n,i,a)` the
BR-subtree continuation value of reference action a for own combo i
(opponent fixed to sigma, evaluated over the opponent's sigma-reach),
define

```
delta_s(n,i) = max_{a in A_ref(n)} C(n,i,a)
             - sum_{a in A_b(n)} sigma(n,i,a) C(n,i,a)
```

The max ranges over the FULL reference menu, including extension edges
that carry zero sigma mass. Telescoping the tree recurrence

```
BR(n) - V(n) = [max_a BR(n_a) - sum_a sigma(a) BR(n_a)]
             + sum_a sigma(a) (BR(n_a) - V(n_a))
```

at own nodes, with linear policy expansion at opponent nodes and zero gap
at terminals, gives

```
sum_i (BR_root(i) - EV_root(i))
  = sum_n sum_i pi_s(n,i) delta_s(n,i)
```

for each seat. The reach-weighted node sum therefore EQUALS the frozen
embedded exploitability to floating-point order: it is conservative by
equality, not by deleting or zeroing low-reach nodes. A node whose own
reach is zero for every combo contributes exactly zero by this identity;
it stays enumerated in the node table and is never removed. The native
instrument pins the identity per seat against the Stage-2b root BR-EV
walk and against the Stage-2b `e_embed` measure under both completion
arms.

### C. Lemma 2 — the selective-widening sufficient certificate

Let W be the set of nodes at which the bounded menu equals the full
reference menu (the Stage-2b edge map contains no -1 entry); no menu
restriction exists at such nodes. The structural upper bound must be
stated in the evaluator's RAW representation, where continuation values
are not normalized per deal. At node n for own combo i, each
continuation is `C(n,i,a) = sum_j pi_opp(n,j) E[payoff(i,j)|a]` over all
1,081 opponent combos (fold terminals count blocker-incompatible reaches
too, so the sum is over all 1,081, not only the 990 compatible deals).
Define

```
R(n) = max over descendant terminals of payoff - min of payoff
S(n) = sum_j pi_opp(n,j)                       (all 1,081 reaches)
B(n) = R(n) * S(n)
```

Every per-deal payoff lies within an interval of width R(n), so any two
continuation values differ by at most `R(n) * S(n)`; hence for the
gap `delta = max_a C(a) - sum_a sigma(a) C(a) = sum_a sigma(a)(C(a*) -
C(a))`, the nodewise dominance `B(n) >= delta_s(n,i)` holds for every
combo. The earlier draft's `B(n) = max |terminal payoff|` was invalid:
it omitted the opponent-reach mass S(n) (order 1,081 at the root) and
the range factor of two (a fold-to-jam swing spans the full payoff
interval), and the corrected bound is what the bench validates
nodewise. Then

```
C = sum_{n in W}     pi_s(n,i) delta_s(n,i)
  + sum_{n not in W} pi_s(n,i) B(n)  >=  e(sigma)
```

since all reaches are non-negative and `B >= delta` nodewise. `C <=
0.002 BB` is a SUFFICIENT certificate for the frozen gate; `C > 0.002 BB`
is inconclusive (loose bound or weak policy), never a policy failure and
never a gate pass. The certificate needs only a forward reach pass on
the bounded tree (which produces both pi_s and S) plus terminal payoff
metadata: no cap-0 equilibrium and, in a production-shaped deployment,
no reference-tree CFV walk. The reference tree is retained in the bench
solely to validate `B >= exact delta` and the Lemma-1 identity
empirically, including a RED fixture that proves the naive
max-payoff bound undercuts a measured positive-reach gap.

Expected tightness (design honesty, not a post-hoc hedge): the corrected
structural bound is generally loose. The root node alone has own reach
1.0 and S = 1,081, contributing up to `R(root)/(990*BB)` per seat to
the BB certificate; on the deep 99 BB anchor that is order 0.2 BB per
seat against the 0.002 bar, and even the shallow eight-chip root gives
order 0.01 BB. A metadata-only structural certificate is therefore
expected to read inconclusive on these roots rather than pass, and Path
B must not be presented as a low-cost route to the gate. Its durable
value is (1) the exact node-level diagnostic allocation of
exploitability from Lemma 1, which locates where the menu restriction
actually bites per infoset and reach, and (2) a conservative, typically
loose screen available without measured continuation values. A tight
nodewise bound is the Lemma-1 identity itself — the D184 measure under
a node-level allocation — not a cheaper certificate.

### D. Lemma 3 — foreign reach and node deletion are not conservative

Weights `rho` not equal to the evaluated profile's own reach make
`sum rho delta` deviate without bound in either direction: a policy that
places near-zero reach on a high-gap node drives the weighted score
toward zero while the exact exploitability stays large. The instrument
recomputes forward reach from the supplied policy and rejects any
supplied reach table that does not match, and it rejects gap tables
whose max omits reference extension actions. A bounded-menu-only gap
omits the extension option value by construction: it equals the local
within-game NashConv residual for a finite-iteration policy (and only
becomes zero at a within-game equilibrium), so scoring against it
without the full-menu max would launder the menu error away. The
instrument retains this residual separately as `bounded_gap` rather
than asserting it is zero. A
native tiny counterexample demonstrates both failures concretely. Root
aggregation is equally protected: for root values `e_r`, any weighted
mean `sum q_r e_r` satisfies `mean <= max_r e_r`, so root-level
weighting can under-report the frozen MAX and is refused; reach
weighting is legal only within a root across its infosets.

### E. Applicability preconditions and failure behavior

1. Same domain as Stage-2b: two-seat, five-card, river-root GameDef;
   MinAndCap abstractions; a valid Stage-2b SubsetMap.
2. Reach must be the evaluated profile's own per-combo forward reach;
   the validator recomputes it with the evaluator's exact snapshot
   multiplicative update (`pi_child = saved * sigma`, the same
   operation order as the Stage-2b walk — not an independently
   associated prefix product, which could spuriously reject a correct
   table) and requires an exact bitwise match: any tolerance would
   admit scaling attacks on the weights. It throws `invalid_argument`
   on mismatch, negative entries, wrong sizes, or non-finite values.
3. Every gap max covers the full reference menu; missing extension
   actions fail closed.
4. A node may be marked widened only when its edge map has no -1 entry;
   a widening claim over a node with a missing reference action is
   rejected rather than silently treated as complete.
5. Any structural bound is accepted only when `B(n) >= delta_s(n,i)`
   nodewise for every positive-reach combo; the bench checks this
   against exact measured gaps and rejects a bound table that
   undercuts any such node, as well as any aggregate certificate
   smaller than the exact exploitability.
6. Outputs are per root; the stratum MAX stays outside this component.
7. The identities hold algebraically for any fixed finite-iteration
   policy; they certify that policy's exploitability, not equilibrium
   convergence.
8. Snapshot immutability: the component takes the policy table
   (`avg_sigma`) once and threads that same immutable snapshot into
   both the reach recomputation and every gap continuation; pairing
   reach from one policy version with gaps from another breaks the
   identity even when every shape check passes.
9. Node domain: the sum runs over MAPPED OWN-SEAT action nodes only.
   Extension (unmapped) nodes never receive a gap term — their option
   value is already realized inside the anchor's full-menu max, so
   adding one would double count. A hybrid policy is admissible as
   long as its support stays inside the bounded menu (enforced by the
   Stage-2b distribution invariants), measured with its own reach.

### F. Native gates and the tiny-game scope

A new benchmark-only component `reach_bound.{hpp,cpp}` and native test
`test_rfc0047_reach_bound.cpp` exercise the hand-built shallow
eight-chip river root (enumerable menus, real seedless solves, no text
matching): cross-instrument identity against the Stage-2b measure under
both completion arms; per-node non-negativity; the per-seat telescope
against root BR-EV; structural-bound dominance with the corrected
`R(n)*S(n)` bound under BOTH completion arms (the bound is
arm-independent by the interval-containment argument, and the test
pins both), including a RED fixture proving the naive
max-|payoff| bound undercuts a measured positive-reach delta, plus the
W-all/W-none degenerate certificates; foreign/tampered-reach rejection;
widening-claim rejection when an action is missing; malformed, NaN, and
under-sized-bound rejection; an enumerated zero-reach node with zero
contribution; the extension laundering trap (zero sigma mass with a
positive gap discovered by an existence scan, bounded-only fake gap
pinned at zero on the same node — the scan fails with an explicit
diagnostic if no positive-gap extension node exists, with a
deeper-stack root as the parameterized fallback); determinism; the
two-root weighted-mean-versus-MAX control; and a node-level
full-joint-reach cross-check (T12) that recomputes each node's
contribution from an independent forward walk over
`pi_s(n,i)*pi_opp(n,j)` per combo pair and asserts equality per node,
not merely per seat, so a node-local opponent-reach error cannot hide
inside a correct aggregate. U1/U2/U3 units reuse the stage-1 taxonomy;
no full-root sweep, long training, or iteration increase is
authorized. A result that fails the conservatism checks is recorded as
a counterexample and the bound route is rejected or re-scoped, never
patched with a heuristic.

### G. Boundary and review

The Stage-2b components stay byte-stable; the reach-bound instrument is
a new parallel benchmark translation unit in `bigshark_rfc0047_bench`
behind the existing link guard, reusing the Stage-2b subset map and
solver entry without modifying them. No production resolver,
composition root, solver public API, oracle, ABI, protobuf contract, or
public CLI changes. The math, proofs, and counterexamples receive an
independent non-author review (recorded below) before implementation,
and the implementation receives a second review before closure.

### H. Independent review record

The Path B review rounds and their explicit verdicts are recorded here
before implementation approval.

Round 1 (2026-11-13, non-author agent a56fda80e49e62818): Changes
Requested. Lemma 1 and Lemma 3 were found mathematically sound; the
original Lemma 2 structural bound `B = max |terminal payoff|` was
rejected as invalid in the raw representation (missing the
opponent-reach mass S(n) and the payoff-range factor of two) and is
replaced above by `B(n) = R(n)*S(n)` with the all-1,081 reach sum,
together with an explicit expected-looseness statement. The round also
added the T12 node-level full-reach cross-check, the RED naive-bound
falsifier, snapshot-immutability and mapped-own-node-only
preconditions, the exact operation-order reach-validator
specification, and the T9 existence-scan form. The re-review verdict
is recorded after this revision.

Round 2 (2026-11-13, same agent): **Approved**, no blocking or major
findings. Two adversarial probes were settled: `R*S` dominance holds
under BrFavorable as well, because completion arms only choose
policies over existing edges and every subtree policy payoff is a
convex combination of the descendant terminal payoffs (interval width
R(n)), and the all-1,081 `S(n)` is well-defined at every node because
reach propagation is dense and blocker-blind (blockers enter only
through the showdown matrix). Adopted advisories: (L1) the T4
nodewise dominance check runs under BOTH completion arms; (L2) the
structural range uses evaluator-consistent signed payoffs (negation
swaps max and min but leaves the width invariant), documented at the
implementation; (L3) this verdict record. The approval gates the
bench-only `reach_bound.{hpp,cpp}` and T1-T12 native implementation,
RED first, with no production surface.

Round 3 (2026-11-13, same agent), implementation review: **Approved**,
no blocking, major, or medium findings. The counterfactual-reach
correction (the BR-walk own reach is counterfactual; the telescope
weight must be the independently propagated full forward reach) is
applied at every entry point, the three lemmas are backed by
falsifiable gates, and the blast radius is exactly the two isolated
bench CMake hunks. Adopted advisories: (L1) T12 now also pins the
opponent-seat forward reach and S(n) bitwise per node; (L2) the
bounded-only gap wording is corrected to "local within-game NashConv
residual, zero only at a within-game equilibrium"; (L3) this verdict
record. The structural certificate's expected looseness remains the
disclosed design property from subsection C; no production, ABI,
proto, persisted-format, or CLI surface changes.

## Path A Root-Menu Feasibility Addendum (D195)

The Stage-2b and Path B instruments showed the sizing-grid menu failure
is large and concentrated at reach-1.0 root-level nodes. This addendum
closes the feasibility question for root-level menu widening and the
offline wide blueprint with an executable bench decomposition, without
modifying any production abstraction or relaxing a frozen gate.

### A. Node-level menu/restriction decomposition

A first-draft decomposition based on the Path B `bounded_gap` column
was rejected in review round 1: that column is built from the FULL
best-response walk's continuation values, so it restricts the menu only
at the node itself while every deeper own node still maximizes over the
full reference menu. It telescopes to an upper bound on the bounded
game's NashConv, not an equality, and is retained in the records only as
a conservative residual allocation. The exact decomposition requires a
separate self-consistent restricted BR walk.

The restricted walk runs over the BOUNDED tree: at every own node it
maximizes only over `A_b`, never entering a reference extension
subtree, and pairs each bounded node back to its reference node through
the existing SubsetMap (whose recursion covers every bounded node). Let
`g_g(n,i)` be its local gap and let `C_g` be its restricted
continuation. At a paired node define

```
delta(n,i)    = max_{a in A_ref} C_full(n,i,a) - sum_b sigma C_full(n,i,b)
g_g(n,i)      = max_{a in A_b}   C_g(n,i,a)    - sum_b sigma C_g(n,i,b)
menu_gap(n,i) = delta(n,i) - g_g(n,i)
```

`menu_gap` is non-negative: set inclusion gives `C_full >= C_g` per
combo under both completion arms (arms only choose policies over
existing edges), while the policy expectations agree because extension
edges carry zero sigma mass and the EV walk never enters them (the
Stage-2b EV-to-kernel-CFV oracle already pins root CFV equality). A
local menu coincidence is NOT sufficient for a zero value: even when a
node's own edge map has no -1 entry, `C_full(n,a)` can still embed
reference-only option value at a deeper own-seat node in its subtree,
so `menu_gap(n) == 0` holds only when no reachable own-seat node in
n's mapped subtree carries an extension edge (equivalently, the full
and restricted BR values at n coincide). The local `widened` flag is
therefore retained as a structural label, never as a zero-gap
assertion.
Telescoping with the full forward reach, which the review confirmed is
the identical own-mass product on paired bounded and reference nodes,
gives exact node-granular identities:

```
sum_n sum_i pi_s(n,i) g_g(n,i)      = e_Gg(sigma)   (bounded NashConv)
sum_n sum_i pi_s(n,i) menu_gap(n,i) = pure_menu_term
e_embed(sigma)                      = e_Gg + pure_menu_term
```

The first sum reconciles against the solver's own
`nash_conv_bb`, and the second against the Stage-2b pure menu column,
under both completion arms. This requires a small benchmark-only walk
extension; no production abstraction changes.

The restricted walk is ARM-FREE. The bounded tree contains no
extension nodes by construction, so the Uniform/BrFavorable dispatch —
which only fires at unmapped reference nodes — has no decision to make
there; the walk takes no completion parameter and produces one
arm-independent `g_g`. "Both completion arms" applies only to the
arm-dependent `delta`/`menu_gap` reconciliations, each paired with the
same single restricted result. Records are keyed on bounded nodes and
paired back through the inverse of `SubsetMap::node_map`, which is a
bijection onto the mapped node set; the walk reuses the existing
terminal, opponent-scaling, and vector-pool conventions with no new
payoff logic.

### B. The fixed-policy floor and the empirical re-solve tier

Two claims must be kept separate.

1. Exact fixed-policy statement. Embedding the measured policy sigma
   (zero mass on any new action) into a game widened at a selected set
   of nodes removes precisely those nodes' reach-weighted
   `menu_gap`; its exploitability stays at least the measured bounded
   NashConv `e_Gg(sigma) = 0.0276 BB`, fourteen times the 0.002 BB
   bar, because the unwidened nodes and the policy residual remain.
2. Empirical re-solve tier. A re-solved policy sigma' at the same
   iteration budget has no theorem ordering its NashConv across
   different games: finite-iteration RM+ NashConv moves
   non-monotonically across game shapes (the D164 cap-k online NC
   sequence 0.0240, 0.1266, 0.0916, 0.1108, 0.0707, 0.1267 at 300
   iterations, and the cap-6 re-split where NashConv falls
   0.127 to 0.068 while the pure menu term rises 0.0022 to 0.0061
   between 300 and 1,000 iterations). Refusal of a within-budget
   re-solve therefore rests on measured evidence, not a universal
   floor: the T^-0.52 convergence law places 0.002 BB at order
   10^5-10^6 iterations with the observed 700 s wall at only 1,000
   iterations, and the across-grid residual band at the frozen 300
   iterations is 0.0240-0.0276 BB for every measured configuration.

### C. Expressibility boundary

The production `ActionAbstraction` builds node menus from a per-street
schedule plus a raise cap and has no per-node override, so a hybrid
that widens only the three reach-1.0 restricted nodes cannot be
constructed without changing a production class, which this addendum
does not authorize. The node-level split is the bench-rigorous
substitute: it states the exact removable quantity and the exact
residual floor for every widening selection, including the unattainable
perfect one, using only benchmark translation units.

### D. Blueprint artifact feasibility and the verdict criteria

The storage side is feasible from existing solve-free evidence: the
wide bounded reached-river tree holds 14,053 U1 infosets (inside the
1M peak), 2,162 U2 rows / 86,480 bytes, and a 24.1 MiB reference tree
inside node/byte limits; the stage-1 turn-trunk census measured the
wide grid at 7.07M infosets / 11.8 MB against 56.4M / 95 MB for cap 2.
Artifact size is not the constraint. A root-menu widening route is
FEASIBLE only if a measured menu configuration leaves a residual floor
at or below 0.002 BB within the existing iteration budget while
staying inside every U1/U2/U3 envelope. The fraction space is
continuous, so a finite menu set cannot prove universal
infeasibility; the evidence closes the practical space instead.
INFEASIBLE on every measured configuration within frozen budgets, with
no plausible within-budget candidate identified: the three measured
menu points (coarse 31.24 BB, wide 22.74 BB, identity cap-1 19.48 BB
embedded exploitability) sit roughly four orders over the bar, their
300-iteration residuals cluster within ~15% (0.0240-0.0276 BB, already
12-14x over the bar at the policy tier), raise-cap widening was closed
by D164, and storage is explicitly non-binding. The residual risk is
an unmeasured grid with materially faster same-budget convergence;
one additional existing-budget fine-grid anchor is available cheaply
if that risk needs another point. No number produced here replaces the
frozen G5 verdict.

### E. Scope, gates, and review

The work stays inside `bigshark_rfc0047_bench`: a self-consistent
restricted BR walk over the bounded tree (a new benchmark-only walk,
not a production change), a per-node exact menu/restriction split in
the manual diagnostic, and one permanent native reconciliation gate
(T13) in the Path B test. T13 asserts non-negativity and the
subtree-extension-free zero condition with a small chip-scale
tolerance (the two maxima come from independent walks over two trees,
so an ulp can appear in either direction even where the math is equal;
bit-exact assertions remain valid only for single-walk quantities such
as the retained `delta - bounded_gap` column), and scale-relative
chip-scale tolerances for the two telescope reconciliations —
restricted-walk gaps to the bounded NashConv and exact menu gaps to
the Stage-2b pure menu column under both completion arms, each paired
with the one arm-free restricted result. The local `widened` flag is
reported as a diagnostic, never used as a zero-gap invariant. The
retained full-walk `bounded_gap` column is pinned only in the form
that is actually true: an AGGREGATE sandwich
`e_Gg <= sum reach*bounded_gap <= e_embed` at chip tolerance, since a
difference of ordered functions has no nodewise ordering, plus the
genuinely single-walk nodewise relation `delta(n) >= bounded_gap(n)`,
which is bit-exact because both maxima select elements of one
continuation vector. The T13 zero clause includes an existence scan:
the fixture must contain at least one extension-free mapped node or
the gate fails with a diagnostic, so it cannot pass vacuously. No
production, ABI, proto, persisted-format, or CLI surface changes; no
full-root sweep or iteration increase. The
independent review rounds are recorded below.

### F. Independent review record

The D195 review rounds and explicit verdicts are recorded here before
closure.

Round 1 (2026-11-14, non-author agent a56fda80e49e62818): Changes
Requested. The sign-level decomposition and bench scope were sound,
but the draft's use of the full-walk `bounded_gap` column as an exact
bounded NashConv telescope was rejected as a one-level upper bound
(F1); the addendum now requires a self-consistent restricted BR walk
over the bounded tree with exact `g_g` and `menu_gap` identities. The
universal post-widening floor was split into the exact fixed-policy
statement and the empirical re-solve tier with the D164
non-monotonicity evidence cited (F2); T13 was upgraded to bit-exact
sign checks plus chip-scale telescope tolerances (F3); and the
infeasibility verdict was scoped to every measured configuration with
the continuous-grid residual risk stated (F4). Round 2 returned Changes
Requested with two gate-spec defects: the local `widened` flag is not a
subtree zero-gap condition (corrected to an extension-free-subtree
structural condition with the flag kept diagnostic-only), and cross-walk
sign/zero checks cannot be bit-exact (corrected to chip-scale
tolerances, bit-exact retained only for single-walk quantities); the
restricted walk is now specified arm-free with inverse-map pairing. Round 3
returned Changes Requested once more with two localized gate-spec
fixes: the nodewise `bounded_gap >= g_g` claim was retracted (only its
telescoped aggregate is ordered), replaced by the aggregate sandwich
`e_Gg <= sum reach*bounded_gap <= e_embed` plus the bit-exact
single-walk `delta >= bounded_gap` pin, and T13's zero clause gained an
extension-free-node existence scan. The re-review verdict is recorded
after this revision.

Round 4 (2026-11-14, same agent): **Approved**, no remaining findings.
The aggregate sandwich endpoints were independently re-derived against
the record semantics (`W(root) = max_{A_b} C_full`; lower bound from
set inclusion, upper bound from `A_b subset A_ref`) and confirmed
under both completion arms using already-emitted columns; the
single-walk `delta >= bounded_gap` pin was confirmed bit-exact. All
four T13 clauses are fully falsifiable with no vacuous path. The
addendum is implementable exactly as recorded; implementation-time
residual risk is confined to coding the restricted walk as a true
bounded-tree BR walk, backstopped by T13(ii) against the solver's own
NashConv. No production, ABI, proto, persisted-format, or CLI surface
changes.

Implementation review (2026-11-14, same agent): **Approved**, no code
findings. The restricted walk was verified as a true bounded-tree BR
walk with no reference/extension reachability by construction, the
arm-free structure holds, T13(ii) has no double normalization, the
extension-free predicate is fail-closed and non-vacuously exercised,
and the formal evidence reconciles bitwise with both the solver
NashConv and the Stage-2b pure-menu column on both grids, including a
real-data instance of the local-widened-but-subtree-restricted
distinction. One documentation-only finding (two tracker labels
blurring the local flag vs subtree condition) was corrected before
closure. The INFEASIBLE verdict remains scoped to measured
configurations within the frozen budgets.

## Averaging-Policy Re-Solve Addendum (D198)

The Path A closure established the fixed-policy residual floor. This
addendum covers the one untested degree of freedom the existing
benchmark solver already exposes: the average-strategy weighting. It
introduces no new menu, abstraction, solver framework, reference
domain, or certificate; it evaluates a genuinely different policy
candidate under the frozen target.

### A. Candidate definition and legality

The river solver accumulates its average strategy each iteration with
weight `w = t` under `linear_averaging=true` (the CFR+ linear average,
recent iterations weighted more) and `w = 1.0` under
`linear_averaging=false` (the uniform time average). Regret matching,
the tree, and the evaluator are identical between the two; only the
average-policy weights change. The snapshot is normalized by the
per-combo action sum with a uniform fallback at a zero total, so both
outputs are finite, non-negative probability rows summing to one.

The regret and reach trajectory is bitwise mode-independent: regret
updates, regret clipping, the regret-matching distribution, and reach
propagation never read the averaging weight; only the average-strategy
sum does. The two runs therefore share identical per-iteration RM+
behavior and an identical zero-total row set. A `tot == 0` row means a
combo has zero own reach at a node across every iteration — a genuinely
unreachable infoset — and the uniform `1/A` fallback fires in both
modes on exactly the same node/combo set. The pre-selected candidate is
the uniform-average policy solved on the exact Path A coarse cap-1
fixture (w1 reached-river root, 300 iterations, one thread), evaluated
in the unchanged cap-0 reference domain.

### B. No certificate transfer

A reach or residual certificate derived for one fixed policy does not
hold for a differently averaged policy without an independent
equivalence or conservatism proof, and none is claimed here. The
candidate is evaluated from scratch through the existing EV/BR and
union instruments; every number is exploratory until it satisfies the
frozen target definition itself. The reference domain, root weights,
target quantity, conservatism direction, and the 0.002 BB bar are all
unchanged, and no action is deleted, reweighted, or zeroed to produce a
pass.

### C. Budget, distinctness metric, and native gates

The governing limit is the per-run single-instance 300-iteration
authorization used by every prior anchor in this line of work; the
historical authorization unit is one single-instance process, never a
per-process solve count (the D164 cap-curve job itself ran many solves
in one process), and no cumulative training pool exists. The anchor
binary therefore performs both averaging modes in one single-instance
process: one new uniform-candidate solve plus one in-process linear
control solve, with no new grid, root, thread, or iteration budget.
This makes every comparison use loadable in-process tables. The
historical linear CSV aggregates remain the cross-run reference; the
in-process linear run is the binding control because no golden policy
table is persisted. A content hash of each `avg_sigma` table is added
as a supporting column; it is evidence, not a historical authenticator.
The default-path gate is an in-process same-binary linear-vs-linear
bitwise repeat, with the zero solver-TU-diff confirmation kept as a
review/CI hygiene item rather than a runtime assertion.

Distinctness is a deterministic metric, not a verbal threshold. For two
snapshots at the same node/combo shape define the row L1 distance
`d = sum_a |p_linear(a) - p_uniform(a)|`, and call a policy pair
distinct when at least one action node has `d >= 1e-12` AND their
content hashes differ. The hash is FNV-1a 64-bit over the flat
`avg_sigma` vectors in deterministic DFS node order, combo-row-major,
per action, so it is reproducible across processes. The gate is
evaluated twice: on the tiny root at 60 iterations as the pre-anchor
spend condition, and again on the two 300-iteration w1 tables inside
the single anchor process. If the anchor tables coincide, the run
closes as a legitimate "no new information" result rather than forcing
a difference.

The native gates assert: (A1) finite non-negative normalized rows for
both modes plus uniform-fallback legality and identical fallback
trigger sets between modes; (A2) the defined L1/hash distinctness
metric at both iteration counts with coincidence reporting; (A3)
independent external EV/reach validation of each supplied policy
through the unchanged evaluator; (A4) in-process linear-path bitwise
repeat stability; (A5) fail-close rejection of malformed,
non-normalized, NaN, and missing-data snapshots including the
zero-reach fallback rows; and (A6) a reported, non-asserted tiny-root
comparison that reuses the existing `nash_conv_bb` field for both
modes with their signed difference, root id, iteration count, and both
content hashes, as an early signal on the uniform-may-be-lower
hypothesis. A failed tiny distinctness gate cancels the anchor rather
than spending it.

### D. Review record

Round 1 (2026-11-14, non-author agent a56fda80e49e62818): Changes
Requested. The mechanism legality and scope were sound, but the
distinctness gate lacked a metric and threshold (resolved with the
explicit L1/hash definition plus an anchor-level re-check), the
baseline-reuse and policy-hash claims were re-based onto solver-TU
immutability and existing determinism gates, fallback-row parity and a
tiny-root NashConv signal were added, and the mode-independence of the
regret/reach trajectory is now stated. Round 2 returned Changes
Requested on an internal one-solve contradiction; the addendum now
uses two solves in one single-instance process (candidate plus in-
process linear control), specifies FNV-1a 64-bit DFS-ordered content
hashing, pins the tiny-root count at 60 iterations, reuses
nash_conv_bb for the A6 report, and classifies the TU-diff check as
review hygiene.

Round 3 (2026-11-14, same agent): **Approved**, no remaining design
findings. The process-level budget unit, distinctness metric, hash
serialization, and A1-A6 falsification set were confirmed executable
against the existing instruments; two stale tracker lines and the
verdict record were the only non-blocking items and are resolved.
Implementation proceeds RED-first with no production surface.

Implementation review (2026-11-14, same agent): **Approved**, no
blocking or medium findings. The FNV hash, snapshot-only root-value
entry, A1-A6 gates, two-solve probe, and bitwise baseline
reconciliation were verified. One low metric-unit mismatch (an
aggregate node L1 versus the defined per-combo row L1) was corrected
in the helper to the RFC definition; the fallback-row inference was
accepted under the mode-independence proof with a future solver-side
flag noted; and the uniform-average candidate was rejected on the
measured anchor (bounded NashConv 0.0746 BB versus the linear control
0.0276 BB). No G5 or M2 verdict changes.

## Rollback Plan

This RFC ships no production behavior, so rollback is confined to abandoning the
benchmark measurement branch; the cap-1 production default and all committed
RFC 0047 serve/certification paths remain intact and are independently
revertible.

## Open Questions

- Is Path A (offline wide blueprint) feasible within the artifact envelope for
  the deep 99 BB (198-chip) roots, and how does it compose with the turn-CV
  oracle?
- Does a wider raise cap or a redesigned bet-sizing (pot-fraction) grid reach M
  at the lower artifact cost in the Path A stage-1 spike?
- Can a reach-weighted gate (Path B) be derived with a certified bound that the
  existing exact-MAX gate cannot?
- Do any shallower frontier roots clear M at a small cap, changing where
  widening is actually needed?

## Acceptance Criteria

- The cap-k widening curve and cap-6 convergence evidence are recorded and
  independently reviewed.
- A quality-preserving path (A or B) is selected with measurable gates, with
  Path C explicitly rejected.
- No production contract, menu, or quality bar is changed by this RFC, and the
  rfc0047 link isolation and native test suite stay green.

## Decision

Accepted 2026-11-13 by an independent non-author approval agent in one review
round (verdict Approved; four non-blocking Low findings F1-F4, all resolved in
this revision before recording the decision).

```text
Author agent: Claude (zzzode agent session)
Approved by: independent approval agent a23f1202442be9357
             (auto_model/alwaysday1_max; non-author; separate launch; distinct
             from the D164 code-review agent)
Decision date: 2026-11-13
Review outcome: Approved
Reviewed scope: the full RFC, the committed D164 measurement component
(engine/benchmarks/rfc0047/cap_embedding.*, rfc0047_cap_materiality.cpp),
the 15-fixture native test, commits 7ea92b3 and f74f4e4, and the cap1..6 /
cap-6 convergence evidence in docs/plans/0047-stage1-tracker.md; the rfc0047
configure-time link guard and commit scope were independently confirmed to
keep production isolated; check-rfcs (48) and check-docs (502) green.
Review summary: the central conclusion (no bounded cap reaches certified
e_embed <= 0.002 BB inside G1) reproduces from the committed data and code;
the T^-0.52 extrapolation is appropriately hedged; Path A/B are credible and
Path C is arithmetically impossible (0.00872 + 0.00610 = 0.01482 > B0);
the 0.002 BB bar is never relaxed and the RFC stays measurement-only.
Finding dispositions:
  F1 unit mislabel "198 BB" corrected to 198 chips = 99 BB (big_blind=2);
     no BB gate value changes (kernel normalizes by big_blind);
  F2 RSS caveat added - rss_peak is a process-level high-water mark and RSS/
     nodes are non-binding; wall time and convergence are the binding G1
     dimensions;
  F3 alternative bet-sizing (pot-fraction) grid named explicitly inside the
     Path A stage-1 spike rather than anchoring on raise-count alone;
  F4 the 10^5-10^6 iteration extrapolation is stated as a
     necessary-not-sufficient lower bound because the cap-6 pure-menu
     structural floor (0.0061) alone keeps e_embed above M.
Authorization scope: this acceptance validates the recorded evidence and the
Path A/B direction only. It authorizes NO production menu, resolver, contract,
or artifact change; each Path A/B implementation requires its own later
independently-approved addendum, and any spend retains its separate approval.
```
