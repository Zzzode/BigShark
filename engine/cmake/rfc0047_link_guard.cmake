# RFC 0047 Stage-1 benchmark-only link isolation guard.
#
# bigshark_rfc0047_bench (canonical board census, per-river structural budget,
# BSNCV01 oracle codec, and later the deterministic CV builder/depth-limited
# certifier) may be linked ONLY by the RFC 0047 benchmark arm and its
# measurement ctests. This configure-time check (included last from the root
# CMakeLists.txt) walks the TRANSITIVE target graph of the protected
# production roots and fails configure if the measurement library is reachable,
# and also fails if the measurement library links a protected root or the
# sibling bigshark_rfc0046_bench (the RFC 0047 harness re-implements the spec
# and stays decoupled). It is a target-graph traversal, not a text grep.

if(NOT TARGET bigshark_rfc0047_bench)
  message(FATAL_ERROR "rfc0047 link guard: bigshark_rfc0047_bench target is missing")
endif()

set(_bs_rfc0047_protected_roots
  bigshark-engine
  bigshark-practice
  bigshark_practice
  bigshark_practice_engine
  bigshark_resident
  bigshark-flop-library-builder
  bigshark-flop-library-pack
  bigshark-flop-library-verify-pack
)

function(_bs_rfc0047_closure root out_var)
  set(_work "${root}")
  set(_seen "")
  while(_work)
    list(POP_FRONT _work _current)
    if("${_current}" IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_current}")
    if(NOT TARGET "${_current}")
      continue()
    endif()
    get_target_property(_iface "${_current}" INTERFACE_LINK_LIBRARIES)
    get_target_property(_plain "${_current}" LINK_LIBRARIES)
    set(_entries "${_iface};${_plain}")
    foreach(_entry IN LISTS _entries)
      if("${_entry}" MATCHES "^[A-Za-z0-9_.-]+$" AND TARGET "${_entry}"
         AND NOT "${_entry}" IN_LIST _seen)
        list(APPEND _work "${_entry}")
      endif()
    endforeach()
  endwhile()
  set(${out_var} "${_seen}" PARENT_SCOPE)
endfunction()

set(_bs_rfc0047_manifest "${CMAKE_BINARY_DIR}/rfc0047_link_closure_manifest.txt")
file(WRITE "${_bs_rfc0047_manifest}"
  "# RFC 0047 Stage-1 protected-target link closures (generated at configure).\n")

foreach(_root IN LISTS _bs_rfc0047_protected_roots)
  if(NOT TARGET "${_root}")
    file(APPEND "${_bs_rfc0047_manifest}" "${_root}: <target not present>\n")
    continue()
  endif()
  _bs_rfc0047_closure("${_root}" _closure)
  list(SORT _closure)
  file(APPEND "${_bs_rfc0047_manifest}" "${_root}: ${_closure}\n")
  if("bigshark_rfc0047_bench" IN_LIST _closure)
    message(FATAL_ERROR
      "RFC 0047 Stage-1 link isolation violated: bigshark_rfc0047_bench is in "
      "the transitive closure of protected target '${_root}'.")
  endif()
endforeach()

# Reverse edges: the measurement library must not pull a production root NOR
# the sibling RFC 0046 benchmark library.
_bs_rfc0047_closure("bigshark_rfc0047_bench" _bench_closure)
foreach(_forbidden IN LISTS _bs_rfc0047_protected_roots)
  if("${_forbidden}" IN_LIST _bench_closure)
    message(FATAL_ERROR
      "RFC 0047 Stage-1 link isolation violated: bigshark_rfc0047_bench links "
      "production target '${_forbidden}'.")
  endif()
endforeach()
if("bigshark_rfc0046_bench" IN_LIST _bench_closure)
  message(FATAL_ERROR
    "RFC 0047 Stage-1 link isolation violated: bigshark_rfc0047_bench links "
    "the sibling bigshark_rfc0046_bench; the successor harness must stay "
    "decoupled and re-implement the specification.")
endif()

if(BUILD_TESTING)
  add_test(NAME rfc0047_link_isolation
    COMMAND "${CMAKE_COMMAND}"
            "-DRFC0047_MANIFEST=${_bs_rfc0047_manifest}"
            -P "${CMAKE_SOURCE_DIR}/engine/cmake/rfc0047_link_guard_check.cmake")
endif()

message(STATUS "rfc0047 link isolation guard passed")
