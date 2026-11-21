# GTO Mainline — 10-Seat / 100 BB Milestone Ledger

Status: Current
Owner: implementation (autonomous between milestones)
Tracking contract (set 2026-10-31): the ONLY reported unit is a
milestone. Technical detail — preflop vs postflop build order, card/
action abstraction choices, run counts, convergence instruments,
depths, failed experiments, retries — is internal and is not reported
mid-milestone. A milestone is reported once, at completion, with its
acceptance metrics against the design expectations below.

## Mainline target

Complete equilibrium (GTO) opponent support for every table size
1..10 at 100 BB effective depth in the offline practice product:
every bot decision on every in-scope shape is served from a verified
offline equilibrium artifact; no operational fallback on covered
shapes; measured exploitability inside the frozen bar.

## Milestones

| Milestone | Scope | Status | Completed |
| --- | --- | --- | --- |
| M1 | 1 seat | N/A by game definition (a one-seat table has no opponent; the CLI already rejects `seats < 2`) | 2026-10-31 (vacuous) |
| M2 | 2 seats / 100 BB full GTO | Not started (preflop artifact exists; postflop blueprint missing) | — |
| M3 | 3 seats / 100 BB full GTO | Not started (preflop exists; one six-way flop root does not cover 3-way) | — |
| M4 | 4 seats / 100 BB full GTO | Not started (preflop exists; postflop missing) | — |
| M5 | 5 seats / 100 BB full GTO | Not started (preflop exists; postflop missing) | — |
| M6 | 6 seats / 100 BB full GTO | Not started (preflop + exactly one flop root — six-way limped — exist; all other flop roots, turn, river missing) | — |
| M7 | 7 seats / 100 BB full GTO | Not started | — |
| M8 | 8 seats / 100 BB full GTO | Not started | — |
| M9 | 9 seats / 100 BB full GTO | Not started | — |
| M10 | 10 seats / 100 BB full GTO | Not started | — |

Note on ordering: existing preflop artifacts (2..6 seats) and the
six-way limped flop library are INTERNAL PROGRESS, not completed
milestones. A milestone closes only when its acceptance bar is met
across all streets. Milestones may be completed in seat-cost order
(likely M2 first, M10 last) regardless of which seat's preflop asset
happened to be trained first.

## Acceptance bar per milestone (design expectations)

Milestone M_n closes only when ALL of the following are measured and
recorded:

1. **Full-street offline coverage at n seats / 100 BB.** Verified
   artifacts cover the complete preflop profile and EVERY reachable
   action-root shape on flop/turn/river (enumerated from the action
   abstraction), mounted with digest verification. No shape served
   from the real-time approximation or a check/call/fold receipt.
2. **Exploitability within the frozen bar at every street.**
   Within-abstraction best-response gap (and exact gap where
   enumerable) at or below the BB threshold frozen by the convergence
   certification RFC; five-seat paired half-widths inside the frozen
   noise cutoff. The certification instrument (currently blocked —
   see dependency D1) is itself a prerequisite, not a per-milestone
   negotiation.
3. **Serving correctness.** The practice table serves the artifacts on
   every in-scope shape; zero fallback receipts over a fixed seeded
   regression run; provenance/digest checks pass; the 10-second p95
   serve envelope holds for artifact LOOKUPS (training is offline and
   has no serve wall).
4. **Engineering gates.** Full release + asan ctest green,
   `npm run check`, proto/RFC/docs checkers, artifact pack
   verification, and the published builder/pack/wiring contract
   unchanged or amended by an approved RFC.

Non-100-BB depths (25/50/200 BB, etc.) are OUT of this mainline; they
are separate post-M10 work and must not dilute milestone scope.

## Internal dependencies (not reporting units)

- D1 (BLOCKING postflop): a reachable convergence-certification
  instrument. RFC 0043 showed the prior 20%-relative-two-rung plateau
  is unreachable under RM+ (~T^-0.55 decay) for every abstraction.
  Next instrument is calibrated first on the incumbent nine-tier grid,
  with an extended ladder; requires a new RFC before mass training.
- D2: complete flop-root enumeration + per-root class libraries at
  n seats (the existing six-way library covers only the limped root).
- D3: turn (16,432 classes) and river (134,459 classes) blueprint
  format/build/mount flip and the 200 GB / 7-day campaign-cost
  measurement (RFC 0042's eight-site flip, RFC 0043's unmeasured
  AC6/AC7).
- D4: 7..10-seat preflop profiles (builder already parameterized;
  compute-only, no design dependency).

## Completion record

(Filled per milestone at completion only — preflop/postflop partial
progress does not appear here.)
