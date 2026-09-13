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
