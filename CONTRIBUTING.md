# Contributing to BigShark

## Before You Start

Read `AGENTS.md`, the documentation index in `docs/README.md`, and the relevant
design or protocol reference before changing behavior.

Architecture, protocol, persistence, migration, and cross-module changes
require an RFC under `docs/rfcs/`. Keep changes focused and preserve the
platform-neutral engine boundary.

## Development

Install dependencies and configure the release build:

```bash
npm ci
cmake --preset release
```

Use the repository's existing CMake, TypeScript, and test conventions. Keep
first-party documentation, source comments, identifiers, and commit messages
in English.

## Verification

Run the full gate before opening a pull request:

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
npm run check
npm run proto:check
node bin/replay.mjs
```

Use the `asan` preset for memory, lifetime, bounds, or undefined-behavior
changes. Use `tools/ci/Dockerfile.linux` for the reproducible Linux GCC lane.

## Commits and Pull Requests

- Use Conventional Commits.
- Keep each commit to one logical change.
- Include tests for behavior changes.
- Update design and reference documentation with protocol or operational
  changes.
- Describe remaining risks and any verification that could not be run.

By contributing, you agree that your contributions are licensed under the
Apache License 2.0.
