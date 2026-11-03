# RFC 0045 G2 run 1 — INVALIDATED evidence (retained for audit only)

Status: Historical. These files are the output of the first G2 campaign
(2026-11-02, 368/368 cells, 44 work-stealing workers, ~202 core-hours).
The independent Stage-4 evidence review returned CHANGES REQUESTED: the
campaign driver treated `NSeatTrainingResult::completed_iterations`
(cumulative absolute) as segment-incremental and accumulated it with +=,
double-counting every warm-started segment and jumping the resumed PRNG
streams across untrained sample windows. Every 300M-rung cell therefore
received only ~128-256M real updates; 64/72 full walks also breached the
frozen 50.996 s +/-20% re-run band. No instrument verdict may be recorded
from these rows; they are retained only to audit the review findings.

Run 2 supersedes these files after the driver fix (guarded cumulative
assignment, wall-bounded segmented-resume regression in test_warm_start,
mechanical walk re-run with walkslow fail-closed) under a single fixed
binary and provenance. See `docs/plans/0045-implementation.md`, Stage 4.

Files:

- `rfc0045-g2-campaign.csv` — 368 run-1 rows (the `completed` column
  shows 528-595M on nominally-300M cells, direct evidence of the defect).
- `rfc0045-g2-full-summary.csv`, `rfc0045-g2-w1-summary.csv` — evaluator
  aggregates of the invalid rows.
- `rfc0045-g2-verdict.json` — evaluator output `needs-conditional-kt128`
  on the invalid rows; not a verdict of record.
- `rfc0045-g2-logs/` — worker logs with the wall receipts the review
  audited.
