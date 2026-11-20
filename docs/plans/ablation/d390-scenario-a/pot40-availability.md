# Pot-40 Final Hull — Ledger Availability and Residual Risk

Status: pre-registered judgment plus a structural availability finding
Date: 2026-11-21
Scope: closing the last unmeasured hull (37 practice hands at river
pot 40) named in
[cap6-serving-options.md](../../../decisions/cap6-serving-options.md). One
bounded local analysis; no engine source, cap configuration, or prior
receipt changes; no run is launched because no target ledger exists.

## 1. Pre-registered classification framework (written before any run)

The family-specific convergence premise established by the pot-32 work
is the only applicable rule, since no pot-40 prediction point exists:

- SRP-family measured 3k -> 20k NashConv improvement is 37.77x; the
  limped family spans 6.39-15.17x. A new decision-bearing ledger whose
  3k e_embed, after applying either factor, lands inside the range of
  the already-measured 20k cells (0.00323-0.01321) is classified as the
  same convergence regime.
- A 3k reading that stays above 0.05 BB after the 6.39x conservative
  factor (i.e. > ~0.0078 at projected 20k, outside the full measured
  20k envelope) would be classified as a new independent convergence
  family and its hands re-labeled unmeasured / possibly worse.
- Thresholds are not editable after a run; the rule is moot if no
  runnable ledger exists.

## 2. Availability finding: no pot-40 ledger exists

The sealed eleven-ledger census
(`../rfc0050/stage0a-eleven-ledger-cap0-census.csv`) contains no pot-40
entry. The only pot-40-class rows are the two pot **400** terminal
all-in ledgers (limped index 4, srp index 5), each with one node, zero
action nodes, and zero projected infosets: structurally no menu, no
decision, nothing a cap-6 probe can score. The ledger-embed probe can
only address `family/family_index` pairs from that enumeration, so it
cannot construct an arbitrary pot-40 GameDef. Per the task instruction,
the pot-32 measured value is NOT substituted.

The 37 practice hands at river pot 40 are nevertheless live decisions:
all 37 carry `all_in = 0` (32 showdown, 5 fold), so the structural-zero
all-in treatment used for pot-400 runouts does not apply to them. They
are a genuine enumeration-coverage gap: pots that occur in play but have
no frozen decision-bearing ledger in the cap-1 frontier.

## 3. Residual risk without a measurement (bounded, not hidden)

Frequency is exact: 37 of 2,400 dealt hands (1.542%), or 5.35% of the
692-hand other bucket. In the corrected remap these 37 are part of the
127 hands nearest pot 32, so the 0.3292 headline currently charges them
at the measured pot-32 20k severity — a proxy contribution of only
0.00249 BB/100. Conservative upper bounds using the worst measured cap-6
single point (labeled proxies, not measurements, and deliberately not a
pot-32 substitution for the conclusion):

| Treatment of the 37 hands | Added BB/100 | Resulting end-to-end | 0.5 / 1.0 |
| --- | --- | --- | --- |
| Excluded (655 hands fully measured) | 0 | 0.3267 | pass / pass |
| Current pot-32 20k proxy (the 0.3292 headline) | 0.00249 | 0.3292 | pass / pass |
| As bad as the worst measured 20k cell (0.01321) | 0.0102 | 0.3369 | pass / pass |
| As bad as the worst measured 3k cell (0.12083) | 0.0931 | 0.4198 | pass / pass |

No scenario constructed from measured severities breaks either rail;
the worst plausible labeled bound (0.4198) keeps 0.0802 margin to the
0.5 target. A true structural ceiling cannot be derived without the
missing ledger, which is stated rather than papered over.

## 4. Classification and headline status

- Rule outcome: **not runnable — no ledger**, so neither "same regime"
  nor "new family" is empirically confirmable. The honest label for the
  37 hands is `unmeasured, non-terminal, bounded by proxy`, not
  extrapolation-closed.
- The 0.3292 BB/100 figure is re-scoped precisely: fully measured over
  655 of 692 other hands (94.65%); the remaining 5.35% rests on a
  disclosed nearest-ledger proxy whose worst-case measured bound still
  passes both rails. It is NOT claimed as an all-ledger measurement.
- Closing the gap for real requires extending the frontier enumeration
  with a pot-40 decision-bearing ledger, an engine/harness change
  outside this task's no-engine boundary; it is recorded as the single
  remaining instrument gap, not a quality failure.
