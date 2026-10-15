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

## Impact on T4 and T5

T4 (six-position preflop policies) and T5 flop/turn/river coverage cannot use
full-tree solves at six seats under the current abstraction. They depend on
the coarsened multiway schedule and/or depth-limited resolving recommended
above. The 3-seat solve-constrained results also show that even where the tree
fits, the trainer wall/information-set budgets must be sized for the actual
seat count before a quality campaign is meaningful.

## Reproduce

```bash
cmake --build --preset release --target bigshark-subgame-nash-conv
./build/release/engine/bigshark-subgame-nash-conv --t3 100000 42
```

The scan writes each fixture's tree metrics, solve termination and coverage to
stdout; tree-cap and trainer-cap outcomes are reported inline. The full log
for the 2026-10-14 run is preserved locally.
