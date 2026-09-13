# BigShark Documentation

Status: Current

This directory is the entry point for BigShark architecture, design, protocol,
integration, and development documentation.

## Documentation Map

| Area | Document | Status | Purpose |
| --- | --- | --- | --- |
| Architecture | [System overview](architecture/system-overview.md) | Current | Components, boundaries, data flow, and runtime safety |
| Design | [GTO engine](design/gto-engine.md) | Current | Implemented decision logic, solvers, range tracking, and limitations |
| Reference | [Engine protocol](reference/engine-protocol.md) | Current | NDJSON request and response contract used by the C++ process |
| Reference | [Protobuf engine protocol](reference/protobuf-engine-protocol.md) | Current | Implemented v1 IDL, pinned generation, compatibility baseline, and conformance vectors |
| Integration | [River Club](integrations/river-club.md) | Current | River Club adapter, state machine, and operational mapping |
| Development | [Build and test](development/build-and-test.md) | Current | CMake presets, formatting, tests, sanitizers, and release publishing |
| Development | [Benchmarking](development/benchmarking.md) | Current | Reproducible solver-quality fixtures, metrics, and thresholds |
| RFC | [RFC process](rfcs/README.md) | Current | How architecture proposals are written and accepted |
| RFC | [RFC template](rfcs/0000-template.md) | Draft | Required structure for new proposals |
| RFC | [RFC 0001: Engineering architecture](rfcs/0001-engineering-architecture.md) | Implementing | Repository, module ownership, dependency rules, and staged migration |
| RFC | [RFC 0002: Protobuf engine protocol](rfcs/0002-protobuf-engine-protocol.md) | Implementing | Typed messages, framing, compatibility, and generated bindings |
| RFC | [RFC 0003: Strict TypeScript application layer](rfcs/0003-typescript-application-layer.md) | Implemented | Strongly typed applications, adapters, clients, tools, and compiled launchers |
| Plan | [RFC 0001 and 0002 implementation](plans/0001-0002-implementation.md) | Current | Ordered stages, gates, rollback points, and verification |
| Strategy | [Poker playbook](../strategy/PLAYBOOK.md) | Current | Baseline poker decisions and bankroll discipline |
| Strategy | [Style selector](../.codex/skills/INDEX.md) | Current | Exploitative style selection and overrides |
| Historical | [River Club v1 state notes](state-schema.md) | Historical | Observed protocol v1 fields retained for comparison |
| Upstream | [River Club skill](SKILL.md) | Upstream | Official operating contract |
| Upstream | [River Club strategy](upstream-STRATEGY.md) | Upstream | Official decision guidance |
| Upstream | [River Club OpenAPI](openapi.json) | Upstream | Official HTTP protocol definition |

## Document Status

- **Current** describes behavior implemented in the repository.
- **Proposed** describes a design that has not been fully implemented.
- **Historical** records obsolete behavior for debugging or migration context.
- **Upstream** is an imported external source and must track its authoritative
  origin.

Every design document and RFC must declare its status near the title. Proposed
behavior must not be described as if it already exists.

## Documentation Rules

1. All first-party documentation and source-code comments must be written in
   English.
2. Preserve exact protocol identifiers, commands, file names, card notation,
   and required external string literals.
3. Update the relevant design or reference document in the same change as a
   behavior, protocol, build, or operational change.
4. Keep implementation truth in design and reference documents. Keep future
   architecture in RFCs until it is accepted and implemented.
5. Prefer links over duplicated explanations. The root `README.md` is an
   introduction; this file is the documentation index.
6. Use Mermaid for architecture and workflow diagrams.

## RFC Workflow

Cross-module architecture, protocol changes, persisted-format changes, and new
platform abstractions require an RFC. Follow [the RFC process](rfcs/README.md).
An accepted RFC still describes intent until its status becomes `Implemented`.
Use the repository RFC skill at
`.codex/skills/rfc-authoring/SKILL.md` when drafting or reviewing one.
