find_program(BIGSHARK_NPM_EXECUTABLE NAMES npm)
set(BIGSHARK_TYPESCRIPT_LOCK_SCRIPT
  "${CMAKE_SOURCE_DIR}/tools/cmake/run-npm-with-typescript-lock.cmake"
)

if(BIGSHARK_NPM_EXECUTABLE)
  add_custom_target(typescript-build
    COMMAND ${CMAKE_COMMAND}
            "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBIGSHARK_NPM_SCRIPT=build:ts"
            -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    COMMENT "Building TypeScript applications and integrations"
    VERBATIM
  )
  add_custom_target(typescript-check
    COMMAND ${BIGSHARK_NPM_EXECUTABLE} run typecheck
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Type-checking TypeScript applications and integrations"
    VERBATIM
  )
  add_custom_target(node-check
    COMMAND ${CMAKE_COMMAND}
            "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBIGSHARK_NPM_SCRIPT=test:node"
            "-DBIGSHARK_ENGINE_BINARY=$<TARGET_FILE:bigshark-engine>"
            -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    DEPENDS bigshark-engine
    COMMENT "Testing TypeScript applications and integrations"
    VERBATIM
  )

  if(BUILD_TESTING)
    add_test(
      NAME typescript
      COMMAND ${BIGSHARK_NPM_EXECUTABLE} run typecheck
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
      NAME node
      COMMAND ${CMAKE_COMMAND}
              "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
              "-DBIGSHARK_NPM_SCRIPT=test:node"
              "-DBIGSHARK_ENGINE_BINARY=$<TARGET_FILE:bigshark-engine>"
              -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
    )
    set_tests_properties(node PROPERTIES RESOURCE_LOCK typescript_dist)
  endif()
else()
  message(STATUS "npm not found; TypeScript build and tests are unavailable.")
endif()
