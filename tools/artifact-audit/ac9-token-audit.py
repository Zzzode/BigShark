#!/usr/bin/env python3
"""RFC 0030 AC9 local pre-publication audit.

Opens every ``*.db`` under an artifacts root and every ``manifest.json`` that
carries ``action_abstraction_parameters``, and requires the canonical
``|max-raises=<unsigned decimal>`` token RFC 0030 adds to the action
abstraction identity. A tokenless or malformed parameter line fails the audit.

This is a local operator gate, intentionally NOT a repository test: the
artifacts tree is gitignored and the audit runs against locally trained
files before publication.

Usage:
    python3 -I tools/artifact-audit/ac9-token-audit.py [artifacts-root]

Exit status: 0 when every inspected identity carries the token, 1 otherwise.
Artifacts without an action-identity column (other SQLite kinds) are reported
as skipped.
"""

import json
import re
import sqlite3
import sys
from pathlib import Path

# The token is appended last to the canonical parameters string with no
# trailing characters (RFC 0030 make_id / make_declared_id).
_TOKEN_RE = re.compile(r"\|max-raises=(\d+)$")


def check_parameters(value: str) -> tuple[bool, str]:
    if not isinstance(value, str):
        return False, "abstraction parameters are not a string"
    match = _TOKEN_RE.search(value)
    if match is None:
        return False, "missing |max-raises=<N> token"
    return True, f"max-raises={match.group(1)}"


def audit_db(path: Path) -> tuple[str, str]:
    """Return (status, detail); status in {PASS, FAIL, SKIP}."""
    try:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    except sqlite3.Error as exc:
        return "FAIL", f"cannot open read-only: {exc}"
    try:
        tables = {r[0] for r in con.execute(
            "select name from sqlite_master where type='table'")}
        if "manifest" not in tables:
            return "SKIP", "no manifest table"
        cols = {r[1] for r in con.execute("pragma table_info(manifest)")}
        if "abstraction_parameters" not in cols:
            return "SKIP", "no abstraction_parameters column"
        rows = con.execute("select abstraction_parameters from manifest").fetchall()
        if not rows:
            return "FAIL", "manifest table has no identity row"
        reports = []
        for (params,) in rows:
            ok, detail = check_parameters(params or "")
            if not ok:
                return "FAIL", detail
            reports.append(detail)
        return "PASS", ", ".join(sorted(set(reports)))
    except sqlite3.Error as exc:
        return "FAIL", f"sqlite error: {exc}"
    finally:
        con.close()


def audit_manifest(path: Path) -> tuple[str, str]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return "FAIL", f"cannot parse: {exc}"
    if not isinstance(data, dict):
        return "SKIP", "manifest is not an object"
    params = data.get("action_abstraction_parameters")
    if params is None:
        # Preflop profile manifests render a compact name:version:digest and a
        # numeric max_raises_per_street; the authoritative tokenized identity
        # lives in the profile .db audited separately.
        return "SKIP", "parameters audited in the .db"
    ok, detail = check_parameters(params)
    return ("PASS" if ok else "FAIL"), detail


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else "artifacts")
    if not root.is_dir():
        print(f"artifacts root not found: {root}", file=sys.stderr)
        return 1

    dbs = sorted(root.rglob("*.db"))
    manifests = sorted(root.rglob("manifest.json"))

    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    failures = []

    print(f"RFC 0030 AC9 audit under {root}")
    for db in dbs:
        status, detail = audit_db(db)
        counts[status] += 1
        print(f"  [{status}] db       {db.relative_to(root)}: {detail}")
        if status == "FAIL":
            failures.append(str(db))
    for manifest in manifests:
        status, detail = audit_manifest(manifest)
        counts[status] += 1
        print(f"  [{status}] manifest {manifest.relative_to(root)}: {detail}")
        if status == "FAIL":
            failures.append(str(manifest))

    print(f"\ntotal {sum(counts.values())}: "
          f"{counts['PASS']} pass, {counts['FAIL']} fail, {counts['SKIP']} skipped")
    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    print("every tokenized action identity carries an explicit |max-raises= token")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
