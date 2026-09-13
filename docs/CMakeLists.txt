find_program(BIGSHARK_NODE_EXECUTABLE NAMES node)
find_program(BIGSHARK_NPM_EXECUTABLE NAMES npm)
set(BIGSHARK_TYPESCRIPT_LOCK_SCRIPT
  "${CMAKE_SOURCE_DIR}/tools/cmake/run-npm-with-typescript-lock.cmake"
)

if(BIGSHARK_NODE_EXECUTABLE AND BIGSHARK_NPM_EXECUTABLE)
  add_custom_target(docs-check
    COMMAND ${CMAKE_COMMAND}
            "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBIGSHARK_NPM_SCRIPT=check:docs"
            -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    COMMENT "Checking documentation language and links"
    VERBATIM
  )
  add_custom_target(rfc-check
    COMMAND ${CMAKE_COMMAND}
            "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBIGSHARK_NPM_SCRIPT=check:rfc"
            -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    COMMENT "Checking RFC structure and lifecycle"
    VERBATIM
  )

  if(BUILD_TESTING)
    add_test(
      NAME docs
      COMMAND ${CMAKE_COMMAND}
              "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
              "-DBIGSHARK_NPM_SCRIPT=check:docs"
              -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    )
    add_test(
      NAME rfc
      COMMAND ${CMAKE_COMMAND}
              "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
              "-DBIGSHARK_NPM_SCRIPT=check:rfc"
              -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    )
    set_tests_properties(docs rfc PROPERTIES RESOURCE_LOCK typescript_dist)
  endif()
else()
  message(STATUS "Node.js or npm not found; documentation checks are unavailable.")
endif()
