# Benchmarking

Status: Current

BigShark keeps solver quality benchmarks separate from microbenchmarks. A
quality benchmark measures exploitability and convergence against deterministic
fixtures. Runtime is recorded for capacity planning, but it is not a hard gate
because it varies across machines.

## Multi-Street CFR Benchmark

The solver-owned benchmark source lives under `engine/benchmarks/`.
The `bigshark-multistreet-benchmark` executable runs three fixture families:

| Case | Seed | Checkpoints | Final exploitability limit |
| --- | ---: | --- | ---: |
| `fixed-balanced` | 1 | 1,000; 5,000; 20,000; 100,000 | 0.002 pot |
| `fixed-weighted-overlap` | 1 | 1,000; 5,000; 20,000; 100,000 | 0.002 pot |
| `sampled-chance-dominant` | 17 | 1,000; 5,000; 20,000 | 0.02 pot |

The fixed fixtures use exact private combinations and deterministic turn and
river cards. The weighted fixture includes an overlapping-card pair that must
be excluded from joint reach. The sampled-chance fixture trains over public
turn and river samples while exploitability enumerates the chance tree.

Each case must satisfy all of these gates:

- every checkpoint returns finite value and exploitability metrics;
- final exploitability does not exceed the case limit;
- final exploitability does not exceed the first checkpoint;
- an independent repeat of the final checkpoint matches value,
  exploitability, and information-set count within `1e-12`.

## Running

Build and run the benchmark through CMake:

```bash
cmake --preset release
cmake --build --preset release --target benchmark-multistreet
```

Run only its CTest gate:

```bash
ctest --preset release -R '^benchmark_multistreet$' --output-on-failure
```

Run every registered benchmark:

```bash
ctest --preset release -L benchmark --output-on-failure
```

The CTest benchmark gate is registered for Release presets, including the
Linux release lane. Debug and sanitizer builds can run the explicit target,
but do not include benchmark timing in their default CTest suite.

The executable can be invoked directly when its CSV output needs to be saved:

```bash
./build/release/engine/bigshark-multistreet-benchmark \
  > build/release/multistreet-cfr-benchmark.csv
```

## Output Contract

The runner writes CSV with `schema_version = 1` and these fields:

| Field | Meaning |
| --- | --- |
| `case` | Stable fixture identifier |
| `seed` | Deterministic solver seed |
| `iterations` | CFR iterations for the checkpoint |
| `ip_combos`, `oop_combos` | Private combinations retained by the fixture |
| `information_sets` | Total trained information sets |
| `value_to_ip` | Equilibrium-policy value from the IP perspective |
| `exploitability_pot` | Sum of both deviation gains divided by starting pot |
| `max_final_exploitability` | Quality gate for the final checkpoint |
| `elapsed_ms` | Wall-clock duration, recorded but not gated |
| `repeat_delta` | Maximum deterministic metric difference on the final repeat |
| `status` | `MEASURED`, `PASS`, or `FAIL` |

Changes to fixture semantics, exploitability definitions, output fields, or
thresholds must update this document and the benchmark schema version when
compatibility is affected.

## Heads-Up Blueprint Benchmarks

The RFC 0004 heads-up trainer has its own versioned benchmark so the legacy
multi-street CSV and its values are never reinterpreted. Two runners share one
schema-version-2 harness:

- `benchmark-heads-up-blueprint`, built from
  `engine/benchmarks/heads_up_blueprint_benchmark.cpp`, is the registered
  Release-only `benchmark_heads_up_blueprint` CTest gate with a 900-second
  timeout (measured local run about 141 seconds; the timeout is CI headroom).
- `benchmark-heads-up-capacity`, built from
  `engine/benchmarks/heads_up_capacity_benchmark.cpp`, is the manual
  release-scale runner. It is deliberately not registered as a CTest because
  it spends several minutes at the pinned 1,000,000-iteration checkpoints; run
  it explicitly with
  `cmake --build --preset release --target benchmark-heads-up-capacity` when
  recording capacity evidence.

| Case | Traversal | Seeds | Default CTest checkpoint | Capacity checkpoint | Limit |
| --- | --- | ---: | ---: | ---: | ---: |
| `heads-up-fixed-full` | Full | 1 | 8,192 | - | 0.002 pot |
| `heads-up-fixed-sampled` | External sampling | 1; 17; 43 | 1,000,000 | 1,000,000 | 0.002 pot |
| `heads-up-free-river-sampled` | External sampling, free river | 1; 17; 43 | 100,000 | 1,000,000 | 0.02 pot |

The fixed cases reserve Js and 9c; the sampled-chance case reserves only the
turn and samples the river. All cases use the same two-combo weighted ranges
and one-chip stack profile. The free-river default checkpoint is bounded at
100,000 iterations but passes the pinned `0.02` gate inside the one-million
compute budget; only the manual capacity runner spends that case at the full
1,000,000-iteration checkpoint. Each case requires finite metrics, the final
exploitability no worse than the first checkpoint, and a same-build repeat
matching every published policy row probability, exploitability,
information-set count, and PRNG state within `1e-12`. The fixed full-traversal
final value is `0.000821200905142`; sampled fixed final values are
seed-dependent but all below `0.001`, and the 100,000-iteration free-river
final values are below `0.012`. Run time is recorded per run in the
`elapsed_ms` column only; it is never gated and is not quoted here as a
portable number.

These runners write CSV with `schema_version = 2`:

| Field | Meaning |
| --- | --- |
| `case` | Stable heads-up fixture identifier |
| `seed` | SplitMix64 seed |
| `iterations` | Training iterations for the checkpoint |
| `information_sets` | Trained information sets |
| `exploitability_pot` | Normalized NashConv per root pot |
| `max_final_exploitability` | Quality gate for the final checkpoint |
| `elapsed_ms` | Wall-clock duration, recorded but not gated |
| `repeat_delta` | Maximum final-repeat difference, including every published row |
| `status` | `MEASURED`, `PASS`, or `FAIL` |

## Frozen Matrix Benchmark (schema 3)

The RFC 0004 Stage 4 frozen release suite is its own versioned runner,
`benchmark-heads-up-matrix`, built from
`engine/benchmarks/heads_up_matrix_benchmark.cpp`. It is deliberately not a
CTest and is not the schema-2 runner: the frozen matrix spends several minutes
on the larger games. Run it manually:

```bash
cmake --build --preset release --target benchmark-heads-up-matrix
./build/release/engine/bigshark-heads-up-matrix-benchmark \
  > build/release/heads-up-matrix.csv
```

Positional id-substring arguments select individual fixtures. Flags:

- `--freeze`: measurement mode for rebasing gates; it prints the raw final
  NashConv in `quality_gate` with `gate_basis = freeze-reference` and final
  `status = FREEZE`, and does not fail on the measured SPR 4/10 gates.
- `--sampled-coverage`: runs the external-sampling coverage comparison and
  prints a separate report (see below).
- Any other `--flag` is rejected with usage text and a nonzero exit.

On POSIX each matrix fixture runs in a forked child so the reported process
peak RSS is fixture-local.

This is a bounded frozen matrix, not full-game equilibrium coverage. Every
row covers exactly one declared root board, range pair, stack profile, action
schedule, and runout policy; nothing here generalizes to unrestricted
Hold'em.

### Fixture catalog

All twelve fixtures share one matched flop-root profile and one pair of
weighted ranges, and freeze the action schedule at the standard
`1/3, 3/4, 3/2` pot bets and `1/2, 1/1` pot raises plus the all-in target.

- Root: limped 10-chip pot (5 chips from each seat), big blind 5, button on
  seat 1 so seat 0 acts first on every postflop street. Root contributions
  sum to the pot; stacks below are the remaining postflop stacks, and SPR is
  `min(stacks) / pot`.
- Ranges (weights in parentheses): seat 0 `AcAd` (2), `8c8d` (3), `KcKd`
  (4); seat 1 `AcAs` (5), `TcTd` (7), `AhKh` (6). The `AcAd`/`AcAs` pair
  shares the ace of clubs, so one of the nine joint pairs is incompatible and
  is dropped, leaving eight weighted joint deals. The ordered set is hashed
  into the `range_digest` column (FNV-1a, hex token
  `9e1b8b78a66f91db`).
- Dry board is the genuinely rainbow, disconnected `Ks7h2c` (three suits,
  no two connected ranks). Card collisions between board, the fixed runout,
  and either range are rejected by construction: `HeadsUpTrainer` reserves
  every flop and runout card and throws on a duplicate, so a collision aborts
  the run rather than silently training an invalid game.
- Fixed runout: turn `3s`, river `5h` (reserved before dealing). One fixture
  fixes only the turn and leaves the river free. With these ranges and this
  fixed runout, no hole/board combination makes five-card flush or straight,
  so the rainbow dry board and the earlier two-spade dry board have identical
  terminal ordering; the rainbow board is kept so the suite actually contains
  an unpaired rainbow family.

| Fixture | Flop family | Board | Stacks | SPR | Runout | Checkpoints (full iterations) |
| --- | --- | --- | --- | ---: | --- | --- |
| `matrix3-spr1-dry-equal-fixed` | dry (rainbow) | Ks7h2c | 10/10 | 1 | fixed | 1,000; 12,000 |
| `matrix3-spr1-connected-equal-fixed` | two-tone connected | 9h8h4d | 10/10 | 1 | fixed | 1,000; 12,000 |
| `matrix3-spr1-paired-asym-fixed` | paired | QdQc6s | 10/20 | 1 | fixed | 1,000; 12,000 |
| `matrix3-spr1-monotone-equal-free` | monotone | JhTh9h | 10/10 | 1 | free river | 100; 500 |
| `matrix3-spr4-dry-equal-fixed` | dry (rainbow) | Ks7h2c | 40/40 | 4 | fixed | 10; 100 |
| `matrix3-spr4-connected-asym-fixed` | two-tone connected | 9h8h4d | 40/80 | 4 | fixed | 10; 100 |
| `matrix3-spr4-paired-equal-fixed` | paired | QdQc6s | 40/40 | 4 | fixed | 10; 100 |
| `matrix3-spr4-monotone-asym-fixed` | monotone | JhTh9h | 40/80 | 4 | fixed | 10; 100 |
| `matrix3-spr10-dry-asym-fixed` | dry (rainbow) | Ks7h2c | 100/200 | 10 | fixed | 1; 4 |
| `matrix3-spr10-connected-equal-fixed` | two-tone connected | 9h8h4d | 100/100 | 10 | fixed | 1; 4 |
| `matrix3-spr10-paired-asym-fixed` | paired | QdQc6s | 100/200 | 10 | fixed | 1; 4 |
| `matrix3-spr10-monotone-equal-fixed` | monotone | JhTh9h | 100/100 | 10 | fixed | 1; 4 |

The matrix covers all four pinned flop families, both equal and asymmetric
stacks, and SPR 1, 4, and 10.

### Traversal, exact versus estimated, and coverage

Every matrix fixture trains with FULL traversal, which enumerates every
branch and produces a policy complete for its declared game root. The
reported `exploitability_pot` is the exact information-set best-response
value from `HeadsUpTrainer::evaluate()` (full betting tree and, for the
free-river fixture, every public river card). `metric_class` is derived, not
hardcoded: it is `EXACT` only when traversal is full and evaluation returned
a finite value; a failed evaluation is `INCOMPLETE` (its exploitability is
`NaN`) and such a final row is never marked `PASS`. Schema 3 implements no
sampled deviation estimator, so it publishes no sampled `ESTIMATE`; this is
stated explicitly rather than labeling a sampled quantity as exact.

The choice of full traversal is itself measured and reproducible via
`--sampled-coverage` (archived counts below): external sampling leaves rare
information sets unvisited at the pinned budgets, and RFC 0004 makes missing
coverage a failure in quality fixtures.

Full traversal consumes no PRNG entropy, so full rows are seed-independent:
the dry SPR-1 fixture is still published under the pinned seeds 1, 17, and
43, and every other row is labeled seed 1. There are no held-out seeds in
this matrix.

### Sampled-coverage evidence (2026-09-15)

`./build/release/engine/bigshark-heads-up-matrix-benchmark --sampled-coverage`
trains the actual matrix SPR-1 fixed game with `train_sampled` for 3,000,000
iterations and the SPR-1 monotone free-river game for 100,000 iterations
(seed 1), then compares the visited information-set keys against the full
tree from one full traversal. Archived output (reference machine):

| Game | Runout | Sampled iterations | Visited info sets | Full info sets | Missing | Sampled elapsed ms | Coverage |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `matrix3-spr1-dry-equal-fixed` | fixed | 3,000,000 | 251 | 252 | 1 | 216,289.6 | incomplete |
| `matrix3-spr1-monotone-equal-free` | free river | 100,000 | 4,028 | 6,192 | 2,164 | 140,793.1 | incomplete |

Both reports use `range_digest 9e1b8b78a66f91db` and
`algorithm_revision rfc0004-rev1-full-kSimple-prng1`. The fixed game misses
one rare information set even after three million sampled iterations, and the
free-river game misses 2,164 of 6,192 sets after one hundred thousand. These
are the concrete reasons the matrix publishes complete full-traversal
policies rather than sampled ones.

### Gates and cross-platform rebase

- SPR 1 uses the pinned RFC 0004 gates: normalized NashConv at most `0.002`
  for the fixed fixtures and `0.02` for the free-river fixture. The frozen
  iteration budgets (12,000 fixed; 500 free) clear them with margin.
- SPR 4 and SPR 10 are new, larger games. They are not claimed to meet
  `0.002`. Each row requires finite metrics, complete coverage, and strict
  improvement over the first checkpoint, and carries a fixture-specific
  `measured-m5pro-2026-09-15` gate equal to the final normalized NashConv
  measured on the reference machine (Apple M5 Pro, 48 GB, macOS 26.5.1,
  Apple clang 21) and rounded up with margin. These measured gates are
  REFERENCE-MACHINE reproducibility guards, not portable equilibrium
  thresholds: a different compiler, standard library, SIMD path, or CPU can
  shift floating-point results, and an unconverged large-game value can move
  with the numeric backend. Linux verification of these targets remains an
  external gate that has not been run.
- Rebase procedure on a new platform:
  1. Run `... --freeze` and save the CSV; final rows are marked `FREEZE` with
     `gate_basis = freeze-reference` and the raw final NashConv in
     `quality_gate`.
  2. Inspect the deltas versus the published `exploitability_pot` values:
     tiny last-digit differences are ordinary floating-point/library drift;
     a materially worse value, a loss of strict first-checkpoint improvement,
     or any incomplete coverage is a real regression to investigate, not a
     reason to re-freeze.
  3. Round each accepted new final value up by roughly 5-9% and update the
     fixture's `measured_gate` constant and the `gate_basis` date, rebuild,
     and run the gated suite to confirm every row `PASS`.
- Every final checkpoint is independently repeated in the same build; the
  repeat must match the policy probabilities, exploitability,
  information-set count, and PRNG state within `1e-12`
  (`repeat_delta = 0` for all full rows).

### Output

The runner writes CSV with `matrix_version = 3`. Fields:

| Field | Meaning |
| --- | --- |
| `matrix_version` | Frozen matrix schema version (3) |
| `fixture_id` | Stable fixture identifier |
| `flop_family` | dry, two-tone-connected, paired, or monotone |
| `spr` | Effective stack-to-pot ratio at the flop root |
| `symmetric_stacks` | `equal` or `asymmetric` |
| `combos_oop`, `combos_ip` | Private combinations per side (3 each) |
| `traversal` | `full` or `sampled` (matrix rows are all `full`) |
| `runout` | `fixed` turn and river, or `free` river |
| `algorithm_revision` | Pinned algorithm token `rfc0004-rev1-full-kSimple-prng1` |
| `range_digest` | FNV-1a hex token of the ordered per-side range card ids and weights |
| `seed` | Seed label (full traversal is seed-independent) |
| `checkpoint`, `iterations` | Checkpoint ordinal and full iterations |
| `information_sets` | Information sets present in the trained policy |
| `missing_information_sets` | Full-tree sets absent from the policy; 0 for every complete row |
| `nodes` | TRAINING traversal nodes only; exact-evaluation node visits are excluded |
| `accounted_bytes` | Conservative solver allocation-accounting peak (not allocated memory) |
| `peak_rss_bytes` | Forked-child peak RSS in BYTES on every platform; Linux KiB are normalized with x1024; `-1` if `getrusage` is unavailable |
| `elapsed_ms` | Wall-clock duration of training plus exact evaluation for the checkpoint |
| `first_exploitability_pot` | First-checkpoint value the final row must beat; blank on checkpoint 1 |
| `metric_class` | `EXACT` for a finite full row; `INCOMPLETE` when evaluation failed |
| `exploitability_pot` | Exact normalized NashConv per root pot; `NaN` on failed evaluation |
| `quality_gate` | Applicable final-checkpoint gate; in `--freeze`, the raw final value |
| `gate_basis` | `pinned-rfc0004`, `measured-m5pro-2026-09-15`, or `freeze-reference` |
| `repeat_delta` | Maximum same-build final-repeat difference; blank off the final checkpoint |
| `coverage` | `complete` when every reachable set has a policy row, else `incomplete` |
| `held_out_seeds` | `none`; no held-out seeds in this matrix |
| `status` | `MEASURED`, `PASS`, `FAIL`, or `FREEZE` |


### Measured results (Apple M5 Pro, 48 GB, macOS 26.5.1)

The complete frozen matrix was measured on 2026-09-15 with Apple clang 21
under the release preset on Apple M5 Pro / 48 GB / macOS 26.5.1. Every final
row passed its gate with complete coverage (`missing_information_sets = 0`)
and `repeat_delta = 0`. Elapsed time and RSS are observations, not portable
gates. All rows carry `algorithm_revision rfc0004-rev1-full-kSimple-prng1`
and `range_digest 9e1b8b78a66f91db`.

| Fixture | Seed | Iterations | Info sets | Nodes (training) | Accounted bytes | Peak RSS bytes | Elapsed ms | First NashConv | Final NashConv | Gate | Result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `matrix3-spr1-dry-equal-fixed` | 1 | 12000 | 252 | 44736009 | 1554728 | 3031040 | 15342 | 0.00629558146108 | 0.000606859664931 | 0.002 | PASS |
| `matrix3-spr1-dry-equal-fixed` | 17 | 12000 | 252 | 44736009 | 1554728 | 3031040 | 15926 | 0.00629558146108 | 0.000606859664931 | 0.002 | PASS |
| `matrix3-spr1-dry-equal-fixed` | 43 | 12000 | 252 | 44736009 | 1554728 | 3031040 | 15523 | 0.00629558146108 | 0.000606859664931 | 0.002 | PASS |
| `matrix3-spr1-connected-equal-fixed` | 1 | 12000 | 252 | 44736009 | 1554728 | 3031040 | 15661 | 0.0112112195973 | 0.001158562503 | 0.002 | PASS |
| `matrix3-spr1-paired-asym-fixed` | 1 | 12000 | 252 | 44736009 | 1554728 | 3031040 | 16008 | 0.010104072123 | 0.00144516124979 | 0.002 | PASS |
| `matrix3-spr1-monotone-equal-free` | 1 | 500 | 6192 | 44864009 | 39639848 | 21266432 | 22279 | 0.0564437929765 | 0.0112887585953 | 0.02 | PASS |
| `matrix3-spr4-dry-equal-fixed` | 1 | 100 | 19176 | 29320009 | 154898888 | 73433088 | 17653 | 1.53823642758 | 0.150113162264 | 0.16 | PASS |
| `matrix3-spr4-connected-asym-fixed` | 1 | 100 | 19176 | 29320009 | 154898888 | 73400320 | 18154 | 1.56102492115 | 0.365752065422 | 0.4 | PASS |
| `matrix3-spr4-paired-equal-fixed` | 1 | 100 | 19176 | 29320009 | 154898888 | 73449472 | 17706 | 1.22966403687 | 0.332260032957 | 0.36 | PASS |
| `matrix3-spr4-monotone-asym-fixed` | 1 | 100 | 19176 | 29320009 | 154898888 | 73400320 | 17538 | 1.42656189468 | 0.137179422871 | 0.15 | PASS |
| `matrix3-spr10-dry-asym-fixed` | 1 | 4 | 493500 | 30852681 | 5461986440 | 2507587584 | 32420 | 6.01481478583 | 4.37322736827 | 4.6 | PASS |
| `matrix3-spr10-connected-equal-fixed` | 1 | 4 | 493500 | 30852681 | 5461986440 | 2507636736 | 32200 | 6.10977314937 | 5.39491778572 | 5.7 | PASS |
| `matrix3-spr10-paired-asym-fixed` | 1 | 4 | 493500 | 30852681 | 5461986440 | 2507603968 | 32106 | 6.14035014457 | 5.5223726767 | 5.8 | PASS |
| `matrix3-spr10-monotone-equal-fixed` | 1 | 4 | 493500 | 30852681 | 5461986440 | 2507620352 | 32237 | 8.69699938272 | 5.209779851 | 5.5 | PASS |

Seed scope: the dry SPR-1 fixture is the pinned three-seed row (1, 17, 43);
every other fixture is run at seed 1. Full traversal is seed-independent, so
the three dry rows agree exactly. There are no held-out seeds.

Memory (computed from the raw CSV, not rounded): the SPR-10 conservative
accounting peak is 5,461,986,440 bytes = 5.087 GiB (5.462 GB decimal), and
the largest measured forked-child peak RSS is 2,507,636,736 bytes =
2.335 GiB (2.508 GB decimal). The runner opts into an explicit 8 GiB
accounting budget above the RFC default 1 GiB for the deep fixtures; the
accounted figure is a conservative bookkeeping charge, not allocated
memory, which is why it can exceed the measured resident set.

Time (sums of the printed `elapsed_ms`): the fourteen final rows total
300,761 ms = 5.01 minutes; all 28 printed rows (fourteen first checkpoints
plus fourteen finals) total 376,047 ms = 6.27 minutes. The whole process,
including the independent same-build repeat of every final checkpoint and
the per-fixture forks, took 678.16 seconds wall clock (`real`; user 673.35 s)
on the reference machine.

These numbers certify complete coverage and measured NashConv for the
declared frozen games only. The SPR 4/10 finals (0.137-0.366 and 4.37-5.52
normalized NashConv respectively) are unconverged large-game measurements
behind frozen reference-machine regression gates, not equilibrium claims and
not evidence of full-game hold\'em coverage.

## Resident Lookup Benchmark (RFC 0005 Stage 6, schema 1)

The RFC 0005 Stage 6 manual runner
`engine/benchmarks/resident_lookup_benchmark.cpp` (target
`benchmark-resident-lookup`, intentionally NOT a CTest) measures cold
resident construction and warm lookup latency for the offline
`bigshark_resident` layer. All timing uses `std::chrono::steady_clock`;
p50/p95/p99 are sort-based percentiles (there is no percentile utility in
the tree), and a process-wide counting `operator new` measures allocations
per batch. The harness pre-builds long strings outside every counter region
so only resident lookups are charged.

Two deterministic measurements are produced from COMPLETE published
artifacts built with `HeadsUpSolverDebug::train_full` at one or more full
traversal iterations (all-zero initial regrets give a uniform policy whose
average weights normalize exactly to the stored probabilities, so the
exports validate). Neither number is a strategy-quality claim.

1. **Single-root budget gate.** The frozen RFC 0004 Stage 4 SPR-10 dry
   asymmetric fixture (Ks7h2c, 100/200 behind, 10-chip pot, the shared
   three-combo weighted ranges with the AcAd/AcAs cross-block, fixed 3s/5h
   runout) is trained at 4 iterations and published. Its tree is the
   matrix-measured 493,500 information sets. The artifact is loaded and
   measured, then correctly REFUSED under the 256 MiB default resident
   budget (`OverBudget`, not advertised).

2. **Warm latency over a six-root aggregate.** Six complete supported roots
   use the same ranges and SPR-4 stack profile (the frozen matrix depth,
   19,176 sets per root at 100 iterations) on six pairwise distinct flops
   that never collide with the range cards: Ks7h2c, 9s8s4h, QdQc6s,
   Js9d2h, 7s6h5c, 5s4s3h (all with the fixed 3s/5h runout). The aggregate
   resident set totals 115,056 information sets (above the 100,000 target)
   and is advertised as one `ResidentPolicySet`. The runner enumerates EVERY
   covered hero decision of EVERY root into independent per-root vectors
   (the total is asserted to equal the published information-set count, so
   the timed batch touches the complete 115,056-row working set rather than
   the first root), runs an untimed one-pass warmup outside every counter
   region, then times a 100,000-call hero-decision batch cycling over all
   enumerated nodes. A 20,000-call miss batch rotates through all coverage
   miss kinds (unsupported root, pinned-identity mismatch, over-budget,
   off-tree amount, untrained combo, board-blocked combo, runout divergence,
   zero-probability observed action, and an unknown digest pin).

### Measured results (Apple M5 Pro, 48 GB, macOS 26.5.1, Apple clang 21)

Measured 2026-10-05 under the release preset, after the compact resident
layout (5-byte history actions, 3-byte `CompactAction`, 8-byte slots) and the
lossless probability codebook (a per-root dictionary of distinct doubles plus
one uint8 index per action, used when a root has at most 256 distinct
probability values). All numbers are observations, not portable gates
except the 10 ms warm p99 promotion target from RFC 0005.

The synthetic SPR fixtures below have more than 256 distinct probability
values, so they use the doubles fallback and their resident bytes are
unchanged by the codebook. Trained artifacts (the flop class libraries) have
63-175 distinct values and use the codebook, cutting their probability blob
from 8 bytes to ~1 byte per action.

| Metric | Value |
| --- | ---: |
| SPR-10 fixture information sets | 493,500 |
| SPR-10 published file bytes | 184,516,608 (175.97 MiB) |
| SPR-10 honest resident bytes | 71,265,576 (67.97 MiB) |
| SPR-10 outcome vs 256 MiB budget | Advertised (fits under the compact layout) |
| SPR-10 4-iteration train time | 48,968.9 ms |
| Aggregate roots | 6 |
| Aggregate information sets | 115,056 (19,176 per root) |
| Enumerated timed hit queries | 115,056 (asserted == information sets) |
| Aggregate 6x100-iteration train time | 123,630.1 ms |
| Cold construction (probe + verified load + index build) | 1,515.3 ms |
| Aggregate honest resident bytes | 14,364,360 (13.699 MiB) of 256 MiB |
| Process peak RSS (after all builds) | 2,974,711,808 bytes (2.77 GiB) |
| Warm hit calls / time | 100,000 / 5,371.8 ms |
| Warm hit p50 / p95 / p99 | 52.58 / 69.54 / 76.75 microseconds |
| Warm miss calls / p99 | 20,000 / 16.42 microseconds |
| Unexpected miss hits | 0 |
| Heap allocations: hit batch / miss batch | 0 / 0 |
| Warm p99 target (<= 10 ms) | MET |

The 256 MiB budget is enforced against honest in-memory resident records
(contiguous key/action/probability blobs, fixed row records, and the
open-addressing slot table, plus the immutable game copy), never the file
size or the SQLite page cache. A lightweight additive artifact probe
(`probe_artifact`) performs every physical and schema validation and
aggregates state/action/key sizes without materializing rows; the final truth
remains the exact measurement after the index is built. Under the compact
layout the 493,500-set SPR-10 root needs 67.97 MiB resident against a
184.52 MiB file and now advertises under the 256 MiB default (it was 301.21
MiB and correctly withheld under the original word-key layout); the
115,056-set six-root aggregate needs 13.699 MiB and advertises. The
over-budget refusal path is covered by the unit suite (`budget_bytes=1`
refuses a root and reports `OverBudget`). Across the complete six-root
working set, warm lookup p99 is 76.75 microseconds, far below the 10 ms
RFC 0005 promotion target, with zero heap allocations across both the
100,000 hit and 20,000 miss calls. Peak RSS is dominated by training
artifacts retained in the benchmark process and is not the resident
footprint. Linux verification remains an external gate.

### Full-library runtime smoke test (2026-10-05)

Measured on the same machine under the release preset, launching
`bigshark-engine --flop-library artifacts/flop-library --flop-library
artifacts/flop-library-3p --resident-budget 12288 --proto` with the complete
2-seat and 3-seat flop class libraries (1,755 classes each, 2 BB effective,
1,000 iterations/class).

| Metric | Value |
| --- | ---: |
| Roots advertised / total | 3,510 / 3,510 |
| OverBudget or LoadFailed roots | 0 |
| 2-seat resident bytes (1,755 classes) | 1,142,602,304 (1.06 GiB) |
| 3-seat resident bytes (1,755 classes) | 4,797,586,254 (4.47 GiB) |
| Total resident bytes | 5,940,188,558 (5.53 GiB) |
| Budget | 12 GiB (12,288 MiB) |

Every class advertised; no root was refused. The probability codebook is
active on all classes (63-64 distinct values for 2-seat, 131-175 for 3-seat,
all under the 256-entry cap), so the probability blob costs ~1 byte per action
plus a small per-root dictionary instead of 8 bytes per action.
