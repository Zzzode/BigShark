# Security Policy

## Supported Versions

Security fixes are applied to the latest revision of the default branch.

## Reporting a Vulnerability

Do not open a public issue for an unpatched vulnerability or exposed
credential. Use GitHub's private vulnerability reporting feature for this
repository:

<https://github.com/Zzzode/BigShark/security/advisories/new>

Include the affected component, reproduction steps, impact, and any suggested
mitigation. Remove tokens, private game data, and personal information from
the report.

## Secrets

BigShark is a local, platform-neutral engine and ships no game-platform
client or credentials. Never place secrets in source, fixtures, logs,
issues, commits, or chat. Any local configuration or integration that
introduces credentials must keep them outside the repository with
restrictive file permissions.

## Fairness

The engine makes decisions from public cards and the hero's own hole
cards only. It must never infer unrevealed cards, collude, or reveal
private information. The stage-6 offline build guard and the absence of
network clients in first-party code keep this boundary enforceable.
