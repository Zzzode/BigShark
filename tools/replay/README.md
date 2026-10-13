# Offline replay regression

Status: Current

`node bin/replay.mjs` reads only local `sessions/*.jsonl`. It reports missing or
unusable historical input explicitly; zero decisions do not establish play
coverage. It never joins a room or submits an action.

`node bin/replay.mjs --synthetic` reuses the frozen River v0 golden fixtures as
a fixed engineering corpus. These inputs are synthetic, not historical hand
records. The output labels their origin and reports no historical coverage.
This checks legal action/amount behavior on the engine; the existing golden
and differential suites separately check exact strategy responses.

Snapshots require an actionable turn and private hero cards. Duplicate states
are scoped by room, hand, revision, and street. Malformed records, skipped
non-actionable records, and duplicates have separate counters. An illegal
action, missing/nonfinite/out-of-range aggressive amount, or unexpected
operational fallback makes the command exit nonzero.

The pure journal/property regression runs in `npm run test:node:compiled` and
the normal CI Node gate. It covers overlapping journals, cross-room identity,
malformed/waiting/no-card inputs, legal amount boundaries, and failure exit
codes. Genuine play history is still required before claiming historical
decision coverage or using these tests as poker-quality evidence.
