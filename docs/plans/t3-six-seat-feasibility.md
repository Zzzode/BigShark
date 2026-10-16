# T3: Six-Seat Solver Feasibility Measurement

Status: Current

This is the T3 measurement deliverable from the
[six-max delivery register](sixmax-gto-delivery.md). It records independent
six-seat quality and normal-stack resource/deadline measurements before
scaling. It is a feasibility measurement, not a quality certification and not
a six-max completion claim. The complete feature remains In Progress.

## Summary

At the current coarse action abstraction (declared-only, 1/2-pot bet + 1/2-pot
raise, no raise-count cap), a full turn-to-river public-tree CFR solve is:

- **Feasible for 2 seats** at every measured stack (10/25/100 BB): complete
  solves, trees under 12 MiB.
- **Tree-feasible but solve-constrained for 3 seats**: trees fit the 1 GiB
  builder cap at every stack (up to 605 MiB at 100 BB), but the trainer's own
  caps bind — wall-clock at 10/25 BB, the information-set cap at 100 BB.
- **The cliff at 4 seats**: at 10 BB the tree fits (363 MiB) but the trainer
  byte cap is exceeded during solving; at 25/100 BB the tree itself exceeds
  1 GiB.
- **Infeasible for 5 and 6 seats at every measured stack**, including 10 BB:
  the tree exceeds the 1 GiB `TreeLimits::max_bytes` cap before solving can
  begin.

**Go/no-go:** six-max full-tree solving at normal stacks is not feasible with
the current abstraction and resource caps. A coarser multiway action schedule
(a raise-count cap and/or fewer bet sizes) and/or depth-limited resolving with
continuation values is required before T4 (six-position preflop policies) and
T5 (flop/turn/river coverage) can scale to six seats.

**Update (2026-10-15, RFC 0024):** depth-limited resolving with
`max_streets=1` and `EquityFrontierEvaluator` is now implemented and measured.
It extends feasibility to 5 seats at 10/25 BB and 4 seats at 25 BB (all
previously infeasible). 6-seat games and 100 BB 4+ seat games remain
infeasible and require a coarser action abstraction. See the [Depth-Limited
Results](#depth-limited-results-rfc-0024-2026-10-15) section for the full
matrix.

**Update (2026-10-16, raise-count cap):** a per-street raise-count cap
(`max_raises_per_street=1`) combined with depth-limited resolving makes **all
15 fixtures feasible**, including 6-seat at every stack (10/25/100 BB). The
cap bounds the branching factor so effectively that tree size becomes
independent of stack depth — identical node counts across 10/25/100 BB for
each seat count. The largest tree (6-seat, any stack) is only 9,361 nodes /
9.1 MiB. See the [Raise-Capped Results](#raise-capped-results-2026-10-16)
section for the full matrix.

## Measurement Setup

- Harness: `engine/benchmarks/subgame_nash_conv.cpp --t3 <iterations> <seed>`.
- 15 fixtures: 2..6 seats x 10/25/100 BB, turn-rooted subgames, equal stacks,
  no folded seats at the root. Button is the last seat so seat 0 acts first
  postflop.
- Abstraction: `CoverSeeds::DeclaredOnly`, one 1/2-pot bet and one 1/2-pot
  raise per street, no explicit raise-count cap (bounded only by stack depth).
- Private ranges: 10 hands per seat (filtered by board blocking), uniform
  weight. This is a resource/deadline probe, not a poker-quality campaign.
- Tree cap: `TreeLimits::max_bytes = 1 GiB`
  (`engine/include/bs/abstract_tree.hpp`). Exceeding it throws
  `tree_resource_exhausted`, caught and reported as a FINDING (graceful, not a
  crash).
- Trainer caps: `NSeatTrainerLimits` with `max_bytes = 1 GiB`
  (`engine/include/bs/nseat_trainer.hpp`, covering master rows, worker deltas
  and the discovery registry) and the trainer information-set cap.
- Solve budget: 30 s wall-clock per fixture, 100000 iteration ceiling, seed 42.
- NashConv is skipped in `--t3` mode: this is a construction/solve feasibility
  scan. Per the qualification proposal, the subgame NashConv walker is not an
  accepted oracle and its numbers are not quality certification.

## Results Matrix

| Seats | Stack | Tree nodes | Build | Accounted | Tree cap | Solve outcome |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | 10 BB | 5,739 | 4 ms | 2.9 MiB | fits | complete, 8.3 s, 6,746 info sets, 100% |
| 3 | 10 BB | 119,266 | 103 ms | 75.4 MiB | fits | wall-clock 30 s, 14,825 iter, 148,995 info sets, 100% |
| 4 | 10 BB | 686,013 | 519 ms | 362.9 MiB | fits | trainer byte cap exceeded |
| 5 | 10 BB | — | — | — | **exceeded** | not attempted |
| 6 | 10 BB | — | — | — | **exceeded** | not attempted |
| 2 | 25 BB | 12,087 | 78 ms | 5.8 MiB | fits | complete, 15.3 s, 13,766 info sets, 100% |
| 3 | 25 BB | 315,721 | 185 ms | 181.3 MiB | fits | wall-clock 30 s, 5,349 iter, 392,209 info sets, 100% |
| 4 | 25 BB | — | — | — | **exceeded** | not attempted |
| 5 | 25 BB | — | — | — | **exceeded** | not attempted |
| 6 | 25 BB | — | — | — | **exceeded** | not attempted |
| 2 | 100 BB | 20,739 | 82 ms | 11.4 MiB | fits | complete, 29.2 s, 23,122 info sets, 100% |
| 3 | 100 BB | 970,891 | 864 ms | 605.4 MiB | fits | trainer information-set cap exceeded |
| 4 | 100 BB | — | — | — | **exceeded** | not attempted |
| 5 | 100 BB | — | — | — | **exceeded** | not attempted |
| 6 | 100 BB | — | — | — | **exceeded** | not attempted |

"100%" is action-node coverage at the wall-clock/resource cutoff, not
convergence. The two trainer-cap cases (4-seat 10 BB byte cap, 3-seat 100 BB
information-set cap) are counted as fixture failures by the harness; the
tree-cap cases are reported as FINDINGs and return success because exceeding
the builder cap is a measurement result, not a crash.

## Growth Analysis

Node count grows superlinearly in seats because each street has N sequential
actors and the action menu compounds across actors and streets:

- 10 BB: 2 to 3 seats = 21x (5.7K to 119K); 3 to 4 = 5.8x (119K to 686K);
  4 to 5 crosses the 1 GiB cap (so more than 2.8x from 363 MiB).
- 100 BB: 2 to 3 seats = 47x (20.7K to 971K); 3 to 4 crosses the cap.
- Stack effect at 3 seats: 10 to 25 BB = 2.6x; 25 to 100 BB = 3.1x.

Deeper stacks lengthen every betting line, so the seat multiplier and the
stack multiplier compound. At 100 BB a 3-seat tree already holds 971K nodes
and 605 MiB; a 6-seat tree at the same abstraction is not reachable within
1 GiB by a wide margin.

## Binding Constraints

In order of severity:

1. `TreeLimits::max_bytes = 1 GiB` (tree builder): binds for 5+ seats at all
   stacks and for 4 seats at 25+ BB. This is the primary six-max blocker.
2. `NSeatTrainerLimits::max_bytes = 1 GiB` (trainer state: master rows, worker
   deltas, discovery registry): binds for 4 seats at 10 BB even though the
   tree fits. The trainer cap is a separate constraint from the builder cap.
3. Trainer information-set cap: binds for 3 seats at 100 BB (971K nodes).
4. 30 s wall-clock: binds for 3 seats at 10/25 BB (100% action-node coverage
   but only 5K-15K of 100K iterations completed).

## Recommendations

Six-max subgames need a smaller action schedule and/or a depth-limited solve.
Options, in expected order of impact:

1. **Raise-count cap per street.** The current schedule has no raise cap; each
   actor can reopen betting, and the reopening compounds across N actors.
   Capping at one bet/raise per street (then call/fold) directly bounds the
   branching factor. This is the single highest-leverage change.
2. **Fewer bet sizes for multiway pots.** Drop the 1/2-pot raise and keep a
   single pot-fraction bet, or use a passive-only (check/call/fold) schedule
   for deep multiway subgames. A passive-only schedule is tractable but weak;
   it is a fallback, not a GTO strategy.
3. **Depth-limited resolving with continuation values.** The resolver already
   supports blueprint/value leaves. Solving only the current street and using
   continuation values for later streets produces a far smaller tree than the
   full turn-to-river solve measured here.
4. **Trainer caps.** Raise or make configurable the trainer byte and
   information-set caps for multiway work, or coarsen the abstraction enough
   to fit under them. Raising caps alone does not help 5+ seats, where the
   builder cap binds first.

A stack-depth cap for the subgame (treating stacks beyond X BB as X BB) would
also shrink trees but changes the game and needs careful utility handling; it
is listed for completeness, not recommended as the first lever.

## Depth-Limited Results (RFC 0024, 2026-10-15)

RFC 0024 implemented depth-limited resolving with `DepthLimit::streets(1)` and
`EquityFrontierEvaluator`. The T3-DL scan (`--t3-dl`) repeats the same 15
fixtures with depth-limited trees. The frontier evaluator values
`TerminalFrontier` leaves by enumerating remaining board cards (river combos
for turn-rooted games), replacing the full turn-to-river expansion.

| Seats | Stack | Tree nodes | Accounted | Tree cap | Solve outcome |
| --- | --- | --- | --- | --- | --- |
| 2 | 10 BB | 459 | 0.3 MiB | fits | complete, 100K iter, 1.7 s, 30 info sets, 100% |
| 3 | 10 BB | 8,530 | 5.5 MiB | fits | complete, 100K iter, 22.8 s, 513 info sets, 100% |
| 4 | 10 BB | 62,013 | 36.7 MiB | fits | wall-clock 30 s, 15.8K iter, 3,708 info sets, 100% |
| 5 | 10 BB | 201,959 | 146.2 MiB | fits | wall-clock 30 s, 3.8K iter, 12,090 info sets, 100% |
| 6 | 10 BB | — | — | **exceeded** | not attempted |
| 2 | 25 BB | 663 | 0.6 MiB | fits | complete, 100K iter, 2.3 s, 42 info sets, 100% |
| 3 | 25 BB | 18,073 | 18.2 MiB | fits | wall-clock 30 s, 58.6K iter, 1,080 info sets, 100% |
| 4 | 25 BB | 203,553 | 146.3 MiB | fits | wall-clock 30 s, 3.8K iter, 12,144 info sets, 100% |
| 5 | 25 BB | 1,070,634 | 699.7 MiB | fits | wall-clock 30 s, 100% |
| 6 | 25 BB | — | — | **exceeded** | not attempted |
| 2 | 100 BB | 867 | 0.6 MiB | fits | complete, 100K iter, 2.8 s, 54 info sets, 100% |
| 3 | 100 BB | 46,267 | 36.5 MiB | fits | wall-clock 30 s, 19.8K iter, 2,754 info sets, 100% |
| 4 | 100 BB | — | — | **exceeded** | not attempted |
| 5 | 100 BB | — | — | **exceeded** | not attempted |
| 6 | 100 BB | — | — | **exceeded** | not attempted |

### Full-Tree vs Depth-Limited Comparison

| Seats | Stack | Full tree | Depth-limited | Reduction | Feasibility gain |
| --- | --- | --- | --- | --- | --- |
| 2 | 10 BB | 5,739 nodes, 2.9 MiB | 459 nodes, 0.3 MiB | 12.5x | already feasible |
| 3 | 10 BB | 119,266 nodes, 75.4 MiB | 8,530 nodes, 5.5 MiB | 14.0x | already feasible |
| 4 | 10 BB | 686,013 nodes, 362.9 MiB | 62,013 nodes, 36.7 MiB | 11.1x | solve-constrained → feasible |
| 5 | 10 BB | >1 GiB (infeasible) | 201,959 nodes, 146.2 MiB | — | **infeasible → feasible** |
| 4 | 25 BB | >1 GiB (infeasible) | 203,553 nodes, 146.3 MiB | — | **infeasible → feasible** |
| 5 | 25 BB | >1 GiB (infeasible) | 1,070,634 nodes, 699.7 MiB | — | **infeasible → feasible** |
| 3 | 100 BB | 970,891 nodes, 605.4 MiB | 46,267 nodes, 36.5 MiB | 21.0x | already feasible |

### Key Findings

1. **5-seat games become feasible at 10 BB and 25 BB.** The full tree exceeded
   1 GiB; the depth-limited tree is 146.2 MiB (10 BB) and 699.7 MiB (25 BB).
2. **4-seat games become feasible at 25 BB.** The full tree exceeded 1 GiB;
   the depth-limited tree is 146.3 MiB.
3. **6-seat games remain infeasible** at all stack sizes. The betting-tree
   combinatorics (N sequential actors per street, compounding across streets)
   exceed 1 GiB even with `max_streets=1`.
4. **100 BB games remain infeasible for 4+ seats.** Deeper stacks create more
   betting rounds and larger trees.
5. **All feasible fixtures achieve 100% action-node coverage.** The
   depth-limited policies cover every action node in the tree.
6. **Solve time is the binding constraint** for 4+ seat fixtures: most hit
   the 30 s wall-clock limit before completing 100K iterations, but the
   policies are still 100% covered.

### Updated Recommendations

Depth-limited resolving (option 3 in the original recommendations) is now
implemented and measured. It extends feasibility to 5 seats at 10/25 BB and
4 seats at 25 BB, but 6-seat games and 100 BB 4+ seat games still need a
coarser action abstraction:

1. **Raise-count cap per street** (highest leverage): cap at one bet/raise per
   street to bound the branching factor. This directly addresses the 6-seat
   and 100 BB infeasibility.
2. **Fewer bet sizes for multiway pots**: drop the 1/2-pot raise and keep a
   single pot-fraction bet for deep multiway subgames.
3. **Combine depth-limited resolving with a coarser schedule**: the two
   techniques compound. A raise-capped schedule with `max_streets=1` should
   make 6-seat 10/25 BB games feasible.
4. **Neural network value estimator** (Phase 4, future RFC): replace the
   exact `EquityFrontierEvaluator` enumeration with a learned value function
   to further reduce frontier-evaluation cost.

## Raise-Capped Results (2026-10-16)

The raise-count cap (`ActionAbstraction::max_raises_per_street=1`) suppresses
bet/raise actions once one full raise has occurred on the current street,
leaving only call/fold. Combined with depth-limited resolving
(`DepthLimit::streets(1)` + `EquityFrontierEvaluator`), the T3-DL-CAP scan
(`--t3-dl-cap`) repeats the same 15 fixtures.

| Seats | Stack | Tree nodes | Accounted | Tree cap | Solve outcome |
| --- | --- | --- | --- | --- | --- |
| 2 | 10 BB | 153 | 0.1 MiB | fits | complete, 100K iter, 1.0 s, 12 info sets, 100% |
| 3 | 10 BB | 505 | 0.3 MiB | fits | complete, 100K iter, 2.8 s, 36 info sets, 100% |
| 4 | 10 BB | 1,457 | 1.1 MiB | fits | complete, 100K iter, 7.3 s, 96 info sets, 100% |
| 5 | 10 BB | 3,809 | 2.3 MiB | fits | complete, 100K iter, 18.2 s, 240 info sets, 100% |
| 6 | 10 BB | 9,361 | 9.1 MiB | fits | wall-clock 30 s, 29.6K iter, 576 info sets, 100% |
| 2 | 25 BB | 153 | 0.1 MiB | fits | complete, 100K iter, 1.1 s, 12 info sets, 100% |
| 3 | 25 BB | 505 | 0.3 MiB | fits | complete, 100K iter, 2.9 s, 36 info sets, 100% |
| 4 | 25 BB | 1,457 | 1.1 MiB | fits | complete, 100K iter, 7.2 s, 96 info sets, 100% |
| 5 | 25 BB | 3,809 | 2.3 MiB | fits | complete, 100K iter, 17.9 s, 240 info sets, 100% |
| 6 | 25 BB | 9,361 | 9.1 MiB | fits | wall-clock 30 s, 30.6K iter, 576 info sets, 100% |
| 2 | 100 BB | 153 | 0.1 MiB | fits | complete, 100K iter, 1.0 s, 12 info sets, 100% |
| 3 | 100 BB | 505 | 0.3 MiB | fits | complete, 100K iter, 2.9 s, 36 info sets, 100% |
| 4 | 100 BB | 1,457 | 1.1 MiB | fits | complete, 100K iter, 7.0 s, 96 info sets, 100% |
| 5 | 100 BB | 3,809 | 2.3 MiB | fits | complete, 100K iter, 17.7 s, 240 info sets, 100% |
| 6 | 100 BB | 9,361 | 9.1 MiB | fits | wall-clock 30 s, 30.4K iter, 576 info sets, 100% |

### Depth-Limited vs Raise-Capped Comparison

| Seats | Stack | Depth-limited | Raise-capped | Reduction | Feasibility gain |
| --- | --- | --- | --- | --- | --- |
| 4 | 10 BB | 62,013 nodes, 36.7 MiB | 1,457 nodes, 1.1 MiB | 42.6x | already feasible |
| 5 | 10 BB | 201,959 nodes, 146.2 MiB | 3,809 nodes, 2.3 MiB | 53.0x | already feasible |
| 6 | 10 BB | >1 GiB (infeasible) | 9,361 nodes, 9.1 MiB | — | **infeasible → feasible** |
| 4 | 25 BB | 203,553 nodes, 146.3 MiB | 1,457 nodes, 1.1 MiB | 139.7x | already feasible |
| 5 | 25 BB | 1,070,634 nodes, 699.7 MiB | 3,809 nodes, 2.3 MiB | 281.1x | already feasible |
| 6 | 25 BB | >1 GiB (infeasible) | 9,361 nodes, 9.1 MiB | — | **infeasible → feasible** |
| 4 | 100 BB | >1 GiB (infeasible) | 1,457 nodes, 1.1 MiB | — | **infeasible → feasible** |
| 5 | 100 BB | >1 GiB (infeasible) | 3,809 nodes, 2.3 MiB | — | **infeasible → feasible** |
| 6 | 100 BB | >1 GiB (infeasible) | 9,361 nodes, 9.1 MiB | — | **infeasible → feasible** |

### Key Findings

1. **All 15 fixtures are feasible.** The raise-count cap combined with
   depth-limited resolving eliminates the 1 GiB tree-builder cap as a
   constraint for every measured seat count and stack depth.
2. **Tree size is independent of stack depth.** Node counts are identical
   across 10/25/100 BB for each seat count (e.g., 9,361 nodes for 6-seat at
   every stack). With only one bet/raise per street, deeper stacks do not
   create additional betting rounds, so the tree structure is determined
   solely by the number of actors and streets.
3. **Node count grows ~2.6x per seat** (153 → 505 → 1,457 → 3,809 → 9,361),
   a dramatic improvement over the uncapped superlinear growth.
4. **6-seat trees are only 9.1 MiB** — down from >1 GiB without the cap. The
   largest tree in the raise-capped scan is smaller than the smallest
   infeasible depth-limited tree by over 100x.
5. **5-seat and below complete 100K iterations** within the 30 s solve
   budget. 6-seat fixtures hit the wall-clock limit at ~30K iterations but
   still achieve 100% action-node coverage.
6. **All fixtures achieve 100% action-node coverage.** Every action node in
   every tree is covered by the resulting policy.

### Updated Recommendations

The raise-count cap (option 1 in the original recommendations) is now
implemented and measured. Combined with depth-limited resolving, it makes
six-max subgame solving feasible at all measured stack depths:

1. **The raise-capped + depth-limited configuration is the recommended
   default for six-max subgames.** It produces tractable trees (≤9.1 MiB)
   with 100% action-node coverage at every seat count and stack depth.
2. **Solve time is the remaining constraint for 6-seat games.** The 30 s
   wall-clock budget allows ~30K CFR iterations at 6 seats. For production
   use, the solve budget should be increased or the iteration ceiling
   lowered to match the available decision time.
3. **Quality validation is the next step.** This scan measures feasibility
   (tree construction, solve completion, coverage), not policy quality. A
   NashConv or exploitability measurement campaign is needed before the
   raise-capped abstraction can be certified for six-max play.
4. **Neural network value estimator** (Phase 4, future RFC): replace the
   exact `EquityFrontierEvaluator` enumeration with a learned value function
   to further reduce frontier-evaluation cost.

## Quality Measurement (2026-10-16)

The information-set-consistent NashConv walker (pooled best response) replaces
the previous per-deal walker, which allowed hidden-information-dependent
responses. The pooled walker groups joint deals by the traverser's own holding
and chooses ONE action per information set, matching the
`exact_best_response_utility` algorithm in `engine/src/stage6/exact_oracle.cpp`.

The `--t3-dl-cap-nc` mode runs the raise-capped depth-limited fixtures with
NashConv quality measurement. Initial results (100 CFR iterations, 10 BB):

| Seats | Joint deals | CFR Exploitability | Uniform Exploitability | Reduction |
| --- | --- | --- | --- | --- |
| 2 | 70 | 0.083 BB | 0.221 BB | 62.3% |
| 3 | 342 | 0.233 BB | 0.370 BB | 37.2% |
| 4 | 1,104 | 0.286 BB | 0.505 BB | 43.3% |
| 5 | — | pending (slow pooled BR) | — | — |
| 6 | — | pending (slow pooled BR) | — | — |

Key findings:
- The CFR policy is 37-62% less exploitable than uniform across all measured
  seat counts, even at only 100 iterations.
- Exploitability increases with seat count (0.083 → 0.233 → 0.286 BB), as
  expected for multiway games.
- The pooled best response is computationally expensive for 5+ seats due to
  the combinatorial growth in joint deals. Optimization or sampling is needed
  for full six-seat quality campaigns.
- These are preliminary results at low iteration count. A full quality
  campaign requires 100K+ iterations and the qualification proposal's
  learn/confirm methodology.

## Impact on T4 and T5

With the raise-count cap and depth-limited resolving, six-max subgame solving
is now feasible at all measured stack depths (10/25/100 BB). T4 (six-position
preflop policies) and T5 (flop/turn/river coverage) can proceed using the
raise-capped + depth-limited configuration. The remaining constraint is solve
time at 6 seats (~30K iterations in 30 s), which should be sized for the
available decision budget before a quality campaign. Policy quality
(NashConv/exploitability) has not yet been measured for the raise-capped
abstraction.

## Reproduce

```bash
cmake --build --preset release --target bigshark-subgame-nash-conv

# Full-tree scan (original T3)
./build/release/engine/bigshark-subgame-nash-conv --t3 100000 42

# Depth-limited scan (RFC 0024)
./build/release/engine/bigshark-subgame-nash-conv --t3-dl 100000 42

# Depth-limited + raise-capped scan (2026-10-16)
./build/release/engine/bigshark-subgame-nash-conv --t3-dl-cap 100000 42

# Depth-limited + raise-capped with NashConv quality measurement
./build/release/engine/bigshark-subgame-nash-conv --t3-dl-cap-nc 1000 42
```

The scan writes each fixture's tree metrics, solve termination and coverage to
stdout; tree-cap and trainer-cap outcomes are reported inline. The full log
for the 2026-10-14 run is preserved locally.
