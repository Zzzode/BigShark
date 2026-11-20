# L1 Cap-7 Discriminator — Pre-Registered Judgment Rules

Status: pre-registered BEFORE the cap-7 run (timestamp in the runner log)
Date: 2026-11-20
Scope: single bench probe, no engine change, no long campaign. Decides
whether uniform cap widening can cheaply close the L1 structural term or
whether the selective-abstraction RFC (fourth path in
[cap6-serving-options.md](../../../decisions/cap6-serving-options.md)) is
required.

## Run configuration (frozen before launch)

`bigshark-rfc0047-ledger-embed-probe limped 1 3000 18 7 uniform`
— L1 ledger (pot 8, 4/4), 3,000 iterations, 18 threads (same thread
configuration as the D-390 20k cap-6 campaign), Uniform arm, outer guard
2,400 s (40 min), 1 GiB RSS hard line. `/usr/bin/time` is absent on this
host; wall and RSS come from the probe's own steady_clock and
getrusage(ru_maxrss), the same kernel sources GNU time reads.

## Same-iteration baseline (the fair comparison)

L1 cap-6 @3,000, receipt `l1-cap6-3000.csv`:
bounded NC 0.05480831, pure_menu 0.00939246, e_embed 0.06420077,
wall 1,286.353 s at 28 threads, peak RSS 335,112 KB.

Unit note: the orchestration brief cites "cap6 structural 0.4777", which
is the cap-4 L1 *bucket* contribution in BB/100. The comparable cap-6
structural cell at equal 3,000 iterations is the two-seat pure_menu
0.00939246 above; all rules use same-iteration two-seat values.

## Two pre-registered rules (decided before any cap-7 number exists)

Let r = pure_menu(cap7,3k) / pure_menu(cap6,3k) = measured / 0.00939246.

- **Rule A — uniform widening cheaply closes L1 (no abstraction RFC
  first):** the aggressive-line 4-5x per-step shrink observed on L3
  (cap4 -> cap5 4.99x, cap5 -> cap6 4.14x at 3k) repeats, i.e.
  pure_menu(cap7) <= 0.00939246 / 4 = **0.00234812** (r <= 0.25).
- **Rule B — plateau, selective-abstraction RFC is essential:** the
  reduction is no better than the w1 D164 cap5 -> cap6 decay (factor
  0.84, a 16% reduction), i.e. pure_menu(cap7) >= 0.00939246 x 0.84 =
  **0.00788967** (r >= 0.84). The 172.2-day full-vs-narrow-b price
  difference is then structural, not an artifact of uniform stacking.
- **Middle zone** (0.00234812 < pure_menu < 0.00788967): neither rule
  is confirmed; reported as inconclusive with the measured factor, not
  forced into a verdict.

Primary metric is pure_menu (the structural term); e_embed is secondary
because at 3,000 iterations the bounded NC is not converged and dominates
it on the smaller-tree cells.

## Marginal cost/quality ruler (same units as the full-vs-narrow-b trade)

Reference trade from the options table: 0.4689 BB/100 end-to-end per
413,185 campaign machine-hours (172.2 days on 100 slots), which is the
L1 cap-4 -> cap-6 swap at 20k. The cap-7 marginal is computed as
L1 cap-6 -> cap-7 at equal 3,000 iterations (measured walls), with the
20k campaign equivalent shown only as an explicitly labeled linear
projection (20k/3k wall scaling, the convention the D-390 memo used and
the measured 20k receipts later superseded).

## Rerun authorization and second-timeout contingency (added before rerun)

The first run (2,400 s guard) returned RC=124 with no data row. A single
rerun is authorized with the identical configuration and a 4,200 s outer
guard; the A/B thresholds above are unchanged and are not relaxed for the
rerun. `/usr/bin/time -v` remains absent on this host, so peak RSS is
again taken from the probe's own getrusage(ru_maxrss), the same kernel
counter GNU time reports.

If the rerun ALSO returns RC=124 (or any non-zero code without a data
row), no third long run is launched. The replacement instrument is a
paired short-run design, pre-specified now:

- Two same-configuration probes back to back: L1 cap-6 and L1 cap-7, each
  at 1,500 iterations, 18 threads, Uniform arm, with a 2,400 s per-run
  guard. The cap-6 member is a same-day measured baseline, not a historical
  comparison.
- Two ratios are reported: pure_menu(cap7,1500) / pure_menu(cap6,1500)
  for the structural direction, and mean wall per iteration cap7/cap6 for
  the cost premium. The A/B thresholds still apply to the pure_menu ratio
  (the rule is iteration-matched, so the factor transfers), with the
  caveat below.
- Comparability boundary: 1,500-iteration policies are substantially less
  converged than 3,000 (the sealed L3 trajectory shows bounded NC roughly
  doubling between 3k and earlier rungs), so a 1,500-iteration A/B verdict
  is explicitly labeled a provisional structural-direction read, not a
  replacement for a 3,000-iteration confirmation. Any cap-7 cap-0-only
  subtree that only becomes BR-walk reachable after more traversals is
  under-exposed at 1,500; if the short-run ratio lands in the middle zone,
  the question stays open rather than being upgraded to a verdict.
- The 4,200 s rerun itself remains the primary evidence; the paired short
  runs are the contingency instrument, not a parallel track to be run
  preemptively.

## Rerun outcome (recorded after completion)

Authorized rerun under the 4,200 s guard finished RC=0 at 44.2 min with a
full data row (`l1-cap7-3000.csv`): pure_menu 0.00001349, bounded NC
0.05475901, e_embed 0.05477250, peak RSS 491.2 MB. Measured r =
0.00001349 / 0.00939246 = 0.001436, so **Rule A is confirmed** (r <= 0.25)
and Rule B and the middle zone are not entered. The 1,500-iteration
paired contingency was therefore not triggered. The 1e-8 exact-zero
predicate is not met (value is 1,349x above it); the result is a
near-zero structural term with the residual 99.975% convergence-bound.
