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

Status: NOT STARTED.

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
