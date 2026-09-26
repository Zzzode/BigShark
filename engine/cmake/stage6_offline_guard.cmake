# RFC 0008 stage 6 offline boundary guard.
#
# The stage-6 measurement machinery (bigshark_behavior and the later
# bigshark_stage6_train / bigshark_stage6_eval targets) is offline-only: it
# must never be linked, directly or transitively, into the live decision
# service, either wire protocol, or the published engine host. This file runs
# at every CMake configure AFTER every add_subdirectory (included last from
# the root CMakeLists.txt) and walks the transitive static-link closure of the
# protected targets; the companion stage6_boundary_guard.sh stamps the
# resolved-include (-MMD) half at build time.
#
# Matching by target-name prefix means a future stage-6 target is forbidden the
# moment it is declared, without editing this guard.

set(_stage6_protected_targets
  bigshark_service
  bigshark_v0_protocol
  bigshark_v1_protocol
  bigshark-engine
)

set(_stage6_forbidden_prefixes
  "bigshark_behavior"
  "bigshark_stage6"
  "bigshark_practice"
)

# Returns the transitive set of linkable CMake targets reachable from `root`
# in global property GLOBAL_PROPERTY_<root>_closure, via a fixed-point
# worklist over INTERFACE_LINK_LIBRARIES and LINK_LIBRARIES.
function(_bs_stage6_link_closure root)
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
      # Only bare names can name linkable targets; generator expressions,
      # keywords, and plain libraries are skipped.
      if("${_entry}" MATCHES "^[A-Za-z0-9_.-]+$" AND TARGET "${_entry}"
         AND NOT "${_entry}" IN_LIST _seen)
        list(APPEND _work "${_entry}")
      endif()
    endforeach()
  endwhile()
  set_property(GLOBAL PROPERTY "_bs_stage6_closure_${root}" "${_seen}")
endfunction()

foreach(_protected IN LISTS _stage6_protected_targets)
  if(NOT TARGET "${_protected}")
    message(FATAL_ERROR "stage-6 offline guard: protected target '${_protected}' "
                        "is missing; the guard target list is stale")
  endif()
  _bs_stage6_link_closure("${_protected}")
  get_property(_closure GLOBAL PROPERTY "_bs_stage6_closure_${_protected}")
  foreach(_linked IN LISTS _closure)
    if(_linked STREQUAL _protected)
      continue()
    endif()
    foreach(_prefix IN LISTS _stage6_forbidden_prefixes)
      if(_linked MATCHES "^${_prefix}")
        message(FATAL_ERROR
          "RFC 0008 stage-6 offline boundary violated: '${_linked}' is linked "
          "(directly or transitively) into live target '${_protected}'. The "
          "behavior/stage-6 targets are offline measurement code and must not "
          "be linked by the decision service, v0/v1 protocols, or the host.")
      endif()
    endforeach()
  endforeach()
endforeach()

# Build-time resolved-include guard: no service/protocol source may pull in
# bs/behavior_policy.hpp or any bs/stage6/* header, by any spelling that
# actually compiles. The stamp runs the compiler in dependency mode.
set(_stage6_guard_stamp "${CMAKE_BINARY_DIR}/stage6_offline_guard.stamp")
set(_stage6_guarded_sources
  "${CMAKE_SOURCE_DIR}/engine/src/service/decision_service.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v0_json.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_frame_stream.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_semantic_validator.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_request_mapper.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_response_mapper.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_resident_mapper.cpp"
  "${CMAKE_SOURCE_DIR}/engine/src/protocol/v1_envelope.cpp"
)
add_custom_command(
  OUTPUT "${_stage6_guard_stamp}"
  COMMAND bash "${CMAKE_SOURCE_DIR}/engine/cmake/stage6_boundary_guard.sh"
          "${CMAKE_CXX_COMPILER}" "-std=c++23"
          "${CMAKE_SOURCE_DIR}/engine/include"
          "${_stage6_guard_stamp}" ${_stage6_guarded_sources}
  DEPENDS ${_stage6_guarded_sources}
          "${CMAKE_SOURCE_DIR}/engine/cmake/stage6_boundary_guard.sh"
  COMMENT "Verifying the RFC 0008 stage-6 offline resolved-include boundary"
)
add_custom_target(bigshark_stage6_offline_guard ALL DEPENDS "${_stage6_guard_stamp}")
add_dependencies(bigshark_service bigshark_stage6_offline_guard)
add_dependencies(bigshark_v1_protocol bigshark_stage6_offline_guard)
