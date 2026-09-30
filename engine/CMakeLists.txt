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
  src/poker/game_definition.cpp
  src/poker/game_definition_settlement.cpp
  src/poker/heads_up.cpp
  src/poker/icm.cpp
  src/poker/multiway.cpp
  src/poker/settlement.cpp
)
target_include_directories(bigshark_poker
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# RFC 0008 stage 3 L2: action and card abstraction. Depends ONLY on the rules
# layer; the rules never learn how they are abstracted, and this target never
# links a solver, storage, or transport (RFC 0008 Dependency Rules).
add_library(bigshark_abstraction STATIC
  src/abstraction/abstraction.cpp
)
target_include_directories(bigshark_abstraction
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_abstraction
  PUBLIC bigshark_poker
)

# RFC 0008 stage 4 L3: the abstract public betting tree. Depends ONLY on the
# rules (L1) and abstraction (L2); it never links a solver, storage, or
# transport, and its sources never name the transitional HeadsUpState/
# HeadsUpRoot adapters or evaluate private holdings (RFC 0008 Dependency Rules).
add_library(bigshark_tree STATIC
  src/tree/abstract_tree.cpp
)
target_include_directories(bigshark_tree
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_tree
  PUBLIC bigshark_poker bigshark_abstraction
)

# RFC 0008 stage 4 L3 boundary guard (build time, NOT gated on BUILD_TESTING).
# A stamp custom command runs the real compiler in dependency mode (-MMD) on
# each L3 source and enforces that every RESOLVED engine header in its include
# closure is on the L1/L2 allowlist. Checking the post-preprocessor closure
# (rather than regex-scanning include directives) closes macro-pasted,
# line-spliced, digraph, no-space, #import, and shadow-header spellings that
# all compile but defeat a text scan -- notably the header-only inline
# evaluators eval.hpp/equity.hpp, which emit a weak symbol and so cannot be
# caught by the link-negative target. bigshark_tree cannot build until the
# stamp is fresh.
set(_l3_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/include/bs/abstract_tree.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tree/abstract_tree.cpp")
set(_l3_guard_stamp "${CMAKE_CURRENT_BINARY_DIR}/l3_boundary_guard.stamp")
add_custom_command(
  OUTPUT "${_l3_guard_stamp}"
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/l3_boundary_guard.sh"
          "${CMAKE_CXX_COMPILER}" "-std=c++23"
          "${CMAKE_CURRENT_SOURCE_DIR}/include"
          "${_l3_guard_stamp}" ${_l3_sources}
  DEPENDS ${_l3_sources} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/l3_boundary_guard.sh"
  COMMENT "Verifying the RFC 0008 L3 -> L1+L2 resolved-include boundary")
add_custom_target(bigshark_l3_boundary_guard ALL DEPENDS "${_l3_guard_stamp}")
add_dependencies(bigshark_tree bigshark_l3_boundary_guard)

add_library(bigshark_solver STATIC
  src/gto/river_gto.cpp
  src/gto/cfr_solver.cpp
  src/gto/range_builder.cpp
  src/gto/range_equity.cpp
  src/gto/range_tracker.cpp
  src/gto/multistreet_cfr.cpp
  src/gto/heads_up_solver.cpp
  src/gto/solve.cpp
  src/gto/solve_projection.cpp
  src/gto/multiway_sampler.cpp
  src/gto/nseat_trainer.cpp
  src/solver/seat_policy.cpp
  src/gto/river_game.cc
  src/gto/sf_lp.cc
)
target_include_directories(bigshark_solver
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bigshark_solver
  PUBLIC  bigshark_poker
  PUBLIC  bigshark_abstraction
  PUBLIC  bigshark_tree
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

# RFC 0008 stage 6 R3b: flop-geometry enumeration + coarse-tree sizing. Manual
# measurement target, NOT a CTest; emits the frozen-matrix evidence.
add_executable(bigshark-stage6-geometry-benchmark
  benchmarks/stage6_geometry_benchmark.cpp
)
target_link_libraries(bigshark-stage6-geometry-benchmark PRIVATE
  bigshark_stage6_eval
)
# bigshark_tree arrives transitively through bigshark_solver (a stage6_eval
# dependency); naming it here links the archive twice.
add_custom_target(benchmark-stage6-geometry
  COMMAND $<TARGET_FILE:bigshark-stage6-geometry-benchmark> --trees
  DEPENDS bigshark-stage6-geometry-benchmark
  COMMENT "Enumerating and sizing the RFC 0008 stage-6 flop geometry matrix"
  USES_TERMINAL
)

# RFC 0008 stage 6 R12 step 8: the larger-table measurement driver. The ONE
# leaf that links the eval and trainer siblings together: it trains the
# buckets, binds the composed candidate, and estimates candidate vs pinned
# baseline general-sum NashConv with paired Student-t CIs. Manual evidence
# binary, NOT a CTest; its numbers are published as-is, including a negative
# or indistinguishable result.
add_executable(bigshark-stage6-measurement-driver
  benchmarks/stage6_measurement_driver.cpp
)
target_link_libraries(bigshark-stage6-measurement-driver PRIVATE
  bigshark_stage6_eval bigshark_stage6_train
)

# RFC 0008 stage 6: the offline interactive practice table leaf. One human at
# stdin, every other seat an offline bot. Like the measurement driver it is a
# leaf over the offline eval surface only: no service, host, protocol,
# network, or credentials.
add_executable(bigshark-practice
  benchmarks/practice_simulator.cpp
)
target_link_libraries(bigshark-practice PRIVATE
  bigshark_practice
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
  src/policy/guarantee.cpp
)
target_include_directories(bigshark_policy
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# RFC 0008 stage 5: the guaranteeFor source->level switch must fail the build
# if a new DecisionSource is added without a level (the repo builds -Wall
# -Wextra without -Werror; this elevates only -Wswitch).
target_compile_options(bigshark_policy PRIVATE -Werror=switch)
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

# RFC 0008 stage 6: the OFFLINE seat-indexed behavior-policy interface and the
# declared fixed uniform reference. This target links only bigshark_poker by
# design (no Ctx/policy vocabulary in L0/L1, no solver), and must never be
# linked by bigshark_service, the protocols, or the engine host; a stage-6
# link guard test enforces that below.
add_library(bigshark_behavior STATIC
  src/behavior/behavior_policy.cpp
)
target_include_directories(bigshark_behavior
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_behavior
  PUBLIC bigshark_poker
)

# RFC 0008 stage 6 offline CORE: pure-poker shared vocabulary for the trainer
# and the evaluation surface — geometry signatures/bucketing/coverage. It may
# reach only L1 poker and the L2 abstraction (plus the poker-only behavior
# value types); it never reaches the deployed policy, the service, or eval. A
# dedicated train-edge guard (stage6_train_edge_guard.cmake) enforces that the
# trainer's transitive closure stays free of bigshark_policy / eval.
add_library(bigshark_stage6_core STATIC
  src/stage6/frozen_artifact.cpp
  src/stage6/geometry_core.cpp
  src/stage6/infoset_id.cpp
  src/stage6/public_path.cpp
  src/stage6/random_streams.cpp
  src/stage6/translator_id.cpp
)
target_include_directories(bigshark_stage6_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_stage6_core
  PUBLIC bigshark_behavior
  PRIVATE bigshark_poker bigshark_abstraction
)

# RFC 0008 stage 6 offline EVALUATION surface: the GameState->Ctx adapter, the
# pinned-baseline behavior, the policy-linked flop geometry enumerators, and
# (later) the simulator, estimator, oracle, stats, digest, translator, frozen
# candidate and measurement drivers. This is the ONE target permitted to
# include both game_definition.hpp and decision.hpp. It links the deployed
# policy for the baseline call ONLY; the configure-time stage-6 guard prevents
# the edge from ever being reversed into the live service/protocols.
add_library(bigshark_stage6_eval STATIC
  src/stage6/adapter.cpp
  src/stage6/baseline_policy.cpp
  src/stage6/br_estimator.cpp
  src/stage6/candidate_policy.cpp
  src/stage6/chart_digest.cpp
  src/stage6/chart_preflop.cpp
  src/stage6/chart_reach.cpp
  src/stage6/geometry_enumerator.cpp
  src/stage6/infoset_key.cpp
  src/stage6/crn_streams.cpp
  src/stage6/exact_oracle.cpp
  src/stage6/simulator.cpp
  src/stage6/statistics.cpp
  src/stage6/translator.cpp
)
target_include_directories(bigshark_stage6_eval
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_stage6_eval
  PUBLIC  bigshark_behavior bigshark_stage6_core
  PRIVATE bigshark_poker bigshark_solver bigshark_policy ${BS_OPENSSL_CRYPTO_TARGET}
)

# RFC 0008 stage 6 R12 step 7: the multiplayer external-sampling MCCFR
# trainer. It is a SIBLING of eval, never a dependent: the trainer produces the
# frozen postflop candidate artifacts but must not reach the deployed policy,
# the eval-side translator/baseline/adapter, or bigshark_policy. Its closure is
# core (pure-poker shared vocabulary) + solver (the RFC 0006 joint sampler,
# which itself links only poker/abstraction/tree) and nothing else. The
# configure-time stage6_train_edge_guard.cmake and a build-time resolved-include
# scan enforce both directions of that edge.
add_library(bigshark_stage6_train STATIC
  src/stage6/mccfr_trainer.cpp
)
target_include_directories(bigshark_stage6_train
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_stage6_train
  PUBLIC bigshark_stage6_core bigshark_solver
)

# RFC 0008 stage 6: the offline single-human practice table. One local human
# seat (driven by a callback) plays 2..10-seat hands against offline bot
# BehaviorPolicies; it never links the service, the engine host, a protocol, or
# anything network/credential-related. It sits on the eval surface because its
# Medium bot IS the pinned baseline policy.
add_library(bigshark_practice STATIC
  src/stage6/practice_table.cpp
)
target_include_directories(bigshark_practice
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bigshark_practice
  PUBLIC  bigshark_stage6_eval
  PRIVATE bigshark_poker
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

  # RFC 0008 stage 6: the offline behavior-policy interface and the declared
  # uniform reference. Links only bigshark_behavior (which links only poker);
  # a separate link-guard test enforces that service/protocols never link it.
  add_executable(test_behavior_policy tests/test_behavior_policy.cpp)
  target_link_libraries(test_behavior_policy PRIVATE bigshark_behavior)
  add_test(NAME behavior_policy COMMAND test_behavior_policy)

  # RFC 0008 stage 6 R4/F5: the GameState->Ctx adapter must drive the pinned
  # baseline byte-identically to the deployed parseRequest path (same action
  # and amount) on the required corpus. Links the eval surface, which is the
  # one place allowed to join poker and policy vocabulary.
  add_executable(test_stage6_adapter_corpus tests/test_stage6_adapter_corpus.cpp)
  target_link_libraries(test_stage6_adapter_corpus
    PRIVATE bigshark_stage6_eval bigshark_v0_protocol)
  add_test(NAME stage6_adapter_corpus COMMAND test_stage6_adapter_corpus)

  # RFC 0008 stage 6 R4: golden SHA-256 over the parsed chart data pins the
  # baseline by content, not prose.
  add_executable(test_stage6_chart_digest tests/test_stage6_chart_digest.cpp)
  target_link_libraries(test_stage6_chart_digest PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_chart_digest COMMAND test_stage6_chart_digest)

  # RFC 0008 stage 6 R8/R9: Student-t moments, scalar-sum CI, paired CI.
  add_executable(test_stage6_statistics tests/test_stage6_statistics.cpp)
  target_link_libraries(test_stage6_statistics PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_statistics COMMAND test_stage6_statistics)

  # RFC 0008 stage 6 Revision 3: geometry bucketing (key, totality, no-mix
  # guard, reduced representative).
  add_executable(test_stage6_geometry tests/test_stage6_geometry.cpp)
  target_link_libraries(test_stage6_geometry PRIVATE bigshark_stage6_eval)
  # bigshark_tree arrives transitively (PUBLIC dep of bigshark_solver); it is
  # not named here or the static archive is linked twice.
  add_test(NAME stage6_geometry COMMAND test_stage6_geometry)

  # RFC 0008 stage 6 R12 step 4: full-hand N-seat simulator + exact 3p oracle
  # (conservation, determinism, total geometry coverage, joint enumeration).
  add_executable(test_stage6_simulator tests/test_stage6_simulator.cpp)
  target_link_libraries(test_stage6_simulator PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_simulator COMMAND test_stage6_simulator)

  # RFC 0008 stage 6 step-5 foundation: infoset-key pooling invariant and
  # content-addressed CRN stream derivation.
  add_executable(test_stage6_infoset_crn tests/test_stage6_infoset_crn.cpp)
  target_link_libraries(test_stage6_infoset_crn PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_infoset_crn COMMAND test_stage6_infoset_crn)

  # RFC 0008 stage 6: the offline single-human practice table core, driven by
  # a scripted HumanAgent (full-hand completion, zero-sum, button/blind
  # layout, fold privacy, illegal-action rejection, seed reproducibility).
  add_executable(test_stage6_practice tests/test_stage6_practice.cpp)
  target_link_libraries(test_stage6_practice PRIVATE bigshark_practice)
  add_test(NAME stage6_practice COMMAND test_stage6_practice)

  # RFC 0008 stage 6 R12 step 5: two-phase learn/confirm Monte Carlo
  # deviation-gain estimator (pooled BR, CRN pairing, exact-oracle gates).
  add_executable(test_stage6_br_estimator tests/test_stage6_br_estimator.cpp)
  target_link_libraries(test_stage6_br_estimator PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_br_estimator COMMAND test_stage6_br_estimator)

  # RFC 0008 stage 6 R12 step 6: the deterministic exact<->coarse translator
  # (R7) — passive passthrough, nearest snap with ties to the smaller total,
  # mass merge, declared call/check/fold fallback, fail-closed input
  # validation, and exact->coarse nearest-index mapping, all pinned against
  # the real GameState legal interval.
  add_executable(test_stage6_translator tests/test_stage6_translator.cpp)
  target_link_libraries(test_stage6_translator PRIVATE bigshark_stage6_eval)
  add_test(NAME stage6_translator COMMAND test_stage6_translator)

  # RFC 0008 stage 6 R12 step 7: external-sampling multiplayer MCCFR (RM+
  # regrets, uniform visit-weighted average), independent scripted-world
  # algebra reference, statistical contraction/repeatability gates, and the
  # exhaustive materialized/streaming cursor-alignment proof. Links the
  # policy-free trainer sibling only.
  add_executable(test_stage6_trainer tests/test_stage6_trainer.cpp)
  target_link_libraries(test_stage6_trainer PRIVATE bigshark_stage6_train)
  add_test(NAME stage6_trainer COMMAND test_stage6_trainer)

  # RFC 0008 stage 6 P1-1: enumerable three-player river game parity between
  # the production external sampler and an independently written full-tree
  # CFR, on both the average policy and the general-sum NashConv scalar.
  add_executable(test_stage6_multiway_average tests/test_stage6_multiway_average.cpp)
  target_link_libraries(test_stage6_multiway_average PRIVATE bigshark_stage6_train)
  add_test(NAME stage6_multiway_average COMMAND test_stage6_multiway_average)

  # RFC 0008 stage 6 P2-2: artifact write/load roundtrip and tamper refusal.
  add_executable(test_stage6_frozen_artifact tests/test_stage6_frozen_artifact.cpp)
  target_link_libraries(test_stage6_frozen_artifact PRIVATE bigshark_stage6_core)
  add_test(NAME stage6_frozen_artifact COMMAND test_stage6_frozen_artifact)

  # RFC 0008 stage 6 R12 step 8: the composed candidate (pinned charts preflop,
  # frozen trained artifact postflop, R7 projection, total geometry lookup).
  # This is the documented LEAF that joins the eval and trainer siblings: it
  # trains a small artifact in-process and binds it into the candidate.
  add_executable(test_stage6_candidate_policy tests/test_stage6_candidate_policy.cpp)
  target_link_libraries(test_stage6_candidate_policy
    PRIVATE bigshark_stage6_eval bigshark_stage6_train)
  add_test(NAME stage6_candidate_policy COMMAND test_stage6_candidate_policy)

  # RFC 0008 stage 6 R11 item 2 (chunk A): chart-only reach carries the
  # reaching preflop log; per-live-seat chart-reach-restricted hole ranges and
  # the fail-closed bucket union, checked against the pinned combo counts.
  add_executable(test_stage6_chart_reach tests/test_stage6_chart_reach.cpp)
  target_link_libraries(test_stage6_chart_reach
    PRIVATE bigshark_stage6_eval bigshark_stage6_train)
  add_test(NAME stage6_chart_reach COMMAND test_stage6_chart_reach)

  add_executable(test_heads_up tests/test_heads_up.cpp)
  target_link_libraries(test_heads_up PRIVATE bigshark_poker)
  add_test(NAME heads_up COMMAND test_heads_up)

  # RFC 0004 Stage 10 preflop profile: blind posting, button-first order, the
  # big blind option after a limp, postflop reversal, conservation, and the
  # unchanged flop-rooted profile.
  add_executable(test_heads_up_preflop tests/test_heads_up_preflop.cpp)
  target_link_libraries(test_heads_up_preflop PRIVATE bigshark_poker)
  add_test(NAME heads_up_preflop COMMAND test_heads_up_preflop)

  # RFC 0006 experimental multiway profile: 3..6-player order, the blinded
  # preflop opener, the raise-reopening rule, and exact settlement.
  add_executable(test_multiway tests/test_multiway.cpp)
  target_link_libraries(test_multiway PRIVATE bigshark_poker)
  add_test(NAME multiway COMMAND test_multiway)

  # RFC 0006 joint-deal sampling: the enumerated joint distribution, the
  # rejection-conditional sampler that must match it, and reproducibility.
  add_executable(test_multiway_sampler tests/test_multiway_sampler.cpp)
  target_link_libraries(test_multiway_sampler PRIVATE bigshark_solver)
  target_include_directories(test_multiway_sampler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME multiway_sampler COMMAND test_multiway_sampler)

  # RFC 0008 cross-type invariants: the boundary between rules-differ and
  # implementation-drift, asserted against both separate implementations before
  # any unification. Guards the §L1 merge hazards.
  add_executable(test_unification_invariants tests/test_unification_invariants.cpp)
  target_link_libraries(test_unification_invariants PRIVATE bigshark_poker)
  add_test(NAME unification_invariants COMMAND test_unification_invariants)

  # RFC 0008 stage 1: the unified 2..10-seat game definition, driven against an
  # independent reference ledger that is written from the rules and never
  # includes bs/heads_up.hpp.
  add_executable(test_game_definition tests/test_game_definition.cpp)
  target_link_libraries(test_game_definition PRIVATE bigshark_poker)
  add_test(NAME game_definition COMMAND test_game_definition)

  # RFC 0008 stage 2: the same machine widened to 3..10 seats. At 3..6 it
  # drives the real MultiwayState in lockstep; 7..10 rides an independently
  # written N-seat ledger and an independent settlement implementation.
  add_executable(test_game_definition_multiway tests/test_game_definition_multiway.cpp)
  target_link_libraries(test_game_definition_multiway PRIVATE bigshark_poker)
  add_test(NAME game_definition_multiway COMMAND test_game_definition_multiway)

  # RFC 0008 stage 3: the L2 component tests link the abstraction library (which
  # itself brings only the rules layer) and no solver, storage, or transport, so
  # a stray dependency out of L2 is a link error.
  add_executable(test_abstraction tests/test_abstraction.cpp)
  target_link_libraries(test_abstraction PRIVATE bigshark_abstraction)
  add_test(NAME abstraction COMMAND test_abstraction)

  # RFC 0008 stage 3: the lifted ordered-menu rule must reproduce the shipped
  # solver abstract_actions element-for-element, and the identity abstraction
  # must add zero exploitability on an enumerable heads-up fixture.
  add_executable(test_abstraction_equivalence tests/test_abstraction_equivalence.cpp)
  target_link_libraries(test_abstraction_equivalence PRIVATE bigshark_solver)
  add_test(NAME abstraction_equivalence COMMAND test_abstraction_equivalence)

  # RFC 0008 stage 4 L3 fidelity oracle: an independently coded GameState
  # enumerator walks the materialized AbstractTree in lockstep at 2 and 3
  # seats, pinning node kinds, menus, chance sets, and terminal ledgers.
  add_executable(test_abstract_tree_fidelity tests/test_abstract_tree_fidelity.cpp)
  target_link_libraries(test_abstract_tree_fidelity PRIVATE bigshark_solver)
  add_test(NAME abstract_tree_fidelity COMMAND test_abstract_tree_fidelity)

  # RFC 0008 stage 4 L4 conformance: solve() reproduces the heads-up CFR's
  # golden results bit-for-bit (full + sampled, all runout variants) and
  # refuses unsupported shapes with the typed exception.
  add_executable(test_solve_conformance tests/test_solve_conformance.cpp)
  target_link_libraries(test_solve_conformance PRIVATE bigshark_solver)
  add_test(NAME solve_conformance COMMAND test_solve_conformance)

  # RFC 0009 W2a L4 conformance: the seat-parameterized MCCFR trainer consumes
  # the L3 tree directly. Two-seat parity against the heads-up route on one
  # game, an independently coded 3-seat update-rule oracle over one visible
  # sweep, determinism, the typed refusals, and zero-sum terminals.
  add_executable(test_solve_nseat tests/test_solve_nseat.cpp)
  # The heads-up evaluator's policy surface is reached through the solver
  # debug passkey header for fixture conversion (as test_heads_up_solver and
  # test_resident_policy already do).
  target_include_directories(test_solve_nseat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_solve_nseat PRIVATE bigshark_solver)
  add_test(NAME solve_nseat COMMAND test_solve_nseat)

  # RFC 0009 W2b: the bucket->concrete seat-policy export and the unified
  # information-key constructor. Links the solver (which owns seat_policy.cpp)
  # and the artifacts codec for the revision-2 key round-trip conformance case.
  add_executable(test_seat_policy tests/test_seat_policy.cpp)
  target_link_libraries(test_seat_policy PRIVATE bigshark_solver bigshark_artifacts)
  add_test(NAME seat_policy COMMAND test_seat_policy)

  # RFC 0008 stage 4 link-negative guard: links bigshark_tree WITHOUT the
  # solver. The solver LIBRARY is not reachable from L3 by construction (the
  # tree target links only poker+abstraction); the source-token half of the
  # boundary is the build-time bigshark_l3_boundary_guard target above.
  add_executable(test_tree_link_negative tests/test_tree_link_negative.cpp)
  target_link_libraries(test_tree_link_negative PRIVATE bigshark_tree)
  add_test(NAME tree_link_negative COMMAND test_tree_link_negative)

  # RFC 0008 stage 6 offline link-negative guard: this binary links the live
  # v0+v1 protocols (and through them the service, policy, solver) and drives
  # one parse/evaluate/serialize path. A companion cmake-script test runs nm
  # and fails if the final link contains ANY bs::stage6 symbol, proving no
  # offline object code reached a live binary.
  add_executable(test_stage6_link_negative tests/test_stage6_link_negative.cpp)
  target_link_libraries(test_stage6_link_negative
    PRIVATE bigshark_v1_protocol bigshark_v0_protocol)
  add_test(NAME stage6_link_negative_scan
    COMMAND ${CMAKE_COMMAND}
            -DSTAGE6_BIN=$<TARGET_FILE:test_stage6_link_negative>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage6_link_negative.cmake")

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
    src/abstraction/abstraction.cpp
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

  add_executable(test_equity_capacity tests/test_equity_capacity.cpp)
  target_link_libraries(test_equity_capacity PRIVATE bigshark_poker)
  add_test(NAME equity-capacity COMMAND test_equity_capacity)

  add_executable(test_v0_protocol tests/test_v0_protocol.cpp)
  target_link_libraries(test_v0_protocol PRIVATE bigshark_v0_protocol)
  add_test(NAME v0_protocol COMMAND test_v0_protocol)

  # RFC 0008 stage 5: the guarantee ladder/single normative table, and the
  # structural decision-source provenance oracle (river backend seam lives in
  # the solver facade, which bigshark_policy links privately).
  add_executable(test_guarantee tests/test_guarantee.cpp)
  target_link_libraries(test_guarantee PRIVATE bigshark_policy)
  add_test(NAME guarantee COMMAND test_guarantee)

  add_executable(test_decision_provenance tests/test_decision_provenance.cpp)
  target_link_libraries(test_decision_provenance PRIVATE bigshark_policy bigshark_solver)
  target_include_directories(test_decision_provenance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME decision_provenance COMMAND test_decision_provenance)

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
