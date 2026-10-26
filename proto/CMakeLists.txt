include(FetchContent)

find_program(BIGSHARK_NPM_EXECUTABLE NAMES npm REQUIRED)
find_program(BIGSHARK_BUF_EXECUTABLE NAMES buf
  HINTS "${CMAKE_SOURCE_DIR}/node_modules/.bin"
  REQUIRED
)
set(BIGSHARK_PROTO_TOOLCHAIN_SCRIPT
  "${CMAKE_SOURCE_DIR}/tools/cmake/ensure-protobuf-toolchain.cmake"
)
set(BIGSHARK_TYPESCRIPT_LOCK_SCRIPT
  "${CMAKE_SOURCE_DIR}/tools/cmake/run-npm-with-typescript-lock.cmake"
)

set(BIGSHARK_PROTOBUF_VERSION "36.1")
set(BIGSHARK_PROTOBUF_URL
  "https://github.com/protocolbuffers/protobuf/releases/download/v${BIGSHARK_PROTOBUF_VERSION}/protobuf-${BIGSHARK_PROTOBUF_VERSION}.tar.gz"
)
set(BIGSHARK_PROTOBUF_SHA256
  "dc74fa582f559cbd31614ddfefb4868f43c919d7184bde514bb47f90c6025eb8"
)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  bigshark_protobuf
  URL "${BIGSHARK_PROTOBUF_URL}"
  URL_HASH "SHA256=${BIGSHARK_PROTOBUF_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(bigshark_protobuf)
if(TARGET libprotobuf AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(libprotobuf PRIVATE -Wno-deprecated-declarations)
endif()
if(BIGSHARK_ENABLE_SANITIZERS)
  foreach(protobuf_target libprotobuf libprotobuf-lite)
    if(TARGET ${protobuf_target})
      target_compile_options(${protobuf_target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
      )
    endif()
  endforeach()
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/buf.gen.cmake.yaml.in"
  "${CMAKE_CURRENT_BINARY_DIR}/buf.gen.yaml"
  @ONLY
)

set(BIGSHARK_PROTO_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/cpp")
set(BIGSHARK_PROTO_TOOLCHAIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/toolchain-js")
set(BIGSHARK_PROTO_GENERATED_FILES
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/engine.pb.cc"
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/engine.pb.h"
  "${BIGSHARK_PROTO_GENERATED_DIR}/buf/validate/validate.pb.cc"
  "${BIGSHARK_PROTO_GENERATED_DIR}/buf/validate/validate.pb.h"
)

add_custom_command(
  OUTPUT ${BIGSHARK_PROTO_GENERATED_FILES}
  COMMAND ${CMAKE_COMMAND}
          "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
          "-DBIGSHARK_PROTO_TOOLCHAIN_DIR=${BIGSHARK_PROTO_TOOLCHAIN_DIR}"
          -P "${BIGSHARK_PROTO_TOOLCHAIN_SCRIPT}"
  COMMAND ${BIGSHARK_BUF_EXECUTABLE} generate
          --template "${CMAKE_CURRENT_BINARY_DIR}/buf.gen.yaml"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bigshark/engine/engine.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/buf.yaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/buf.lock"
    "${CMAKE_CURRENT_SOURCE_DIR}/buf.gen.cmake.yaml.in"
    "${CMAKE_SOURCE_DIR}/package.json"
    "${CMAKE_SOURCE_DIR}/package-lock.json"
    "${CMAKE_SOURCE_DIR}/tsconfig.json"
    "${BIGSHARK_PROTO_TOOLCHAIN_SCRIPT}"
    "${CMAKE_SOURCE_DIR}/tools/proto/ensure-toolchain.ts"
    "${CMAKE_SOURCE_DIR}/tools/proto/tsconfig.json"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Generating preset-local Protobuf C++ bindings"
  VERBATIM
)
add_custom_target(protobuf-generate DEPENDS ${BIGSHARK_PROTO_GENERATED_FILES})

add_library(bigshark_protocol STATIC
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/engine.pb.cc"
  "${BIGSHARK_PROTO_GENERATED_DIR}/buf/validate/validate.pb.cc"
)
add_dependencies(bigshark_protocol protobuf-generate)
target_compile_features(bigshark_protocol PUBLIC cxx_std_17)
target_include_directories(bigshark_protocol
  PUBLIC "${BIGSHARK_PROTO_GENERATED_DIR}"
)
target_link_libraries(bigshark_protocol PUBLIC protobuf::libprotobuf)
if(BIGSHARK_ENABLE_SANITIZERS)
  target_compile_options(bigshark_protocol PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
  )
endif()

# Framed Protobuf engine protocol. The target is declared in this directory
# because engine/ is configured before proto/ (its vendored Abseil satisfies
# protobuf's Abseil dependency); sources and tests remain in
# engine/src/protocol and engine/tests. Generated Protobuf headers stay
# PRIVATE: the public include/bs/protocol.hpp boundary is protobuf-free.
set(BIGSHARK_ENGINE_DIR "${CMAKE_SOURCE_DIR}/engine")
add_library(bigshark_engine_protocol STATIC
  "${BIGSHARK_ENGINE_DIR}/src/protocol/frame_stream.cpp"
  "${BIGSHARK_ENGINE_DIR}/src/protocol/semantic_validator.cpp"
  "${BIGSHARK_ENGINE_DIR}/src/protocol/request_mapper.cpp"
  "${BIGSHARK_ENGINE_DIR}/src/protocol/response_mapper.cpp"
  "${BIGSHARK_ENGINE_DIR}/src/protocol/resident_mapper.cpp"
  "${BIGSHARK_ENGINE_DIR}/src/protocol/envelope.cpp"
)
target_compile_features(bigshark_engine_protocol PUBLIC cxx_std_23)
target_compile_options(bigshark_engine_protocol PRIVATE
  -Wall -Wextra -Wno-unused-parameter
)
# Enum-conversion switches must be exhaustive. The repo builds without
# -Werror; this elevates only -Wswitch, matching bigshark_policy.
target_compile_options(bigshark_engine_protocol PRIVATE -Werror=switch)
target_include_directories(bigshark_engine_protocol
  PUBLIC  "${BIGSHARK_ENGINE_DIR}/include"
  PRIVATE "${BIGSHARK_ENGINE_DIR}/src"
)
target_link_libraries(bigshark_engine_protocol
  PUBLIC  bigshark_service
  PRIVATE bigshark_protocol
  PRIVATE bigshark_resident
)
if(BIGSHARK_ENABLE_SANITIZERS)
  target_compile_options(bigshark_engine_protocol PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
  )
endif()

# libFuzzer target for the untrusted-input framing, envelope parse, and
# request validator/mapper paths. Off by default so release and ASan ctest
# runs are unaffected; see docs/development/build-and-test.md. The entry point
# (fuzz_harness.cpp) and the standalone main (fuzz_standalone_driver.cpp) are
# separate translation units, so both configurations link without a
# conditional main or a duplicate-symbol hazard.
option(BIGSHARK_ENABLE_FUZZ "Build the protocol fuzz target" OFF)
option(BIGSHARK_FUZZ_LIBFUZZER "Link the fuzz target against libFuzzer" OFF)
if(BIGSHARK_ENABLE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "BIGSHARK_ENABLE_FUZZ requires Clang.")
  endif()
  # The harness TU is always present; the standalone driver is added only
  # without libFuzzer, whose runtime supplies main.
  set(BIGSHARK_FUZZ_SOURCES
    "${BIGSHARK_ENGINE_DIR}/tests/fuzz_harness.cpp"
  )
  if(NOT BIGSHARK_FUZZ_LIBFUZZER)
    list(APPEND BIGSHARK_FUZZ_SOURCES
      "${BIGSHARK_ENGINE_DIR}/tests/fuzz_standalone_driver.cpp"
    )
  endif()
  if(BIGSHARK_FUZZ_LIBFUZZER)
    set(BIGSHARK_FUZZ_SANITIZERS fuzzer,address,undefined)
  else()
    set(BIGSHARK_FUZZ_SANITIZERS address,undefined)
  endif()
  add_executable(fuzz_proto ${BIGSHARK_FUZZ_SOURCES})
  target_compile_features(fuzz_proto PRIVATE cxx_std_23)
  target_compile_options(fuzz_proto PRIVATE
    -fsanitize=${BIGSHARK_FUZZ_SANITIZERS}
    -fno-omit-frame-pointer
    -g
  )
  target_link_options(fuzz_proto PRIVATE
    -fsanitize=${BIGSHARK_FUZZ_SANITIZERS}
  )
  target_include_directories(fuzz_proto PRIVATE "${BIGSHARK_ENGINE_DIR}/src")
  target_link_libraries(fuzz_proto PRIVATE
    bigshark_engine_protocol bigshark_protocol
  )
endif()

add_custom_target(protobuf-lint
  COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:lint
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Linting Protobuf sources"
  VERBATIM
)
add_custom_target(protobuf-format
  COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:format
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Formatting Protobuf sources"
  VERBATIM
)
add_custom_target(protobuf-format-check
  COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:format:check
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Checking Protobuf formatting"
  VERBATIM
)
add_custom_target(protobuf-breaking
  COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:breaking
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Checking Protobuf compatibility"
  VERBATIM
)
add_custom_target(protobuf-check
  COMMAND ${CMAKE_COMMAND}
          "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
          "-DBIGSHARK_NPM_SCRIPT=proto:check"
          -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
  DEPENDS bigshark_protocol
  COMMENT "Checking Protobuf sources and bindings"
  VERBATIM
)

if(BUILD_TESTING)
  # Framed protocol conformance tests. Sources live in engine/tests;
  # targets are declared here so they can link bigshark_protocol directly.
  add_executable(test_conformance
    "${BIGSHARK_ENGINE_DIR}/tests/test_conformance.cpp"
  )
  target_compile_features(test_conformance PRIVATE cxx_std_23)
  target_compile_options(test_conformance PRIVATE -Wall -Wextra)
  target_include_directories(test_conformance PRIVATE "${BIGSHARK_ENGINE_DIR}/src")
  target_link_libraries(test_conformance PRIVATE
    bigshark_engine_protocol bigshark_protocol
  )
  if(BIGSHARK_ENABLE_SANITIZERS)
    target_compile_options(test_conformance PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(test_conformance PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME conformance COMMAND test_conformance)

  # Offline tool that publishes a tiny complete postflop blueprint artifact
  # for the real-binary matrix tests. It prints the artifact path,
  # pinned SHA-256, and the JSON root/range spec the test client must build.
  add_executable(bs_resident_fixture
    "${BIGSHARK_ENGINE_DIR}/tests/resident_fixture_publisher.cpp"
  )
  target_compile_features(bs_resident_fixture PRIVATE cxx_std_23)
  target_compile_options(bs_resident_fixture PRIVATE -Wall -Wextra)
  target_include_directories(bs_resident_fixture PRIVATE "${BIGSHARK_ENGINE_DIR}/src")
  target_link_libraries(bs_resident_fixture PRIVATE
    bigshark_engine_protocol bigshark_protocol bigshark_sqlite
  )
  if(BIGSHARK_ENABLE_SANITIZERS)
    target_compile_options(bs_resident_fixture PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(bs_resident_fixture PRIVATE -fsanitize=address,undefined)
  endif()
  if(BIGSHARK_PUBLISH_ENGINE)
    add_custom_command(TARGET bs_resident_fixture POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bs_resident_fixture>
              ${CMAKE_SOURCE_DIR}/bin/bs-resident-fixture
      COMMENT "Publishing bs-resident-fixture to bin"
    )
  endif()

  add_test(
    NAME protobuf_lint
    COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:lint
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  add_test(
    NAME protobuf_format
    COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:format:check
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  add_test(
    NAME protobuf_breaking
    COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:breaking
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()
