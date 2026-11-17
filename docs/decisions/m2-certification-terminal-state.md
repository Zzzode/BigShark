# Decision Record: M2 Certification Terminal State — the Closed Menu Dilemma

Status: Current
Date: 2026-11-18
Scope: the M2 full-street heads-up production blueprint (RFC 0046) and
its RFC 0047/0048/0049/0050 measurement lineage. Companion to
[exact-menu-online-blueprint-closure.md](exact-menu-online-blueprint-closure.md),
which records the RFC 0050 campaign and the three serving walls; this
record states why the M2 certification line itself has no remaining
attack path.

## 1. One-sentence conclusion

M2 is uncertifiable under the current menu constraints and compute
because the menu-restriction residual and the convergence cost form a
**closed dilemma with no intersection**: every bounded menu that is
computable inside the online envelope carries a structural pure-menu
term orders of magnitude over the certified error budget, while the one
menu whose structural term is zero (exact cap-0) has no measured
convergence trajectory to the gate — and every intermediate point of
the menu lattice has been measured and fails one horn or the other.

This is a provable terminal state, not unfinished work. "Run longer",
"widen a bit more", or "ship cap-1 anyway" are each pre-rejected on
sealed evidence (sections 4 and 5).

## 2. Verification of the premises

Every premise in the closure request was checked against the sealed
record. All are accurate. Two precision nuances are recorded so future
readers do not overstate either horn:

- "cap-1 is computable" means the cap-1 tree is the only menu
  serviceable inside the G1 online envelope (10 online nodes, ~30-47 MB
  RSS on the anchor). Its own bounded NashConv at the 300-iteration
  measurement is 0.02395 BB, itself ~12x over M; the menu term is simply
  three orders larger and therefore the binding, dominant error. At
  20,000 iterations the control's bounded NC falls to 0.00006391 BB
  while the exposed pure-menu term grows to 31.19 BB — converging the
  policy does not dilute the structural term, it reveals it.
- The B0 residual margin (1.28e-3 BB, derived below) is tighter than the
  menu-materiality gate M = 0.002 BB. A structural fix must clear the
  smaller number to fit the ledger, not just the larger one.

## 3. Evidence chain, with receipt paths

### 3.1 The certified error budget and its remaining margin

RFC 0047 fixes the whole-strategy contract at B0 = 0.010 summed-BB
hybrid NashConv and R0 = 0.002 summed-BB river term on all six frozen
strata at full range
([RFC 0047](../rfcs/0047-depth-limited-certified-river-resolve.md),
G2/G3 text). RFC 0048 lines 34-39 record the allocation: 8.72e-3 BB is
already committed (G3 turn gap 1.06e-3 + propagated envelope 5.66e-3 +
river R0 <= 2.0e-3). The unallocated margin available to any menu term
is therefore

  B0_margin = 0.010 - 0.00872 = **1.28e-3 BB**.

### 3.2 Horn A — bounded cap-1 menus carry an un-absorbable structural residual

The T6 embedding measurement (frozen identity
`e_embed = bounded_NC + pure_menu_term`, both non-negative) on the deep
w1 check-to-river anchor, six-stratum sweep, 300 cap-1 iterations:

| stratum | cap-1 NC | Uniform e_embed | BrFavorable e_embed |
| --- | --- | --- | --- |
| w1 | 0.02395 | 19.4822 | 152.9223 |
| unpaired-rainbow-ahigh | 0.02554 | 19.4486 | 154.0793 |
| paired | 0.02412 | 19.5817 | 154.2606 |
| monotone | 0.00975 | 20.8220 | 169.7682 |
| unpaired-twotone | 0.02360 | 19.5265 | 153.7816 |
| raised-srp | 0.01437 | 29.4414 | 188.7606 |

Every stratum exceeds M = 0.002 BB by roughly 10^4x under the Uniform
primary arm and ~10^5x under the BrFavorable upper bound. Narrative and
the independent review record:
[docs/plans/0047-stage1-tracker.md](../plans/0047-stage1-tracker.md),
"T6 SIX-STRATUM ANCHOR SCOPE RESULT". The cap-k widening curve
(D164) and its conclusion ("no bounded cap achieves a certified
e_embed <= 0.002 inside any online-feasible budget") are frozen in
[RFC 0048](../rfcs/0048-bounded-menu-materiality-widening-path.md),
Current State and Evidence.

### 3.3 Horn B — the exact cap-0 menu has zero structural term but cannot be converged

RFC 0050 measured a zero pure-menu term on all six decision-bearing
anchors (`pure_menu_measured=1`, bitwise reproduced), eliminating the
structural wall by construction. The convergence measurement on rank 0
(28 threads, exact cap-0, decisive three-rung run):

| iteration | e_embed (BB) | segment log2 slope |
| --- | --- | --- |
| 400 | 0.24053200 | — |
| 800 | 0.14159415 (best; **71x over M**) | -0.7645 |
| 1600 | 0.17584872 (NashConv rose 24.2%) | +0.3126 |

The steep ~-1.0 law holds only for the first ~200 iterations; the tail
is shallow and non-monotone, with no observed trajectory toward the
gate. Receipts:
[docs/plans/ablation/rfc0050/decisive-convergence.csv](../plans/ablation/rfc0050/decisive-convergence.csv)
and the bit-identical rerun `decisive-convergence-rerun.csv`; full
argument in
[final-evidence-and-conclusion.md](../plans/ablation/rfc0050/final-evidence-and-conclusion.md)
and
[warm-start-legitimacy.md](../plans/ablation/rfc0050/warm-start-legitimacy.md).
Coverage is independently binding: 134,459 canonical classes at
6.1-27.2 s per iteration under a measured 2.2561x multi-thread ceiling
(arithmetic in the companion record, section 3.2).

### 3.4 The intermediate menu lattice was measured, not skipped

The "third path" between the horns is not an open gap; every candidate
in it has a sealed negative result:

- **Uniform cap widening k=2..6** (RFC 0049 Gate 2, frozen 1,800 s
  offline gate; receipts under
  [docs/plans/ablation/rfc0049/gate2/](../plans/ablation/rfc0049/gate2/),
  summary in [docs/plans/0049-stage1-implementation.md](../plans/0049-stage1-implementation.md)):
  every cap is BUDGET_NO_GO or CONVERGENCE_NO_GO. The smallest
  structural term observed on any bounded menu is cap-6 at 0.00959 BB
  (200 iterations) — 4.8x over M and 7.5x over the B0 margin — while
  the process latched at 227 of 20,000 requested iterations at 802 MB
  RSS.
- **Bet-sizing grids instead of more raises** (Coarse/Wide/Fine at
  cap-1): e_embed 22.22-31.32 BB with 99.81% of the error structural;
  the Fine five-point grid changes the Coarse result by only +0.022 BB
  (D249, receipt
  [s3/fine-grid/w1-fine-300.csv](../plans/ablation/rfc0047/s3/fine-grid/w1-fine-300.csv);
  Gate-2 CSVs `w1-{control,wide,fine}-cap1.csv`). Grid density does not
  recover root-level menu value.
- **Reach-aware selective widening (RFC 0048 Path B, D192)**: ~96% of
  the removable menu value sits at reach-1.0 root nodes, so widening
  deep low-reach lines cannot recover it; structural certificates read
  206-494 BB, five orders over the bar. Receipts:
  [s3/pathb-reach/](../plans/ablation/rfc0047/s3/pathb-reach/).
- **Perfect root-only widening (D195)**: even an unattainable perfect
  root widening leaves a fixed-policy floor of 0.0276 BB (13.8x over M);
  receipts [s3/patha-rootmenu/](../plans/ablation/rfc0047/s3/patha-rootmenu/).
- **Same-family solver switches**: uniform averaging measured 2.7x
  worse (D198), vanilla regret matching 2.86x worse (D251); linear RM+
  is the measured best mode. Same-game warm-start is analytically
  foreclosed (it is the bitwise-neutral resume trajectory).
- **Reopening B0 with a menu-error line (RFC 0048 Path C)**: rejected;
  the smallest proposed line (>= 0.006 BB) alone exceeds the 1.28e-3 BB
  residual margin, and the quality bar is frozen.

## 4. Formalization: the dilemma and its empty feasible column

Let `m(k)` be the pure-menu structural term and `c(k)` the cost of
driving the bounded NashConv to M for identity menu cap-k. Measurements
establish `m(k)` falls roughly 4-6x per added raise while tree nodes
grow ~3x per cap and wall time explodes; certification requires
simultaneously (i) `m(k) <= B0_margin = 1.28e-3 BB` on the per-stratum
MAX, (ii) bounded NashConv within M under the G1 envelope, and
(iii) closed campaign arithmetic over 134,459 classes. The three
representative columns:

| Column | cap-1 identity (w1) | cap-6 identity (w1) | exact cap-0 (six anchors) |
| --- | --- | --- | --- |
| Pure-menu term | 19.46 BB Uniform / 152.90 BB BrFav (300 iter); 31.19 BB at 20k iter | 0.00610 BB at 1,000 iter (D164); 0.00959 at 200 iter (Gate 2) — min still 4.8x over M, 7.5x over B0 margin | **zero, measured on all six anchors** |
| Own NashConv vs M | 0.02395 at 300 iter; 0.00006391 at 20k — can shrink, menu dominates | 0.06821 at 1,000 iter, 34x over M; T^-0.52 needs 10^5-10^6 iter | best 0.14159415 BB at iter 800, 71x over M, then non-monotone |
| Resource position | 10 online nodes; the only G1-serviceable menu | 1,924 nodes; 700 s per 1,000 iter (70x the 10 s G1 p95); offline BUDGET_NO_GO at 227/20,000 iter, 802 MB | 202 MiB tree (memory non-binding); 6.1-27.2 s/iter; 2.2561x thread ceiling; no crossing trajectory |
| Binding horn | **structural** | **both** | **convergence + coverage** |
| Verdict | fails B0 by ~10^4x | fails M, B0 margin, and G1 simultaneously | structural win, certification unreachable |

There is no column where the structural term fits the ledger and the
policy is computable to the gate. At cap-0 the structural term reaches
zero exactly where the convergence and coverage horns bind; at cap-1
the envelope is affordable exactly where the structural term is
catastrophic. The feasible sets do not intersect on the measured menu
lattice, and section 3.4 closes the routes around the lattice.

## 5. Restart criteria

The three serving-wall criteria in the companion decision record apply
unchanged and are not duplicated here:

- **R1** a certified non-CFR/RM+ solver path to e_embed <= 0.002 BB on
  all six anchors with a guarantee translatable to the project's
  one-sided NashConv;
- **R2** a zero-residual transport measured on the actual embedding
  path (`pure_menu_measured=1` discipline);
- **R3** 134,459-class campaign arithmetic fitting a declared core-hour
  budget, evaluable only after R1 evidence exists.

M2 adds a fourth, independent criterion:

- **R4 — a structural menu improvement inside the B0 ledger margin.**
  A bounded menu design whose per-stratum MAX pure-menu term is at most
  **1.28e-3 BB** (the unallocated B0 margin) while remaining inside the
  G1 envelope, measured on all six frozen strata with the frozen T6
  embedding identity and both completion arms. No candidate exists:
  cap-6's best measured term is 0.00610 BB (4.8x over the margin) at a
  70x G1 wall overrun, sizing grids retain 22-31 BB, selective widening
  cannot reach the root-level mass, and B0 reopening is contractually
  rejected. R4 is stricter than R2's zero-residual target in numeric
  terms but applies to any bounded-menu design, including ones that do
  not serve the exact cap-0 tree.

All four criteria are necessary; none is sufficient alone. A restart
needs a new RFC, pre-registered gates, native bench evidence, bitwise
reproducibility, and independent non-author review per the standards in
the companion record's section 4.4. An amendment to any closed RFC in
this lineage cannot carry a restart.

## 6. Downstream impact

- **G5 (certified serve; menu materiality).** The menu-materiality
  clause — per-stratum `(NC_cap1 - NC_cap0)` UCB <= M = 0.002 BB —
  FAILed on all six strata at T6 and has no measured route to a pass
  (sections 3.2, 3.4). G5 cannot certify either serve point; its
  fail-closed rule to the existing bounded path is the standing
  behavior. No `certified_bound` M2 strategy is mounted, shipped, or
  promotable until R1-R4 are met.
- **M2 production qualification (RFC 0046).** Uncertifiable against
  B0 = 0.010 BB on measured evidence. The blueprint's solver/oracle/
  mount enablement was never authorized; the bench-only measurement
  lineage leaves all production solver, schema, codec, mount, host, and
  wire surfaces unchanged. Existing engine and offline practice
  behavior continues with no M2 quality claim attached.
- **"Ship cap-1 directly".** Explicitly rejected twice on the record:
  RFC 0048 Alternatives rejects "do nothing and ship cap-1" (leaves M2
  uncertifiable against B0) and rejects certifying against the cap-1
  equilibrium's own NashConv while ignoring the menu restriction as
  quality laundering; RFC 0048 Path C rejects adding a menu-error line
  to B0. This option may not be re-proposed without R4-class evidence.
  The product-level analysis of what shipping it would actually mean —
  online-budget quality in BB per hand, the per-decision concentration
  of the leak, and its operational risk profile — is recorded separately
  in [cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md)
  as decision support, not as a certification.

## 7. Maintenance rule

This record stands while B0, its 8.72e-3 BB allocation, M = 0.002 BB,
the G1 envelope, the six frozen strata, and the 134,459-class frontier
remain as described. Any change to one of them requires re-evaluating
the affected criterion in the same change and updating this record; the
terminal verdict itself stands until at least one of R1-R4 is satisfied
with the recorded evidence standards.
