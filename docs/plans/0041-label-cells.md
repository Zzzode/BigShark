# RFC 0041 Stage 1 — Fixed Label-Cell Plan

Status: **Historical** — RFC 0041 closed at Stage 1 with a recorded
FAILURE on 2026-10-29 (Amendment 2, rfc0041-amend2-approval-1). The
quotas below were never generated: the 10M x 3 scheme was withdrawn
after the cross-texture pilot, and the 150-class hardness survey proved
the 0.003 BB floor unreachable on 15.3% of real turn boards (see
Section 5 and docs/plans/ablation/rfc0041-*.csv). Retained as the
failure record; superseded for future work by whatever new RFC cites it.
RFC: docs/rfcs/0041-learned-river-continuation-value-network.md
Companion evidence: docs/plans/ablation/rfc0041-leaf-inventory.csv
Frozen: 2026-10-28 (before any `--rfc0041-genlabels` code exists);
T-scheme withdrawn 2026-10-29; RFC closed 2026-10-29

This document is the predeclared, fixed label-cell plan required by
RFC 0041 Rollout Stage 1 ("Before generation, commit the fixed label-cell
plan: target board-class count, line quotas incl. bet/raised lines,
stack/SPR grid, records per class, seeds, and the per-class
compute/storage budget"). The generator enumerates exactly these cells;
gates are frozen before the first gated training run.

## 1. Scope

- Seats: **2 seats only** for the v1 campaign (RFC rollout: 2 seats
  first; 3+ seats need their own campaign, evidence, and review).
- Stack profile: **100 BB only**, cap-1 action abstraction
  (`ActionAbstraction::identity(1)`), turn-rooted subgame
  (`make_subgame_def_np(2, 100, /*river_rooted=*/false)`) with
  `DepthLimit::streets(1)`. This is the production practice-table
  default stack. Other stack/SPR grids are later campaigns and must
  re-run AC4/AC5 and their own review.
- Labels are river-continuation marginals `V_p(c)` (chips, then
  normalized to BB in statistics) at turn-tree `TerminalFrontier`
  leaves, produced by `NestedRiverValueModel::refine` against the exact
  nested river game — the RFC 0039 seam, no other producer.

## 2. Board-class universe and split

- The information boundary is the **4-card turn board**, not the 5-card
  river board: all 48 river descendants of one concrete turn board
  share its information and must stay in one split partition (a
  5-card-only split would leak the turn board into validation).
- Universe: every suit-isomorphic turn board class under the engine's
  `bs::abstraction::canonicalize_turn` minimum-key rule (same rule as
  the flop canonicalizer, four cards), enumerated by
  `enumerate_canonical_turn_classes()`. Engine-verified pinned count:
  **16,432 classes** over 270,725 concrete turn boards
  (`test_canonicalize_turn_classes`, `--rfc0041-turn-classes`).
- Campaign quota: **360 classes total — 300 train, 60 held-out**. The
  60 held-out classes satisfy AC4's minimum of 60 independent board
  classes; no river descendant of a held-out turn class appears in any
  training shard.
- Partition rule (deterministic, frozen): enumerate the 16,432 class
  representatives in `std::array<int,4>` order, hash each index with a
  fixed SplitMix64-style mixing constant, select the campaign's 360
  candidate classes from the hash ordering, then within the candidates
  take the 60 smallest hash ranks as held-out and the remaining 300 as
  train — exactly 60 held-out classes, no river descendant of a held-out
  turn class appears in any training shard. The candidate cut and hash
  constant are pinned in the generator before the first shard and
  recorded verbatim in the manifest.
- Stratum coverage (monotone / two-tone / rainbow / paired / trip
  textures) over both partitions is recorded in the committed manifest;
  if any texture stratum lands fewer than 3 held-out classes, the
  campaign stops and this plan is amended before generation (no
  post-hoc re-splitting).
- The manifest records the exact class-index list, the SHA-256 digest
  of the enumerated class table, `suit_canonicalization_id()`, and the
  feature-scheme/abstraction ids; shards are validated against it.

## 3. Line quotas per board class

Empirical frontier census at 100 BB, 2 seats
(`docs/plans/ablation/rfc0041-leaf-inventory.csv`): 5 non-fold turn
closures per turn board, each fanning out into 48 river-card leaves
(240 frontier leaves / 357 tree nodes). All 5 closures are generated for
every class — the RFC 0041 AC4 floor makes bet/raised-line leaves
mandatory, and with only five closures there is no subsampling:

| Closure (F/C/R turn-line) | Pot (BB) | Kind | Quota/class |
| --- | --- | --- | --- |
| CC | 1.0 | check-through | 1 closure × 48 rivers |
| CRC | 3.0 | single bet, called | 1 × 48 |
| RC | 3.0 | bet into the turn lead, called | 1 × 48 |
| CRC | 4.0 | bet then raise, called (bet sizes) | 1 × 48 |
| RC | 4.0 | lead then raised, called (raise sizes) | 1 × 48 |

Total: **240 leaves per (class × range profile)**. The two same-pot
closures differ in which seat led the aggression and are distinct
ledgers; both are kept.

## 4. Range profiles per leaf

The network takes the exact conditioned per-seat ranges as inputs, so
labels cover a frozen set of three deterministic range profiles per
leaf (range support is the full 1,326-combo index space; the exporter
card-blocks board collisions, ~1,081 live combos/seat at a 4-card turn
board):

- **P1 — uniform conditioned:** the block-0 export under the uniform
  turn policy (the production outer block-PI block-0 distribution; same
  conditioning as the RFC 0039 M2/oracle100 protocol).
- **P2/P3 — perturbed conditioned:** per-seat independent
  Dirichlet(alpha = 1.0) weights over the legal support, drawn from a
  SplitMix64 stream keyed by (partition id, class index, closure, river
  card, seat, profile), card-blocked and renormalized to sum 1. These
  stand in for the non-uniform conditioned ranges outer blocks 1+
  produce; the joint-reachability correlation structure of real
  policy-induced ranges is out of scope for v1 and listed as a residual
  risk.

Each (leaf, profile) is solved with 3 independent seeds (below) and the
label is the **seed-averaged** `V_p(c)` map.

## 5. Solve scheme, seeds, and the label-noise floor

- Solver: `NestedRiverValueModel::refine` on the nested
  `DepthLimit::none()` river game, exactly as RFC 0039; MCCFR external
  sampling, RM+.
- Fixed iteration count per solve: **T = 10,000,000 iterations** per
  leaf/profile; **3 independent solve seeds** per label, master seed
  bases `4101`, `5101`, `6101` plus the model's per-leaf seed mix.
  Labels are the mean of the three marginal maps.
- Basis for T (`docs/plans/ablation/rfc0041-label-noise.csv`; full
  range, 2,162 seat-combos, 4 seeds, fixture board 2c 3d 7h Js):
  at 3M the worst of the five cells has mean half-spread 0.00277 BB
  (8% margin), at 10M the worst cell is 0.00131 BB (2.3x margin),
  range 0.00055-0.00131 across cells; worst-single-combo half-spread
  0.015-0.031 BB (reported, not gated). 10M is also the scheme RFC
  0041's acceptance record anchors (oracle100, ten-hand range,
  <= 0.00225 BB).
- **WITHDRAWN AS A UNIFORM SCHEME 2026-10-29 after the cross-texture
  pilot** (`docs/plans/ablation/rfc0041-label-noise-textures.csv`):
  at 10M x 3 the coordinated board 9h Th Jd Qc measures
  0.00377-0.02445 BB mean across its five closures (perturbed profile
  up to 0.10280) and trip-aces As Ad Ac 7h reaches 0.00357-0.02700;
  five other textures pass with margin (0.00028-0.00250). A fixed
  iteration count cannot guarantee the 0.003 floor over all 16,432
  classes. Follow-up diagnostics on 9h Th Jd Qc cell 0 (2026-10-29):
  half-spread 0.300 (100k) / 0.186 (1M) / 0.113 (3M) / 0.0456 (30M) /
  0.0221 (100M) BB mean with 4 seeds — decay flattens; even the
  optimistic lower-bound fit puts the floor past ~2.7e9 iterations per
  leaf (wider-window fits give 1e10-3e11), so pure iteration scaling
  is not viable; the 10M x 8 half-spread is 0.084 BB (a range statistic
  implying ~90+ seeds at 10M under ideal sqrt(S) scaling, with
  heavy-tail behavior making that worse), so more seed averaging also
  fails to rescue them. The residual variance is the joint private-card
  deal; the measured cheap trainer variants (Linear/discounted CFR,
  full opponent-action enumeration) all failed — see the closure record
  in RFC 0041 Amendment 2. The
  binding re-frozen scheme is a PER-CELL convergence criterion plus a
  declared hard per-leaf ceiling that fails the cell, contingent on
  the hardness-distribution prevalence measured next: the
  `--rfc0041-hardness-survey` driver takes a deterministic 150-class
  SplitMix sample of the 16,432-class universe (1M proxy + direct
  10M x 3 verdict, survey seed 4101, evidence
  rfc0041-hardness-survey.csv). This document is re-frozen from that
  prevalence number before any mass generation; the 10M x 3 figures
  remain the cost anchor for easy cells only.
- **SURVEY OUTCOME 2026-10-29**
  (`rfc0041-hardness-survey.csv`, 150 deterministic classes, cell 0,
  10M x 3): 28/150 classes (18.7%; 15.3% concrete-orbit-weighted) fail
  the 0.003 floor; 11.3% (6.7% weighted) exceed 0.01 BB; worst class
  4.12 BB. Strata, weighted failure: rainbow 1.1%, two-tone 13.3%
  (mild tail), three-flush 37.5%, monotone 77.8% (median label noise
  1.33 BB — unlabellable at 10M x 3). This is 15x the 1% Decline
  headroom the 99% serve-rate gate allows, so rare-class Decline is
  rejected. Mass generation is STOPPED pending the project decision:
  (A) trainer-side variance-reduction spike (Linear/discounted CFR in
  NSeatTrainer; the engine currently has RM+ external sampling only),
  (B) reduced certification scope (flush-texture boards Declined,
  requiring an AC5 serve-rate amendment), or (C) RFC 0041 closure on
  the predeclared honest-failure path. No class/line/profile quotas in
  this document are re-frozen until that decision.
- Label-noise statistic (the AC4 floor): for each leaf/profile, the
  per-combo half-spread across the three solves
  `(max_s V - min_s V)/2` in BB; the class statistic is the
  range-mass-weighted mean across its seat-combos and leaves. Gate:
  every held-out class statistic <= **0.003 BB**, and the 10k-bootstrap
  95% upper bound over the 60 held-out class units is also <= 0.003 BB.
  The worst-single-combo half-spread is reported (not gated here; it
  feeds AC4's p95 analysis).
- Failure rule: any held-out class above the floor stops the campaign
  (recorded FAILURE; no iteration escalation without amending and
  re-freezing this plan). The measured curve pinning T is committed as
  `docs/plans/ablation/rfc0041-label-noise.csv`; the cross-texture
  pilot is committed alongside as
  `docs/plans/ablation/rfc0041-label-noise-textures.csv`.

## 6. Records and shard layout

- Per leaf/profile the shard stores, normalized once per leaf (not
  repeated per combo): 5-card board encoding and board features,
  per-seat 1,326-f32 conditioned range vectors (zero-filled for absent
  combos, each seat normalized to sum 1), ledger scalars (pot, net
  contributions, remaining stacks, folded flags, position/live counts,
  BB, action-abstraction digest), and per-combo triple-seed-averaged
  `V_p(c)` labels plus per-seed values for the noise audit.
- Approximate record counts: ~1,081 live combos/seat × 2 seats ≈ 2,162
  marginal records/leaf/profile; 240 leaves × 3 profiles ≈ **1.56
  million marginal records per board class**; 360 classes ≈ **~560
  million records**.
- Storage (f32, normalized): ~20 KB/leaf/profile (range vectors +
  labels), ≈ 14 MB/class, **~5 GB total raw shards**.
- Location: bulk shards under the gitignored campaign directory
  `artifacts/rfc0041-labels/` (same convention as stage6/flop-library
  artifacts): one shard per (class, profile) plus a JSONL/JSON
  manifest; per-shard SHA-256, generator code revision, seed/iteration
  metadata, record counts, and label-noise statistics. Manifests and
  aggregate statistics are committed under
  `docs/plans/ablation/rfc0041-label-manifest-*.json`; raw shards are
  never committed.

## 7. Per-class compute budget

Anchors (release build; MCCFR solves are single-threaded, parallelism is
process-level across leaves):

- RFC 0039 anchor: 138.6 s cumulative cold wall to 10M iterations for
  the 10-hand check-through leaf; bet/raised leaves are harder.
- Full-range measured cost (`rfc0041-leaf-cost.csv`, 100k probes):
  fixed nested-tree build ~18-20 s/leaf, iteration phase ~1.3-2.0 s per
  100k, peak RSS ~117 MB/solve. At 10M the measured per-solve wall is
  ~164-218 s across the five cells (`rfc0041-label-noise.csv`).
- Budget per class: 240 leaves × 3 profiles × 3 seeds = 2,160 solves,
  ≈ 390-470 CPU-kiloseconds (≈ **110-130 CPU-hours**) per board class.
- Campaign at the frozen full quota (360 classes, 3 profiles):
  777,600 solves, ≈ 40k-47k CPU-hours; with 50 concurrent processes on
  the 56-core measurement host ≈ **33-38 days wall** (≈ double at the
  28-core AC5 reference host). This is the dominant economic cost of
  RFC 0041 and the campaign size was re-approved against this number
  before generation starts (the RFC names label compute as a residual
  risk with an honest-failure escape hatch). Smaller predeclared
  quotas (fewer training classes and/or fewer profiles, never below 60
  held-out classes or below 3 seeds) are recorded here as amendment
  rows if the full quota is not approved; the quota actually generated
  is pinned in the manifest and gates how AC4 may be claimed.
- A cross-texture pilot (Section 5) doubles as the throughput
  measurement at ~45-way process concurrency; the final wall estimate
  in this section is updated from it before the campaign launch.

## 8. Generator contract recap

`--rfc0041-genlabels` enumerates exactly the frozen cells above, emits
provenance-tagged board-class-disjoint shards from the exact RFC 0039
solver, and refuses to run if the feature-scheme/abstraction ids or the
class-table digest mismatch this plan. Reproducibility: deterministic
seeds, no network I/O, resumable by (class, profile, seed), and
idempotent shard rewrites verified by SHA-256. The generator cannot
change any quota in this document.
