# D-346 Cap-1 Table-Level Expectation Campaign

Read-only measurement campaign backing
`docs/decisions/cap1-table-level-expectation.md`. No engine code, cap
configuration, or training artifact is touched; every run drives the existing
`bigshark-practice` CLI against the existing `bigshark-engine` binary.

## Layout

- `drive-practice.mjs` — transcript driver with five scripted seat-0
  policies (station, folder, passive-random, uniform, srp-passive).
- `parse-hands.mjs` — transcript + event sidecar to per-hand rows and
  reach-frequency summaries.
- `table-expectation.mjs` — combines sealed per-occurrence residuals with
  measured frequencies into BB/100 expectations; `--pool=name=a,b` pools
  binomial counts across runs.
- `extract-meta.mjs` — served/fallback accounting and wall times per
  segment.
- `engine-hu-root.sh` — launches the published engine with the local HU
  100BB cap-0 preflop profile mounted as a pinned resident root (4 GiB
  resident budget; the 256 MiB default rejects the 413 MB footprint).
- `run-regen.sh`, `run-replication.sh` — same-seed regeneration (seeds
  401-405) and the independent replication campaign (seeds 501-505).
- `mounted-{rows,summary,expectation}.csv` — parsed evidence for the
  seed-401 campaign retained transcripts.
- `r2-{rows,summary,expectation}.csv` — the independent seed-501 campaign.
- `both-{summary,expectation}.csv` — both campaigns concatenated, including
  the primary 2,400-hand pooled-passive estimate (`pooled-passive-2400`).
- `campaign1-meta.csv`, `campaign2-meta.csv` — per-segment served/fallback
  accounting and wall times, extracted from the transcripts and ledgers.
- `fb-{rows,summary,expectation}.csv` — unmounted mechanical control
  corpus (1,000 hands per passive style, no resident root).
- `campaign.timing`, `regen.timing`, `run2.timing` — wall-time ledgers.
- `hu-*.log(.events.log)` and `r2-*.log(.events.log)` — raw transcripts and
  machine-readable action sidecars. Git-ignored (`*.log`), regenerable with
  the recorded seeds; transcript-level variation across same-seed re-runs is
  expected because the online resolver stops on a wall-clock deadline (see
  section 2.5 of the decision doc).

## Reproduction

```sh
# One segment (mounted engine, station style, 400 hands).
ENGINE_PATH=docs/plans/ablation/cap1-table-level/engine-hu-root.sh \
  node docs/plans/ablation/cap1-table-level/drive-practice.mjs \
  station 400 401 300 /tmp/hu-station.log --engine-timeout-ms 120000

# Parse and score (example labels/paths).
node docs/plans/ablation/cap1-table-level/parse-hands.mjs \
  station=/tmp/hu-station.log --rows /tmp/rows.csv --summary /tmp/summary.csv
node docs/plans/ablation/cap1-table-level/table-expectation.mjs \
  /tmp/summary.csv /tmp/expectation.csv
```

The unmounted control omits `ENGINE_PATH` (the default engine path serves
with zero resident roots, so every decision is the labeled operational
fallback).
