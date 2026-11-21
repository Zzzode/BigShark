# RFC 0046 Gate 2 — Early CLOSE/REDIRECT Review Report

Date: 2026-11-04. Author: Claude (zzzode agent session).
Status: **GOVERNANCE-LEVEL CLOSURE OF ROUTE A, supported by an independent
non-author architect ruling (see "Closure decision").** The frozen Gate-2
gates were never invoked (no 720-slot Phase A); this is a research-instrument
falsification on the precedented RFC 0041/0043/0045 path. All numbers are
reproducible research-probe rows. Supporting raw evidence and reproduction
commands:
[gate2-feasibility-probe.md](../plans/ablation/rfc0046/gate2-feasibility-probe.md);
the structured-solver design and its APPROVED residual research scope:
[0046-solver-probe.md](0046-solver-probe.md).

## Closure decision (governance level)

An independent non-author architect agent (id `a3a6b28a2ec6fbd15`, separate
from the prototype/report author; three review rounds DISSENT -> CHANGES
REQUESTED -> APPROVE, then the closure ruling, 2026-11-04) ruled that **Route
A can be closed now and that completing the V1/V3 probe is NOT a
precondition**, because the full-range infoset footprint falsifies the
envelope conjunct a priori for 5 of 6 frozen strata for EVERY exact-key
algorithm (see "Second, independent structural wall"). It independently
re-derived the footprint and produced a row-exact cross-validation:

> W1's ten combos give 20 off-board card incidences; live combos summed over
> the 48 public river children = 48·10 - 20 = 460; predicted table rows =
> 10·10 (turn) + 82·460 (river) + 10 (gadget -x rows) = **37,830**, exactly
> the measured `information_sets = 37,830`. This both validates the counter
> and shows the real solver reached 100% structural coverage, so the
> fully-mixed count is not an over-estimate.

**Precise falsification scope.** What is dead: a *real-time, exact
per-combo, turn-rooted, certified endgame inside the frozen 1M-infoset /
1 GiB / 10 s Gate-2 envelope*. What is explicitly PRESERVED as the surviving
successor hypothesis (not part of Route A): a depth-decomposed, per-river-card
resolver fed by an offline CERTIFIED per-combo turn continuation-value oracle
(the Route C / margin-oracle family, needs a new RFC and gates). A single
river subtree is only 82·1081 = **88,642 infosets** (raised 108·1081 =
116,748) - inside the infoset envelope - so the surviving open problem there
is certified CV quality at 1e-6-pot scale and wall, NOT footprint; that is
exactly the problem RFC 0043 left open. The record must not read "exact online
endgame solving is infeasible" - the narrower statement above is the verdict.

Conditions attached to this closure (disposition): (1) scope as above and
preserve the successor - DONE here; (2) finish the running 5M and the
200k/400k/1M/2M margin-iters ladder purely as documented convergence/
reference-floor evidence and cite single-seed/single-stratum honestly -
DONE: against the 400k reference the gadget floor is 10.48 chips at 5 M
(leg N^-0.06, 20k/100k/400k/1M/2M/5M = 47.16/20.64/13.77/11.33/11.10/10.48),
and at fixed 1 M gadget the reference ladder 200k/400k/1M/2M =
13.30/11.33/9.17/8.28 chips (margin leg also flattening ~-0.21); all W1,
seed 4320, single stratum, post-byte-fix instrument - a documented floor, not
a gate statistic; (3) W1 V1/V3 is downgraded from closure prerequisite to parallel
research with two residual uses only (the convergence-law record on the object
that fits, and a prototype of the offline certified margin oracle); a dramatic
V3 law on W1 would reopen ONLY the depth-decomposed river architecture, never
turn-rooted Route A - recorded in the probe doc; (4) non-author evidence pass
on the counter/arithmetic - DONE by this ruling (re-derived + 37,830
identity); (5) record both instrument bugs with instrument-binary tags - DONE
in the quarantine doc.

## Independent review lineage

A first independent architect (id `a1fa61ac3ad9d325e`, 2026-11-04), which did
not author the prototype or this report, reviewed the early-exit evidence and
returned **DISSENT (conditional)**. It concurred that Route A as prototyped
cannot meet Gate 2 and that the 720-resolve driver should not be built now,
but rejected closing the Route-A hypothesis on the original data because the
load-bearing measurement solved the wrong augmented game. Its mandatory
sequence, adopted verbatim in substance:

1. fix the margin-wiring blocker and re-measure (two-axis ladder + one
   offline 5-10 M-iteration asymptote cell);
2. then a bounded 1.5-2 week structured exact-solver engineering probe
   (highest-EV true alternative);
3. Route C tabular boundary-value trunk only if the solver probe fails.

## Blocker found and fixed

The cell runner populated `GadgetInput gin` but never assigned
`gin.margins` (`rfc0046_cell.cpp:211`); `GadgetInput::margins` defaulted to
empty, so `turn_gadget.cpp` used `margin = 0.0` for every combo, while the
extracted 200k margins were passed only to the certifier. Every original
"locked margin" slack point (42.43 / 16.23 / 7.51) and the `--probe-nc`
NashConv (6.20 summed BB) therefore scored a **zero-margin gadget** against
non-zero reference margins — two different augmented games. The native
ctest already wires both sides and asserts locked slack 5.01 < zero-margin
8.92 on a controlled object (`test_rfc0046_gadget.cpp`). Fix landed:
`gin.margins = locked_margins;` (same site). The earlier ladder is
quarantined in the probe doc and must not be quoted as Gate-2 evidence.

## Corrected-wiring re-measurement (W1, seed 4320, cap-1)

Object: board `2c 3d 7h Js` / river `9c`, exact ConcreteCards key,
37,830 concrete infosets / 10,923 tree nodes / ~27 MB RSS — far inside the
1 M infoset / 5 M node / 1 GiB envelope. Responder certifier slack
`max_c (BR_continue(c) - b(c) - 1e-6·pot)` (chips), certifier tolerance
`1e-6·pot = 4e-6` chips.

| gadget iters | margin iters | wall (s) | max slack (chips) | termination |
|---:|---:|---:|---:|---|
| 20,000 | 400,000 | 42.1 | 47.1593 | BoundExceeded |
| 100,000 | 200,000 | 206.5 | 20.1217 | BoundExceeded |
| 100,000 | 400,000 | 204.7 | 20.6361 | BoundExceeded |
| 400,000 | 400,000 | 906.2 | 13.7666 | BoundExceeded |
| 1,000,000 | 400,000 | 2,324.3 | 11.3325 | BoundExceeded |
| 2,000,000 | 400,000 | 5,049.3 | 11.0957 | BoundExceeded |
| 5,000,000 | 400,000 | 11,848.0 | 10.4781 | BoundExceeded |

Correct-wiring slack is the same order as the invalid ladder (20.1 chips at
100k vs the buggy 16.2); it does not collapse once the gadget plays the real
augmented game. The fixed-reference (margin=400k) leg shows a **flattening to
a floor**: leg exponents N^-0.51 (20k->100k), N^-0.29 (100k->400k), N^-0.21
(400k->1M), N^-0.03 (1M->2M), **N^-0.06 (2M->5M)** - the 2.5x increase from
2 M to 5 M buys only 0.62 chips (11.10 -> 10.48), so the curve has a hard
floor around ~10 chips against the 400k reference (~2.5 pots, ~5 summed BB),
6 orders above the 4e-6 chip bound. The margin ladder below shows a better
reference lowers the joint floor to ~8.28 chips at gadget 1 M but flattens
there too, so ~8-10 chips is the shared gadget-plus-reference RM+ floor on
W1, not a tuning point. Margin sensitivity at 100k gadget is flat
(200k margin 20.12 vs 400k margin 20.64); the separate margin-iters ladder at
gadget = 1 M tests the reference component explicitly. Actual getrusage RSS
stays 26-28 MiB with no growth; throughput degrades 484 -> 441 -> 430 -> 396
-> 422 it/s as the table saturates (the last figure is a contended
parallel-run rate; gap values are load-independent).

### Margin-iters ladder at fixed gadget = 1,000,000 (the ~11-chip "floor" is partly the reference)

Holding the gadget at 1 M and improving the LOCKED reference (post-byte-fix
binary; `margin_done` confirms each reference ran its full requested
iterations, no silent truncation):

| margin iters | max slack (chips) | ratio vs prior |
|---:|---:|---:|
| 200,000 | 13.2955 | - |
| 400,000 | 11.3325 | 0.85 |
| 1,000,000 | 9.1709 | 0.81 |
| 2,000,000 | 8.2812 | 0.90 |

So the ~11-chip plateau seen against the 400k reference is NOT a pure
gadget-convergence floor: once the gadget error is pushed down to the 1 M
scale, the fixed reference's own CFV error becomes a measurable co-driver,
and slack tracks margin-iters (200k -> 1 M cuts slack 31%). But the margin
axis ALSO flattens (1 M -> 2 M only -9.7%; 200k -> 2 M, a 10x reference
increase, cuts slack just 38%, margin-leg exponent ~ -0.21) because the
reference is produced by `train_nseat`, the SAME slow external-sampling RM+
family as the gadget. Both error sources share the ~8-chip W1 floor. This is
the reviewer's **reference-floor branch in effect**, and it sharpens the
Route-C requirement: an offline per-combo oracle cannot reach the 1e-6-pot
scale with more MCCFR iterations; it must be solved by a structured,
deterministic solver that changes the convergence law (probe variant V3) and
must SHIP A CERTIFIED ERROR BOUND on every boundary value, not just a larger
iteration count. Tighter offline boundary values do materially reduce the
violation (direct motivation for the oracle), and offline construction has
no 10 s limit, but the absolute level (8.28 chips at 1 M/2 M, ~2.1e6x over
4e-6) does not change the full-range envelope closure. (Wall times here are
contended by parallel cells and are not gate timing; the gap values are
load-independent.)

## Second instrument bug found and fixed (resource accounting)

The first long-run attempt exposed another measuring-instrument defect.
`tick()` accumulated `bytes += add_bytes` on **every walk visit** (the
action-node site added 64+8·#actions per visit) and then compared that
monotone per-visit tally against `max_bytes = 1 GiB` as though it were peak
memory. Actual RSS stays ~27 MB, but the cumulative charge crossed 1 GiB at
exactly iteration 401,945, so both the 1 M and 5 M cells were killed with a
spurious `ResourceExhausted` at the identical point (same slack 13.7541),
making every run past ~402k impossible. Fix: byte accounting is now
resident-only — rows are charged once at creation (`Table::touch` reports a
`created` flag; the two `gadget[]` insert sites charge on size delta), and
`tick()` no longer takes a per-visit byte charge. `peak_bytes` is a resident
estimate (~7 MB for this object), not a visit tally. The three affected
native tests (rfc0046_gadget — determinism, identity ≤1e-9, locked-vs-zero
margin direction, B4 summed BB — concrete_keyer, keyer_seam) all still pass,
so strategy values are unchanged; only the broken resource accounting moved.
The 1 M / 2 M / 5 M ladder is re-running with the corrected accounting.

## Why the simple extrapolation was withdrawn

The original report fitted ~N^-0.5 to three points and extrapolated an
iteration gap. Two independent defects make that inadmissible even with the
wiring fixed:

- **Arithmetic.** `400,000·(7.51/4e-6)² = 1.41e18`, not 1.4e14 (~14.5
  orders, not ~10). The error was conservative but wrong.
- **Two error sources.** Slack against a fixed finite-iteration MCCFR
  reference has a non-shrinking floor equal to that reference's own
  max-combo CFV error; a 200k-400k profile of this slow family plausibly
  carries 10^-1-10^0 chip error against a 4e-6 chip bound. Single-seed
  max-norm points cannot separate gadget convergence error from the
  reference floor, so no exponent may be extrapolated across many orders.
  The valid statement is the two-error-source analysis plus the measured
  offline asymptote, not a fitted law.

## Binding constraint (rewritten per review)

- **Envelope on W1: non-binding, verified** (37.8k / 10.9k infosets-nodes /
  ~27 MB vs 1 M / 5 M / 1 GiB). W1-only; the full-range envelope is a
  separate hard wall - see the next section.
- **External-sampling RM+ convergence is too slow at the current
  per-iteration efficiency: supported.** The independent plain-`train_nseat`
  corroboration (~2.82 summed BB at 40k) and the RFC 0043 ladder (same RM+
  family, ~T^-0.55, no certifiable plateau to 10 M) are legitimate evidence
  that external-sampling RM+ in this engine converges far slower than the
  bound requires.
- **A structured exact solver's CONVERGENCE law is untested/open, but its
  full-range envelope already fails.** The measured ~400-500 it/s carries demonstrably avoidable cost
  (full-table O(38k) `clip_positive()` every iteration, `std::map` rows with
  three heap vectors each, per-call vector allocation, a `steady_clock` read
  per walk visit, a full 48-card average walk per iteration). The game has a
  single public river card and only 10,923 nodes — the ideal case for
  deterministic full-chance/vanilla or range-vectorized CFR+, which has not
  been tried. Declaring the rate "structural" before that test was the
  original report's largest analytical gap. The Stage-0 addendum froze the
  envelope and predicates, not the solver algorithm, so this probe is inside
  Gate 2's hypothesis, not a budget increase. A faster solver can at most
  address convergence; it cannot fix the full-range infoset wall below.

## Second, independent structural wall: full-range infoset envelope (measured exact)

A new manual no-solve counter (`bigshark-rfc0046-infoset-count`,
benchmark-only) counts the exact concrete-infoset footprint of a fully-mixed
exact-key solution. Betting actions are public information, so every live
combo reaches every action node of its street:
`infosets = turn_action_nodes x 1128 + river_action_nodes x 1081`, where
1128 = C(48,2) live turn combos after the four board cards and 1081 =
C(47,2) at a river node (the dealt river card blocks the 47 pairs
containing it).

| cap-1 object | turn act | river act | exact concrete infosets | vs frozen 1,000,000 |
|---|---:|---:|---:|---:|
| four limped strata (incl. monotone) | 10 | 3,936 | **4,266,096** | **4.27x over** |
| raised-srp | 12 | 5,184 | **5,617,440** | **5.62x over** |

Five of the six frozen strata are full range; only the 10-combo W1 anchor
fits (exactly why W1 looked "envelope non-binding"). A full-range exact-key
resolve misses the infoset envelope by 4.3-5.6x **even if the solver were
instant and perfectly converged - merely allocating the exact table exceeds
the bound.** Reach sparsity does not rescue it: RM+ starts fully mixed, the
certifier fail-closes on any missing reached row, and a combo's reach goes
to zero only if every inbound action is zeroed for that combo. Material
shrinkage requires card abstraction (forbidden by the exact-key hypothesis),
a coarser/depth-limited boundary, or offline per-combo value nesting (Route
C). Certification itself forces the full materialization, not just solving:
the certifier fans out every river card and requires a served row at each
reached river infoset (MissingRow / StrictPolicy fail-closes), so a scheme
that solved turn values by looping the 48 river subtrees one card at a time
and DISCARDING river rows could not be certified as one turn-rooted subgame -
that decomposition is depth-limited continual resolving with boundary values
(Route C, a new RFC), not the frozen single turn-rooted gadget, and it still
pays all 48 river solves plus a certified boundary nesting inside the wall.
Gate 2 thus has TWO INDEPENDENT structural failure modes on the
production object: convergence rate and full-range infoset size. The
infoset axis of the decision rule's branch-(i) condition is settled without
a solve; V3's remaining value is the convergence-law test on the W1-scale
object and as a possible offline certified margin oracle.

## What would actually constitute closure

A single diagnostic cell cannot emit a typed Gate-2 CLOSE: the frozen
evaluator requires all four candidates and exactly 30 timing slots per
candidate×stratum and aborts a lone cell on `timing-count`/
`missing-candidate`. The honest choice is:

- **Full Phase A** — 720 typed cold-resolve slots (failed resolves occupy
  slots as `truncated`; ~2-4 h host once the driver exists) plus a diagnostic
  on a failing block — to obtain the frozen protocol's own typed CLOSE; or
- **Governance-level research closure** (precedented: RFCs 0041/0043/0045
  closed without invoking the frozen gates) — correct-wiring re-measurement
  + offline-asymptote cell + the structured-solver probe, recorded in an
  addendum-level note / RFC as "hypothesis falsified by the research
  instrument; frozen gates never invoked."

The 720-cell driver remains stopped under TechLead round 79 pending the
re-measurement and solver-probe outcome.

## Redirect ranking (independent architect, adopted)

> **Superseded by the closure decision above.** This was the pre-envelope
> ordering. With the exact full-range infoset wall now measured, the
> resolution is: item 1 is done (both instrument bugs fixed, ladder
> documented); item 2 is DOWNGRADED to parallel research (the W1
> convergence-law record and an offline margin-oracle prototype), not a
> closure prerequisite; **item 3 is the confirmed successor**, specified more
> precisely as a *depth-decomposed per-river resolver behind an offline
> certified per-combo turn-CV oracle* (a single river subtree fits at
> 88,642 infosets; the open problem is certified CV quality, not footprint);
> item 4 remains a last-resort fallback.

1. **Fix + re-measure (prerequisite, in progress).** Splits instrument bug /
   10 s too tight / bound too tight. Trivial cost; decisive for every option.
2. **Structured exact-solver probe (1.5-2 weeks, benchmark-only; highest
   EV).** Dense/flat-hash rows, lazy per-row RM+ clip, elimination of the
   separate full-fan-out average traversal, range-vectorized blocker-aware
   showdown evaluation over 1,128 combos per public node, deterministic
   enumeration of the 44 legal river cards (48 public children with blocker
   masking), CFR+/DCFR/linear weighting, T1 warm start. Predeclared
   W1→monotone two-tier kill criteria (a cheap necessary-not-sufficient
   1e-4-pot/0.02-BB/30 s screen, then a 10 s 1e-6-pot/0.002-BB commitment
   gate plus a full-range 1 M-infoset/1 GiB envelope check). Pure constant-factor
   engineering at a fixed MCCFR exponent cannot suffice (even ~4 M iters in
   10 s, already ~820x the measured 484 it/s, leaves slack ~3.3 chips; the
   measured N^-0.5 law needs ~1.5-2.7e18 iters, ~1e8 years, to reach 4e-6);
   the pass must change both implementation and sampling structure.
   DeepStack-class turn/river subgames of this size were solved on CPU in
   sub-second-to-second budgets, and a single river card with a cap-1 tree is
   the best possible case — failure here generalizes. The detailed two-tier
   kill gates, the V1/V2/V3 definitions, and the full-range envelope caveat
   are in [0046-solver-probe.md](0046-solver-probe.md).
3. **Route C tabular boundary-value trunk (3-5 weeks to RFC + probes).**
   Pursue only if (2) fails. RFC 0043 already recorded a structural no-go on
   this lineage (T^-0.55 RM+, no certifiable plateau, +0.26 BB benefit but no
   r*, 134,459-class AC6/AC7 cost OPEN); a tabular CV removes the 10 s wall
   but not the per-subgame convergence-rate wall. Requires RFC 0043's mandated
   absolute-BB slope certification.
4. **Offline-certified approximate bound (days); fallback only.** It
   documents the gap, does not produce M2 GTO, and the 9-category path
   already failed R0 in RFC 0045.

## What is NOT requested

No commit, no push, no gate campaign, no production/schema/BSFLIB/mount
change. The Stage-1 measurement code stays benchmark-only under no-legacy
until a successor RFC decides what to promote.
