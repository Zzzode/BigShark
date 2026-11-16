# RFC 0050 Stage 1 — D307 Resume Checkpoint Result

Status: Current. Bench-only resumable checkpoint for the seedless
exact-menu river kernel, built so a multi-hour solve becomes
interruptible segments without changing the science. Receipts:
`stage1-resume-cost.csv` (real anchor write/load cost) and the native
`test_rfc0050_resume` suite (bitwise neutrality and fail-close
validation).

## Hard requirement 1 — numeric neutrality (acceptance gate)

Verified at the ten-iteration scale, no long solve. A split run
(capture after 5-6 iterations, write, reload, resume to the total) is
compared against an uninterrupted solve of the same total iteration
count:

- `resume_neutral_bitwise=1` on the real srp (32,16,16) anchor: the
  resumed and uninterrupted NashConv are exactly equal
  (4.03377467 BB at 12 iterations) and the average-strategy hashes
  match, with no tolerance — bitwise, not approximate.
- The native shallow-fixture test additionally requires exact equality
  of NashConv, average-strategy hash, reach-bound bounded NashConv,
  reach-bound exact exploitability, and a zero pure-menu term across
  the split and uninterrupted paths.
- Any single-bit mismatch that is not explainable floating-point
  non-determinism is a stop-and-report condition by directive; none
  occurred because the kernel is seedless and the reloaded tables are
  the exact prior state (the iteration counter resumes at t+1 with the
  same linear-average and RM+ operations).

## Hard requirement 2 — long-lived, fail-close format

The binary format (`river_state_codec`) carries, before any table byte:

- magic `RFC050CP` and a format version field (currently 1);
- full GameDef identity (players, button, blinds, stacks,
  contributions, pot, five-card board, board size, terminal depth,
  preflop flag);
- the action-abstraction digest;
- tree size and action-node counts;
- the completed iteration counter;
- the frozen nine-rung ladder and the 0.002 BB gate value baked in;
- the solver-mode flags (RM+, linear averaging, DCFR);
- the declared payload length and an FNV-1a 64-bit checksum over the
  header prefix and the entire payload.

Loading validates every field and rebuilds the cap-0 tree to cross-
check each action node's exact slot width (1,081 combos x menu
actions). Any mismatch — bad magic, wrong version, GameDef or digest
mismatch, ladder or gate mismatch, wrong solver mode, truncated file,
tampered payload byte, trailing bytes, or checksum failure — throws a
specific runtime error and imports nothing; there is no best-effort
recovery. The native test exercises tampered payload, bad magic,
truncation, wrong digest, and wrong GameDef independently.

## Hard requirement 3 — measured write cost

Receipt on the real rank-0 anchor (83 MB checkpoint, six-iteration
capture):

| cost | measured | fraction of one ~6 s iteration |
| --- | --- | --- |
| in-memory serialize (no I/O) | 141.6 ms | 2.36% |
| disk write | 243.5 ms | 4.06% |
| load + validate | 438.8 ms | — |

The first implementation measured 329.6 ms (5.49%, over the 5% gate);
it was optimized before proceeding, per the directive. The change
replaced per-row string appends and two write syscalls with one
pre-sized contiguous buffer and a single file write, plus a pointer-
view reader. The optimized disk write is 4.06%, inside the 5% gate
(`within_write_gate=1`). Load is more expensive than write because it
rebuilds and validates the cap-0 tree, but it happens once per resume,
not per iteration, so it does not enter the per-iteration cost gate.

## Scope

All artifacts are bench-only under `bigshark_rfc0047_bench`; no
production solver, persisted product format, schema, codec, mount,
service, threshold, or frozen constant changes. The checkpoint format
is a benchmark artifact, not a new on-disk product contract. The 9-rung
ladder, 1,800 s wall, and 0.002 BB gate are unchanged.
