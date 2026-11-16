# RFC 0050 Stage 0 Implementation Plan

Status: Current. Scope owner: `bigshark_rfc0047_bench` only. This plan
maps the accepted RFC 0050 Stage 0 gates to concrete artifacts and
records their measured outcomes. Stage 1 (exact-menu convergence),
Stage 2 (six-ledger boundary map), and the Stage 3 BSFLIB04 addendum
are not authorized work items here and do not appear below as
implementation tasks.

## Step 0a — eleven-ledger cap-0 public-tree census (closed 2026-11-17)

Objective: without solving, enumerate the distinct betting ledgers of
the two frozen turn roots, build each exact cap-0 public tree under the
frozen offline envelope, and name the hard-coded six-anchor
pre-registration in measured ascending order.

Artifacts (all new, bench-only):

- `engine/benchmarks/rfc0047/ledger_census.{hpp,cpp}` — folds the
  cap-1 frontier enumeration of the w1 and raised-srp strata into the
  eleven distinct (family, ledger key) anchors, runs the exact cap-0
  census through the RFC 0049 `offline_census` helper, and provides the
  measured-tuple ascending order plus the Amendment 1
  `is_decision_bearing` eligibility predicate and
  `decision_bearing_ascending_order`.
- `engine/benchmarks/rfc0047/rfc0050_anchors.hpp` — the hard-coded six
  anchors, transcribed from the sealed receipt with no run-time
  selection.
- `engine/benchmarks/rfc0047/rfc0050_ledger_census_probe.cpp` — manual
  single-instance probe, no argv; prints the ledger enumeration, the
  eleven cap-0 census rows, and the measured order with eligibility
  flags and anchor match flags.
- `engine/tests/test_rfc0050_ledger_census.cpp` — native behavioral
  ctest: frontier ledger constants (432/528 leaves, 5/6 ledgers),
  census structure and determinism, card-independence of the cap-0
  shape within every ledger, the terminal all-in row typing, the
  composite (family, key) identity, the ascending-order permutation,
  and the R2-1 consistency gate pinning the hard-coded anchors.
- `engine/CMakeLists.txt` — three additive hunks only (one library
  source, one probe executable, one ctest).

Receipt: `docs/plans/ablation/rfc0050/stage0a-eleven-ledger-cap0-census.csv`,
one run, RC 0, empty stderr; a second run is byte-identical outside the
anchor echo block. All eleven trees fit the 25M-node / 8 GiB envelope
and every per-node menu is at most six actions, inside the 32-action
BSFLIB ceiling.

### Structural finding: two terminal all-in ledgers

Two rows — key (400, 200, 200), one per turn root — are single-node
terminal trees with zero action nodes: both stacks are fully committed
before the river, so no menu exists. They sort first on the zero tuple,
which would make a literal "first six" selection vacuous for the
Step 0b allocation gate. RFC 0050 Amendment 1 (Approved 2026-11-17 by
an independent non-author agent) restricts anchor eligibility to
decision-bearing rows (`fits && action_nodes > 0`); both terminal rows
stay in the eleven-row receipt.

### Measured decision-bearing order and hard-coded six anchors

| eligible rank | family | key (pot, c0, c1) | action nodes | total nodes | projected infosets | accounted bytes |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | srp | (32, 16, 16) | 1,600 | 4,797 | 1,729,600 | 2,978,208 |
| 1 | srp | (20, 10, 10) | 2,632 | 7,893 | 2,845,192 | 4,983,200 |
| 2 | limped | (16, 8, 8) | 3,344 | 10,029 | 3,614,864 | 5,972,576 |
| 3 | srp | (14, 7, 7) | 3,984 | 11,949 | 4,306,704 | 6,036,384 |
| 4 | srp | (12, 6, 6) | 4,880 | 14,637 | 5,275,280 | 9,929,184 |
| 5 | limped | (10, 5, 5) | 5,720 | 17,157 | 6,183,320 | 11,845,984 |
| 6 | limped | (8, 4, 4) | 7,824 | 23,469 | 8,457,744 | 12,059,552 |
| 6 (tie) | srp | (8, 4, 4) | 7,824 | 23,469 | 8,457,744 | 12,059,552 |
| 8 | limped | (4, 2, 2) (w1) | 11,232 | 33,693 | 12,141,792 | 23,677,536 |

The anchor set spans both families (four SRP, two limped). The T6
historical figures reproduce exactly: 7,824 SRP action nodes and
11,232 w1 action nodes.

### RED to GREEN

The native test was written before the anchor list existed and failed
on the R2-1 gate against placeholder anchors (and first exposed the
cross-family key collision, fixed with the composite (family, key)
identity). After transcribing the measured six anchors and implementing
the eligibility predicate, `rfc0050_ledger_census` passes.

Verification at Step 0a closure: focused ctest set
`rfc0047_link_isolation`, `rfc0047_tree_budget`,
`rfc0047_frontier_enumerator`, `rfc0047_offline_census`,
`rfc0050_ledger_census` 5/5 green in 0.74 s; the standalone
`bigshark-rfc0047-frontier-check` reproduces 432/528 leaves and 5/6
ledgers; clang-format applied to the new files only; check-rfcs (50)
and check-docs (557) green. Full release ctest, ASan, and the
multi-seat matrix remain stage-closure runs per the RFC verification
plan. The ASan preset is currently blocked by the separately registered
sanitizer build blocker B1 in `docs/development/build-and-test.md`.

## Step 0b — exact RiverGame allocation (not started)

Target: the measured-smallest decision-bearing anchor, srp
(32, 16, 16). The probe must attempt the full exact-menu RiverGame
allocation under `TreeLimits{25M nodes, depth 256, 8 GiB}` with zero
CFR iterations and record actual allocated bytes and peak RSS; a
construction failure is a typed memory NO-GO that terminates the
campaign before any format work. Pre-registered estimate (not a
receipt): roughly 166 MB of strategy vectors at two double tables of
six entries over 1,729,600 infosets, estimated 0.2-0.35 GB peak RSS,
second-scale wall. Step 0b starts only after the Amendment 1
independent review decision.

Amendment 1 was Approved 2026-11-17 by an independent non-author agent
(no blocker/major; four nits, one observation). Nit 1 and nit 2 are
closed in the native test (sealed absolute action-node pins including
the T6 7,824/11,232 anchors; family/index/leaf-count determinism
fields). Nit 3 binds the Step 0b receipt: it must itemize every table
resident at measurement — regret/sum vectors, per-node reach and value
storage, the pre-warmed scratch pool, and any result-side average
snapshot — because a fully resident result set may reach roughly
0.4 GB rather than the 0.2-0.35 GB allocation-only figure.
