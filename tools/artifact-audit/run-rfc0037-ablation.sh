#!/usr/bin/env bash
# RFC 0037 pre-registered ablation matrix (manual offline measurement).
#
# Appends rows to docs/plans/ablation/rfc0037-matrix.csv. Turn-rooted
# streets(1), PRODUCTION identity menu. Two iteration counts, three seeds.
#
# Measured reference-cap feasibility (production menu, streets(1), 100 BB):
#   cap-0 reference tree: FITS only at 2 seats standard and 3 seats shallow;
#   at 3/4/5/6 seats standard it exceeds the 1 GiB builder byte cap (the T3
#   small numbers were declared-only coarse-menu trees and do not transfer).
# Consequently every 100 BB cell above 2 seats is a PROXY reference (richest
# feasible cap), and the RFC 0037 total function makes none of them
# promotable; the 100K decisive-count runs are the frozen-table evidence.
# Six-seat exact pooled BR evaluation is combinatorially intractable
# (>173 min wall at 1,000 iterations); it is recorded as an eval-time
# INFEASIBLE proxy and needs the sampled estimator for any number, never a
# promotion.
#
# Re-runnable; dedupe before checking. Local compute only; no live play.
set -u
BIN=./build/release/engine/bigshark-subgame-nash-conv
OUT=docs/plans/ablation/rfc0037-matrix.csv
SEEDS=(101 202 303)
ITERS=(1000 100000)

# args: seats stack cand ref
run_cell() {
  local seats=$1 stack=$2 cand=$3 ref=$4
  for it in "${ITERS[@]}"; do
    for seed in "${SEEDS[@]}"; do
      echo "  ${seats}p ${stack}BB cand=$cand ref=$ref it=$it seed=$seed" >&2
      "$BIN" --rfc0037-ablation "$seats" "$stack" "$cand" "$ref" "$it" "$seed"
    done
  done
}

{
  # 2 seats standard: direct cap-0 reference; candidates 0/1/2.
  for cand in 0 1 2; do run_cell 2 100 "$cand" 0; done
  # 3 seats shallow: direct cap-0 reference; candidates 0/1/2.
  for cand in 0 1 2; do run_cell 3 10 "$cand" 0; done
  # 3/4/5 seats standard: proxy reference at richest feasible cap (2);
  # candidates 1/2 (cap 0 itself does not build). Diagnostic for 4/5.
  for seats in 3 4 5; do
    for cand in 1 2; do run_cell "$seats" 100 "$cand" 2; done
  done
} >> "$OUT"

echo "wrote $OUT ($(wc -l < "$OUT") rows)"
