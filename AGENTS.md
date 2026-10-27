# BigShark Agent Instructions

BigShark is a platform-neutral C++23 no-limit Texas Hold'em decision and GTO
engine. It builds strategy artifacts, serves decisions over a local framed
Protobuf protocol, and provides an offline human-vs-bot practice table. There
is no game-platform client in the repository: the River Club Agent API v2.1
integration was retired by [RFC 0040](docs/rfcs/0040-retire-river-club-agent-api.md)
when the provider discontinued agent support. All work here is offline engine,
tooling, and documentation work.

## Strategy Core

The C++23 engine under `engine/` is the only strategy implementation. Do not
introduce parallel heuristic strategy cores in another language; operational
fallbacks must stay inside the engine and be labelled for what they are.

## No Legacy Code

This project does not maintain versioned implementations or backward-
compatibility fallbacks. When a new approach replaces an old one, the old
code, tests, documentation, and configuration are deleted in the same change.

- Do not introduce feature flags to select between old and new implementations.
- Do not keep deprecated code paths "just in case."
- Do not version internal concepts (v0, v1, v2, etc.). A transport protocol's
  version field is a transport detail, not a project versioning scheme.
- Aggressive refactoring is the default: replace, do not accumulate.
- External protocol versions are outside our control and are referenced as-is.

## Engine Build Contract

Use the root `CMakePresets.json` for every C++ build:

- `debug` for local debugging and assertions;
- `release` for publishing;
- `asan` for AddressSanitizer and UndefinedBehaviorSanitizer.

After any C++ change, run:

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target benchmark-multistreet
npm run check
npm run proto:check
```

Use `asan` for memory, lifetime, bounds, and undefined-behavior changes.
Detailed architecture and verification requirements are in
`docs/design/gto-engine.md` and `docs/development/build-and-test.md`.

## Offline Boundary

First-party code performs no game-network I/O and reads no game-platform
credentials. Engine decisions use public cards and the hero's own hole cards
only; never infer unrevealed cards. The stage-6 offline build guard enforces
this at CMake configure time. The offline practice CLI is
`bigshark-practice` (`engine/benchmarks/practice_simulator.cpp`); run it with
`--hands N` for non-interactive smoke runs.

## Documentation Rules

- All first-party documentation and source-code comments must be in English.
- `docs/README.md` is the documentation index and status registry.
- Update design and reference documents in the same change as behavior,
  protocol, build, or operational changes.
- Use `Current` for implemented behavior, `Proposed` for planned behavior,
  and `Historical` for obsolete records.
- Cross-module architecture, external contract, and persisted-format changes
  require an RFC under `docs/rfcs/`.
- Use Mermaid for diagrams.
- Run `node bin/check-docs.mjs` after changing documentation or comments.
- Use `.codex/skills/rfc-authoring/SKILL.md` for every RFC-triggering change,
  and run `node bin/check-rfcs.mjs` before independent approval-agent review.
- RFC approval requires a separately launched agent that did not author the
  proposal; user confirmation is not required. Record its explicit decision
  and resolve blocking findings before implementation. Follow
  `docs/rfcs/README.md`; this delegation does not authorize spending,
  credentials, live play, or unrelated destructive operations.

## Repository Map

- `docs/README.md`: documentation index and policy
- `docs/architecture/system-overview.md`: implemented component boundaries
- `docs/design/gto-engine.md`: C++ strategy and solver implementation
- `docs/reference/protobuf-engine-protocol.md`: engine protocol IDL and generation contract
- `docs/development/build-and-test.md`: presets, formatting, tests, and release
- `docs/development/practice-table.md`: offline practice CLI
- `docs/rfcs/`: architecture proposals and RFC process
- `.codex/skills/rfc-authoring/SKILL.md`: RFC authoring and review workflow
- `strategy/PLAYBOOK.md`: provider-neutral baseline poker decision framework
- `strategy/table-notes.md`: durable opponent and table observations
- `.codex/skills/`: exploitative style selector and style modules
- `engine/`: C++23 poker, solver, policy, service, tests, and solver benchmarks
- `apps/engine-host/`: C++ process composition root
- `clients/node/`: generic TypeScript engine process client
- `proto/`: authoritative engine protocol IDL, Buf lock, baseline, and vectors
- `tools/`: TypeScript source-policy/RFC/docs tooling and native CI tooling
- `bin/`: documentation/RFC checker launchers and the published engine
  executable (`bin/bigshark-engine`, gitignored)
- `sessions/`: local, ignored historical journals (no tracked reader)
