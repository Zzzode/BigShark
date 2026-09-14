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
