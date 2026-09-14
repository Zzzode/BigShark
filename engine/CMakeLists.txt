cmake_minimum_required(VERSION 3.20)
project(bigshark_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(RELATIVE_PATH BIGSHARK_COMPILE_COMMANDS_TARGET
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_BINARY_DIR}/compile_commands.json"
)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          "${BIGSHARK_COMPILE_COMMANDS_TARGET}"
          "${CMAKE_SOURCE_DIR}/compile_commands.json"
  COMMAND_ERROR_IS_FATAL ANY
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
endif()

option(BIGSHARK_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BIGSHARK_PUBLISH_ENGINE "Publish bigshark-engine to the project bin directory" ON)
if(BIGSHARK_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "BIGSHARK_ENABLE_SANITIZERS requires Clang or GCC.")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

file(GLOB_RECURSE BIGSHARK_FORMAT_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(engine-format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${BIGSHARK_FORMAT_FILES}
    COMMENT "Formatting engine C++ sources"
    VERBATIM
  )
  add_custom_target(engine-format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${BIGSHARK_FORMAT_FILES}
    COMMENT "Checking engine C++ formatting"
    VERBATIM
  )
else()
  message(STATUS "clang-format not found; engine format targets are unavailable.")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND TARGET engine-format)
  add_custom_target(format DEPENDS engine-format)
  add_custom_target(format-check DEPENDS engine-format-check)
endif()

# Vendored yyjson is private to the v0 protocol boundary.
add_library(bigshark_yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(bigshark_yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bigshark_yyjson PRIVATE -w)

# Vendored OpenSpiel is private to the solver target.
add_subdirectory(third_party/open_spiel EXCLUDE_FROM_ALL)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_library(bigshark_poker STATIC
  src/poker/charts.cpp
  src/poker/heads_up.cpp
  src/poker/icm.cpp
  src/poker/settlement.cpp
)
target_include_directories(bigshark_poker
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(bigshark_solver STATIC
  src/gto/river_gto.cpp
  src/gto/cfr_solver.cpp
  src/gto/range_builder.cpp
  src/gto/range_equity.cpp
  src/gto/range_tracker.cpp
  src/gto/multistreet_cfr.cpp
  src/gto/heads_up_solver.cpp
  src/gto/river_game.cc
  src/gto/sf_lp.cc
)
target_include_directories(bigshark_solver
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark_solver
  PUBLIC  bigshark_poker
  PRIVATE open_spiel_min
)

find_package(highs CONFIG QUIET)
if(highs_FOUND)
  target_sources(bigshark_solver PRIVATE src/gto/highs_backend.cc)
  target_link_libraries(bigshark_solver PRIVATE highs::highs)
  target_compile_definitions(bigshark_solver PRIVATE BSGTO_HAVE_HIGHS=1)
  message(STATUS "HiGHS found: exact river LP backend enabled.")
else()
  message(STATUS "HiGHS not found: river solver uses bounded DCFR (brew install highs for exact LP).")
endif()

add_executable(bigshark-multistreet-benchmark
  benchmarks/multistreet_cfr_benchmark.cpp
)
target_include_directories(bigshark-multistreet-benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark-multistreet-benchmark PRIVATE
  bigshark_solver
)
add_custom_target(benchmark-multistreet
  COMMAND $<TARGET_FILE:bigshark-multistreet-benchmark>
  DEPENDS bigshark-multistreet-benchmark
  COMMENT "Running the multi-street CFR exploitability benchmark"
  USES_TERMINAL
)

add_library(bigshark_policy STATIC
  src/policy/decision.cpp
)
target_include_directories(bigshark_policy
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_policy
  PUBLIC  bigshark_poker
  PRIVATE bigshark_solver
)

add_library(bigshark_service STATIC
  src/service/decision_service.cpp
)
target_include_directories(bigshark_service
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_service
  PUBLIC  bigshark_poker
  PRIVATE bigshark_policy
)

add_library(bigshark_v0_protocol STATIC
  src/protocol/v0_json.cpp
)
target_include_directories(bigshark_v0_protocol
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bigshark_v0_protocol
  PUBLIC  bigshark_poker
  PRIVATE bigshark_yyjson
)

if(BUILD_TESTING)
  add_executable(test_eval tests/test_eval.cpp)
  target_link_libraries(test_eval PRIVATE bigshark_poker)
  add_test(NAME eval COMMAND test_eval)

  add_executable(test_heads_up tests/test_heads_up.cpp)
  target_link_libraries(test_heads_up PRIVATE bigshark_poker)
  add_test(NAME heads_up COMMAND test_heads_up)

  add_executable(test_icm tests/test_icm.cpp)
  target_link_libraries(test_icm PRIVATE bigshark_poker)
  add_test(NAME icm COMMAND test_icm)

  add_executable(test_settlement tests/test_settlement.cpp)
  target_link_libraries(test_settlement PRIVATE bigshark_poker)
  add_test(NAME settlement COMMAND test_settlement)

  add_executable(test_heads_up_solver tests/test_heads_up_solver.cpp)
  target_link_libraries(test_heads_up_solver PRIVATE bigshark_solver)
  add_test(NAME heads_up_solver COMMAND test_heads_up_solver)

  # Global allocation replacement must not interpose on HiGHS shared-library
  # initializers. Compile the unchanged subject with only its actual dependency.
  add_executable(test_heads_up_allocations
    tests/test_heads_up_allocations.cpp
    src/gto/heads_up_solver.cpp
  )
  target_link_libraries(test_heads_up_allocations PRIVATE bigshark_poker)
  add_test(NAME heads_up_allocations COMMAND test_heads_up_allocations)

  add_executable(test_equity tests/test_equity.cpp)
  target_link_libraries(test_equity PRIVATE bigshark_poker)
  add_test(NAME equity COMMAND test_equity)

  add_executable(test_v0_protocol tests/test_v0_protocol.cpp)
  target_link_libraries(test_v0_protocol PRIVATE bigshark_v0_protocol)
  add_test(NAME v0_protocol COMMAND test_v0_protocol)

  foreach(test_name gto multistreet_ref)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE bigshark_solver)
    target_include_directories(test_${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ${test_name} COMMAND test_${test_name})
  endforeach()

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_test(
      NAME benchmark_multistreet
      COMMAND bigshark-multistreet-benchmark
    )
    set_tests_properties(benchmark_multistreet PROPERTIES
      LABELS benchmark
      TIMEOUT 180
    )
  endif()
endif()
