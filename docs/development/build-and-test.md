# Build and Test

Status: Current

## Requirements

- CMake 3.20 or newer
- Ninja
- A C++23 compiler
- clang-format
- Node.js 20 or newer
- npm with dependencies installed through `npm ci`
- unzip for the checksum-verified Protobuf compiler bootstrap
- Optional: HiGHS for the exact river LP backend

On macOS, install the optional solver with:

```bash
brew install highs
```

Without HiGHS, the engine builds and uses bounded DCFR for river solving.

The application, platform integration, engine client, tests, and interactive
tools use strict TypeScript. Install the pinned compiler and Node types once:

```bash
npm ci
```

## Presets

`CMakePresets.json` is the required project build interface.

| Preset | Build directory | Assertions | Sanitizers | Publishes to `bin/` |
| --- | --- | --- | --- | --- |
| `debug` | `build/debug` | Enabled | Disabled | No |
| `release` | `build/release` | Disabled | Disabled | Yes |
| `asan` | `build/asan` | Enabled | Address and undefined behavior | No |
| `linux-release` | `build/linux-release` | Disabled | Disabled | No |

List all available configure, build, and test presets:

```bash
cmake --list-presets=all
```

## Daily Development

```bash
cmake --preset debug
cmake --build --preset debug --target format
cmake --build --preset debug
ctest --preset debug
```

`debug` keeps assertions enabled and is the default preset for local
investigation.

## Release Verification

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
cmake --build --preset release --target format-check
ctest --preset release
node bin/replay.mjs
```

The `release` build publishes
`build/release/apps/engine-host/bigshark-engine` to
`bin/bigshark-engine`. Debug and sanitizer builds never overwrite that
runtime binary.

## Sanitizer Verification

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The preset enables AddressSanitizer and UndefinedBehaviorSanitizer through the
`BIGSHARK_ENABLE_SANITIZERS` engine option. Tests stop on the first sanitizer
failure. Leak detection is not enabled because Apple AddressSanitizer does not
support it on the current development platform.

## Formatting

The format targets cover first-party C and C++ files under:

- `engine/include`
- `engine/src`
- `engine/tests`
- `apps/engine-host`

Vendored code under `engine/third_party` is excluded.

```bash
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

`format` edits files. `format-check` is read-only and fails when formatting is
required.

## Compilation Database

Every configure operation writes `compile_commands.json` in its preset build
directory. CMake also updates the repository-root `compile_commands.json`
symbolic link to the most recently configured preset:

```text
compile_commands.json -> build/release/compile_commands.json
```

The root `.clangd` configuration reads this link. Configure the preset whose
flags clangd should use.

## TypeScript

The canonical source under `apps/`, `clients/`, `platforms/`, and interactive
`tools/` is TypeScript. Compiled output is ignored under `dist/`.

```bash
npm run typecheck
npm run build:ts
npm run test:node
npm run check
```

`npm run build:ts` removes `dist/` before compiling, so deleted or renamed
sources cannot survive as stale runtime artifacts.

`npm run check` runs strict type checking, a clean reproducible build, the source
policy gate, documentation and RFC checks, process-client tests, launcher
compatibility tests, and River golden decisions.

CMake exposes matching `typescript-build`, `typescript-check`, and
`node-check` targets. The source policy rejects handwritten `.js` or `.mjs`
implementation under the TypeScript-owned directories. JavaScript is allowed
only for thin compatibility launchers under `bin/`. CMake targets and tests
that rebuild shared TypeScript outputs use an interprocess lock under
`build/.locks`, so separate preset test processes cannot clean each other's
artifacts.

## Protobuf

The protocol toolchain is pinned to Buf 1.73.0, Protobuf 36.1, and
Protobuf-ES 2.13.0. `npm ci` installs Buf and the TypeScript generator.
`npm run proto:toolchain` downloads the matching `protoc` archive for the host
platform, verifies its SHA-256 digest, and installs it under ignored
`.tools/`. The bootstrap tool has an isolated TypeScript build under
`build/generated/proto-toolchain`; CMake compiles it inside each preset's
build tree so protocol generation never cleans or depends on shared `dist/`.
Concurrent presets serialize the shared download and publish a validated
installation atomically.

```bash
npm run proto:check
cmake --build --preset release --target protobuf-check
ctest --preset release -R '^protobuf_'
```

`proto:check` runs linting, formatting, breaking checks against
`proto/baseline/v1.binpb`, C++ and TypeScript generation, generated TypeScript
checking, and cross-language golden-vector tests. Generated files live under
`build/generated`.

## Native Tests

CTest registers native C++ tests and benchmarks, strict TypeScript checking,
Node tests, one RFC check, and one documentation check when npm is available:

| Test | Coverage |
| --- | --- |
| `eval` | Hand categories, score ordering, wheel and straight boundaries |
| `heads_up` | Offline heads-up legal transitions, all-ins, public runouts, refunds, and independent chip accounting |
| `heads_up_solver` | Multi-size full CFR, exact pure-response oracle, coverage, convergence, and resource rollback |
| `heads_up_solver_sampled` | Pinned SplitMix64, enumerated external-sampling update expectations under weighted ranges and free chance, kSimple averages, repeatability, PRNG/iteration rollback, chance-conditioned best response, and sampled convergence smoke gates |
| `heads_up_allocations` | Every-allocation fault injection, transactional publication, and measured peak memory budgets |
| `settlement` | Contribution layers, refunds, ties, capped rake, odd-chip order, exhaustive grids, and conservation |
| `icm` | Bounded prize equity, independent permutation oracle, bust handling, and prize-unit conservation |
| `equity` | Deterministic equity, multiway sanity, and draw classification |
| `v0_protocol` | Legacy JSON mapping and repeated response serialization |
| `gto` | River LP/DCFR policy contracts, exploitability, and range tracking |
| `multistreet_ref` | Independent fixed-run best-response reference |
| `benchmark_multistreet` | Deterministic multi-street convergence, exploitability thresholds, and timing measurements |
| `benchmark_heads_up_blueprint` | RFC 0004 version-2 heads-up full and external-sampling fixed/sampled-chance quality gates (Release only) |
| `protobuf_generated` | C++ binary, ProtoJSON, bigint, unknown-field, and enum compatibility |
| `protobuf_cpp_vector` | C++ generation for the TypeScript cross-language consumer |
| `protobuf_lint` | Buf STANDARD lint rules |
| `protobuf_format` | Canonical Protobuf formatting |
| `protobuf_breaking` | FILE compatibility against the accepted v1 descriptor baseline |
| `protobuf_typescript` | Generated TypeScript compilation and shared vectors |
| `typescript` | Strict TypeScript compiler diagnostics |
| `node` | Process client, launchers, and River v0 golden decisions |
| `rfc` | RFC metadata, lifecycle, sections, decisions, and index membership |
| `docs` | English-only first-party text and valid local Markdown links |

Run one test with:

```bash
ctest --preset debug -R '^gto$'
```

Run the solver-quality benchmark with:

```bash
cmake --build --preset release --target benchmark-multistreet
```

Run the RFC 0004 heads-up blueprint quality benchmark (full and external
sampling, schema version 2; the pinned one-million-iteration fixed gate takes
roughly two and a half minutes) with:

```bash
cmake --build --preset release --target benchmark-heads-up-blueprint
```

The manual one-million-iteration capacity runner spends both sampled cases at
the pinned 1,000,000 checkpoint and is not a CTest; run it explicitly and
expect several minutes:

```bash
cmake --build --preset release --target benchmark-heads-up-capacity
```

The RFC 0004 Stage 4 frozen matrix runner is also a manual target and is not
a CTest; the SPR 4/10 fixtures take several minutes and use an explicit
8 GiB solver-accounting budget. Run it explicitly:

```bash
cmake --build --preset release --target benchmark-heads-up-matrix
```

The benchmark contract and CSV fields are documented in
[Benchmarking](benchmarking.md).

Run the RFC check directly with:

```bash
npm run check:rfc
cmake --build --preset debug --target rfc-check
```

## Replay

`node bin/replay.mjs` replays unique actionable snapshots from
`sessions/*.jsonl`. Completion requires:

- zero illegal actions;
- zero unexpected operational fallbacks;
- every bet and raise inside the recorded legal interval.

Replay is read-only, but it uses the currently published
`bin/bigshark-engine`, so run a release build first.

## Documentation Checks

Run the repository documentation check after changing documentation,
source-code comments, or local links:

```bash
node bin/check-docs.mjs
node bin/check-rfcs.mjs
cmake --build --preset debug --target docs-check
cmake --build --preset debug --target rfc-check
```

The documentation check scans first-party text files for CJK characters and
verifies local Markdown links. The RFC check validates metadata, lifecycle,
required sections, decision records, and index membership. Build outputs,
runtime state, session logs, and vendored source are excluded.

## Adding C++ Files

Engine library sources, tests, and solver benchmarks are listed in
`engine/CMakeLists.txt`. `apps/engine-host/CMakeLists.txt` owns the C++ host.
Add implementation files to their owning target. Header-only files do not need
target registration. Owner-local format targets feed the root `format` and
`format-check` aggregates; the root does not maintain source lists.

## Linux Verification

The reproducible Linux lane uses a digest-pinned Node 22 Debian bookworm
image, GCC 12, CMake, Ninja, and the same pinned protocol toolchain:

```bash
docker build \
  -f tools/ci/Dockerfile.linux \
  -t bigshark-linux-verify:stage4 \
  .
docker run --rm bigshark-linux-verify:stage4
```

`tools/ci/verify-linux.sh` runs all protocol and project gates, verifies the
no-HiGHS DCFR fallback, emits the benchmark CSV through a verbose labeled
CTest run, checks dynamic library resolution with `ldd`, and rejects any
CoreFoundation entry in the ELF dynamic section. Private session logs and host
build outputs are excluded from the image.
