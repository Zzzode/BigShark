#!/usr/bin/env python3
"""RFC 0037 shape-policy local pre-publication audit.

Companion to ac9-token-audit.py. The AC9 audit checks that every action
identity CARRIES an explicit ``|max-raises=N|`` token; this audit checks
that the token's VALUE is the production cap the RFC 0037 shape policy
assigns to the artifact's shape, and that a BSFLIB03 library does not mix
roots whose shapes require different caps (the file header stores one
file-level action identity).

The frozen table (shape-policy-v1) mirrors ``production_raise_cap`` in
engine/src/abstraction/abstraction.cpp; the native exhaustive table test is
authoritative and this standalone copy exists so the audit runs without a
build. Bands: shallow <= 16 BB, standard 17..100 BB.

Tri-state:
  PASS       exit 0, current revision, every shape agrees.
  SUPERSEDED exit 0, manifest explicitly declares an older
             ``shape_policy_revision`` (migration window warning).
  FAIL       exit 1, same-revision mismatch, a mixed-cap library, an
             unparseable shape, or a missing cap token.

Like AC9 this is a local operator gate over the gitignored artifacts tree,
never a repository test.

Usage:
    python3 -I tools/artifact-audit/rfc0037-policy-audit.py [artifacts-root]
"""

import json
import re
import sys
from pathlib import Path

POLICY_REVISION = "shape-policy-v1"
_TOKEN_RE = re.compile(r"\|max-raises=(\d+)$")

# Frozen TRAINING table [players - 2][band] (0=shallow, 1=standard).
# Artifacts train under this table; board-3 serving has two measured
# one-raise deferrals that live in the resolver, not in artifacts.
TRAINING_CAP = [
    [0, 0],  # 2 players (heads-up uncapped)
    [1, 1],  # 3 players
    [1, 1],  # 4 players
    [1, 1],  # 5 players
    [1, 1],  # 6 players
]


def band_for_bb(stack_bb: int) -> str | None:
    if stack_bb <= 16:
        return "shallow"
    if stack_bb <= 100:
        return "standard"
    return None  # deep: unsupported


def required_cap(players: int, stack_bb: int) -> int | None:
    band = band_for_bb(stack_bb)
    if band is None or players < 2 or players > 6:
        return None
    return TRAINING_CAP[players - 2][0 if band == "shallow" else 1]


def cap_from_parameters(params: str) -> int | None:
    match = _TOKEN_RE.search(params or "")
    return int(match.group(1)) if match else None


def verdict_for_revision(manifest: dict) -> str:
    rev = manifest.get("shape_policy_revision")
    if rev is not None and rev != POLICY_REVISION:
        return "SUPERSEDED"
    return "PASS"


def audit_preflop_profile(path: Path, data: dict) -> tuple[str, str]:
    base = verdict_for_revision(data)
    players = data.get("player_count")
    stack_bb = data.get("stack_bb")
    stored = data.get("max_raises_per_street")
    if not isinstance(players, int) or not isinstance(stack_bb, int) or not isinstance(stored, int):
        return "FAIL", "manifest lacks numeric player_count/stack_bb/max_raises_per_street"
    expected = required_cap(players, stack_bb)
    if expected is None:
        return "FAIL", f"unsupported shape: {players} players, {stack_bb} BB"
    if stored != expected:
        return "FAIL", f"{players}p {stack_bb} BB requires cap {expected}, manifest has {stored}"
    if base == "SUPERSEDED":
        return "SUPERSEDED", f"older revision; current cap {expected} agrees"
    return "PASS", f"{players}p {stack_bb} BB -> cap {expected}"


def audit_flop_library(path: Path, data: dict) -> tuple[str, str]:
    base = verdict_for_revision(data)
    params = data.get("action_abstraction_parameters")
    stored = cap_from_parameters(params if isinstance(params, str) else "")
    if stored is None:
        return "FAIL", "file-level action identity missing |max-raises= token"
    big_blind = data.get("big_blind")
    roots = data.get("served_roots")
    if not isinstance(big_blind, int) or not isinstance(roots, list) or not roots:
        return "FAIL", "manifest lacks numeric big_blind or a non-empty served_roots list"

    required: set[int] = set()
    for root in roots:
        live = root.get("live_count")
        stacks = root.get("stacks")
        contributions = root.get("contributions")
        if (not isinstance(live, int) or not isinstance(stacks, list)
                or not isinstance(contributions, list) or not stacks):
            return "FAIL", f"root {root.get('root_id', '?')} has an unparseable shape"
        # Root stacks are chips behind at the flop; starting stack adds the
        # contribution. Deepest effective stack sets the band.
        try:
            max_start = max(int(s) + int(c) for s, c in zip(stacks, contributions))
        except (TypeError, ValueError):
            return "FAIL", f"root {root.get('root_id', '?')} has non-numeric stacks"
        stack_bb = max_start // big_blind
        need = required_cap(live, stack_bb)
        if need is None:
            return "FAIL", f"root {root.get('root_id', '?')} unsupported shape: {live} live, {stack_bb} BB"
        if need != stored:
            return ("FAIL", f"root {root.get('root_id', '?')} {live}-live {stack_bb} BB requires "
                           f"cap {need}, file header carries cap {stored}")
        required.add(need)
    if len(required) > 1:
        return "FAIL", f"roots require differing caps {sorted(required)} under one file identity"
    if base == "SUPERSEDED":
        return "SUPERSEDED", f"older revision; {len(roots)} root(s), cap {stored} agrees"
    return "PASS", f"{len(roots)} root(s), {len(required)} distinct required cap ({stored})"


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else "artifacts")
    if not root.is_dir():
        print(f"artifacts root not found: {root}", file=sys.stderr)
        return 1

    counts = {"PASS": 0, "SUPERSEDED": 0, "FAIL": 0}
    failures = []

    print(f"RFC 0037 shape-policy audit ({POLICY_REVISION}) under {root}")
    for manifest_path in sorted(root.rglob("manifest.json")):
        try:
            data = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            status, detail = "FAIL", f"cannot parse: {exc}"
        else:
            if not isinstance(data, dict):
                status, detail = "FAIL", "manifest is not an object"
            elif "served_roots" in data:
                status, detail = audit_flop_library(manifest_path, data)
            elif "player_count" in data and "stack_bb" in data:
                status, detail = audit_preflop_profile(manifest_path, data)
            else:
                status, detail = "PASS", "no policy-shaped artifact (ignored)"
        counts[status] += 1
        print(f"  [{status}] {manifest_path.relative_to(root)}: {detail}")
        if status == "FAIL":
            failures.append(str(manifest_path))

    print(f"\ntotal {sum(counts.values())}: {counts['PASS']} pass, "
          f"{counts['SUPERSEDED']} superseded-warning, {counts['FAIL']} fail")
    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    print("every artifact's stored cap matches the shape policy for its shape")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
