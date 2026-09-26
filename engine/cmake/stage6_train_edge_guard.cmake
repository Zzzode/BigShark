# RFC 0008 stage 6 trainer edge guard (link-closure half).
#
# The MCCFR trainer (bigshark_stage6_train) is a SIBLING of the evaluation
# surface, not a dependent, and the edge must never exist in either direction:
#
#   * the trainer produces frozen postflop artifacts but must not reach the
#     deployed heuristic (bigshark_policy) or any eval-only component
#     (adapter, pinned baseline, geometry enumerator, simulator, estimator,
#     oracle, translator, candidate policy) — the artifact must be derivable
#     from pure poker + abstraction + the joint sampler alone;
#   * eval must not link the trainer: the driver that joins both is the leaf
#     measurement binary, never either library.
#
# This file runs at every CMake configure AFTER every add_subdirectory (it is
# included last from the root CMakeLists.txt, next to stage6_offline_guard).
# The companion stage6_train_edge_guard.sh stamps the resolved-include (-MMD)
# half at build time.

if(NOT TARGET bigshark_stage6_train)
  message(FATAL_ERROR "stage-6 train edge guard: bigshark_stage6_train is missing")
endif()
if(NOT TARGET bigshark_stage6_eval)
  message(FATAL_ERROR "stage-6 train edge guard: bigshark_stage6_eval is missing")
endif()

# Transitive static-link closure (same fixed-point walk as the offline guard).
function(_bs_train_edge_closure root)
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
  set_property(GLOBAL PROPERTY "_bs_train_closure_${root}" "${_seen}")
endfunction()

# Forbidden in the TRAINER closure: the deployed policy, its live callers, and
# the eval surface. open_spiel/HiGHS are solver-internal and policy-free.
set(_train_forbidden
  bigshark_policy
  bigshark_stage6_eval
  bigshark_service
  bigshark_v0_protocol
  bigshark_v1_protocol
  bigshark-engine
)

_bs_train_edge_closure("bigshark_stage6_train")
get_property(_train_closure GLOBAL PROPERTY "_bs_train_closure_bigshark_stage6_train")
foreach(_forbidden IN LISTS _train_forbidden)
  if("${_forbidden}" IN_LIST _train_closure)
    message(FATAL_ERROR
      "RFC 0008 stage-6 trainer edge violated: '${_forbidden}' is in the "
      "transitive link closure of bigshark_stage6_train. The trainer must be "
      "derivable from pure poker + abstraction + the joint sampler alone; it "
      "must never reach the deployed policy or the eval surface.")
  endif()
endforeach()

# The reverse edge: eval must not depend on the trainer. Only the leaf
# measurement driver may link both.
_bs_train_edge_closure("bigshark_stage6_eval")
get_property(_eval_closure GLOBAL PROPERTY "_bs_train_closure_bigshark_stage6_eval")
if("bigshark_stage6_train" IN_LIST _eval_closure)
  message(FATAL_ERROR
    "RFC 0008 stage-6 trainer edge violated: bigshark_stage6_eval links "
    "bigshark_stage6_train. They are siblings; only the leaf measurement "
    "driver may join them.")
endif()

# Build-time resolved-include guard: trainer sources may resolve only the
# policy-free stage-6 core headers (an explicit whitelist) and must never pull
# in bs/decision.hpp or bs/policy.hpp. The stamp runs the compiler in
# dependency mode, so macro-pasted or transitive includes are caught too.
set(_train_guard_stamp "${CMAKE_BINARY_DIR}/stage6_train_edge_guard.stamp")
# Scan EVERY translation unit in the trainer target, not a hand-maintained
# list: a new trainer source must inherit the resolved-include edge the day it
# is added. Sources are CMake-relative; resolve them against the engine root.
get_target_property(_train_target_sources bigshark_stage6_train SOURCES)
set(_train_guarded_sources "")
foreach(_src IN LISTS _train_target_sources)
  if(IS_ABSOLUTE "${_src}")
    list(APPEND _train_guarded_sources "${_src}")
  else()
    list(APPEND _train_guarded_sources "${CMAKE_SOURCE_DIR}/engine/${_src}")
  endif()
endforeach()
if(NOT _train_guarded_sources)
  message(FATAL_ERROR "stage-6 train edge guard: bigshark_stage6_train has no sources to scan")
endif()
add_custom_command(
  OUTPUT "${_train_guard_stamp}"
  COMMAND bash "${CMAKE_SOURCE_DIR}/engine/cmake/stage6_train_edge_guard.sh"
          "${CMAKE_CXX_COMPILER}" "-std=c++23"
          "${CMAKE_SOURCE_DIR}/engine/include"
          "${_train_guard_stamp}" ${_train_guarded_sources}
  DEPENDS ${_train_guarded_sources}
          "${CMAKE_SOURCE_DIR}/engine/cmake/stage6_train_edge_guard.sh"
  COMMENT "Verifying the RFC 0008 stage-6 trainer resolved-include edge"
)
add_custom_target(bigshark_stage6_train_edge_guard ALL DEPENDS "${_train_guard_stamp}")
add_dependencies(bigshark_stage6_train bigshark_stage6_train_edge_guard)
