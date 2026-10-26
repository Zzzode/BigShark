# RFC 0037 Implementation Plan

Status: Current. RFC 0037 Implemented 2026-10-26 (Accepted 2026-10-25
after three review rounds; approval design-only — no spending,
credentials, or live play). The 2026-10-26 legacy-retirement change
retired the 3-seat 10 BB cap-0 profile and unified the table to
heads-up cap 0 / every 3+ seat shape cap 1; see the amendment in the
RFC and the entry at the bottom of this file.

## Acceptance-criteria map

| AC | Stage | Evidence |
| --- | --- | --- |
| AC1 (harness, translator projection, negative control, infeasible layers) | 1 | `--ablation` mode, CSVs |
| AC2 (total-function checker, frozen table recorded) | 1 | committed checker script + RFC record |
| AC3 (policy module, revision token, table tests) | 2 | native tests |
| AC4 (exhaustive activation wiring; resolve_uniform unchanged) | 3 | menu tests + grep enumeration |
| AC5 (tri-state root-aware audit) | 3 | audit tool + synthetic fixtures |
| AC6 (3 profiles Complete + resident-root wiring) | 4 | manifests, sidecars, launcher test |
| AC7 (street-start invariant, diagnostics) | 3 | native tests |
| AC8 (two Stage-0a spike tables, per-cell kill) | 0a | measured tables recorded here |
| AC9 (full gates + docs) | all | Engine Build Contract, docs updates |

## Stage 0a — heads-up flop feasibility spikes (no production change)

Extend `bigshark-rfc0036-calibration` with `--flop-spike` (default
campaign unchanged). Build production-identity-menu, cap-0,
`streets(1)`, flop-rooted, EquityFrontierEvaluator trees:

1. 2 live, shallow (10 BB, BB 2, limped contribution 2, stacks 18).
2. 2 live, standard (100 BB, contribution 2, stacks 198).
3. 3 live, shallow diagnostic only (no activation follows).

Per shape: nodes, action/chance/leaf breakdown, bytes, median build ms,
realized infosets, ms/iteration, envelope-projected iterations
`(1600 - build_ms)/ms_per_iteration`, visits/infoset. Kill verdict per
cell against `ResolveLimits` (5,000,000 nodes / 1,000,000 infosets /
1 GiB) and the 1,600 ms envelope. Each heads-up cell activates
independently only on its own pass. Results recorded in this file.

## Stage 2 — policy module

`bs::abstraction::{GameShape,StackBand(Shallow/Standard/Deep),
production_raise_cap,production_flop_resolve_cap,stack_band_for_bb,
kShapePolicyRevision,kUnscopedShapeLegacyCap,
kHeadsUpStandardFlopSpikeDroppedCap}`; exhaustive table/boundary/nullopt
tests in `test_abstraction::shape_policy`. **Complete 2026-10-25**
(ctest case green). The third constant `kDeferredThreeShallowFlopCap`
existed briefly and was deleted on 2026-10-26 when legacy retirement made
3-live shallow read cap 1 directly from the table.

## Stage 3 — scoped wiring and audit

- `resolve_flop` per the activation table; the shape is computed at STREET
  START (flop fold events in history restore folded-this-street seats;
  preflop folds are excluded; street-start stack = current +
  street_committed). `resolve_uniform` untouched. **Done**:
  `test_flop_resolve_shape_policy` pins node-count equality per row
  (2-live shallow 63,804 cap-0 nodes vs 5,068 cap-1; standard/3-shallow/
  6-standard unchanged), `test_flop_resolve_midstreet_fold_cap_stable`
  pins the no-mid-street-relookup invariant. `build_subgame_def_from_node`
  moved to the resolver's public surface to build comparison trees.
- Tri-state root-aware policy audit:
  `tools/artifact-audit/rfc0037-policy-audit.py`. **Done**: all five
  shipped artifacts PASS; synthetic miscapped profile and mixed-cap
  library FAIL nonzero; an older-revision manifest SUPERSEDED with exit 0.
- `docs/design/gto-engine.md` policy section added.

## Stage 4 — profiles and wiring

Train 3-seat 100 BB, 4-seat, 5-seat standard profiles (100K, full range,
per-build trainer-limit raises as required, SHA-256 sidecars); retire
the 10 BB 3-seat cap-0 validation artifact (2026-10-26, moved to
`~/.bigshark-backups/retired-2026-10-26/`); pin the complete 2..6
dealt-count 100 BB resident root set in runtime configuration;
launcher-level advertising test.

Progress (2026-10-25):
- 4-seat 100 BB cap-1 profile Complete (`artifacts/preflop-profile-4way`):
  100K iterations, 44,616 information sets, 58 s wall / 69 s pipeline /
  757 MB peak RSS, 67 MB artifact, sha256 `d47f17ce…`.
- 5-seat 100 BB cap-1 profile Complete (`artifacts/preflop-profile-5way`):
  100K iterations, 157,170 information sets, 174 s wall / 2.0 GB peak RSS,
  260 MB artifact.
- Both pass the RFC 0037 policy audit (7/7 artifacts PASS).
- 3-seat 100 BB cap-1 profile Complete
  (`artifacts/preflop-profile-3way-100bb`, 2026-10-25): cap 1 confirmed by
  the reference-feasibility measurement (cap 0 tree exceeds the 1 GiB byte
  cap, so the cell is a non-promotable proxy and stays cap 1); 100K
  iterations, 11,154 information sets, 29 s wall / 314 MB peak RSS,
  15 MB artifact, sha256 `f5723549…`, raised trainer limits
  (`--max-infosets 2000000 --train-bytes 8589934592`). The full 2..6 dealt-
  count preflop set now exists and the policy audit passes 8/8 artifacts.
- Runtime resident-root configuration wiring and the launcher-level
  advertising test: auto-discovery implemented
  (`autoDetectedPreflopProfileRoots` + `resolveResidentRoots`, manifest
  sha256 pins, module-level cache, explicit-config/env precedence), npm
  133/133 green. A real launched-engine advertising assertion remains if a
  live binary integration is added; unit coverage pins the launch line.

## Stage 1 — ablation harness and pre-registered run — COMPLETE 2026-10-26

Frozen verdict KEEP (no promotion); after the 2026-10-26 legacy-retirement
amendment the operating table is heads-up cap 0 and every 3+ seat shape
cap 1. Matrix: `docs/plans/ablation/rfc0037-matrix.csv` (72 rows, seeds
101/202/303, 1K and 100K); checker
`tools/artifact-audit/rfc0037-ablation-check.py` exits 0 with no
fail-fixed-cell-surprise; runner
`tools/artifact-audit/run-rfc0037-ablation.sh`. Harness: `bigshark-subgame-nash-conv
--rfc0037-ablation <seats> <stack-bb> <cand-cap> <ref-cap> <iterations>
<seed>` trains the cap-c candidate, builds the cap-r reference tree
(same turn-rooted streets(1) production-identity geometry), pairs nodes
by exact action path, projects rows onto reference nodes, fills
beyond-cap nodes (passive default + uniform sensitivity arm), and
prints FITS/INFEASIBLE plus in-model and both translated NashConv BB
values. Identity negative control verified (2p100 cap1=cap1:
E==I==2.10802525); a first translation point 2p100 cap1->cap0 at 1,000
iterations gave gap 38.1 BB (passive) / 22.9 BB (uniform) against
in-model 2.1 BB — the cap error is large at this shape/count (not a
frozen-table input; the decisive count is 100K).

The declared full matrix (2/3/6 seats x caps 0/1/2, 1K and 100K, seeds
101/202/303, including 3-seat shallow and the reference-feasibility
verdicts) ran as background training; per-cell CSVs are under
`docs/plans/ablation/rfc0037-*.csv` and the checker froze the table.
The M2 harness shared this build cycle (0036 plan; M2 measured FAIL,
see the 0036 plan).

### Reference-cap feasibility measured (2026-10-25, production menu streets(1))

The coarse-menu T3 numbers do NOT transfer to the production rfc0007
identity schedule, confirming round-2 n1:

| Shape | cap-0 reference tree | richest feasible cap |
| --- | --- | --- |
| 2 seats 100 BB | FITS (718,233 nodes) | 0 |
| 3 seats 10 BB | FITS | 0 |
| 3 seats 100 BB | **>1 GiB byte cap** | 2 (14,572 nodes) |
| 4 seats 100 BB | **>1 GiB** | 2 |
| 5 seats 100 BB | **>1 GiB** | 2 |
| 6 seats 100 BB | **>1 GiB**; exact pooled BR also intractable (>173 min at 1K iters) | 2, proxy only |

Consequences under the pre-registered total function:

- The direct cap-0 reference cells were 2-seat standard and 3-seat
  shallow. 2-seat standard trained cap 0 (resolver serving later stayed
  cap 1 via the dropped Stage-0a spike). 3-seat shallow was frozen cap 0
  by the shipped preflop artifact at the time; the 2026-10-26
  legacy-retirement amendment moved it to cap 1 and retired that
  artifact, so the cell no longer has any operating cap-0 consumer.
- The sole nominally promotable cell (3-seat standard 1 -> 2) has no
  non-proxy reference (cap 0 does not build), so it cannot promote:
  **three-seat standard is fixed at cap 1 regardless of the cap-2 proxy
  gap** (the proxy run measured cap-1 policy at 55-96 BB gap in the
  cap-2 tree at 1K iterations versus cap-2 policy 18 BB in-model).
- Six-seat exact evaluation is combinatorially intractable; per RFC 0036
  precedent it requires the sampled BR estimator for any number and can
  never promote.
- The checker verdict kept every cell at the provisional value; the
  matrix runs supply the multi-seed/two-count evidence. The only later
  change was the 2026-10-26 legacy-retirement amendment (3-shallow
  0 -> 1), which was a maintainer-directed artifact retirement, not an
  ablation outcome.

Early 1K translation evidence (seed 42, not a frozen input): 2-seat
100 BB cap-1 policy in the cap-0 tree gaps 38.1 BB (passive fill) /
22.9 BB (uniform fill) against 2.1 BB in-model — the cap error is large
heads-up deep, which is exactly why that cell trains cap 0.

The committed matrix runner is
`tools/artifact-audit/run-rfc0037-ablation.sh`; output accumulates in
`docs/plans/ablation/rfc0037-matrix.csv`; aggregate with
`rfc0037-ablation-check.py`.

## Verification gates per stage

Release: `cmake --preset release; format; build; ctest --preset release;
benchmark-multistreet; npm run check; npm run proto:check; replay`.
ASan on resolver/abstraction/translator surfaces. `check-rfcs`,
`check-docs`.

## Stage 0a results

Measured 2026-10-25 (release build, raw output
`docs/plans/ablation/rfc0037-stage0a.txt`; production identity rfc0007
menu, cap 0, streets(1), flop-rooted, equity frontier, uniform full-range
priors, BB=2, limped contribution 2, seed 42, 1,000 iterations):

| Cell | Nodes | Bytes | Build ms | Infosets | ms/iter | Envelope verdict |
| --- | --- | --- | --- | --- | --- | --- |
| 2 live, shallow 10 BB | 147,308 | 149.0 MiB | 132.6 | 480 (bound 480) | 0.314 | **ACTIVATE** (~4,700 iters inside 1,600 ms) |
| 2 live, standard 100 BB | — | > 1 GiB | — | — | — | **DROP**: "abstract tree exceeded the byte cap" |
| 3 live, shallow 10 BB (diagnostic) | — | > 1 GiB | — | — | — | infeasible at this menu/depth |

Frozen activation consequence for Stage 3:

- Two-live shallow-band `resolve_flop` moves cap 1 -> cap 0 (the only
  runtime change that activates).
- Two-live standard-band `resolve_flop` **stays cap 1** — the pre-declared
  kill tripped (byte cap). Heads-up 100 BB flop serving is unchanged from
  today.
- Three-live shallow served cap 1 before and after; its diagnostic build
  is infeasible at this menu/depth. The 2026-10-26 legacy retirement
  removed the separate named deferral constant, so this cell now reads
  cap 1 directly from the unified table (3+ seats, cap 1 at every band).

The T3-DL two-seat 100 BB small-node figures do not transfer: they used
the declared-only coarse menu, while the production identity menu's
uncapped sizing chain crosses 1 GiB at the flop-rooted depth-limited
shape even heads-up. (The ablation's own reference geometry is
turn-rooted per the RFC and is unaffected by this measurement.)
