---
name: "rfc-authoring"
description: "Creates and reviews BigShark RFCs. Invoke for architecture, protocol, persistence, migration, or cross-module design changes before implementation."
---

# BigShark RFC Authoring

Use this skill for material engineering decisions that need review before
implementation.

## Mandatory Triggers

Invoke this skill when a request changes any of the following:

- cross-module architecture or dependency direction;
- an external or engine-facing protocol;
- a persisted data or model artifact format;
- a platform adapter boundary;
- a solver architecture or production strategy model;
- a long-lived extension point;
- a migration with compatibility or rollback requirements;
- repository-wide build, deployment, or observability policy.

Do not invoke it for a local bug fix, isolated implementation detail, or
documentation correction that preserves all established contracts.

## Required Inputs

Before drafting:

1. Read `AGENTS.md`.
2. Read `docs/rfcs/README.md` and `docs/rfcs/0000-template.md`.
3. Read all RFCs that overlap the proposed ownership or contract.
4. Inspect the current implementation and tests. Do not describe assumptions
   as current behavior.
5. Identify the owning modules, affected callers, compatibility surface, and
   verification commands.

## RFC Lifecycle

Use exactly these statuses:

```text
Draft -> Proposed -> Accepted -> Implementing -> Implemented
                    \-> Rejected
Any terminal or active RFC -> Superseded
```

- `Draft`: incomplete and not ready for a decision.
- `Proposed`: complete and ready for explicit maintainer review.
- `Accepted`: explicitly approved by a maintainer; implementation may start.
- `Implementing`: at least one implementation change has landed or is in
  progress.
- `Implemented`: every acceptance criterion is verified and documentation is
  current.
- `Rejected`: explicitly declined.
- `Superseded`: replaced by another numbered RFC.

The authoring agent may set `Draft` or `Proposed`. It must never infer
`Accepted`, `Rejected`, or `Implemented`. Those transitions require explicit
maintainer approval or objective implementation evidence as defined by the
RFC.

## Authoring Workflow

1. Classify the change and confirm that an RFC is required.
2. Reserve the next four-digit RFC number. Never reuse a number.
3. Copy the metadata and sections from `docs/rfcs/0000-template.md`.
4. Write current-state evidence before proposing a design.
5. Define goals, non-goals, ownership, dependency rules, and invariants.
6. Specify contracts precisely enough for independent implementation.
7. Compare credible alternatives, including the simplest viable option.
8. Define compatibility, staged migration, rollout, rollback, and observability.
9. Define measurable acceptance criteria and project-native verification.
10. Record unresolved decisions under Open Questions.
11. Set the RFC to `Proposed`.
12. Run `node bin/check-rfcs.mjs` and `node bin/check-docs.mjs`.
13. Present the RFCs for review and stop before implementation.
14. After explicit approval, update status to `Accepted`, record the decision,
    and produce an implementation plan.

## Quality Standard

An RFC must:

- separate verified current behavior from proposed behavior;
- identify every module whose ownership changes;
- define dependency direction and forbidden dependencies;
- avoid speculative interfaces unsupported by a concrete use case;
- state protocol or data semantics without ambiguous units;
- include failure behavior, security boundaries, and observability;
- provide migration stages that remain independently reversible;
- define exact acceptance criteria and verification evidence;
- document unresolved risks rather than hiding them;
- be understandable without reading the originating conversation.

## Review Checklist

Challenge the proposal on:

- whether the expected value justifies the complexity;
- whether a smaller local solution satisfies the requirement;
- whether responsibilities belong in the proposed modules;
- whether generated code and source-of-truth files are clearly distinguished;
- whether compatibility and rollback are realistic;
- whether protocol versioning and unknown-field behavior are defined;
- whether performance claims have a benchmark plan;
- whether security and fairness boundaries are preserved;
- whether implementation can be staged without a flag day.

Record material findings in the RFC or resolve them before requesting approval.

## Repository Conventions

- RFC files live under `docs/rfcs/`.
- File names use `NNNN-short-slug.md`.
- All RFCs and source-code comments are written in English.
- Use Mermaid for architecture diagrams.
- Current implementation truth belongs in `docs/architecture`,
  `docs/design`, and `docs/reference`.
- Proposed behavior belongs in RFCs until implemented.
- Update `docs/rfcs/README.md` and `docs/README.md` for every new RFC.
- Run the full project verification required by `AGENTS.md` after
  implementation.

## Prohibited Shortcuts

- Do not mark an RFC accepted because the design looks reasonable.
- Do not implement a proposed architecture before explicit approval.
- Do not hide breaking changes behind adapters or compatibility shims without
  documenting their lifetime and removal plan.
- Do not mix unrelated cleanup into an RFC implementation.
- Do not use generated artifacts as the protocol source of truth.
- Do not duplicate one contract in multiple independently edited formats.
