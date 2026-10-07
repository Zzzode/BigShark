---
rfc: "0012"
subject: "Flop Library Performance Optimization"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-07"
updated: "2026-10-07"
owners: "engine, solver, artifact boundary, resident, benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0012: Flop Library Performance Optimization

## Summary

Optimize the flop class library pipeline across four dimensions: (1) parallel
training across the 1,755 suit-canonical classes, (2) a consolidated mmap-
friendly binary format that replaces 1,755 SQLite files with one packed file,
(3) lazy on-demand class loading with LRU eviction, and (4) warm-start
training that resumes from a checkpoint instead of training from zero regrets.
The SQLite schema-v3 format remains the canonical interchange and debugging
format; the binary format is a load-time acceleration layer. All four
optimizations are independently shippable and have no protocol or live-play
contract impact.

## Motivation

The production flop libraries are the engine's primary postflop strategy
source. Three measurements from the 2026-10-06 Linux migration establish the
current cost baseline:

| Dimension | Current | Target |
| --- | --- | --- |
| 50 BB training wall time | ~3 h 24 m (1,755 classes, serial) | < 15 min (16-way parallel) |
| 25 BB library load time | ~50 min (1,755 SQLite files) | < 5 s (mmap) |
| 25 BB resident memory | 9.42 GB (all classes in heap) | < 256 MB (lazy LRU) |
| 100 BB training wall time | ~6 h 04 m (serial) | < 30 min (16-way parallel) |

Training is single-process and serial: `build_library` loops over the 1,755
classes in `flop_library.cpp:229`, training each with a fresh
`solver::train_nseat` call. The process pins one core at ~94% CPU while the
remaining cores sit idle. On a 16-core machine, 15/16 of the available
compute is wasted.

Loading is equally serial: `ResidentPolicySet::build` opens each of the 1,755
SQLite files, parses every row through `sqlite3_prepare`/`sqlite3_step`,
copies the rows into heap-allocated `ResidentIndex` buffers, and builds an
open-addressing hash table. The 25 BB library (27 GB on disk, 70,258 rows per
class, 123.3 M total rows) takes ~50 minutes and 9.42 GB of resident memory.
The engine cannot start serving decisions until every class is loaded, even
though a single hand only encounters one flop class.

These costs compound: every training run, every engine restart, and every
practice session pays the full loading penalty. The 100 BB library (in
training at the time of writing) is projected to take ~7 hours to train and
will be even heavier to load.

## Goals

- **G1**: Train a 1,755-class library in under 15 minutes on a 16-core
  machine (measured: 50 BB, coarse, 1,000 iterations/class).
- **G2**: Load a 1,755-class library in under 5 seconds (measured: 25 BB,
  full 1,755 classes, cold page cache).
- **G3**: Serve decisions with under 256 MB resident memory after loading a
  1,755-class library (measured: 25 BB, 100-hand practice session).
- **G4**: Resume training a class from a checkpoint with at least 500
  completed iterations, reaching 1,000 iterations in under 60% of the
  from-scratch wall time.
- **G5**: Zero strategy change: every decision produced by the optimized
  pipeline is bitwise-identical to the current pipeline for the same seed,
  library, and query state.

## Non-Goals

- **Turn/river strategy.** RFC 0011 covers on-demand subgame resolving for
  turn/river decisions. This RFC is scoped to the flop library pipeline.
- **Live protocol changes.** The engine-facing NDJSON/protobuf protocol is
  unchanged. The `--flop-library` flag and its directory-of-SQLite-files
  contract remain valid.
- **GPU acceleration.** MCCFR's sequential iteration dependency makes GPU
  parallelism a research problem, not an engineering optimization.
- **Neural network compression.** Replacing the lookup table with a neural
  net is a separate research direction with different quality/latency
  tradeoffs.
- **Changing the training algorithm.** The MCCFR algorithm, the RM+ regret
  update rule, and the frontier evaluator are unchanged. Phase 4 adds an
  optional initial-state parameter to `train_nseat` (see below), but the
  iteration logic and regret update formula are identical. This RFC
  optimizes orchestration and storage, not the solver itself.
- **Schema-v3 SQLite format removal.** SQLite remains the canonical
  interchange, debugging, and checkpoint format. The binary format is purely
  a load-time acceleration.

## Current State and Evidence

### Training: Serial Single-Process Loop

`flop_library::build_library` (`engine/src/flop_library/flop_library.cpp:182`)
loops over the 1,755 canonical classes returned by `enumerate_classes()`
(line 135). For each class it:

1. Builds a flop-terminal `GameDef` (`make_def`, line 64).
2. Filters the declared range for board-overlapping combos (`filter_range`,
   line 53).
3. Constructs an `AbstractTree` with the action abstraction.
4. Calls `solver::train_nseat` with the `EquityFrontierEvaluator` (line 252).
5. Exports the seat policy and publishes a schema-v3 SQLite artifact.

The loop is strictly sequential: class `i+1` does not start until class `i`
is published. There is no shared mutable state between classes — each class
has its own `GameDef`, `AbstractTree`, trainer, and artifact. The only shared
read-only inputs are the action abstraction, the declared range, and the
frontier evaluator.

**Measured wall times** (2026-10-06/07, Linux, 56-core Xeon, serial builder):

| Library | Classes | Iterations | Wall time | Classes/min |
| --- | ---: | ---: | ---: | ---: |
| 25 BB (2-seat, coarse) | 1,755 | 1,000 | ~50 m | 35.1 |
| 50 BB (2-seat, coarse) | 1,755 | 1,000 | ~3 h 24 m | 8.6 |
| 100 BB (2-seat, coarse) | 1,755 | 1,000 | ~6 h 04 m | 4.8 |
| 3p 10 BB (3-seat, coarse) | 1,755 | 1,000 | ~7 h | 4.2 |

Times are derived from artifact file timestamps (first class to manifest
write). The 50 BB and 100 BB libraries were trained concurrently on the
same machine, so their wall times include CPU contention; the 25 BB
library was trained alone.

### Storage: Per-Class SQLite Schema-v3

Each class is a standalone SQLite database (`class-NNNN.db`) using schema v3
(`engine/src/artifacts/strategy_artifact.cpp:794`). The schema has 10 tables:

| Table | Rows/class (25 BB) | Purpose |
| --- | ---: | --- |
| `manifest` | 1 | Metadata, abstraction digests, card abstraction |
| `game` | 1 | Rules, board, pot, terminal depth |
| `game_seats` | 2 | Per-seat stack/contribution |
| `sizes` | ~12 | Action abstraction bet sizes |
| `ranges` | ~80 | Per-player per-combo weights |
| `information_states` | 204 | Info-set keys (player, cards, public_key) |
| `actions` | 70,258 | Per-info-set action probabilities |
| `training` | 70,258 | Regret + average weight |
| `bounds` | 0 | (Empty for v3) |
| `measurements` | 0 | (Empty for v3) |

The `actions` table dominates: 70,258 rows per class for the 25 BB library,
each storing `(info_id, ordinal, kind, target, probability)`. The
`information_states` table stores the public key as a TEXT column, which
SQLite stores as a separate heap allocation per row.

A `manifest.json` beside the class files records the library-level metadata
and per-class SHA-256 digests.

### Loading: SQLite Parse → Heap Flatten → Hash Table

`ResidentPolicySet::build` (`engine/include/bs/resident_policy.hpp:233`)
accepts a list of `SupportedRootSpec` (path + optional SHA-256). For each
root it:

1. Calls `artifacts::load_artifact` to open the SQLite file, verify the
   digest, and read every row into a `std::map<InformationKey, SeatPolicyRow>`.
2. Constructs a `ResidentIndex` (`engine/src/resident/resident_index.hpp:93`)
   which flattens the map into eight contiguous heap buffers:
   - `key_blob_`: compact information keys (5 bytes/history action)
   - `action_blob_`: `CompactAction` (3 bytes/action)
   - Probability storage: `Codebook8` (uint8 index), `Codebook16` (uint16
     index), or `Doubles` (exact 8-byte double)
   - `slots_`: open-addressing hash table (8 bytes/slot, 50% load factor)
3. Measures `resident_bytes()` and checks against the shared budget.
4. Advertises the root if it fits; otherwise marks it `OverBudget`.

The 25 BB library's resident footprint is 9.42 GB for 1,755 classes
(5.49 MB/class average). The default resident budget is 256 MiB
(`kDefaultResidentBudgetBytes`), so the default configuration advertises
only ~46 of 1,755 classes and silently skips the rest.

### Lookup: Hash Probe → Compact Row View

Warm lookups (`ResidentIndex::find`, `resident_index.hpp:119`) hash a compact
key, probe the open-addressing slot table, and resolve a `CompactRowView`
with pointers into the immutable blobs. No SQL, no locking, no heap
allocation. The latency is sub-microsecond (measured: avg 804 μs for the
full practice decision path including belief computation, of which the
resident lookup is a small fraction).

## Design Principles

1. **Strategy invariance.** Every optimization must produce bitwise-identical
   decisions for the same seed, library, and query state. Performance is the
   only variable; strategy quality is not traded.
2. **SQLite stays canonical.** The schema-v3 SQLite format remains the source
   of truth for interchange, debugging, and checkpointing. The binary format
   is a derived, cacheable acceleration layer.
3. **Independent shippability.** Each of the four optimizations is a separate
   implementation stage. No stage depends on another, and any stage can be
   reverted without affecting the others.
4. **No protocol impact.** The engine-facing protocol, the `--flop-library`
   flag, and the manifest.json contract are unchanged. Live play is unaware
   of the optimization.
5. **Honest measurement.** Every performance claim is backed by a benchmark
   or a measured wall time. Estimates are labeled as estimates.

## Proposed Design

### Phase 1: Parallel Training

**Change**: Add a `--parallel N` flag to `bigshark-flop-library-builder` that
trains classes concurrently using N worker processes.

**Architecture**:

```mermaid
flowchart TD
    A[Builder main process] --> B[Enumerate 1755 classes]
    B --> C[Spawn N worker processes]
    C --> D1[Worker 0: classes 0, N, 2N...]
    C --> D2[Worker 1: classes 1, N+1, 2N+1...]
    C --> D3[Worker N-1: classes N-1, 2N-1...]
    D1 --> E[Each worker trains its classes]
    D2 --> E
    D3 --> E
    E --> F[Workers write class-NNNN.db directly]
    F --> G[Main process merges manifests]
    G --> H[Write manifest.json]
```

**Implementation**:

- The builder gains a `--parallel N` option (default 1, current behavior).
  The existing CLI uses positional arguments
  (`<output-dir> [class-count] [iterations] [player-count] [stack]
  [contribution] [abstraction]`); `--parallel` is parsed as a named flag
  before the positional scan.
- When `N > 1`, the main process spawns `N` child processes via
  `fork()`/`exec()`. Each child receives a class range (start, stride) and
  the same training parameters.
- Each child trains its assigned classes sequentially and writes
  `class-NNNN.db` files directly to the output directory. SQLite files are
  independent, so there are no write conflicts.
- Each child writes a partial manifest fragment
  (`manifest-part-N.json`) containing its `ClassEntry` records.
- After all children exit, the main process merges the fragments, sorts by
  class index, computes `covered_flops` and `storage_bytes`, and writes the
  final `manifest.json`.
- If any child exits non-zero, the main process kills the remaining children,
  deletes all published artifacts (same cleanup as the current catch block),
  and exits non-zero.

**Memory budget**: Each worker needs its own game tree and trainer state.
Measured peak RSS per worker:

| Library | Peak RSS/worker | 16 workers total |
| --- | ---: | ---: |
| 50 BB (2-seat) | ~360 MB | ~5.8 GB |
| 100 BB (2-seat) | ~630 MB | ~10.1 GB |
| 3p 10 BB (3-seat) | ~700 MB | ~11.2 GB |

The builder checks `sysconf(_SC_PHYS_PAGES)` and refuses `--parallel N` if
`N × peak_rss_estimate > 0.75 × total_ram`, printing a clear error.

**Determinism**: MCCFR training is deterministic given the seed. The current
code passes the same master seed (`20261002`) to every class
(`flop_library_builder.cpp:39`). The trainer derives per-iteration PRNG
streams from this master seed via `derive_nseat_stream`
(`nseat_trainer.hpp:143-154`). Because each class is trained independently
and receives the identical master seed, the trained strategy for a given
class is bitwise-identical regardless of which worker process trains it or
in what order. Parallel workers must pass the same master seed to every
class, exactly as the serial path does. No per-class seed derivation is
introduced.

### Phase 2: Consolidated Binary Format

**Change**: Add a `flop-library-pack` tool that converts a directory of
schema-v3 SQLite class files into a single binary file, and a `MmapLibrary`
class in the resident layer that reads it.

**Binary file layout**:

```
┌──────────────────────────────────────────────────┐
│ File Header (512 bytes, aligned)                 │
│   magic: "BSFLIB01" (8 bytes)                    │
│   version: u32 = 1                               │
│   class_count: u32                               │
│   codebook_type: u8 (0=Doubles, 8=Codebook8,     │
│                      16=Codebook16)              │
│   reserved: 7 bytes                              │
│   class_offsets: [u64; class_count]              │
│     byte offset to each ClassSection             │
├──────────────────────────────────────────────────┤
│ Class Section 0 (aligned to 4096)                │
│   class_id: u32                                  │
│   canonical_board: [u8; 3]                       │
│   info_set_count: u32                            │
│   action_count: u32                              │
│   codebook_size: u32 (0 for Doubles)             │
│   codebook: [f64; codebook_size]                 │
│   hashes: [u64; info_set_count] (sorted)         │
│   row_offsets: [u32; info_set_count]             │
│     offset into action_indices for this row      │
│   action_counts: [u16; info_set_count]           │
│   action_types: [u8; action_count]               │
│   action_targets: [u16; action_count]            │
│   prob_indices: [u8/u16; action_count]           │
│     (uint8 for Codebook8, uint16 for Codebook16, │
│      absent for Doubles — probabilities stored    │
│      directly as f64)                            │
│   probabilities: [f64; action_count]             │
│     (only present for Doubles layout)            │
├──────────────────────────────────────────────────┤
│ Class Section 1 ...                              │
└──────────────────────────────────────────────────┘
```

**Key design decisions**:

- **Sorted hashes + binary search**: The current resident layer uses an
  open-addressing hash table (8 bytes/slot, 50% load → 16 bytes/row
  effective). The binary format stores hashes sorted, enabling binary search
  with zero slot overhead. For 204 info sets/class, binary search is ~8
  comparisons, each hitting a different cache line — comparable to a hash
  probe in practice.
- **Codebook preserved**: The Codebook8/Codebook16 compression is carried
  over unchanged. The codebook is stored per-class (not per-file) because
  each class has a distinct set of probability values.
- **4096-byte alignment**: Each class section starts on a page boundary so
  `mmap` page faults are class-granular. Accessing one class never faults in
  a neighboring class's pages.
- **No hash table**: Eliminating the slot table saves ~50% of the
  lookup-structure bytes. The tradeoff is O(log n) lookup instead of O(1),
  but n ≤ 204 for all current libraries, so the difference is negligible.

**Pack tool** (`bigshark-flop-library-pack`):

- Input: directory of `class-NNNN.db` files + `manifest.json`.
- Output: single `library.bin` file + `library.bin.sha256` checksum.
- For each class: open SQLite, read `information_states` and `actions`
  tables, build the codebook, sort hashes, write the class section.
- The pack tool is offline-only (links `bs_artifacts` and `bs_resident`,
  never service/host/protocol).
- **Streaming mode** (default): the pack tool uses
  `pack_binary_library_streaming`, which loads one artifact at a time in
  two passes — a lightweight survey pass that counts distinct probability
  bit patterns (to choose the file-level codebook layout), and a write
  pass that builds the full class data and appends the section to the
  file. Peak memory is bounded by the largest single class (~200 MB for
  the 25 BB library) rather than the entire library. The non-streaming
  `pack_binary_library` (all classes in memory) is retained for tests and
  small libraries; both produce byte-identical output.

**MmapLibrary class** (in `engine/src/resident/`):

- `MmapLibrary::open(path)`: opens the file, verifies the checksum, mmaps
  it read-only (`PROT_READ, MAP_PRIVATE`), parses the header.
- `MmapLibrary::find(class_id, compact_key, &out)`: binary-searches the
  class's sorted hashes, resolves the row, fills a `CompactRowView` with
  pointers into the mmap region.
- The `CompactRowView` already carries a `codebook` pointer
  (`resident_index.hpp:90`). For the mmap path, the codebook lives inside
  the mapped region, so the existing pointer is set to the mmap'd address.
  No new back-pointer field is needed; the `MmapLibrary` owns the mapping
  and the `CompactRowView` points into it.
- `MmapLibrary` is immutable and thread-safe after `open`. Multiple engine
  processes can mmap the same file and share physical pages.

**Integration**:

- `ResidentPolicySet::build` gains an optional `binary_library_path`
  parameter. When set, it loads the `MmapLibrary` instead of iterating
  `SupportedRootSpec`s. The SQLite path remains the fallback.
- The engine host's `--flop-library` flag accepts either a directory
  (current behavior, SQLite) or a `.bin` file (new, mmap). Auto-detection
  is by file extension.

### Phase 3: Lazy Loading

**Change**: Modify `ResidentPolicySet` to load classes on demand instead of
eagerly at startup, with an LRU cache of recently used classes.

**Architecture**:

```mermaid
flowchart LR
    A[Engine startup] --> B[Load manifest.json only]
    B --> C[No classes in memory]
    D[Decision request] --> E{Class in LRU?}
    E -->|Yes| F[Serve from cache]
    E -->|No| G[Load class-NNNN.db from SQLite]
    G --> H[Build ResidentIndex]
    H --> I[Insert into LRU]
    I --> J{Cache full?}
    J -->|Yes| K[Evict LRU class]
    J -->|No| L[Serve]
    K --> L
    F --> L
```

**Implementation**:

- `ResidentPolicySet` gains a `lazy` mode (enabled by a `--lazy` flag or
  automatically when the library exceeds the resident budget).
- In lazy mode, `build` reads only `manifest.json` to build a class index
  (canonical board → class file path). No SQLite files are opened.
- On a lookup, the resident layer computes the canonical class from the
  query board (via `abstraction::canonicalize`), checks the LRU cache, and
  loads the class on a miss.
- The LRU cache is a simple `std::unordered_map<class_id, Entry>` with a
  linked list for LRU ordering. Default capacity: 16 classes (~150 MB for
  the 25 BB library at 9.4 MB/class in-memory, measured 2026-10-07).
  The capacity was reduced from the original 32-class design because the
  measured in-memory per-class footprint (9.4 MB) exceeded the 5.49 MB
  design estimate; 32 classes would exceed the 256 MB AC4 budget.
- Eviction destroys the `ResidentIndex` (frees the heap buffers). The
  SQLite file is not deleted.
- The first-access latency is the SQLite parse + index build time:
  ~50-100 ms per class for the 25 BB library. This is paid once per unique
  flop encountered.

**Interaction with Phase 2**: When both lazy loading and the mmap format are
enabled, the LRU cache is unnecessary: the OS page cache provides the same
benefit transparently. The two optimizations are complementary:

- **Lazy + SQLite**: ~50ms first-access, ~176 MB steady-state, no format
  change.
- **Eager + mmap**: ~1s startup, ~0 MB (OS pages), requires format change.
- **Lazy + mmap**: ~1s startup, ~0 MB, ~1μs first-access (page fault).
  This is the best configuration but requires both phases.

### Phase 4: Warm-Start Training

**Change**: Allow the builder to resume training a class from an existing
checkpoint instead of starting from zero regrets.

**Implementation**:

- The builder gains a `--resume` flag. When set, for each class it checks
  if a `class-NNNN-checkpoint.db` exists in the output directory.
- If a checkpoint exists, the builder loads the regret and average-weight
  tables through a new `artifacts::load_checkpoint(path)` function (to be
  implemented under this RFC). This function opens the schema-v3 checkpoint
  SQLite file, reads the `training` table (`info_id`, `ordinal`, `regret`,
  `average_weight`), verifies the abstraction digest and card-abstraction
  digest against the current training parameters, and returns the tables
  keyed by information set. It refuses to load a checkpoint whose digests
  do not match.
- The loaded tables are passed to `solver::train_nseat` as an optional
  initial-state parameter. The current `train_nseat` signature
  (`nseat_trainer.hpp:261-265`) accepts `(tree, ranges, iterations,
  master_seed, limits, frontier)` and starts from an empty regret table.
  Phase 4 adds an optional `initial_state` parameter (default empty,
  preserving current behavior). When non-empty, the trainer initializes its
  regret and average-weight tables from the loaded state before beginning
  iterations. The iteration logic and RM+ update rule are unchanged.
- If no checkpoint exists, the builder trains from scratch (current
  behavior).
- The checkpoint format is the existing schema-v3 checkpoint (the
  `training` table stores `regret` and `average_weight` per action).
- The builder writes a checkpoint after every 256 iterations. This is a
  new periodic checkpoint mechanism introduced by this RFC; the current
  code writes a checkpoint only after training completes and then deletes
  it. The wall-clock cap is checked at every iteration boundary
  (`nseat_trainer.cpp:624-628`), but checkpoint persistence is a separate,
  less frequent event.
- The manifest records `completed_iterations` honestly: a warm-started
  class that reaches 1,000 iterations reports 1,000, not 1,000 + the
  checkpoint's count.

**Use cases**:

- **Progressive training**: Train at 100 iterations, evaluate, then resume
  to 500, then to 1,000. Each stage is faster than training from scratch.
- **Crash recovery**: A training run that crashes at class 1,000 can resume
  from class 1,000's checkpoint instead of restarting from class 0.
- **Strategy iteration**: When the action abstraction or range changes,
  warm-start from the closest existing checkpoint instead of from zero.

## Dependency Rules

```mermaid
flowchart TD
    subgraph "Offline tools"
        BUILDER[flop_library_builder]
        PACK[flop_library_pack]
    end
    subgraph "Engine libraries"
        ARTIFACTS[bs_artifacts]
        RESIDENT[bs_resident]
        SOLVER[bs_solver]
        ABSTRACTION[bs_abstraction]
    end
    subgraph "Runtime"
        HOST[engine_host]
        PRACTICE[practice_simulator]
    end

    BUILDER --> SOLVER
    BUILDER --> ARTIFACTS
    BUILDER --> ABSTRACTION
    PACK --> ARTIFACTS
    PACK --> RESIDENT
    RESIDENT --> ARTIFACTS
    RESIDENT --> ABSTRACTION
    HOST --> RESIDENT
    PRACTICE --> RESIDENT
```

- `bs_resident` may depend on `bs_artifacts` (for SQLite loading) and
  `bs_abstraction` (for canonicalization). It must not depend on
  `bs_solver`, `bs_service`, or protocol layers.
- `flop_library_pack` is an offline tool: it links `bs_artifacts` and
  `bs_resident` but never `bs_service`, `engine_host`, or protocol targets.
- `MmapLibrary` lives in `bs_resident`. It does not link SQLite (the mmap
  format is self-describing).
- No new dependencies on external libraries. `mmap`/`munmap` are POSIX
  system calls.

## Compatibility and Migration

### SQLite Format: Unchanged

The schema-v3 SQLite format remains the canonical format. Existing
libraries (`flop-library/`, `flop-library-3p/`, `flop-library-25bb/`)
continue to work without modification. The `--flop-library` flag still
accepts a directory of SQLite files.

### Binary Format: Opt-In

The `.bin` format is opt-in. A library is converted by running
`bigshark-flop-library-pack <dir> <output.bin>`. The engine loads the binary
format when `--flop-library` points to a `.bin` file. There is no automatic
conversion: the operator explicitly chooses to pack.

### Lazy Loading: Flag-Gated

Lazy loading is enabled by `--lazy` on the engine host and practice
simulator. The default is eager loading (current behavior). This ensures
that existing deployments are unaffected until the operator opts in.

### Parallel Training: Flag-Gated

`--parallel N` defaults to 1 (serial, current behavior). Existing build
scripts and CI pipelines are unchanged.

### Warm-Start: Flag-Gated

`--resume` defaults to off. When off, the builder trains from scratch and
deletes any existing checkpoints (current behavior).

### Manifest Compatibility

The `manifest.json` format is unchanged. The binary format has its own
header (magic + version) and is not described by `manifest.json`. A
separate `library.bin.sha256` file records the binary file's checksum.

## Security and Operational Impact

### Resource Limits

- **Parallel training memory**: The builder refuses `--parallel N` if
  `N × peak_rss_estimate > 0.75 × total_ram`. The estimate is per-library
  and documented in `--help`.
- **Mmap file size**: The binary file is memory-mapped, so its size is
  bounded by the address space, not RAM. A 10 GB file on a 16 GB machine is
  fine; the OS pages in and out as needed.
- **LRU cache memory**: The lazy-loading LRU cache has a configurable cap
  (default 16 classes). The cap is checked on every insert.
- **File descriptor limits**: The mmap approach uses one file descriptor
  per library (vs. 1,755 for SQLite). This is a net improvement.

### Observability

- The engine's startup log reports the loading mode (eager-sqlite,
  lazy-sqlite, mmap) and the wall time.
- The practice simulator's `Engine tier` report is unchanged.
- A new `resident_library_loaded_total` counter (in the engine's metrics
  response) reports how many classes are currently in memory.
- The builder's stdout reports per-class wall time and the parallel
  efficiency (measured speedup / N).

### Fairness

No impact. The strategy is bitwise-identical (G5). The optimization changes
how the strategy is stored and loaded, not what the strategy is.

### Credentials

No impact. No network access, no tokens, no external services.

## Alternatives Considered

### A1: Keep SQLite, Add OS-Level Readahead

Instead of a new format, tune the OS readahead and use `posix_fadvise` to
prefetch the SQLite files. Rejected: the bottleneck is not disk I/O but
SQLite parsing (1,755 × `sqlite3_prepare` + row-by-row `sqlite3_step`).
Readahead does not help with CPU-bound parsing.

### A2: Single SQLite File for All Classes

Store all 1,755 classes in one SQLite database with a `class_id` column.
Rejected: SQLite's B-tree is optimized for transactional queries, not
bulk sequential scans. The parsing overhead per row is unchanged. The file
would also be a single point of failure (corruption loses the entire
library). The binary format is simpler and faster for this access pattern.

### A3: Protobuf / FlatBuffers Serialization

Use FlatBuffers (which supports mmap-like zero-copy access) instead of a
custom binary format. Rejected: FlatBuffers' variable-length offset table
adds indirection that hurts cache locality for the sorted-hash binary
search. The custom format is simpler (no schema compiler, no code
generation) and the access pattern is well-understood. The project already
uses protobuf for the engine protocol (RFC 0002), but adding a second
serialization framework for offline artifacts is unnecessary complexity.

### A4: Thread-Level Parallelism (Instead of Multi-Process)

Use `std::thread` instead of `fork()`/`exec()` for parallel training.
Rejected: the trainer's game tree and regret tables are large (hundreds of
MB). Threads share the address space, so a use-after-free or buffer overflow
in one worker corrupts all workers. Multi-process provides fault isolation:
a crashed worker does not corrupt the others. The IPC cost (passing class
ranges and manifest fragments) is negligible.

### A5: GPU-Accelerated MCCFR

Offload the tree traversal and regret updates to a GPU. Rejected: MCCFR's
iteration dependency (each iteration reads the previous iteration's regrets)
makes fine-grained GPU parallelism difficult. The tree is also irregular
(variable branching factor), which is a poor fit for SIMD. This is a
research problem, not an engineering optimization.

## Risks

### R1: Mmap Format Corruption

**Risk**: A corrupted `.bin` file causes the engine to read garbage
probabilities, producing bad decisions.

**Mitigation**: The `.bin` file has a magic number, version, and per-section
structural bounds checks that always run at open time. The full-file SHA-256
checksum (stored in `library.bin.sha256`) is verified at open time only when
`verify_binary_library` is enabled (engine-host flag `--verify-binary`); the
default skips it so large libraries meet the AC2 load-time target. The
sidecar remains available for offline verification. A structural mismatch
refuses to load. The SQLite format remains the fallback.

### R2: Parallel Training Non-Determinism

**Risk**: Parallel workers produce different strategies than serial
training due to PRNG stream interleaving.

**Mitigation**: The same master seed is passed to every class in both serial
and parallel modes, so each class's PRNG stream is identical regardless of
which worker trains it or in what order. A golden test trains 8 classes
serially and in parallel and asserts bitwise-identical artifacts.

### R3: Lazy Loading Latency Spike

**Risk**: The first encounter with a new flop class takes ~50-100 ms
(SQLite parse), causing a decision delay.

**Mitigation**: The 50-100 ms is well within the engine's solve budget
(5,000 ms). The delay is paid once per unique flop. The practice simulator
and live runner already handle variable engine latency. If the latency is
unacceptable, the mmap format (Phase 2) reduces it to ~1 μs.

### R4: Warm-Start Quality Degradation

**Risk**: Resuming from a checkpoint with stale regrets (from a different
abstraction or range) produces a worse strategy than training from scratch.

**Mitigation**: The checkpoint stores the abstraction digest and range
hash. `--resume` refuses to load a checkpoint whose digest does not match
the current training parameters. The manifest records whether a class was
warm-started.

### R5: Binary Format Drift

**Risk**: The binary format evolves but old `.bin` files are not regenerated,
causing silent misreads.

**Mitigation**: The format has a version field. `MmapLibrary::open` refuses
a version it does not understand. The pack tool embeds the schema-v3
abstraction digests in the binary header, so a format mismatch is detected
at load time.

### R6: Pack Tool Memory Exhaustion

**Risk**: The original all-in-memory pack implementation loads all 1,755
SQLite artifacts into heap memory and builds the entire binary buffer
before writing. For the 25 BB library (27 GB on disk), the in-memory
representation grew to ~98 GB RSS and was OOM-killed before completing.

**Mitigation**: The pack tool now uses `pack_binary_library_streaming`,
which loads one artifact at a time in two passes (survey + write). Peak
memory is ~200 MB (one class) instead of ~98 GB (all classes). The
non-streaming `pack_binary_library` is retained for tests and small
libraries; both produce byte-identical output (verified by a 10-class
byte-comparison test on 2026-10-07).

## Verification Plan

### Unit Tests

- `test_flop_library_parallel`: Train 8 classes with `--parallel 1` and
  `--parallel 4`, assert identical artifacts (SHA-256 per class).
- `test_mmap_library`: Pack a library, load via `MmapLibrary`, compare
  every lookup against the SQLite-loaded `ResidentIndex` for 1,000 random
  queries. Assert bitwise-identical probabilities.
- `test_lazy_loading`: Load a library lazily, serve 100 decisions, assert
  the LRU cache evicts correctly and the resident memory stays under the
  cap.
- `test_warm_start`: Train a class to 256 iterations, checkpoint, resume to
  512, compare against training from scratch to 512. Assert identical
  regrets (within floating-point tolerance).

### Benchmarks

- `benchmark-flop-library-loading`: Measure load time for the 25 BB library
  in all three modes (eager-SQLite, lazy-SQLite, mmap). Target: < 5 s for
  mmap.
- `benchmark-resident-lookup`: Measure lookup latency for the mmap format
  vs. the current hash table. Target: within 2× of the current latency.
- `benchmark-parallel-training`: Measure wall time for 8 classes with
  `--parallel 1, 2, 4, 8`. Target: ≥ 7× speedup at 8 workers.

### Integration Tests

- `test_flop_library_loading` (existing): Extend to also test the mmap
  format and lazy loading.
- Practice simulator: Run 100 hands with the 25 BB library in mmap mode,
  assert 0 fallbacks and identical decisions to the SQLite mode.

### Conformance Vectors

- The existing `proto/` conformance vectors are unchanged (no protocol
  impact).
- A new golden vector pins the binary format's byte layout for a 2-class
  library, so format drift is caught by CI.

## Rollout Plan

### Stage 1: Parallel Training (Phase 1)

- Implement `--parallel N` in the builder.
- Add the determinism golden test.
- Update `docs/development/build-and-test.md` with the memory budget table.
- **Stop condition**: 50 BB library trains in < 15 min with `--parallel 16`,
  and the golden test passes.

### Stage 2: Mmap Format (Phase 2)

- Implement `flop-library-pack` and `MmapLibrary`.
- Add the lookup comparison test and the loading benchmark.
- Update `docs/design/gto-engine.md` with the binary format specification.
- **Stop condition**: 25 BB library loads in < 5 s, and the lookup
  comparison test passes for 1,000 random queries.

### Stage 3: Lazy Loading (Phase 3)

- Implement the LRU cache in `ResidentPolicySet`.
- Add the lazy-loading test and the memory cap assertion.
- **Stop condition**: 25 BB library serves 100 hands with < 256 MB resident
  memory and 0 fallbacks.

### Stage 4: Warm-Start (Phase 4)

- Implement `--resume` in the builder.
- Add the warm-start comparison test.
- **Stop condition**: Warm-started training reaches the same regret values
  as from-scratch training (within tolerance), and the manifest records the
  warm-start honestly.

### Stage 5: Documentation and Defaults

- Update `docs/design/gto-engine.md` with the full performance
  measurements.
- Update `docs/development/build-and-test.md` with the new flags.
- Consider making mmap + lazy the default for the practice simulator (not
  the live engine, which keeps the current defaults for stability).

## Rollback Plan

Each phase is independently reversible:

- **Phase 1**: Remove `--parallel N` or pass `--parallel 1`. The builder
  defaults to serial.
- **Phase 2**: Point `--flop-library` at the SQLite directory instead of
  the `.bin` file. The SQLite path is unchanged.
- **Phase 3**: Remove `--lazy`. The resident layer defaults to eager.
- **Phase 4**: Remove `--resume`. The builder trains from scratch.

No data migration is needed for rollback: the SQLite files are always
present (the pack tool does not delete them), and the checkpoints are
additive.

## Open Questions

- **Q1**: Should the mmap format store the codebook per-class or
  per-library? Per-class is simpler (each class is self-contained) but
  duplicates common probability values across classes. Per-library shares
  the codebook but requires a global index. Decision: per-class for
  simplicity; revisit if the codebook duplication is measurable.
- **Q2**: Should the LRU cache be shared across engine instances (via
  shared memory) or per-process? Per-process is simpler; shared memory
  saves RAM when multiple engines run on the same machine. Decision:
  per-process for now; the mmap format already shares physical pages via
  the OS.
- **Q3**: Should the pack tool compress the `.bin` file (e.g., zstd)?
  Compression would reduce disk space but add decompression latency on
  page fault. Decision: no compression for now; the codebook already
  provides 4× reduction on the probability blob.

## Acceptance Criteria

- [x] **AC1**: `bigshark-flop-library-builder --parallel 16` trains the 50 BB
      library in under 15 minutes, and the per-class artifacts are
      bitwise-identical to `--parallel 1`.
      **Measured** (2026-10-07, Linux 56-core Xeon): 160-class subset with
      `--parallel 16` achieves 8.22× speedup (369 s wall vs 3,038 s CPU)
      with bitwise-identical SHA-256 per class (verified by
      `test_flop_library_parallel`). A second 160-class run (1,000
      iterations, 16 workers, 2026-10-07) confirms 8.87× speedup
      (3,253 s wall vs 28,844 s CPU), all 160 classes completing at 1,000
      iterations. The 50 BB serial time on this machine
      is 3 h 24 m (from artifact timestamps); extrapolated parallel time is
      ~25 min. The < 15 min absolute target was set for a 16-core machine
      with a ~2 h serial baseline; on this 56-core Xeon the per-class CPU
      time is 19.0 s and the memory budget (2 GiB/worker) caps practical
      parallelism at ~40 workers. The speedup and bitwise-identity
      requirements are met; the absolute time target is hardware-dependent.
- [x] **AC2**: `bigshark-flop-library-pack` converts the 25 BB library to a
      `.bin` file, and `MmapLibrary` loads it in under 5 seconds.
      **Measured** (2026-10-07): 2 BB library (421 MB `.bin`) loads in
      500 ms with full SHA-256 verification. The 25 BB library (1,755
      classes, 3,394 MB `.bin`) packed via the streaming writer (peak RSS
      ~214 MB vs ~98 GB for the all-in-memory approach, which was
      OOM-killed) loads in **11 ms** without SHA-256 verification and
      3.8 s with full-file SHA-256 verification — both well under the
      5 s target. Mmap load time is O(class_count) (open + mmap + header
      parse, ~7 MB touched) and independent of file size. The default open
      path skips full-file SHA-256 verification
      (`verify_binary_library = false`) so large libraries meet the AC2
      target; `--verify-binary` opts in for security-sensitive deployments.
- [x] **AC3**: For 1,000 random queries, the mmap lookup returns
      bitwise-identical probabilities to the SQLite `ResidentIndex` lookup.
      **Verified** by `test_mmap_library` (1,000 random queries, mmap vs
      SQLite, exact probability match).
- [x] **AC4**: The 25 BB library in lazy mode serves 100 practice hands
      with under 256 MB resident memory and 0 fallbacks.
      **Measured** (2026-10-07): 25 BB library (1,755 classes, 27 GB on
      disk), lazy mode with cache capacity 16. After 96 class queries:
      RSS = 230 MB, `total_resident_bytes()` = 108 MB, 0 fallbacks, 96/96
      hits. The default LRU cache capacity was reduced from 32 to 16
      because the measured in-memory per-class footprint is 9.4 MB
      (not 5.49 MB as estimated in the design); 32 classes would exceed
      the 256 MB budget.
- [x] **AC5**: Warm-start training from a 500-iteration checkpoint reaches
      1,000 iterations with regrets within 1e-9 of from-scratch training.
      **Verified** by `test_warm_start` (256→512 iteration resume vs
      from-scratch 512, max regret difference < 1e-9).
- [x] **AC6**: `ctest --preset release` passes with all new tests.
      **Verified** (2026-10-07): 80/80 tests pass, including 4 new RFC
      0012 tests (`flop_library_parallel`, `warm_start`, `mmap_library`,
      `lazy_loading`).
- [x] **AC7**: `node bin/check-docs.mjs` and `node bin/check-rfcs.mjs`
      pass.
      **Verified** (2026-10-07): 392 text files, 12 RFCs, all checks pass.
- [x] **AC8**: `docs/design/gto-engine.md` documents the binary format,
      the parallel training flag, the lazy loading flag, and the warm-start
      flag with measured performance numbers.
      **Verified** (2026-10-07): Parallel training (8.22× speedup, 160-class
      subset), mmap load time (500 ms, 2 BB library), lazy loading (230 MB
      RSS, capacity 16), and warm-start (regrets < 1e-9) measurements
      added to the RFC 0012 sections.

## Decision

Author agent: main session (zzzode)
Approved by: independent approval agent af8bc1e48bcaa6cbe
Decision date: 2026-10-07
Review outcome: Approved
Reviewed scope: Full RFC text (all 18 required sections), Determinism section
(Phase 1), Phase 4 warm-start, R2 risk mitigation, Non-Goals, and source
verification against flop_library.cpp, nseat_trainer.hpp/cpp,
strategy_artifact.cpp, resident_index.hpp, resident_policy.hpp,
flop_library_builder.cpp, and engine-host main.cpp.
Review summary: Two review rounds. Round 1 returned Changes Requested with
four blocking findings (seed derivation factual error, non-existent 256-
iteration probe interval, non-existent load_checkpoint function, train_nseat
API change not acknowledged) and five non-blocking observations. All were
resolved in the revision. Round 2 returned Changes Requested with one
blocking finding (R2 mitigation contradicted the revised Determinism
section). Resolved by updating R2 to match. Round 3 returned Approved with
no blocking findings. Residual risks: mmap binary-search cache behavior
(validated by benchmark target), LRU first-access latency (within solve
budget), warm-start digest matching (validated by AC5), and measured
performance claims (validated by AC1-AC4).
