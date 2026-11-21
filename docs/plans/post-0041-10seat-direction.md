# Post-RFC-0041 Direction — Live-Seat-Scoped Resolving for up to 10 Seats in the 10-Second Envelope

Status: Current (measurement plan; 2026-10-29)
Context: RFC 0041 closed at Stage 1 with a recorded FAILURE (Amendment 2);
its Amendment 1 (decision envelope p95/p95 <= 10,000 ms, resolver hard
cap 10,000 ms) remains in force. This plan is the best-practice path
stated when the neural label campaign was abandoned: no learned
components, extend the established offline-blueprint + bounded-resolve
architecture.

## Principle

Cost is governed by the number of LIVE seats at the current street
(measured turn-tree nodes: 2p 357, 3p 1,864, 4p 5,669, 5p 15,074,
6p 37,279, 7p 110,341, 8p 254,097, 9p 573,853; 10 live seats exceeds
the 1 GiB tree byte cap). A 10-handed hand funnels 10 -> typically
3-6 to the flop and 2-4 to the turn/river, the same structure Pluribus
exploited (offline blueprint + depth-limited real-time resolving; it
shipped 6-max, never 10-handed full-tree).

## Work items

### W1. 10-second wall recalibration, seats 2-6 (COMPLETE 2026-10-29)

Driver: `--m1-wall-recalibrate <lo> <hi> <wall_seconds> [seed]` in
bigshark-subgame-nash-conv (runs each production M1 shape to a trainer
wall with a huge iteration cap; river rows additionally report the
bucket-constrained BR gap to isolate the card-abstraction floor).
Evidence: docs/plans/ablation/rfc0042-m1-10s-recalibration.csv (30
rows, 3 seeds, 10,000 ms).

Results (mean summed BB vs 0.15 gate):

| live seats | river exact / bucket | turn exact | verdict |
| --- | --- | --- | --- |
| 2 | 0.335 / **0.032** | 0.306 | river abstracted game converged; ~0.30 is card abstraction |
| 3 | 0.710 / **0.139** | 0.805 | river marginal within abstraction; ~0.57 is card abstraction |
| 4 | 1.087 / 1.00 | 2.134 | FAIL: bucket ~= exact => iteration-limited |
| 5 | 4.418 / 4.416 | 3.206 | FAIL: iteration-limited (~17k/15k iters) |
| 6 | 5.763 / 5.126 | 5.881 | FAIL: iteration-limited (~9k/6k iters) |

Conclusions that reshape the plan:

- The 6.25x wall increase over RFC 0036's 1,600 ms does NOT promote
  4+ live seats: their bucket gap equals the exact gap, so they are
  iteration-limited within the abstraction (1/sqrt(N) puts the gate
  hundreds of seconds away), not bounded by time settings. Bounded
  real-time resolving inside the card abstraction is viable only up
  to ~3 live seats on the river; turn bucket split is unmeasurable
  (chance nodes) and RFC 0031's ~0.26 floor explains most of the 2-seat
  turn 0.306.
- The dominant exact-game loss at EVERY seat count is therefore the
  9-tier card abstraction (2-seat river: 0.032 bucket vs 0.335 exact).
  W5 was promoted to the next investment and evaluated under RFC 0042
  (2026-10-30): finer equity buckets remove most of that loss but do
  not converge inside the 10 s online wall — KEEP online, rerouted as
  a candidate abstraction for the offline W4 blueprint track.
- For the 10-handed goal this sharpens the architecture: rivers with
  2-3 live seats resolve in time; 4+ live seats (and all flops/turns
  early in the hand, which still have 4+ live seats) must be served by
  OFFLINE precomputed policy artifacts (blueprints/libraries), never by
  longer real-time solves. W4 (preflop) extends into postflop
  blueprint coverage scoped by live-seat band.

### W2. High-seat Monte Carlo NashConv estimator (deprioritized)

W1 showed seats 4-6 fail the wall gate with the exact estimator; adding
a sampled estimator for seats 7-9 would only reconfirm iteration
limits under current abstractions. Revisit after W5 (finer abstraction
re-prices both convergence and the estimator need). Owner: engine
benchmarks. No production behavior change.

### W5. Card-abstraction refinement (MEASURED 2026-10-30 via RFC 0042: KEEP online; direction proven, reroute to offline W4)

W1 quantified it directly: at 2 seats the river exact-hole summed gap is
0.335 BB while the same-policy bucket gap is 0.032 BB — ~0.30 BB of
unavoidable loss comes from the 9-tier card abstraction (RFC 0031's
~0.26 BB floor, same phenomenon on a different shape); at 3 seats the
abstraction part is ~0.57 summed.

**RFC 0042 outcome: KEEP for the online resolver.** Exact-EHS
EquityStrengthV1 buckets (K=32/64/64) produced large, statistically
decisive exact-game improvements — 2p-river 0.335 -> 0.129, 2p-turn
0.303 -> 0.194, 3p-river 0.716 -> 0.361 BB; paired superiority
d=0.206 with 95% lower bound 0.204 (S=32, BCa bootstrap) — proving the
abstraction direction, but the frozen gates returned KEEP: the finer
partition does not converge inside its own abstraction in the 10 s
wall (river bucket sums 0.099/0.361 > 0.05; 3p-river exact == bucket,
the iteration-limited signature) and turn sealed rows exceed the
2.0x byte gate (2.18x/2.29x). TiersDrawV1 also failed (no river
benefit; worse 3p-turn). The Keep decision was independently
verified; measurement code was deleted under the no-legacy rule, with
evidence and the mechanical evaluator retained
(`docs/plans/ablation/rfc0042-*.csv`, `tools/rfc/rfc0042-gates.ts`).

Carried-forward implication: finer equity-based buckets are now a
prime candidate for the OFFLINE W4 blueprint/library track (no
per-request wall; the flop exact table builds in ~3 minutes/board,
feasible offline), not for the live 10-second resolver. Taking it
there needs a new RFC and the eight-site artifact flip listed in RFC
0042 Section 2.

UPDATE 2026-10-31 (RFC 0043, Implemented/no-go): the first offline
convergence-feasibility campaign measured the equity buckets without a
serve wall. They reach <=0.05 BB river within-abstraction gaps at 10M
iterations in every band (2p 0.016 / 3p 0.027 / 4p 0.025 BB) with
large, seed-strict positive exact-gap benefits over the nine-tier
control (2p +0.26, 3p +0.79, 4p +0.012 BB), but the frozen
convergence certification — a 20% relative two-rung plateau — never
forms for equity OR the nine-tier control (both decay at ~T^-0.55,
roughly halving per tripled rung), so the campaign could not select an
r* and the decisive 134,459-class disk/wall cost gates were never
measured. Independent ruling: structural no-go, not incomplete. The
equity-quality direction stands (equity IS strictly better), but the
next step is NOT to re-run this campaign; it is a new RFC that (a)
predeclares a reachable convergence instrument — calibrate an
absolute-BB slope window on the nine-tier incumbent first — and (b)
only then measures the 134,459-class build cost. The 200 GB / 7-day
feasibility question remains open. All RFC 0043 measurement code was
deleted; evidence CSVs remain under `docs/plans/ablation/`.

### W4. Preflop and postflop blueprints for high live-seat bands (offline)

Extend the RFC 0037 unified cap-1 100 BB preflop profile set (3-6 seats
shipped) with 7/8/9/10-seat builds through the existing flop-library /
mmap mount machinery; preflop is never solved online at any seat count.
W1 further shows flops/turns with 4+ live seats cannot be converged in
10 s, so postflop library coverage scoped by live-seat band (the
flop-library already does board-class pre-solves) is the offline answer
for early-street 4+-way pots; the resolver handles 2-3-way late
streets. Campaign cost and storage go in the builder manifest before
launching.

## Explicit non-goals

- No neural/learned river continuation values (RFC 0041 closure; a new
  RFC would need a proven deal-level variance reducer).
- No promise of exact real-time solving for 9-10 live seats deep in a
  hand; those shapes get honest fallback receipts.
- No heuristic leaf cores (project rule: GTO defaults, no parallel
  heuristic strategy).
