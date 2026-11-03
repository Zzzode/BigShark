# M2 Full-Street HU Blueprint — Production Design (pre-RFC, DRAFT for review)

> **SUPERSEDED 2026-11-04 by [`../rfcs/0046-m2-fullstreet-hu-blueprint.md`](../rfcs/0046-m2-fullstreet-hu-blueprint.md).**
> The first independent review found this draft's central substrate claim
> wrong: the RFC 0011 subgame resolver is NOT certified and its CFR infosets
> merge postflop to nine hand categories, so "reuse the existing resolver"
> does not remove the RFC 0045 static-bucket floor. The RFC now requires a
> NEW exact per-combo-keyed, independently certified turn-rooted endgame
> solver, plus pre-frozen measurement gates. Keep this file only as the
> decision history; the RFC is the authoritative design.

- Status: **DESIGN-ONLY DRAFT 2026-11-04 (superseded by RFC 0046 after independent review).** No implementation is
  authorized by this document. It existed for Tech Lead review before
  being promoted to RFC 0046 and going through the independent-approval
  process.
- Author: Claude (zzzode agent session, 2026-11-04).
- Supersedes nothing; consumes the terminal verdict of
  [RFC 0045](../rfcs/0045-m2-instrument-scope-census.md)
  (`instrument-inadequate-uniform-ehs`) and its G3 census.
- Scope: platform-neutral offline engine; no network, credentials,
  learned models, or heuristic-strategy production path.

## 1. Decision requested

Approve, or redirect, the recommended production route for full-street
heads-up 100 BB GTO before any code is written:

**A bounded, cap-1 offline blueprint TRUNK plus real-time exact
endgame (river) subgame resolving** (the "blueprint + endgame solver"
architecture already partially present in the engine), instead of a
single static bucketed full-street blueprint.

The draft below states what is already built, what RFC 0045 proved
about each route, the exact open choices that must be settled by
measurement gates, the artifact/mount changes, and the acceptance
plan. Numeric targets for anything not already frozen by RFC 0045 are
deliberately left as proposals to be frozen in the RFC, not asserted.

## 2. What RFC 0045 established (the failure to design around)

1. A static, uniform EHS postflop bucket policy, played directly,
   cannot certify the full range: at the valid 300M rung,
   - exact-hole gap QUAL bar (<=0.03 BB) PASSES at k_river 128/256
     (worst 0.02241 / 0.01895),
   - within-abstraction CERT bar (<=0.005 BB) FAILS at every frozen
     grid (worst bucket 0.01240 / 0.01531 / 0.01594), and gets WORSE as
     k_river refines 64 -> 256, with half-widths <=0.0006 (a tight
     floor, not noise).
   - On the ten-hand W1 range the same grids already certify
     (0.002768). The residual error is therefore specifically the full
     range's boundary/merged river holdings — different concrete hands
     collapsed to one bucket action with strategically distinct river
     values.
2. Finer uniform banding is not a cure (monotone non-improvement). The
   conditional (32,128,256) arm is river-inert.
3. The learned/nested continuation-value routes were already closed:
   RFC 0041 (neural CV) failed Stage 1 and was never built; RFC 0039's
   nested river matrix showed a high exact floor (~0.26 BB). The
   project direction is offline, no learned components.
4. G3: the uncapped (cap-0) HU 100 BB River-terminal tree exceeds
   100 M nodes / ~55 GiB — a full per-flop-class exact static blueprint
   is structurally infeasible. The cap-1 (identity raises, one raise
   per street) HU River-terminal object builds: 4.77-6.29 M nodes,
   ~3.0 GiB tree, 1.32-1.74 M sealed rows.
5. Exact enumeration is deterministic and cheap to time; the chance
   gap is load-independent. (Operational lesson only.)

Design consequence: the error is not "not enough CFR iterations on the
right abstraction" — it is playing a permanently merged static river
bucket. A production design must stop playing merged river buckets on
the hands that matter, while staying inside the G3 object bound and the
real-time wall.

## 3. Principles and existing substrate

Design principles (in priority order):

1. **Exact where it matters, bounded where it cannot be exact.** Real
   river play must condition on the actual two hole cards over the
   actual public board; only the pre-river trunk may be abstracted.
2. **Zero-sum HU guarantees apply.** Unlike the 3..10-seat resolver,
   the M2 object is strictly two-player zero-sum, so the resolving
   gadget has an equilibrium/safe-subgame interpretation (the existing
   certifier reduces to the RFC 0005 responder bound at two seats).
3. **Reuse the existing offline resolving substrate; do not invent a
   second path.**
4. **Fail closed and stay offline.** Any coverage/certification miss
   falls through the existing typed fallback chain; no network, no new
   credential or serving surface.
5. **No learned components and no production heuristic strategy.**

Already built in the engine (verified in source):

- `bs::resolver::resolve` — RFC 0005 terminal-only postflop gadget with
  an independent best-response certifier; `ResolveLimits.time` defaults
  to 10,000 ms; `BlueprintSource` is implemented over the resident
  policy set; at two seats the certifier is the exact responder
  non-regression bound (`engine/include/bs/resolver.hpp`,
  `engine/src/resolver/resolver.cpp`, `certifier.cpp`, `gadget_cfr.cpp`).
- `bs::resolver` subgame resolver — RFC 0011 turn-rooted River-terminal
  subtree: rebuilds the remaining action streets plus the 44-card river
  chance, solves once, and walks to the observed node
  (`engine/src/resolver/subgame_resolver.cpp`,
  `engine/include/bs/subgame_resolver.hpp`).
- Fallback chain blueprint -> terminal-only resolver -> subgame
  resolver -> operational fallback, already wired in
  `apps/engine-host/main.cpp`.
- `tree::DepthLimit` and the `TerminalFrontier` leaf plus
  `ContinuationValueModel` seam (RFC 0024/0039) exist for depth-limited
  roots.
- Resident mmap blueprint library (BSFLIB03), board 0/3 gating,
  practice-table offline engine process.

The production work is therefore mostly: (a) define/build the bounded
full-street HU TRUNK artifact, (b) confirm and, if needed, tighten the
real-time subgame's exactness and HU latency, (c) open board sizes 4/5
under the same fail-closed gate, and (d) certify the HYBRID strategy's
full-game exploitability — not a new solver.

## 4. Routes considered

### Route A — recommended: bounded cap-1 trunk + exact real-time river endgame

Offline: per flop class, build a cap-1 blueprint down through the
TURN (a bounded trunk; exact per-class river rows are NOT stored). Use
a coarse-but-adequate trunk card abstraction (grid to be chosen by
measurement; see 6.1) and identity action cap 1, staying within the G3
object bound. The trunk supplies the locked prefix, ranges, and
counterfactual margins.

Online (per observed public board, offline process): when a turn or
river decision is reached, run the existing subgame resolver to rebuild
the River-terminal subtree for THAT board from concrete joint deals and
re-solve it within the 10 s budget; serve the certified candidate and
cache the per-public-board result. The re-solved river policy
conditions on real hole cards over the real board, so the merged-static
river bucket that failed RFC 0045 is not what is played at the river.
The within-subgame CFR uses concrete deals and exact chip settlement
(subject only to the action cap and the chance enumeration budget), so
there is no river card abstraction in the served decision.

Why it targets the measured failure: the 0.012-0.016 bucket CERT gap is
the error of a policy that plays one merged bucket. Re-solving the river
subgame on concrete holes replaces that policy at the boundary. The
residual abstraction error moves entirely into (i) the trunk's turn
actions that the resolver takes as prefix and (ii) the action cap; both
are measured directly by the full-game hybrid NashConv in the
acceptance plan rather than assumed.

Risk/unknown to measure: whether a 10 s real-time re-solve of the HU
turn-rooted subtree at full ranges reaches the local certifier on the
six RFC 0045 worst textures, and how much trunk error leaks through.
These are explicit gates (sections 6.2, 7).

### Route B — non-uniform / strategic river bucketing for a static blueprint

Replace uniform EHS bands with solution-aware or strongly non-uniform
river clusters (fewer, strategically-placed classes), store a static
per-class river policy. Avoids real-time compute.

Why not recommended as the primary route: RFC 0045 already shows the
floor is from merging strategically-distinct holdings and does not
yield to adding uniform classes; designing and certifying non-uniform
clusters is an open research loop with no monotone convergence
predicate (the exact failure mode of RFC 0043), and it still stores a
merged policy. Keep it as a possible TRUNK compression for the
pre-river streets, not the river endgame.

### Route C — nested continuation values at the turn boundary (RFC 0039)

Depth-limited turn solving with a precomputed range-conditioned river
value matrix. Deprioritized: RFC 0039 measured a high exact floor
(~0.26 BB) and the nested design failed its Stage 1 design gate. Only
revisit if Route A's real-time wall proves infeasible and a cheaper
boundary value is required.

### Route D — learned continuation-value network (RFC 0041)

Closed at Stage 1 (label noise floor), never built, conflicts with the
offline/no-learned-components direction. Rejected.

### Route E — exact static full-tree blueprint

Structurally infeasible from G3 (>100 M nodes uncapped; even cap-1
per-flop-class exact rows are ~1.3-1.7 M and would need 1,755 classes).
Rejected as the stored object; its information enters through real-time
resolution instead.

## 5. Target architecture (Route A)

### 5.1 Offline trunk

- HU, 100 BB, cap-1 raise action abstraction, identity action schedule;
  roots over the same 200 identity-complete roots the G3 census
  enumerated, with the flop-class mapping already used by the resident
  library.
- Trunk depth: solve through the TURN; the river is deliberately left
  to the online resolver. The exact truncation (store turn rows only vs
  store a coarse river hint used purely for resolver warm start) is a
  measurement-gated choice (6.1); storing exact river rows is not
  required and would re-enter the G3 size problem.
- Trunk card abstraction: to be selected by a SMALL offline comparison
  (6.1). Candidate: retain a coarse EHS/tiers banding only pre-river;
  the trunk is allowed a (measured) abstraction error because the river
  boundary is corrected live. Do NOT assume the 0.005 CERT bar for the
  trunk; the bar applies to the served hybrid strategy.
- Artifact: extend BSFLIB03 / the sealed section format minimally for a
  trunk-without-river object (board-size 4 terminal rows; board-size 5
  rows optional/absent). Exact schema change is designed in the RFC
  with codec round-trip and a versioned kind; old artifacts keep
  loading. Packing stays deterministic and byte-reproducible.

### 5.2 Online resolution (offline engine process)

- Reuse the RFC 0011 subgame resolver entry; ensure it is invoked for
  HU board sizes 4 AND 5 (today's gate is board 0/3), keeping the same
  typed statuses and fail-closed behavior.
- The subtree is built with concrete joint deals conditioned on the
  observed board (blocker-aware), cap-1 action menu, and exact
  settlement. Confirm by audit and a ctest that the served subtree
  policy never looks up an EHS/merged river bucket (the RFC 0045
  seam is already deleted; the resolver must use concrete deals and the
  resident blueprint only for locked-prefix rows/margins).
- Per-public-board solve result is cached in-process (memory bound TBD,
  bounded LRU keyed by the canonical public state). Cache is optional
  performance, never a correctness source; a cold miss resolves or
  fails closed.
- Latency/memory governed by ResolveLimits (default 10 s) with a
  request-derived public budget; never condition on hero cards. The
  independent certifier must return Certified; SolveDeadline/
  CertifyDeadline/Ineligible fall through unchanged.

### 5.3 Mount and serving

- Resident policy set mounts the trunk artifact; the board gate is
  extended from 0/3 to 4/5 by an explicit allow list validated at
  mount (fail closed on any other board), mirroring the current
  same_root/board policy.
- engine-host path is unchanged in shape; the existing resolver
  dispatch and fallback chain already exist. Practice table consumes
  the same offline engine process (no new client/API; the project
  stays platform-neutral).
- No network, no credentials, no new public protocol field beyond the
  existing diagnostic tokens.

## 6. Measurement gates before the design is frozen (all offline, no production change)

These are the facts the RFC must pin before committing the schema and
serving behavior. Each is a small measurement RFC-style stage with an
independent review; none authorizes production by itself.

### 6.1 Trunk selection

- For 2-3 candidate trunk card abstractions (e.g. tiers-only, coarse
  EHS, and a strategic-clustering pilot) at cap 1, measure hybrid
  full-game exploitability when paired with the existing resolver, on
  the six RFC 0045 worst textures plus the W1 anchor. Pick the coarsest
  trunk whose hybrid meets the acceptance bar; the trunk river rows are
  not served, so trunk training can use far fewer iterations than a
  standalone blueprint.
- Measure whether a coarse stored river hint materially reduces real
  solve time without changing the certified result (warm start only).

### 6.2 Real-time feasibility and exactness (HU-specific)

- On each of the six worst textures, time a cold subgame resolve to
  Certified at full ranges under 10 s, recording iteration count, peak
  RSS, subtree nodes, and the responder margin; confirm determinism
  (fixed-budget bitwise or stated tolerance) and that the candidate is
  exact-hole (no merged river bucket) by audit + ctest.
- If 10 s cannot certify on the largest texture, gate the options
  (larger but still bounded cap for the ONLINE subtree only, stratified
  solve, or Route C) — the choice is made on data, not assumed.

### 6.3 Action-cap error

- Quantify the exploitability contributed by cap-1 (no second/third
  raise) on HU 100 BB, since it is now the only action abstraction in
  the resolved endgame. Reuse a cap-0-vs-cap-1 comparison on the
  bounded object; if material, this changes the cap for the online
  subtree independently of the stored trunk.

## 7. Verification and acceptance (to be frozen in the RFC)

Proposed acceptance dimensions; exact numeric bars are frozen in the
RFC, reusing RFC 0045 conventions where applicable:

1. **Served-strategy exploitability (primary).** Full-game chance-free
   NashConv of the HYBRID strategy (trunk policy + certified real-time
   river resolves), summed BB, on the full 1,326-combo HU range, at the
   six worst textures plus W1, with n>=3 independent seeds and t-based
   half-widths. Target: a single frozen BB bar proposed in the RFC and
   independently reviewed — it must be clearly tighter than the static
   blueprint's and must hold on the FULL range (not only W1). The
   static-bucket 0.005 CERT bar is NOT assumed to transfer; the hybrid
   is a different strategy and gets its own frozen bar plus a per-street
   decomposition showing the residual error lives in the trunk/action
   cap, not the river.
2. **Per-decision safety.** Every served river/turn decision is either
   resolver `Certified` under the HU responder bound or a typed
   fail-closed fallback; zero silently-unresolved decisions on
   gated boards (instrumented in tests and at the host).
3. **Real-time budget.** Cold solve p50/p95 wall and RSS on the worst
   textures within a frozen budget (default 10 s) with a bounded cache;
   no decision budget derived from hole cards.
4. **Artifact & mount.** Trunk BSFLIB packs deterministically and
   byte-reproducibly; codec round-trip; board 4/5 mount allow-list
   rejects others; old artifacts still load; mmap bucket validation
   stays reject-closed.
5. **Offline boundary / protocol.** No network/credential dependency;
   engine-host and practice exercise the path; protocol unchanged or
   independently reviewed.
6. **Project-native regression.** C++ ctests for subtree exactness,
   board gating, cache bounds, fallback statuses; the retained RFC 0045
   TypeScript evaluator and chance-free walker are reused for the
   exploitability measurements (no script-text tests).
7. **Build gate.** Full release + ASan ctest, npm check, proto check,
   docs/RFC checkers green; no measured path in production.

## 8. Risks and open questions (to resolve in the RFC)

- Trunk error leaking into turn prefixes could dominate even with a
  perfect river solver — answered directly by acceptance item 7.1 and gate 6.1.
- 10 s feasibility at the largest HU texture is unproven (offline
  per-class solves ran much longer); gate 6.2 decides.
- Caching state across decisions must not become a correctness or
  information-leak channel; cache key is public-state only.
- Action-cap error (cap-1) at 100 BB HU is unmeasured (gate 6.3).
- Where precisely the trunk ends (turn rows only vs coarse river hint)
  affects both artifact size and warm start; gate 6.1.
- The 200-root / 1,755-class build envelope (G4) for the bounded trunk
  must be costed on the chosen truncation before committing compute.
- Whether the existing multiway-oriented subgame resolver needs a
  HU-specialized fast path (zero-sum exploitability vs the current
  per-seat non-regression framing).

## 9. Sequencing (proposed, RFC to authorize each stage)

1. RFC approval of this design (this document becomes RFC 0046).
2. Measurement stage 6.1-6.3 (offline, measurement-only, independently
   reviewed; reuse RFC 0045 harness conventions and evaluator).
3. Schema/mount design freeze (trunk BSFLIB kind, board 4/5 gate) with
   its own review.
4. Trunk builder + online exactness/feasibility implementation behind
   a default-deny gate; ctests.
5. Hybrid exploitability certification campaign; independent evidence
   review.
6. Enablement RFC for any production serving flip; nothing in this
   design changes served behavior before then.

## 10. Explicitly out of scope for this design

- Any production enablement, format flip, or mount activation (a
  separate enablement decision).
- 3..10-seat full-street (M3+); this design is HU zero-sum only.
- Learned models, network play, platform clients, heuristic
  production strategies.
- Implementation code; this document is docs-only pending Tech Lead
  review and independent RFC approval.
