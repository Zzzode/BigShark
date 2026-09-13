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

## Credential Handling

River Club Agent Tokens must exist only in
`~/.config/river-club-agent/config.json` with mode `0600`, or in the
`RIVER_CLUB_TOKEN` environment variable. Never place credentials in source,
fixtures, logs, issues, commits, or chat.

If a token may have been exposed, revoke it through River Club before
continuing.
