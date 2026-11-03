# RFC 0045 Stage-3 Pre-Run Freeze Addendum (APPROVED)

- RFC: [0045 M2 Instrument Resolution and Blueprint-Scope Census](../rfcs/0045-m2-instrument-scope-census.md)
- Status: **APPROVED 2026-11-02** by an independent approval agent
  (separately launched, non-author; the same agent that issued the
  2026-11-02 CHANGES REQUESTED on the first draft). The first review
  returned one blocking finding (a default `g2` verdict could omit the
  raised-srp board) and four non-blocking observations; all five are
  resolved and re-reviewed. Gate effect: full-range G2 cells above the
  1M smoke rung are now authorized to run under the frozen contract.
- Decision record of record:
  "RFC 0045 Stage-3 pre-run freeze addendum (revised, 11 sections)
  APPROVED 2026-11-02 by independent approval agent (separately
  launched, non-author; same agent that issued the 2026-11-02 CHANGES
  REQUESTED); scope: six frozen river boards including census-selected
  raised-srp 2c3d7hJs2s with mandatory coverage validation, four-point
  grid freeze (32/64/{64,128,256} plus conditional 32/128/256), reduced
  four-rung ladder, pinned seeds/multipliers, exact-walk budget, G3
  pot-interval reinterpretation (2/6/40/150/2=200), sampled-estimator
  contract with fnv1a64 seed derivation and internal exhaustive
  enumeration, and section-11 re-approval clause; the conditional
  (32,128,256) arm remains the sole pre-authorized exception."
- Author: Claude (zzzode agent session, 2026-11-02).
- Evidence this freeze is pinned from:
  - `docs/plans/ablation/rfc0045-stage1-river-smoke.csv` (W1 regression and
    the one authorized full-range river walk wall),
  - `docs/plans/ablation/rfc0045-g3-roots-preliminary.csv` (identity-complete
    root census),
  - `docs/plans/ablation/rfc0045-g3-structural-preliminary.csv` (cap 0 /
    cap 1 River-terminal builds).

## 1. Texture strata (frozen)

The G2 gate covers the chance-free RIVER completion of these strata.
Card spellings use the `s=0,h=1,d=2,c=3` suit order (`cardId`); every
river board is five concatenated card names.

| name | kind | flop | turn | river (gate board) |
|---|---|---|---|---|
| `w1` | anchor (W1 ten-hand range AND full range) | `2c3d7h` | `2c3d7hJs` | `2c3d7hJs9c` |
| `unpaired-rainbow-ahigh` | full range | `7sKhAd` | `7sKhAdJs` | `7sKhAdJs9c` |
| `paired` | full range | `7sKhKd` | `7sKhKdJs` | `7sKhKdJs9c` |
| `monotone` | full range | `QsKsAs` | `QsKsAsJs` | `QsKsAsJs9c` |
| `unpaired-twotone` | full range | `7sKhAs` | `7sKhAsJs` | `7sKhAsJs9c` |
| `raised-srp` | full range, raised root | `2c3d7h` | `2c3d7hJs` | `2c3d7hJs2s` |

The six river boards above are registered in the frozen evaluator
(`GATE_BOARDS` in `tools/rfc/rfc0045-gates.ts`); the mechanical `g2`
verdict defaults to all six and **refuses to emit a verdict over an
incomplete board set** unless `--allow-partial` is passed explicitly
(diagnostics only). The canonical invocation is

```
node bin/rfc0045-gates.mjs g2 <campaign-csv...> [--json]
```

with no `--boards` override; a verdict that does not cover all six
frozen river boards is invalid and may not be recorded as the verdict
of record.

Turn completions stay descriptive and never gate a grid (RFC section 3).

## 2. Raised-root selector result (data-blind application)

The deterministic RFC section-2 selector applied to the frozen G3 census
(before any G2 run):

- among identity-complete raised-root tuples in census-key order, the
  smallest pot is **8 chips** (two tuples, one per button); the
  census-key-first is `root_id=1, button=0, SRP, pot=8, live_count=2,
  stacks {196,196}, contributions {4,4}, sequences=2`;
- the lexicographically smallest non-blocking river card completing the
  W1 flop+turn `2c3d7hJs` is `2s` (card id 0; `2c` is on the flop).

The raised stratum therefore trains the SRP root tuple above on the
river board `2c3d7hJs2s`. If the census of record is regenerated and
the selector no longer returns this tuple, the freeze is reopened rather
than silently substituted.

## 3. Grids (frozen)

- Primary axis: `(k_flop,k_turn,k_river)` = `(32,64,64)`, `(32,64,128)`,
  `(32,64,256)`.
- Conditional secondary arm: `(32,128,256)`, run only if `(32,64,256)`
  fails G2, with its cells/seeds/rungs fixed here as part of the same
  freeze.
- The set of legal grids is frozen to exactly those four points, in BOTH
  the benchmark (`EquityStrengthGrid::is_supported()`: k_flop must be
  32; k_river in {64,128,256}; k_turn 64, or 128 paired only with
  k_river 256) and the evaluator (`schemeKey` rejects every other
  point). In particular `(32,128,64)` and `(32,128,128)` are not
  campaign points and are refused.
- Any other grid point is rejected before a bucket or digest is
  produced; on failure no larger grid or other abstraction is added.
- The dense id ceiling stays `169 + 9*(32+128+256) = 3,913` (exclusive).

## 4. Rungs, walls, resume

- Reduced ladder: `{1,000,000; 10,000,000; 100,000,000; 300,000,000}`.
- Per-segment safety wall: 21,600 s (RFC 0044 convention); a truncated
  segment is sealed via BSWARM03 (kind ordinal 5, bucket cap 3,913,
  tree-shape/root-table/card/action digests in preflight) and resumed by
  re-invoking the identical shard command; absolute-iteration PRNG
  seeding makes resume bitwise identical to a from-zero run.
- Under the retained RFC 0044 tail rules the only rule-4 eligible rung on
  a four-rung ladder is index 3 = 300M, so the gate rung is 300M.

## 5. Seeds, multipliers, independent deal batches

- W1 anchor: five independent training seeds **4320, 4321, 4322, 4323,
  4324**, half-width multiplier `t_0.975,4 = 2.7764`.
- Full-range strata: three independent training seeds **4320, 4321,
  4322**; every seed gets its own independent sampled-deal batch where
  sampling is used, half-width multiplier `t_0.975,2 = 4.303`.
- Half-width bar 0.001 BB; CERT bucket bar 0.005 BB; worst-stratum exact
  bar 0.03 BB.
- Paired superiority: at the gate rung the candidate must be strictly
  below the nine-tier incumbent on every identical seed.

## 6. Exact-walk budget pinned from the Stage-1 smoke

Measured single-thread (`OMP_NUM_THREADS=1`, release, n37-100-184):

- full-range chance-free river exact walk: **50.996 s** over 1,070,190
  disjoint ordered hand pairs (unweighted uniform 1,326-combo range,
  board-blocking combos filtered);
- training at the 1M smoke rung: 33.282 s.

Full-range exact river walks are paid **only at the gate rung (300M)
and at 300M** — the same point under the reduced ladder — i.e. once per
seed per stratum per grid, never at the lower rungs. W1 exact walks
(38 deals) are cheap and run at every rung. The evaluator reads the
per-cell `exact_walk_s` column; a cell whose full walk deviates by more
than 20% from the pinned 50.996 s (machine load) is re-run, not
silently accepted.

## 7. G3 pot-interval strata (resolution of the "three tuples" rule)

The census contains exactly **two** identity-complete tuples per pot
(one per explicit button), so the RFC's "at least three tuples per
pot-stratum" cannot be read per exact pot. The frozen reinterpretation
is pot-INTERVAL strata, each spanning at least three distinct tuples:

| stratum | pot interval (chips) | tuples |
|---|---|---|
| shallow-limped | 4 (limped, both buttons) | 2 |
| raised-low | 8..16 (SRP/3bet, both buttons) | 6 |
| mid | 20..96 | 40 |
| deep | 100..396 | 150 |
| all-in-no-flop | 400 | 2 (frontier disposition, no blueprint rows) |

The counts are recomputable from the committed census and sum to
2 + 6 + 40 + 150 + 2 = 200. G4 reports per-stratum MAXIMUM and the range of per-class
seconds/rows/bytes. A stratum with fewer than three tuples in the
census is reported as such (shallow-limped and all-in-no-flop) rather
than padded.

## 8. Structural cap-0 / cap-1 finding (census, not a gate)

Preliminary River-terminal builds (full 1,326-combo range, flop
`2c3d7h`, 2,000 iterations, bounds raised to 50M nodes/40 GiB):

- cap 1: 4.77M nodes (limped pot 4) to 6.29M nodes (SRP pot 8),
  3.03..3.08 GiB tree, 1.32M..1.74M sealed rows, 2.27..2.98 GiB rows,
  10.5..13.0 s per class;
- cap 0: materialization exceeds **100M nodes / 55 GiB** on the limped
  worst shape (node cap) — structurally infeasible as a full
  River-terminal object on n37-100-184.

This is an observation the production blueprint/mount RFC adjudicates;
it does not change the G2 gate (the instrument campaign trains
chance-free river subgames sized to the strata, not one uncapped
multi-street tree).

## 9. Sampled-estimator contract (turn/flop descriptive cells)

The structural contract fixed in RFC 0045 section 3 is incorporated
here by reference (it is not re-frozen): the frame is uniform over
ordered disjoint two-card-hand pair assignments conditioned on the
fixed board; every sampled deal exhaustively enumerates the opponent's
two-card holding and every runout INTERNALLY (no nested sampling); the
sampled BR is an exact best response over the sampled set; aggregation
weights and the per-deal normalizer (summed over two live seats / big
blind) are identical to the exact walker.

The addendum pins only the deal count, the seed derivation, and the
budget/agreement:

- frozen RNG: XorShift64*; the sampled-deal stream seed for cell
  (training seed `s`, stratum name) is
  `0x00452025u64 ^ (s * 1_000_003) ^ fnv1a64(stratum_name)` where
  `fnv1a64` is the standard FNV-1a 64-bit hash of the UTF-8 stratum
  name (offset 0xcbf29ce484222325, prime 0x100000001b3);
- frozen deal count: **200,000 sampled deals per seed** for
  chance-resolved turn/flop cells;
- each training seed uses its own independently derived batch (above),
  so sampling error enters the per-seed half-width, never as a shared
  bias;
- exact-vs-sampled agreement: on W1 (exact) and on the `monotone` fixed
  board, the sampled estimate at 200,000 deals must lie within
  0.002 BB of the exact value before any sampled cell is reported.
  These parameters gate only the reporting of never-gating descriptive
  cells and cannot make a grid pass the river gate.

## 10. Command/evaluator identity

- Benchmark: `engine/bigshark-subgame-nash-conv --rfc0045-river-wall
  <board5> <full|w1> <iterations> <seed> [KF KT KR]` and
  `--rfc0045-census ...` (measurement build only).
- Evaluator of record: `tools/rfc/rfc0045-gates.ts` (frozen
  2.7764/4.303 multipliers, the six registered `GATE_BOARDS` with
  incomplete-set rejection, worst-stratum selector, rung selector,
  census/envelope ingestion), invoked through
  `bin/rfc0045-gates.mjs`. The RFC 0044 evaluator is unchanged.
- Raw CSVs commit under `docs/plans/ablation/`.

## 11. Re-approval clause

Per RFC 0045 AC2, after this addendum is approved any change to a
half-width bar, the 0.005 bucket or 0.03 exact threshold, a grid point,
the rung ladder, a seed or t multiplier, the sampled deal count or
agreement tolerance, or a frozen stratum (including the raised-root
selection) requires a NEW independent approval before further cells
run. The conditional `(32,128,256)` arm is the only pre-authorized
exception, and only after the `(32,64,256)` arm fails G2.
