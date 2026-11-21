# RFC 0046 Stage-1 benchmark-only link isolation guard.
#
# The measurement library bigshark_rfc0046_bench (exact-key ConcreteCards
# seam implementation and later the gadget/certifier extensions) may be
# linked ONLY by the RFC 0046 benchmark arm and measurement ctests. This file
# runs at every CMake configure AFTER every add_subdirectory (included last
# from the root CMakeLists.txt), walks the TRANSITIVE CMake target dependency
# graph of the protected production targets (engine-host, practice, the
# resident/mmap library, and flop-library pack/build targets), and fails
# configure if the measurement library is reachable from any of them. It is a
# target-graph traversal, not a source/symbol grep.
#
# The resolved closures are additionally written to a manifest in the build
# tree; the rfc0046_link_isolation ctest (cmake -P checker below) verifies the
# recorded graph is clean, giving the isolation regression CTest visibility.

if(NOT TARGET bigshark_rfc0046_bench)
  message(FATAL_ERROR "rfc0046 link guard: bigshark_rfc0046_bench target is missing")
endif()

set(_bs_rfc0046_protected_roots
  bigshark-engine
  bigshark-practice
  bigshark_practice
  bigshark_practice_engine
  bigshark_resident
  bigshark-flop-library-builder
  bigshark-flop-library-pack
  bigshark-flop-library-verify-pack
)

# Transitive static-link closure: same fixed-point walk as the stage-6 edge
# guards, over INTERFACE_LINK_LIBRARIES and LINK_LIBRARIES target edges.
function(_bs_rfc0046_closure root out_var)
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

set(_bs_rfc0046_manifest "${CMAKE_BINARY_DIR}/rfc0046_link_closure_manifest.txt")
file(WRITE "${_bs_rfc0046_manifest}"
  "# RFC 0046 Stage-1 protected-target link closures (generated at configure).\n")

foreach(_root IN LISTS _bs_rfc0046_protected_roots)
  if(NOT TARGET "${_root}")
    file(APPEND "${_bs_rfc0046_manifest}" "${_root}: <target not present>\n")
    continue()
  endif()
  _bs_rfc0046_closure("${_root}" _closure)
  list(SORT _closure)
  file(APPEND "${_bs_rfc0046_manifest}" "${_root}: ${_closure}\n")
  if("bigshark_rfc0046_bench" IN_LIST _closure)
    message(FATAL_ERROR
      "RFC 0046 Stage-1 link isolation violated: bigshark_rfc0046_bench is in "
      "the transitive target closure of protected target '${_root}'. The "
      "benchmark-only measurement library must never link into engine-host, "
      "practice, the resident/mmap library, or flop-library pack/build "
      "targets.")
  endif()
endforeach()

# Reverse edge: the measurement library may reuse engine libraries but must
# never pull a production composition root or practice/host target.
_bs_rfc0046_closure("bigshark_rfc0046_bench" _bench_closure)
foreach(_forbidden IN LISTS _bs_rfc0046_protected_roots)
  if("${_forbidden}" IN_LIST _bench_closure)
    message(FATAL_ERROR
      "RFC 0046 Stage-1 link isolation violated: bigshark_rfc0046_bench links "
      "production target '${_forbidden}'.")
  endif()
endforeach()

if(BUILD_TESTING)
  add_test(NAME rfc0046_link_isolation
    COMMAND "${CMAKE_COMMAND}"
            "-DRFC0046_MANIFEST=${_bs_rfc0046_manifest}"
            -P "${CMAKE_SOURCE_DIR}/engine/cmake/rfc0046_link_guard_check.cmake")
endif()

message(STATUS "rfc0046 link isolation guard passed")
