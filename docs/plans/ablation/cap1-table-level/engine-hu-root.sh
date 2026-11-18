#!/bin/sh
# D-346 campaign helper: launch the published engine with the HU 100BB cap-0
# preflop profile mounted as a resident root and a resident budget large
# enough to hold it. The practice CLI has no --resident-budget flag, so its
# --engine-path points here. All arguments are forwarded verbatim.
# The script lives four directories below the repo root
# (docs/plans/ablation/cap1-table-level).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
exec "$ROOT/bin/bigshark-engine" \
  --resident-budget 4096 \
  --resident-root "$ROOT/artifacts/preflop-profile/preflop-profile.db=85c1e71d0ad8e8738f105da4753de9c8d8400c4d3bb85990103b3956cb4cef27" \
  "$@"
