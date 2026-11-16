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

## Step 0b — exact RiverGame allocation (closed 2026-11-17: PASS)

Target: the measured-smallest decision-bearing anchor, srp
(32, 16, 16). The probe constructs the full exact-menu RiverGame
under `TreeLimits{25M nodes, depth 256, 8 GiB}` with zero CFR
iterations and records actual resident bytes itemized per table (the
Amendment 1 nit 3 requirement) plus peak RSS and construction wall.

Artifacts (new, bench-only):

- `river_solver.{hpp,cpp}` — a bench-only `RiverAllocationReceipt`,
  a `probe_river_allocation` entry that converts
  `tree_resource_exhausted` and `std::bad_alloc` into typed memory
  NO-GO rows, and a const per-table byte itemizer on the existing
  RiverGame. No default solve path changes: the new code is an added
  function and an added const method; the CFR driver and all existing
  fields are untouched.
- `rfc0050_river_allocation_probe.cpp` — manual no-argv probe on the
  hard-coded rank-0 anchor; RC 0 on allocation, RC 4 on a typed
  NO-GO.
- `test_rfc0050_river_allocation.cpp` — native ctest: itemized totals
  cross-checked against an independent tree walk (regret/average-sum
  equal `sum(menu widths) x 1,081 x 8`, reach and value derived from
  total nodes, sigma snapshot provably absent at zero iterations),
  the injected-limit typed NO-GO with all byte fields zero, and
  itemization determinism across repeated constructions.

Receipt: `docs/plans/ablation/rfc0050/stage0b-exact-river-allocation.csv`,
single run, RC 0, empty stderr. The exact game allocates:

| table | bytes |
| --- | --- |
| public tree (accounted) | 2,978,208 |
| showdown matrix A0 (int8, 1,081^2) | 1,168,561 |
| regret vectors | 41,475,808 |
| average-sum vectors | 41,475,808 |
| per-node reach (2 seats) | 82,968,912 |
| per-node value | 41,484,456 |
| average snapshot | 0 (not built at zero iterations) |
| scratch pool | 553,472 |
| bookkeeping | 161,341 |
| **itemized total** | **212,266,566 (~202.4 MiB)** |

Peak RSS 211,620 KB (~206.7 MiB), construction wall 181.8 ms, envelope
headroom 40x under 8 GiB. The pre-registered estimate said ~166 MB of
strategy vectors alone under a worst-case six-action width; actual
menus are narrower on average (4,796 total menu slots vs the 9,600
all-width-six bound), and the full itemized game lands at 202 MB,
inside the reviewer's ~0.28 GB allocation-only recomputation. A fully
resident result snapshot in a later stage would add at most one more
~41 MB table and still remain ~20x under the envelope, per Amendment 1
nit 3.

Verdict: PASS, not NO-GO. The exact-menu RiverGame is buildable within
the offline envelope on the measured-smallest decision-bearing ledger,
so the campaign's single falsification point was exercised and
cleared; its fail-closed branch is covered natively by the injected
tiny-limit test. Stage 0 is complete and Stage 1 (exact-menu
convergence on this anchor) is the next gate, subject to its own
pre-registered 1,800 s budget estimate before the long solve.
