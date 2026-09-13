---
rfc: "0003"
subject: "Strict TypeScript Application and Integration Layer"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-09-13"
updated: "2026-09-13"
owners: "applications, platform integrations, engine clients, developer tools"
supersedes: ""
superseded-by: ""
---

# RFC 0003: Strict TypeScript Application and Integration Layer

## Summary

Require strict TypeScript for every first-party application, platform adapter,
engine client, and interactive developer tool. JavaScript under `bin/` is
limited to minimal compatibility launchers that load compiled output from
`dist/`.

## Motivation

Frontend-triggered and platform-triggered gameplay crosses untrusted network
data, state-machine transitions, legal-action constraints, timeouts, process
boundaries, and large chip amounts. Plain JavaScript does not provide
compile-time guarantees for these contracts.

The first RFC 0001 extraction introduced typed ownership boundaries but began
with `.mjs` files to preserve behavior. Continuing that approach would make
platform adapters and generated Protobuf clients weakly typed at precisely the
boundary where correctness matters most.

## Goals

- Make TypeScript the source language for `apps/`, `platforms/`, `clients/`,
  and interactive `tools/`.
- Enable strict type checking with no implicit `any`.
- Model River state, engine v0 compatibility, actions, and process failures
  explicitly.
- Prepare the Node layer for generated Protobuf types and `bigint` amounts.
- Compile production JavaScript into an ignored `dist/` directory.
- Keep existing `bin/` command paths stable through minimal launchers.
- Make type checking and Node tests native CMake and CTest gates.

## Non-Goals

- Rewriting the C++ engine in TypeScript.
- Adding a frontend application in this RFC.
- Defining frontend UI architecture.
- Replacing Protobuf-generated types with handwritten TypeScript interfaces.
- Removing compatibility launchers before RFC 0001 packaging stages.
- Changing River Club runtime behavior or engine decisions.

## Current State and Evidence

The repository currently contains first-party `.mjs` implementation under:

- `apps/river-club-agent`;
- `clients/node`;
- `platforms/river-club`;
- `tools`.

The files perform JSON parsing, process management, network requests,
state-machine transitions, action validation, and file coordination without
compile-time type checks.

`bin/` has already been reduced to thin launchers for most commands. Golden
River v0 fixtures and Node process-client tests provide a behavioral baseline
for the language migration.

## Design Principles

- Types model trusted internal state after explicit boundary parsing.
- External JSON starts as `unknown`.
- Type assertions are confined to parsers and validated boundaries.
- Domain unions replace stringly typed branches where practical.
- Optional values remain explicit.
- Runtime validation remains necessary; TypeScript does not replace it.
- Compiled JavaScript is generated output, not handwritten source.
- Compatibility launchers contain no business logic.

## Proposed Design

### Source and output

```text
apps/**/*.ts
clients/**/*.ts
platforms/**/*.ts
tools/**/*.ts
       |
       | tsc
       v
dist/apps/**/*.js
dist/clients/**/*.js
dist/platforms/**/*.js
dist/tools/**/*.js
```

`dist/` is ignored by Git. `bin/` launchers import the corresponding compiled
JavaScript file.

### Compiler policy

The root `tsconfig.json` enables:

- `strict`;
- `noImplicitAny`;
- `noUncheckedIndexedAccess`;
- `exactOptionalPropertyTypes`;
- `useUnknownInCatchVariables`;
- `noImplicitOverride`;
- `noFallthroughCasesInSwitch`;
- Node ESM output.

Source imports use `.js` specifiers so emitted ESM resolves correctly.

### Boundary types

River Club defines explicit types for:

- public state responses;
- room, hero, seat, event, solver, and legal action views;
- CLI success and error responses;
- normalized v0 engine context;
- engine decisions;
- runner configuration and runtime files.

The engine process client is generic over request and response payload types.
The future Protobuf client replaces handwritten wire types with generated
types while preserving a typed process-client interface.

### Launchers

Tracked launchers may remain JavaScript only when all conditions hold:

- no domain or platform logic;
- no data transformation;
- no branching beyond reporting a missing build artifact;
- no more than a few lines;
- target compiled output under `dist/`.

### Build integration

Project-native commands are:

```bash
npm run typecheck
npm run build:ts
npm run test:node
```

CMake exposes:

- `typescript-build`;
- `typescript-check`;
- `node-check`.

CTest runs strict type checking and Node tests.

## Dependency Rules

Permitted:

```text
apps -> platforms -> clients
tools -> public platform or client APIs
TypeScript boundary parsers -> external unknown data
```

Forbidden:

- handwritten `.js` or `.mjs` business implementation under `apps/`,
  `clients/`, `platforms/`, or interactive `tools/`;
- `any` in production source without a documented, localized interoperability
  reason;
- platform adapter types leaking into the generic engine client;
- source imports from `dist/`;
- manual edits to compiled output;
- compatibility launchers importing TypeScript source directly in production.

## Compatibility and Migration

1. Add the strict compiler configuration and pinned TypeScript toolchain.
2. Rename extracted modules and tests from `.mjs` to `.ts`.
3. Introduce boundary types and fix all strict diagnostics.
4. Compile to `dist/`.
5. Point existing `bin/` launchers to compiled JavaScript.
6. Register type checking and Node tests in CMake and CTest.
7. Preserve command output, exit status, fixtures, and replay behavior.

No River Club command or engine v0 message changes.

## Security and Operational Impact

- External JSON remains runtime-validated; TypeScript alone is not trusted.
- Secret-bearing config types never appear in logs.
- Strong action unions reduce invalid action construction.
- Exact Protobuf amounts will use `bigint` in the future Node client.
- Compiled output must not embed credentials or local absolute paths.
- Production launchers fail clearly when `dist/` has not been built.

## Alternatives Considered

### Keep JavaScript with JSDoc

Rejected. It improves editor hints but provides weaker enforcement and an
inconsistent foundation for generated Protobuf types.

### Execute TypeScript directly with Node type stripping

Rejected for production. It raises the minimum runtime version, supports only
erasable syntax, and makes production execution depend on runtime TypeScript
behavior.

### Use `tsx` in production

Rejected. Runtime transpilation adds a production dependency and startup
behavior that a compiled application does not need.

### Bundle applications

Deferred. Bundling may help packaging later, but `tsc` output is sufficient
for the current local process deployment.

## Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Strict conversion expands the refactor | Delayed module extraction | Convert one ownership boundary at a time |
| Types mirror unstable provider JSON | Maintenance overhead | Confine types and parsing to River adapter |
| Compiled output becomes stale | Runtime mismatch | Build through CMake and test launcher targets |
| Launchers fail before build | Poor onboarding | Document and verify `npm run build:ts` |
| Type assertions hide invalid input | Runtime bugs | Restrict assertions to parsers and keep fixture tests |

## Verification Plan

- `tsc --noEmit` with all strict options.
- TypeScript build from a clean `dist/`.
- Node process-client tests against a fake engine.
- River golden context and decision tests.
- Compatibility launcher output and exit-status tests.
- Search gate rejecting implementation `.mjs` files outside `bin/`.
- Existing Release, ASan, RFC, documentation, and replay gates.

## Rollout Plan

1. Add TypeScript configuration and build integration.
2. Convert the extracted engine client and its tests.
3. Convert River normalizer and adapter tests.
4. Convert River CLI, journal, runner, and operational commands.
5. Convert replay and maintenance tools.
6. Switch launchers to compiled output.
7. Remove obsolete `.mjs` implementation.

## Rollback Plan

The golden River fixtures remain authoritative. Until all launchers point to
compiled TypeScript, each boundary can temporarily restore its former `.mjs`
implementation. After the switch, rollback restores the prior launchers and
source files without changing C++ or River protocol behavior.

## Open Questions

- Whether frontend UI code, when introduced, uses the same TypeScript project
  or a separate workspace configuration.
- Whether packaging later adds a bundler for distributable applications.

## Acceptance Criteria

- Strict TypeScript is the canonical source for applications, adapters,
  clients, and interactive tools.
- No business logic remains in JavaScript compatibility launchers.
- Type checking, build, and Node tests are project-native gates.
- River golden fixtures and historical replay preserve decisions.
- Generated output is ignored and reproducible.
- RFC 0002 generated Node types can integrate without relaxing compiler
  strictness.

Implementation evidence:

- `tsconfig.json` enables every accepted strict compiler option.
- TypeScript is the only handwritten implementation language under `apps/`,
  `clients/`, `platforms/`, and interactive `tools/`.
- `tools/typescript/check-source-policy.ts` enforces the source-language rule.
- `bin/` launchers load compiled `dist/` output and report a missing build.
- CMake exposes `typescript-build`, `typescript-check`, and `node-check`;
  CTest runs TypeScript and Node gates.
- Twenty-five Node tests cover the process client, timeout restart, launchers,
  River runtime parsing, River v0 golden decisions, unsupported river lines,
  and runner state transitions.
- Every TypeScript build removes stale `dist/` output before compilation.
- Debug, Release, and ASan/UBSan CTest suites pass. Historical replay covers
  156 decisions with zero illegal actions and zero operational fallbacks.

## Decision

Approved by: BigShark project maintainer

Decision date: 2026-09-13

Approved by direct maintainer instruction. Frontend-triggered and
platform-triggered gameplay must use a strict TypeScript application layer;
plain JavaScript is limited to compatibility launchers without business logic.
