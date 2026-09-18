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

# Vendored SQLite amalgamation (RFC 0005). The system/Homebrew SQLite is never
# linked. SQLITE_OMIT_LOAD_EXTENSION removes extension loading at compile time;
# threads stay enabled. The include directory is internal to the engine tree
# and the artifacts public headers never expose sqlite3.h.
add_library(bigshark_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(bigshark_sqlite PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
target_compile_options(bigshark_sqlite PRIVATE -w)
target_compile_definitions(bigshark_sqlite PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION=1
)

# OpenSSL 3 Crypto is required for SHA-256 artifact digests and is linked
# PRIVATELY into bigshark_artifacts. Prefer the CMake package, then the
# Homebrew keg-only prefixes; fail clearly instead of silently disabling.
if(NOT DEFINED BS_OPENSSL_CRYPTO_TARGET)
  find_package(OpenSSL 3 QUIET)
  if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
    set(BS_OPENSSL_CRYPTO_TARGET OpenSSL::Crypto)
    set(BS_OPENSSL_FOUND_VERSION "${OPENSSL_VERSION}")
  else()
    set(_bs_openssl_prefixes)
    if(APPLE)
      list(APPEND _bs_openssl_prefixes
        /opt/homebrew/opt/openssl@3
        /usr/local/opt/openssl@3
      )
    endif()
    if(DEFINED ENV{OPENSSL_ROOT_DIR})
      list(PREPEND _bs_openssl_prefixes "$ENV{OPENSSL_ROOT_DIR}")
    endif()
    foreach(_bs_prefix IN LISTS _bs_openssl_prefixes)
      if(EXISTS "${_bs_prefix}/include/openssl/evp.h"
         AND EXISTS "${_bs_prefix}/lib/libcrypto.dylib")
        set(_bs_openssl_include "${_bs_prefix}/include")
        set(_bs_openssl_lib "${_bs_prefix}/lib/libcrypto.dylib")
        break()
      elseif(EXISTS "${_bs_prefix}/include/openssl/evp.h"
             AND EXISTS "${_bs_prefix}/lib/libcrypto.so")
        set(_bs_openssl_include "${_bs_prefix}/include")
        set(_bs_openssl_lib "${_bs_prefix}/lib/libcrypto.so")
        break()
      endif()
    endforeach()
    if(DEFINED _bs_openssl_lib)
      add_library(bs_openssl_crypto SHARED IMPORTED)
      set_target_properties(bs_openssl_crypto PROPERTIES
        IMPORTED_LOCATION "${_bs_openssl_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_bs_openssl_include}"
      )
      set(BS_OPENSSL_CRYPTO_TARGET bs_openssl_crypto)
      file(READ "${_bs_openssl_include}/openssl/opensslv.h" _bs_opensslv)
      set(BS_OPENSSL_FOUND_VERSION "prefix ${_bs_prefix}")
      unset(_bs_opensslv)
    endif()
  endif()
  if(NOT DEFINED BS_OPENSSL_CRYPTO_TARGET)
    message(FATAL_ERROR
      "OpenSSL 3 Crypto was not found. RFC 0005 artifacts require it for "
      "SHA-256. Install it (macOS: brew install openssl@3) or set "
      "OPENSSL_ROOT_DIR. It is never acceptable to build without digest "
      "support.")
  endif()
  message(STATUS "OpenSSL 3 Crypto for bigshark_artifacts: ${BS_OPENSSL_CRYPTO_TARGET} ${BS_OPENSSL_FOUND_VERSION}")
endif()

# RFC 0005 offline strategy artifacts. SQLite and Crypto stay PRIVATE; the
# public surface exchanges only solver domain records.
add_library(bigshark_artifacts STATIC
  src/artifacts/artifact_codec.cpp
  src/artifacts/strategy_artifact.cpp
  src/artifacts/artifact_digest.cpp
)
target_include_directories(bigshark_artifacts
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark_artifacts
  PUBLIC  bigshark_solver
  PRIVATE bigshark_sqlite
  PRIVATE ${BS_OPENSSL_CRYPTO_TARGET}
)

# RFC 0005 Stage 9 bounded heads-up resolving gadget plus independent
# best-response certification. Offline solver-domain component; it reuses the
# poker state machine and exact settlement but owns a parallel augmented-game
# traversal. It never opens storage or touches the wire.
add_library(bigshark_resolver STATIC
  src/resolver/continuation.cpp
  src/resolver/counterfactual_reach.cpp
  src/resolver/gadget_cfr.cpp
  src/resolver/certifier.cpp
  src/resolver/resolver.cpp
)
target_include_directories(bigshark_resolver
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark_resolver
  PUBLIC bigshark_solver
)

# RFC 0005 Stage 6 resident policy lookup. Offline-only, immutable compact
# indices over validated artifacts; it never opens SQLite on a lookup and is
# not linked by the policy, service, host, v0, or protocol targets.
add_library(bigshark_resident STATIC
  src/resident/resident_policy.cpp
  src/resident/public_reach.cpp
  src/resident/resident_index.cpp
)
target_include_directories(bigshark_resident
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark_resident
  PUBLIC bigshark_artifacts
  PUBLIC bigshark_resolver
)


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

add_executable(bigshark-heads-up-blueprint-benchmark
  benchmarks/heads_up_blueprint_benchmark.cpp
)
target_link_libraries(bigshark-heads-up-blueprint-benchmark PRIVATE
  bigshark_solver
)
add_custom_target(benchmark-heads-up-blueprint
  COMMAND $<TARGET_FILE:bigshark-heads-up-blueprint-benchmark>
  DEPENDS bigshark-heads-up-blueprint-benchmark
  COMMENT "Running the RFC 0004 heads-up blueprint quality benchmark"
  USES_TERMINAL
)

# Manual release-scale capacity runner. It spends both sampled cases at the
# pinned 1,000,000-iteration checkpoint and takes several minutes, so it is
# built as a target but intentionally NOT registered as a CTest (RFC 0004
# lines 324-326). It includes only public solver headers.
add_executable(bigshark-heads-up-capacity-benchmark
  benchmarks/heads_up_capacity_benchmark.cpp
)
target_link_libraries(bigshark-heads-up-capacity-benchmark PRIVATE
  bigshark_solver
)
add_custom_target(benchmark-heads-up-capacity
  COMMAND $<TARGET_FILE:bigshark-heads-up-capacity-benchmark>
  DEPENDS bigshark-heads-up-capacity-benchmark
  COMMENT "Running the RFC 0004 heads-up one-million-iteration capacity benchmark"
  USES_TERMINAL
)

# RFC 0004 Stage 4 frozen release matrix (schema_version 3 CSV): frozen flop
# families, stack profiles, SPR 1/4/10, weighted ranges, and runout policies
# with exact/estimated labeling and node, memory, elapsed-time, and coverage
# reporting. It spends several minutes on the larger SPR 4/10 fixtures, so it
# is built as a target but intentionally NOT registered as a CTest. It
# includes only public solver headers and reuses the schema-2 support harness.
add_executable(bigshark-heads-up-matrix-benchmark
  benchmarks/heads_up_matrix_benchmark.cpp
)
target_link_libraries(bigshark-heads-up-matrix-benchmark PRIVATE
  bigshark_solver
)
add_custom_target(benchmark-heads-up-matrix
  COMMAND $<TARGET_FILE:bigshark-heads-up-matrix-benchmark>
  DEPENDS bigshark-heads-up-matrix-benchmark
  COMMENT "Running the RFC 0004 Stage 4 frozen heads-up matrix benchmark"
  USES_TERMINAL
)

# RFC 0005 Stage 6 resident lookup latency benchmark. Deterministically
# publishes a complete ~100k+ information-set artifact and measures cold
# construction plus warm lookup p50/p95/p99 with a counting allocator. It is a
# manual target, NOT a registered or timing-gated CTest.
add_executable(bigshark-resident-lookup-benchmark
  benchmarks/resident_lookup_benchmark.cpp
)
target_include_directories(bigshark-resident-lookup-benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(bigshark-resident-lookup-benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)
target_link_libraries(bigshark-resident-lookup-benchmark PRIVATE
  bigshark_resident
)
add_custom_target(benchmark-resident-lookup
  COMMAND $<TARGET_FILE:bigshark-resident-lookup-benchmark>
  DEPENDS bigshark-resident-lookup-benchmark
  COMMENT "Running the RFC 0005 Stage 6 resident lookup benchmark"
  USES_TERMINAL
)

# RFC 0005 Stage 9 bounded-resolve microbenchmark. Tiny fixed-runout terminal
# game; reports solve+cert wall time, worst bound slack, and iterations. Manual
# target, NOT a registered CTest.
add_executable(bigshark-resolver-benchmark
  benchmarks/resolver_benchmark.cpp
)
target_link_libraries(bigshark-resolver-benchmark PRIVATE
  bigshark_resolver
)
add_custom_target(benchmark-resolver
  COMMAND $<TARGET_FILE:bigshark-resolver-benchmark>
  DEPENDS bigshark-resolver-benchmark
  COMMENT "Running the RFC 0005 Stage 9 bounded resolver benchmark"
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

  add_executable(test_heads_up_solver_sampled tests/test_heads_up_solver_sampled.cpp)
  target_include_directories(test_heads_up_solver_sampled PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_heads_up_solver_sampled PRIVATE bigshark_solver)
  add_test(NAME heads_up_solver_sampled COMMAND test_heads_up_solver_sampled)

  # Global allocation replacement must not interpose on HiGHS shared-library
  # initializers. Compile the unchanged subject with only its actual dependency.
  add_executable(test_heads_up_allocations
    tests/test_heads_up_allocations.cpp
    src/gto/heads_up_solver.cpp
  )
  # The debug raw-row export fault branch is exercised through the private
  # passkey header; the compiled subject itself remains unchanged.
  target_include_directories(test_heads_up_allocations PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_heads_up_allocations PRIVATE bigshark_poker)
  add_test(NAME heads_up_allocations COMMAND test_heads_up_allocations)

  add_executable(test_artifacts tests/test_artifacts.cpp)
  target_include_directories(test_artifacts PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_artifacts PRIVATE
    bigshark_artifacts
    bigshark_sqlite
  )
  add_test(NAME artifacts COMMAND test_artifacts)

  # RFC 0005 Stage 6 resident lookup. The gto debug passkey header is used
  # ONLY to synthesize complete policy fixtures, as in test_artifacts.
  add_executable(test_resident_policy tests/test_resident_policy.cpp)
  target_include_directories(test_resident_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(test_resident_policy PRIVATE
    BS_RESIDENT_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/include/bs/resident_policy.hpp"
  )
  target_include_directories(test_resident_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
  )
  target_link_libraries(test_resident_policy PRIVATE
    bigshark_resident
  )
  add_test(NAME resident COMMAND test_resident_policy)

  # RFC 0005 Stage 9 bounded resolver and independent certification. Internal
  # resolver headers are included only to drive the whitebox fixtures; the
  # independently written oracle is compiled as a separate translation unit.
  add_executable(test_resolver
    tests/test_resolver.cpp
    tests/resolver_oracle.cpp
  )
  target_include_directories(test_resolver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_resolver PRIVATE bigshark_resolver)
  add_test(NAME resolver COMMAND test_resolver)

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
    add_test(
      NAME benchmark_heads_up_blueprint
      COMMAND bigshark-heads-up-blueprint-benchmark
    )
    set_tests_properties(benchmark_heads_up_blueprint PROPERTIES
      LABELS benchmark
      TIMEOUT 900
    )
  endif()
endif()
