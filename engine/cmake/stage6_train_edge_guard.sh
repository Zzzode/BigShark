#!/bin/bash
# RFC 0008 stage 6 trainer edge guard (resolved-include half).
#
# Companion to stage6_train_edge_guard.cmake. A text scan is beatable, so this
# asks the COMPILER to resolve every include to an absolute path (-MMD) and
# enforces a whitelist for the trainer translation units:
#
#   * bs/decision.hpp and bs/policy.hpp are forbidden outright (deployed
#     heuristic vocabulary);
#   * under bs/stage6/, only the policy-free CORE headers the trainer shares
#     with eval are permitted. The adapter, pinned baseline, geometry
#     enumerator, simulator, estimator, oracle, CRN/infoset exact-key, stats,
#     chart digest and the eval-side translator all pull the deployed policy
#     and are forbidden.
#
# Usage: stage6_train_edge_guard.sh <compiler> <std-flag> <engine-include-dir>
#        <stamp> <src>...
# Per-source preprocessor flags are taken from compile_commands.json, exactly
# as the offline boundary guard does.

CXX="$1"; shift
STD="$1"; shift
INCDIR="$1"; shift
STAMP="$1"; shift

SRCROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE_INC="$SRCROOT/engine/include"
BUILDROOT="$(dirname "$STAMP")"
CCJSON="$BUILDROOT/compile_commands.json"

# Returns nonzero when the resolved engine-relative include path is forbidden.
is_forbidden() {
  case "$1" in
    engine/include/bs/decision.hpp) return 0 ;;
    engine/include/bs/policy.hpp) return 0 ;;
    engine/include/bs/stage6/adapter.hpp) return 0 ;;
    engine/include/bs/stage6/baseline_policy.hpp) return 0 ;;
    engine/include/bs/stage6/br_estimator.hpp) return 0 ;;
    engine/include/bs/stage6/chart_digest.hpp) return 0 ;;
    engine/include/bs/stage6/crn_streams.hpp) return 0 ;;
    engine/include/bs/stage6/exact_oracle.hpp) return 0 ;;
    engine/include/bs/stage6/geometry_enumerator.hpp) return 0 ;;
    engine/include/bs/stage6/infoset_key.hpp) return 0 ;;
    engine/include/bs/stage6/simulator.hpp) return 0 ;;
    engine/include/bs/stage6/statistics.hpp) return 0 ;;
    engine/include/bs/stage6/translator.hpp) return 0 ;;
    engine/include/bs/stage6/candidate_policy.hpp) return 0 ;;
    *) return 1 ;;
  esac
}

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
    echo "RFC 0008 stage-6 trainer edge guard: cannot preprocess '$src':" >&2
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
    if is_forbidden "$rel"; then
      echo "RFC 0008 stage-6 trainer edge violation: '$src' resolves a" >&2
      echo "policy/eval-only header the trainer must never reach:" >&2
      echo "  $rel" >&2
      echo "The trainer derives frozen artifacts from pure poker +" >&2
      echo "abstraction + the joint sampler alone." >&2
      rc=1
    fi
  done < <(sed -e 's/\\$//' "$dep" | tr '\n\t' '  ' | tr -s ' ' | tr ' ' '\n')
done

if [ "$rc" -eq 0 ]; then
  echo "Stage 6 trainer edge guard passed" > "$STAMP"
fi
exit "$rc"
