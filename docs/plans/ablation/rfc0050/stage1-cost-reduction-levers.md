# RFC 0050 Stage 1 — Cost-Reduction Lever Assessment

Status: Current. Scope: analysis only, no production change and no long
compute. Produced after the Stage 1 BUDGET_NO_GO gate to decide whether
the ~43 h single-thread projection to a possible 0.002 BB crossing
justifies a project commitment, by evaluating three levers against the
measured kernel and receipts. Every speedup figure below that is not a
measured receipt is labeled an estimate.

## Measured starting point

- Anchor srp (32,16,16), exact cap-0: 1,600 action nodes, 4,797 total
  nodes, 3,197 terminals (river-root trees carry no chance nodes),
  4,796 total action menu slots, 1,081 concrete combos per seat.
- Single-thread throughput 5.95-6.87 s/iteration; reach-bound scoring
  10.46 s/rung; peak RSS 615 MB. Observed convergence slope -1.004
  through iteration 200; a crossing near 26,000 iterations if that law
  holds, i.e. ~43 h single-thread at 5.95 s/iter.
- Host: 56 physical cores, two sockets x 28, no hyperthreading.
- The seedless river kernel is documented bitwise identical across
  thread counts (`river_solver.hpp`): the only parallel work is
  independent own-combo rows with a serial inner reduction, so changing
  thread count changes wall time but never the numeric result.

## Lever 1 — multi-threading (highest potential, zero numeric risk)

What exists. `thread_pool.hpp` provides a persistent fork-join pool
with chunked dispatch and an epoch barrier. The river kernel has only
one parallel region per traversal — the terminal showdown matvec
(`river_solver.cpp:214`, `parallel_for(kC)`); the second `parallel_for`
is the one-time showdown-matrix precompute at construction. The
top-down reach propagation, fold evaluation, and bottom-up regret/
average backprop are all serial loops, because within one traversal the
reach pass must finish before the backprop pass.

Structural Amdahl shape. The serial traversal work is on the order of
the 4,796 menu slots x 1,081 combos (~5.2M element operations per
traversal, twice for reach and backprop). The parallel matvec is
showdown leaves x 1,081^2 operations; with hundreds of showdown
terminals among the 3,197 terminals this is the dominant arithmetic
(the exact showdown/fold split is one of the figures the recommended
probe must print). If the matvec is ~95-99% of the arithmetic, the
thematic ceiling under 28 socket-local cores is roughly a 15-20x
speedup, which would bring the ~26,000-iteration crossing from ~43 h
toward a same-day multi-hour run.

Honest caveats, not yet measured on this anchor. There is no
multi-thread receipt for this exact cap-0 tree; the only scaling
evidence in the repository is the Loom thread study on other shapes,
which found the socket-local range (up to 28 threads) viable, cross-
socket (t > 28) NUMA regression, and fork-join barrier/straggler
overhead as the practical limit. The matvec is a matrix reduction that
is partly memory-bandwidth bound, so realized efficiency will be below
the arithmetic ratio. Two barriers per iteration plus the serial
sections cannot overlap. The estimate is therefore "potentially a
decisive 10-20x", never a promised number.

Why it is safe. Because results are bitwise identical across thread
counts, a multi-threaded Stage 1 run is not a new scientific candidate
and changes no frozen constant, gate, ladder, or verdict; it only
shortens the same measurement. The pool already supports `threads=0`
(hardware count) and the solver config already carries a threads field.

## Lever 2 — warm-start and resumable checkpoints (numerically neutral)

What does not exist. Neither the rfc0047 river nor turn bench kernel
serializes regret/average state; their "checkpoints" are NashConv
logging only. Every river solve starts from zeroed tables and a 43 h
run has no resume path, so an interruption discards the whole solve.

Two distinct variants.

1. Resume checkpoint (recommended, low risk). Persist the per-node
   regret and average-sum tables (~83 MB combined per the Step 0b
   itemized receipt) plus the iteration counter, and reload to continue.
   This is numerically neutral — the reloaded tables are the same state,
   so convergence and every receipt are unchanged; it only adds
   durability and lets rungs span processes. It mirrors the RFC 0028
   sealed warm-start work already done for the turn component
   (BSWARM01), so a precedent and codec pattern exist, though none is
   wired into this bench kernel. Moderate engineering, no science risk.
2. Cross-ledger warm-start (not recommended now). Initializing one
   anchor's tables from another ledger's solution needs a node mapping
   across differently shaped trees (1,600 to 5,720 action nodes), has
   an unknown hit rate, and changes the initial conditions, which would
   require re-validating the convergence law. High uncertainty for a
   Stage 1 single-anchor question.

The resume variant pairs naturally with Lever 1: a multi-threaded run
that is still multi-hour should be resumable.

## Lever 3 — a coarser "exact" subgame (largely closed on the river)

The error-free coarsening options are exhausted:

- Card bucketing is closed by RFC 0042-0045; reintroducing it restores
  the card-abstraction floor the exact-combo kernel was built to
  remove.
- Restricting the betting menu restores a nonzero pure-menu term — the
  exact structural quantity Stage 1 just proved zero, and the whole
  premise of RFC 0050.
- A depth limit has no object on the terminal street; the tree is
  already river-rooted.
- Sparse per-node combo vectors are the only untested idea, but RM+
  regret matching keeps full action support so per-combo reach rarely
  collapses to exactly zero; the gain is uncertain and it is a kernel
  redesign that would need fresh exactness argumentation. Parked as
  research, not a near-term lever.

## Recommendation

The credible path is Lever 1 first, optionally combined with Lever 2's
resume checkpoint; Lever 3 has no clean error-free form on the river.
Before any commitment, run one cheap bench-only thread-scaling probe on
this exact anchor — threads {1, 2, 4, 8, 16, 28}, a handful of
iterations each (single-digit minutes per point, well under any long-
solve budget), printing per-thread per-iteration wall, the showdown/
fold terminal split, and the measured serial fraction. That curve
decides directly whether the ~26,000-iteration crossing becomes a
same-day run (in which case a long-wall amendment is cheap and
compelling) or stays a multi-day commitment (in which case the plateau
question dominates and Stage 2 or a pause is the better spend). No
frozen constant changes for this measurement; the numeric result stays
bitwise identical to the single-thread gate.
