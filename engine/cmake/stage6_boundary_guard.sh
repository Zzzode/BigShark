#!/bin/bash
# RFC 0008 stage 6 offline boundary guard (resolved-include half).
#
# Companion to stage6_offline_guard.cmake. A text scan of include directives is
# beatable by macro-pasted includes and shadow headers, so instead this guard
# asks the COMPILER to resolve every include to an absolute path (-MMD) and
# fails if a guarded source pulls in engine/include/bs/behavior_policy.hpp or
# any engine/include/bs/stage6/* header -- the offline measurement surface.
#
# Usage: stage6_boundary_guard.sh <compiler> <std-flag> <engine-include-dir>
#        <stamp> <src>...
# Per-source preprocessor flags (engine/src and generated protobuf include
# dirs) are taken from the build's compile_commands.json, which carries
# absolute -I paths, so the check is independent of the caller's cwd.

CXX="$1"; shift
STD="$1"; shift
INCDIR="$1"; shift
STAMP="$1"; shift

# engine/cmake -> engine -> repository root (exactly two levels).
SRCROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE_INC="$SRCROOT/engine/include"
BUILDROOT="$(dirname "$STAMP")"
CCJSON="$BUILDROOT/compile_commands.json"

# Forbidden when this returns NONZERO (shell-false), mirroring the L3 guard's
# is_allowed convention.
is_forbidden() {
  case "$1" in
    engine/include/bs/behavior_policy.hpp) return 1 ;;
    engine/include/bs/stage6/*) return 1 ;;
    *) return 0 ;;
  esac
}

# Prints the extra preprocessor flags (-I/-D/-std/-isystem/--target) recorded
# for $1 in compile_commands.json. Empty when the source is not listed.
extra_flags() {
  [ -f "$CCJSON" ] || return 0
  CCJSON="$CCJSON" SRC="$1" python3 - <<'PY'
import json, os, shlex, sys
want = os.path.realpath(os.environ["SRC"])
try:
    db = json.load(open(os.environ["CCJSON"]))
except Exception:
    sys.exit(0)
for entry in db:
    if os.path.realpath(entry.get("file", "")) != want:
        continue
    tokens = shlex.split(entry["command"]) if "command" in entry else entry.get("arguments", [])
    keep = []
    skip_next = False
    for t in tokens:
        if skip_next:
            keep.append(t)
            skip_next = False
            continue
        if t in ("-I", "-D", "-isystem", "--target"):
            keep.append(t)
            skip_next = True
        elif t.startswith(("-I", "-D", "-std=", "-isystem", "--target")):
            keep.append(t)
    print(" ".join(shlex.quote(t) for t in keep))
    break
PY
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
rc=0

for src in "$@"; do
  dep="$tmpdir/$(echo "$src" | tr '/ ' '__').d"
  extras="$(extra_flags "$src")"
  # shellcheck disable=SC2086
  if ! eval "\"\$CXX\" $extras \"\$STD\" -I \"\$INCDIR\" -MMD -MF \"\$dep\" -c \"\$src\" -o /dev/null" >/dev/null 2>"$dep.err"; then
    echo "RFC 0008 stage-6 offline guard: cannot preprocess '$src' for the dependency check:" >&2
    cat "$dep.err" >&2
    rc=1
    continue
  fi
  while read -r tok; do
    [ -z "$tok" ] && continue
    case "$tok" in
      *:*) continue ;;
    esac
    abspath="$tok"
    case "$abspath" in
      /*) ;;
      *) abspath="$SRCROOT/$abspath" ;;
    esac
    case "$abspath" in
      "$ENGINE_INC"/*) ;;
      *) continue ;;
    esac
    rel="${abspath#"$SRCROOT"/}"
    if ! is_forbidden "$rel"; then
      echo "RFC 0008 stage-6 offline boundary violation: '$src' resolves the" >&2
      echo "offline measurement header:" >&2
      echo "  $rel" >&2
      echo "The decision service and v0/v1 protocols must not link or include" >&2
      echo "the behavior/stage-6 offline surface." >&2
      rc=1
    fi
  done < <(sed -e 's/\\$//' "$dep" | tr '\n\t' '  ' | tr -s ' ' | tr ' ' '\n')
done

if [ "$rc" -eq 0 ]; then
  echo "Stage 6 offline boundary guard passed" > "$STAMP"
fi
exit "$rc"
