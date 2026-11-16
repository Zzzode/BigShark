# RFC 0050 Stage 1 — D303 Thread-Scaling Result

Status: Current. Bench-only measurement on the exact cap-0 srp
(32,16,16) anchor. Receipt: `stage1-thread-scaling.csv`. The probe runs
a hard-coded ten-iteration solve at threads {1,2,4,8,16,28}, prints
per-point wall, speedup, the Amdahl inverse serial fraction, terminal
split, and a bitwise cross-thread determinism gate. Thresholds were
pre-registered before any data: >=10x long-wall amendment cheap;
3-10x plateau risk dominates (the directive's 3-5x band, extended
fail-closed over the unstated 5-10x gap); <3x compute incompressible.

## Structural facts measured first

- Terminal split on the 4,797-node tree: 1,600 action nodes, 1,599
  showdown leaves, 1,598 fold leaves (no chance nodes, terminal street).
- Construction is ~100-180 ms and effectively thread-count independent
  after the first point; peak RSS 615,704 KB at every thread count.
- The host was quiescent during the run (56 physical cores, load
  average 1.45-2.31, no competing tenant), so the saturation below is
  an Amdahl property of the kernel, not external contention.

## Speedup and Amdahl (receipt rows)

| threads | ms/iteration | speedup vs t=1 | Amdahl serial fraction | parallel fraction |
| --- | --- | --- | --- | --- |
| 1 | ~10,033 | 1.0000 | undefined (single worker) | — |
| 2 | ~6,366 | 1.58 | 0.269 | 0.731 |
| 4 | ~5,609 | 1.79 | 0.412 | 0.588 |
| 8 | ~4,925 | 2.04 | 0.418 | 0.582 |
| 16 | ~4,589 | 2.19 | 0.421 | 0.579 |
| 28 | ~4,447 | 2.26 | 0.423 | 0.577 |

The implicit serial fraction stabilizes at ~0.42 from four threads
upward and the speedup curve is nearly flat past eight threads. The
asymptotic Amdahl ceiling at that serial fraction is 1/0.423 ~= 2.37x,
which the 28-thread point (2.26x) is already close to; adding the
second socket's cores cannot help because roughly 42% of each
traversal (top-down reach propagation, fold evaluation, bottom-up
regret/average backprop, and the per-iteration fork-join barriers) is
serial by construction, and the single parallel showdown matvec is
also partly memory-bandwidth bound.

## Determinism gate (D303 requirement 4)

All six thread points print the identical NashConv and e_embed
4.89227959 BB, an identical pure-menu term 0.00000000 (within 1e-8),
and the identical average-strategy hash
18337703177597720050. The single-thread NashConv matches the sealed
Stage 1 calibration value at iteration 10 within 5e-9, validating the
probe itself. Multi-threading therefore changes only wall time, never
the numeric result, and the exact-menu zero identity holds at every
thread count.

## Pre-registered verdict: COMPUTE_INCOMPRESSIBLE

The 28-thread speedup is 2.2561, below the 3x incompressible
threshold, so the hard-coded verdict is `compute_incompressible`. Per
the pre-registered rule this closes the multi-threading lever on
measured evidence: even a fully parallel 28-core run reduces the
projected ~43 h single-thread crossing only to roughly 19 h, still a
multi-day single-anchor commitment, and no thread count converts it
into a same-day run.

Consequence for the campaign. The long-wall amendment is not made
cheap by parallelism on this kernel. The remaining honest options are
(1) accept the ~19-43 h per-anchor cost only if a strong reason exists
to believe the -1.0 law persists (still uncertified past iteration
200), (2) pursue the resume-checkpoint durability work so a multi-hour
run survives interruption without changing the science, or (3) move to
the Stage 2 boundary map or pause, rather than treating compute as
readily compressible. The 0.002 BB gate, frozen ladder, verdicts, and
all constants are unchanged; this is a measurement-only result.
