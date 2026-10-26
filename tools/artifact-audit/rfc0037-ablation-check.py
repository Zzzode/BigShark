#!/usr/bin/env python3
"""RFC 0037 ablation decision checker (AC2).

Aggregates the raw per-seed CSV rows emitted by
``bigshark-subgame-nash-conv --rfc0037-ablation`` and applies the
pre-registered total decision function from RFC 0037. There is no human
adjudication branch; the output is the frozen-table verdict per cell.

Two geometries establish different parts of the frozen policy, and the
checker keeps their evidence separate (RFC 0037 "Evidence split by
geometry"):

* preflop-profile cells (shallow caps) are proven feasible by the shipped
  Complete artifacts (TerminalDepth::Flop, all-in-at-flop frontier); the
  streets(1) resolver ablation is allowed to report INFEASIBLE for them
  without a fail-fixed-cell-surprise. They are listed in
  PREFLOP_PROVEN with their manifest evidence.
* resolver-geometry cells are the ablation rows themselves.

The promotion comparison for the sole promotable cell (3 seats standard,
cap 1 -> cap 2) requires, at the decisive 100,000 count, with reference
cap 0 FITS (non-proxy): the whole 95% t interval of cap 2 lies strictly
below cap 1's under BOTH fill arms, improvement clears the floor, and
the cap-2 tree FITS. A proxy reference (cap 0 INFEASIBLE) cannot promote.

Usage:
    python3 -I tools/artifact-audit/rfc0037-ablation-check.py <csv> [csv...]

Exit 0 when no cell is FAIL/fail-fixed-cell-surprise, 1 otherwise.
"""

import csv
import math
import statistics
import sys

DECISIVE_ITERATIONS = 100_000
MIN_EFFECT_FLOOR_BB = 0.01

# Resolver-geometry cells that must be FITS at their declared reference.
RESOLVER_FIXED = {
    (2, 100, 0),
}
# Six-seat cap 0 resolver reference is an EXPECTED infeasibility.
EXPECTED_INFEASIBLE = {(6, 100, 0)}
# Cells whose cap is proven by a shipped Complete preflop artifact
# (Flop-terminal geometry); resolver INFEASIBLE here is geometric, not a
# surprise, and must never drive a fixed-cell failure. No shallow-band
# artifact ships any more: the 3-seat 10 BB cap-0 profile was retired
# 2026-10-26 (100 BB is the operating buy-in) and a 2-seat 10 BB profile
# never shipped in this set — the 2-live shallow resolver cell is proven
# directly by the Stage-0a spike instead.
PREFLOP_PROVEN = {
    (2, 100, 0),   # artifacts/preflop-profile
    (3, 100, 1),   # artifacts/preflop-profile-3way-100bb
    (4, 100, 1),   # artifacts/preflop-profile-4way
    (5, 100, 1),   # artifacts/preflop-profile-5way
    (6, 100, 1),   # artifacts/preflop-profile-6way
}
PROMOTABLE = (3, 100)  # standard three-seat, 1 -> 2 only


def t95_critical(n: int) -> float:
    table = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571}
    return table.get(n, 1.96)


def load(path: str):
    cells = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row or not row[0].startswith("rfc0037-ablation"):
                continue
            (_, seats, stack, cand, ref, iters, seed, verdict) = row[:8]
            key = (int(seats), int(stack), int(cand), int(ref), int(iters))
            entry: dict = {"verdict": verdict,
                           "inmodel": None, "passive": None, "uniform": None}
            if verdict == "FITS" and len(row) >= 14:
                # FITS,...,cnodes,rnodes,cinfos,inmodel,gap_p,gap_u,notes
                entry["inmodel"] = float(row[11])
                entry["passive"] = float(row[12])
                entry["uniform"] = float(row[13])
            cells.setdefault(key, []).append(entry)
    return cells


def interval(values):
    n = len(values)
    mean = statistics.fmean(values)
    sem = statistics.stdev(values) / math.sqrt(n) if n > 1 else 0.0
    h = t95_critical(n) * sem
    return mean, mean - h, mean + h


def worse_arm_interval(group):
    p = interval([r["passive"] for r in group if r["passive"] is not None])
    u = interval([r["uniform"] for r in group if r["uniform"] is not None])
    return p if p[2] >= u[2] else u


def all_infeasible(cells, seats, stack, cap, iters=None):
    keys = [k for k in cells if k[0] == seats and k[1] == stack and k[2] == cap
            and (iters is None or k[4] == iters)]
    if not keys:
        return None
    return all(any(r["verdict"] == "INFEASIBLE" for r in cells[k]) for k in keys)


def main(paths):
    cells = {}
    for path in paths:
        for key, rows in load(path).items():
            cells.setdefault(key, []).extend(rows)

    failures = []
    print("RFC 0037 ablation decision check (decisive count = %d)" % DECISIVE_ITERATIONS)

    # Resolver-geometry fixed feasibility (only cells that MUST build there).
    for seats, stack, cap in sorted(RESOLVER_FIXED):
        inf = all_infeasible(cells, seats, stack, cap, DECISIVE_ITERATIONS)
        if inf is None:
            print(f"  [MISSING] resolver fixed {seats}p {stack}BB cap{cap}")
            failures.append("missing resolver fixed")
        elif inf and (seats, stack, cap) not in EXPECTED_INFEASIBLE:
            print(f"  [FAIL-FIXED-SURPRISE] {seats}p {stack}BB cap{cap}")
            failures.append("resolver surprise")
        elif inf:
            print(f"  [expected-infeasible] {seats}p {stack}BB cap{cap}")
        else:
            print(f"  [resolver-fixed-ok] {seats}p {stack}BB cap{cap}")

    # Preflop-artifact proven caps: report the geometry split explicitly.
    for seats, stack, cap in sorted(PREFLOP_PROVEN):
        inf = all_infeasible(cells, seats, stack, cap)
        note = "resolver geometry INFEASIBLE (preflop artifact proves cap)" if inf else \
               "feasible in both geometries"
        print(f"  [preflop-proven cap{cap}] {seats}p {stack}BB: {note}")

    # The single promotable cell, with explicit proxy detection.
    seats, stack = PROMOTABLE
    base = [k for k in cells if k[0] == seats and k[1] == stack and k[2] == 1
            and k[4] == DECISIVE_ITERATIONS and k[3] in (0, 2)]
    cand = [k for k in cells if k[0] == seats and k[1] == stack and k[2] == 2
            and k[4] == DECISIVE_ITERATIONS]
    if not base or not cand:
        print(f"  [PENDING] {seats}p {stack}BB: decisive rows not present yet")
    else:
        cap0_inf = all_infeasible(cells, seats, stack, 0, DECISIVE_ITERATIONS)
        proxy = cap0_inf is not False  # None = never attempted; True = infeasible
        # Prefer a direct cap-0 comparison when one FITS, else cap-2 proxy.
        direct_base = [k for k in base if k[3] == 0 and
                       all(r["verdict"] == "FITS" for r in cells[k])]
        chosen_base = direct_base[0] if direct_base else sorted(base)[-1]
        bm, blo, bhi = worse_arm_interval(cells[chosen_base])
        cm, clo, chi = worse_arm_interval(cells[sorted(cand)[-1]])
        minimum = max(MIN_EFFECT_FLOOR_BB, abs(bm) / 3.0)
        improvement = bm - cm
        strict = chi < blo
        promote = strict and improvement >= minimum and not proxy
        ref = chosen_base[3]
        print(f"  [{'PROMOTE' if promote else 'KEEP'}] {seats}p {stack}BB cap1->cap2 "
              f"(ref cap {ref}, proxy={proxy}): cap1 [{blo:.3f},{bhi:.3f}] "
              f"cap2 [{clo:.3f},{chi:.3f}] improvement={improvement:.3f} "
              f"minimum={minimum:.3f}")
        # A PROMOTE here would require a non-proxy reference; the checker only
        # prints it. The frozen table only changes on an explicit table edit,
        # never silently from this diagnostic proxy comparison.

    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        return 1
    print("no fail-fixed-cell-surprise; table frozen by mechanical rule")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
