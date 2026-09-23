#!/bin/bash
# RFC 0008 L3 source boundary guard (compiler-resolved dependency closure).
#
# A regex scan of include directives is fundamentally beatable by the
# preprocessor: macro-pasted includes (#define H(a,b) <a##b>), line splices
# (#inc\<newline>lude), digraphs (%:include), form feed/vertical tab, no-space
# (#include<...>), #import, and shadow headers all pass a text scan and
# compile. Instead this guard asks the COMPILER to resolve every include to an
# absolute path (-MMD -MF), then enforces that the resolved set of PROJECT
# headers pulled into an L3 source is within the L1/L2 allowlist. The
# header-only inline evaluators (eval/equity) and any solver/storage/transport
# header are therefore unreachable by any spelling that actually compiles.
#
# Usage: l3_boundary_guard.sh <compiler> <std-flag> <include-dir> <stamp> <src>...
# Exits nonzero on the first disallowed resolved header.

CXX="$1"; shift
STD="$1"; shift
INCDIR="$1"; shift
STAMP="$1"; shift

# engine/cmake -> engine -> repository root (exactly two levels; three
# overshoots into the parent directory and made the ENGINE_INC prefix never
# match, so the guard passed vacuously under absolute CMake paths).
SRCROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE_INC="$SRCROOT/engine/include"

# Repo-relative project headers L3 may depend on (resolved, so transitive
# pulls are included automatically). L1 rules + L2 abstraction + L3's own
# header. game_definition.hpp transitively pulls heads_up.hpp (shared
# Action/LegalActions types) and settlement.hpp; those are L1 and allowed.
ALLOW='
engine/include/bs/abstract_tree.hpp
engine/include/bs/abstraction.hpp
engine/include/bs/game_definition.hpp
engine/include/bs/heads_up.hpp
engine/include/bs/settlement.hpp
'

is_allowed() {
  case "$ALLOW" in
    *"
$1
"*) return 0 ;;
  esac
  return 1
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
rc=0

for src in "$@"; do
  dep="$tmpdir/$(echo "$src" | tr '/ ' '__').d"
  # Resolve includes; -MMD emits only user (non-system) headers, which is
  # exactly the closure we police (standard library/system headers are out).
  if ! "$CXX" "$STD" -I "$INCDIR" -MMD -MF "$dep" -c "$src" -o /dev/null >/dev/null 2>"$dep.err"; then
    echo "RFC 0008 L3 boundary guard: cannot preprocess '$src' for the dependency check:" >&2
    cat "$dep.err" >&2
    rc=1
    continue
  fi
  # The .d file is "target: h1 h2 ..." with backslash-newline continuations.
  # Remove the continuation backslashes, collapse every whitespace run, and
  # read one resolved path per token (skipping the make "target:"). Process
  # substitution (not a pipe) keeps rc assignments in this shell.
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
    # Only police headers that resolve inside the engine include tree;
    # anything outside (system headers emitted despite -MMD, third_party not
    # under engine/include) is not an L3->solver/storage route and is ignored.
    case "$abspath" in
      "$ENGINE_INC"/*) ;;
      *) continue ;;
    esac
    rel="${abspath#"$SRCROOT"/}"
    if ! is_allowed "$rel"; then
      echo "RFC 0008 L3 boundary violation: '$src' resolves disallowed header:" >&2
      echo "  $rel" >&2
      echo "L3 (abstract_tree) may depend only on L1 rules and L2 abstraction." >&2
      echo "A solver, storage/transport, or private-holding evaluator (eval/equity)" >&2
      echo "belongs in L4, or extend the dependency rule (and this allowlist) via RFC." >&2
      rc=1
    fi
  done < <(sed -e 's/\\$//' "$dep" | tr '\n\t' '  ' | tr -s ' ' | tr ' ' '\n')
done

if [ "$rc" -eq 0 ]; then
  echo "L3 boundary guard passed" > "$STAMP"
fi
exit "$rc"
