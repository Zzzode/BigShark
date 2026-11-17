# RFC 0050 — Final Evidence and Campaign Conclusion (D326)

Status: Final. This is the closing evidence chapter for the exact-menu
river campaign, written after the decisive three-rung measurement.
Everything below is measured or derived from sealed receipts; no frozen
constant, the nine-rung ladder, the 1,800 s wall, the 0.002 BB gate, or
the shared verdict classifier was changed to produce it.

## 1. Deliverable: the structural menu wall is eliminated

The RFC 0049 campaign showed bounded-menu policies carried an irreducible
22-31 BB pure-menu term. RFC 0050 solved the exact cap-0 menu instead and
measured the pure-menu structural term on every one of the six
decision-bearing anchors:

- anchors 0-3 scored zero in the fixed-wall Stage 2 run;
- anchors 4-5, which could not reach a scoring rung under the fixed wall,
  were re-measured with a fixed-rung rerun and also scored zero with
  `pure_menu_measured=1`;
- the decisive run on rank 0 again recorded a measured zero at rungs
  400/800/1600, reproduced bitwise across two independent processes.

The structural result is therefore a delivered fact, independent of any
convergence outcome: when the served menu matches the solved menu, the
pure-menu term is zero and e_embed equals the bounded NashConv.

## 2. The online 0.002 BB gate is unreachable under current compute and coverage

### 2.1 The decisive convergence measurement (rank 0, exact cap-0, 28 threads)

Receipts `decisive-convergence.csv` and the bit-identical
`/tmp/decisive_run2.csv` rerun. Three rungs to iteration 1600:

| iteration | e_embed (BB) | segment slope (log2) |
| --- | --- | --- |
| 400 | 0.24053200 | — |
| 800 | 0.14159415 | -0.7645 |
| 1600 | 0.17584872 | +0.3126 (NashConv rose 24.2%) |

Three-point OLS slope across 400/800/1600: **-0.2259**. The
pre-registered classifier (bands applied to slope magnitude because the
directive's signed inequalities conflicted with its own labels) returns
`decayed_confirmed` (|slope| < 0.60), but the more important finding is
that no stable power law exists. The full segment timeline across every
measured rung is:

| interval | slope |
| --- | --- |
| 100 -> 200 | -1.0043 |
| 200 -> 400 | -0.1424 |
| 400 -> 800 | -0.7645 |
| 800 -> 1600 | +0.3126 |

The steep ~-1.0 law seen in Stage 1 held only over the first 200
iterations. After that the average-strategy NashConv flattened and then
became non-monotonic, rising 24.2% from iteration 800 to 1600. This is
not noise or a threading artifact: the two independent processes
produced bitwise identical values, and the flattening is already visible
in the single-thread Stage 1 data at 200->400. The NashConv is computed
from the linear average strategy, whose exploitability is not guaranteed
monotone at finite iterations on large menu trees.

The best e_embed reached anywhere in 100-1600 iterations is **0.14159415
BB at iteration 800 — still 71x over the 0.002 BB gate** — and it
worsened afterward. The pre-registered scenarios were a persistent
-1.0 law (crossing near 26,000 iterations) versus a -0.52 decay; the
measurement rejects the persistent-steep-law branch and shows the
reality is even less favorable than a clean decay: a short steep
transient followed by a shallow, non-monotone tail with no observed
trajectory toward the gate.

### 2.2 The coverage scale is independently infeasible

Even if a single anchor did converge, the online frontier is 134,459
canonical classes (2,598,960 concrete boards). At the measured
6.1-27.2 s per iteration per anchor, the D303 2.2561x multi-thread
ceiling, and thousands to millions of iterations per anchor under the
observed tail, a full-campaign 0.002 library is infeasible at current
compute regardless of the convergence law. This conclusion does not
depend on section 2.1.

## 3. The two corrected online-serving gates (review findings retained)

The Stage 3 review corrected two author errors, and the corrected
understanding stands:

- the real online envelope is a 256 MiB resident budget (with an
  existing mmap/lazy LRU mechanism), a 1 GiB RSS / p95 10 s / max 12 s /
  1,000,000-infoset per-resolve limit; the 1 MiB constant is only the
  wire-frame cap. A single-anchor exact policy (39.55 MiB smallest,
  ~141 MiB largest extrapolated) IS resident-feasible at 0.155x-0.55x
  of the budget;
- therefore serving infeasibility is not a footprint problem. It narrows
  to two independent gates, neither satisfied: (1) no Stage 1 PASS — no
  measured conformant exact policy exists; (2) no zero-residual
  transport — embedding the exact menu into a bounded online tree
  reintroduces the measured 22-31 BB pure-menu residual.

## 4. One-sentence conclusion

The exact-menu campaign delivered its structural result — the pure-menu
wall is genuinely zero on all six anchors — but an online 0.002 BB
exact-policy serve is unreachable under current compute and coverage:
the average-strategy NashConv loses its steep -1.0 law after ~200
iterations, flattens and turns non-monotonic while still 71x over the
gate at its best point, and even a converged single anchor could not be
served at full-campaign scale or through a zero-residual transport.

## 5. Stage 3 addendum: reviewed but not authorized, suspended not discarded

The Stage 3 addendum proposal (`stage3-addendum-proposal.md`) passed
independent review (after envelope corrections) as fit to proceed toward
a numbered addendum, but it is not authorized and is now suspended. The
parent RFC gates any BSFLIB04 addendum on a Stage 1 PASS, which does not
exist. The convergence-neutral persistence container (3a) remains a
valid design for durable offline policy storage, and the whole addendum
can be reopened without rework if a future change creates either a
conformant exact policy or a zero-residual transport. It is not deleted;
it is parked with its evidence and review record intact.

## 6. What was NOT changed and what remains open

Unchanged: the 0.002 BB gate, B0, the nine-rung ladder, the 1,800 s
wall, the frozen verdict classifier, all production solver/schema/codec/
mount/host/wire surfaces, and the no-push boundary. Open questions for a
future program, not this campaign: whether a different CFR variant or
warm-started trunk produces a monotone tail on the exact menu; whether
the margin-gadget path can ever provide a zero-residual transport; and
what quality contract the project adopts if the exact 0.002 online line
is abandoned.
