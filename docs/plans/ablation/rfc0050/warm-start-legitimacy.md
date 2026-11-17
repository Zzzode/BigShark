# RFC 0050 — Warm-Start Mathematical Legitimacy Analysis (D328)

Status: Analysis only. Per the D328 directive ("if the legitimacy
argument does not hold, say so directly; do not force it"), this note
evaluates whether a warm-start switch in the existing solver can
restore monotonicity of the average-strategy NashConv. The conclusion
is that the experiment's causal premise does not hold against the code,
so no warm-start switch is added and no 400/800/1600 rerun is launched.
No frozen constant, gate, ladder, wall, classifier, or production
surface is touched.

## The premise to test

D328 hypothesized that the non-monotone NashConv observed after
iteration 200 comes from "the average strategy being recomputed from
zero each round", and that warm-starting — letting the strategy carry
across iterations — could restore a monotone, steeper tail.

## Code fact: the average strategy is already cumulative, never from zero

The seedless river kernel is linear-averaged CFR. Two sites settle the
premise:

- `river_solver.cpp:291` updates the average-sum table with
  `(*sum_[n])[...] += w * rm(...)` where `w = t` under linear averaging.
  The table is allocated once at zero in `layout()` and only ever
  accumulates; nothing resets it between iterations.
- `build_sigma()` (`river_solver.cpp:514-532`) forms the snapshot by
  normalizing that accumulated sum over each node's actions
  (`sum / tot`, uniform fallback only when the total is exactly zero).
  `nash_conv_chips()` then scores this snapshot.

So the measured NashConv is the exploitability of the linear average
strategy

  s̄^T(a) = ( Σ_{t=1..T} t·σ^t(a) ) / ( Σ_{t=1..T} t ),

which is continuous across iterations by construction. There is no
"from zero each round" step in this solver. The premise is factually
false here.

## What warm-start can mean, and why none of the in-algorithm forms helps

1. Regret/average warm-start on the same game (the only form that keeps
   the algorithm and definition unchanged). Initializing from a nonzero
   (R^0, S^0) and continuing CFR is mathematically identical to running
   one uninterrupted CFR trajectory and entering at its prefix. The
   resulting (R^T, S^T, s̄^T) are the same entries of the same sequence,
   not a new sequence. This is exactly the D307 resume path, already
   proven bitwise neutral on the rank-0 anchor
   (`stage1-resume-cost.csv`, `resume_neutral_bitwise=1`; native
   `test_rfc0050_resume`). A warm/cold 400/800/1600 comparison would
   therefore reproduce the cold points bit-for-bit and carry zero new
   information.

2. Warm-start from a different but related subgame. This changes the
   initial condition to a foreign policy, so the running average no
   longer corresponds to any CFR average of this game and the CFR
   convergence theorem's premises (nonnegative regret accumulated from
   the game's own visits) no longer apply. It is a new heuristic without
   an exploitability guarantee and would also reintroduce cross-game
   error into an exact-menu campaign — changing the definition rather
   than improving convergence, which D328 explicitly ruled out.

3. Changing the averaging rule or the algorithm (uniform vs linear
   weights, optimistic/regularized updates, exploitability descent).
   These can change the tail shape, but they are algorithm changes, not
   a warm-start switch, and each needs its own RFC and certification.

## Why the non-monotonicity is intrinsic, not an initialization artifact

CFR's theorem bounds average-strategy regret, and through it
exploitability, by an O(1/sqrt(T))-style upper envelope (tighter regret
bounds under CFR+). The envelope decreases monotonically, but the
theorem gives no pointwise monotonicity guarantee for the realized
exploitability e(s̄^T). The average strategy is a point moving through
strategy space as each weighted current strategy σ^t is added; a new σ^t
can pull s̄^T toward a region with a larger best-response gap, so e can
rise locally while staying under the shrinking envelope. The observed
800->1600 rise (+0.31 log slope, +24.2%) sits well inside any such
bound and is standard finite-iteration behavior on a large-menu tree,
not a solver fault. Warm-starting changes where one enters the
trajectory; it does not change the trajectory's dynamics or remove the
absence of a monotonicity guarantee.

## The one-sentence legitimacy verdict

Warm-start itself is mathematically legitimate — it is the D307 resume,
already shown equivalent to an uninterrupted solve — but the claim that
it can restore NashConv monotonicity in this solver is not, because the
average strategy is already cumulative and same-game warm-start only
replays the identical CFR trajectory, while the non-monotonicity is the
known lack of a pointwise monotonicity guarantee for linear-averaged
CFR exploitability, not a cold-start artifact.

## Decision and the genuinely targeted direction

Per D328, the warm-start switch is not built and the comparison is not
run: it is foreclosed by an existing bitwise-equivalence proof and would
spend compute the campaign already judged incompressible to learn
nothing. The negative result is itself the deliverable here.

If monotone exploitability is a real requirement, the correct target is
a different algorithm family with an explicit monotone gap guarantee —
exploitability-descent / game-space optimization, or regularized
mirror/saddle methods whose monotone guarantee is typically on a saddle
gap rather than the one-sided NashConv used here. That is a new RFC with
its own feasibility risk; even those methods rarely promise pointwise
monotone NashConv. It must not be smuggled in as a "warm-start" option
on the current frozen solver.
