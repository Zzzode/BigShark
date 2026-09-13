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
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/v1/engine.pb.cc"
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/v1/engine.pb.h"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/bigshark/engine/v1/engine.proto"
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
  "${BIGSHARK_PROTO_GENERATED_DIR}/bigshark/engine/v1/engine.pb.cc"
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
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(protobuf-cpp-format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_generated.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/write_cpp_vector.cpp"
    COMMENT "Formatting Protobuf C++ tests"
    VERBATIM
  )
  add_custom_target(protobuf-cpp-format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_generated.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/write_cpp_vector.cpp"
    COMMENT "Checking Protobuf C++ test formatting"
    VERBATIM
  )
endif()
add_custom_target(protobuf-breaking
  COMMAND ${BIGSHARK_NPM_EXECUTABLE} run proto:breaking
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Checking Protobuf v1 compatibility"
  VERBATIM
)
add_custom_target(protobuf-check
  COMMAND ${CMAKE_COMMAND}
          "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
          "-DBIGSHARK_NPM_SCRIPT=proto:check"
          -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
  DEPENDS bigshark_protocol
  COMMENT "Checking Protobuf sources, bindings, and vectors"
  VERBATIM
)
if(TARGET protobuf-cpp-format-check)
  add_dependencies(protobuf-check protobuf-cpp-format-check)
endif()

if(BUILD_TESTING)
  add_executable(test_protobuf_generated tests/test_generated.cpp)
  target_compile_features(test_protobuf_generated PRIVATE cxx_std_17)
  target_link_libraries(test_protobuf_generated PRIVATE bigshark_protocol)
  if(BIGSHARK_ENABLE_SANITIZERS)
    target_compile_options(test_protobuf_generated PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(test_protobuf_generated PRIVATE
      -fsanitize=address,undefined
    )
  endif()
  target_compile_definitions(test_protobuf_generated PRIVATE
    BIGSHARK_PROTO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  )
  add_test(NAME protobuf_generated COMMAND test_protobuf_generated)

  add_executable(write_protobuf_cpp_vector tests/write_cpp_vector.cpp)
  target_compile_features(write_protobuf_cpp_vector PRIVATE cxx_std_17)
  target_link_libraries(write_protobuf_cpp_vector PRIVATE bigshark_protocol)
  if(BIGSHARK_ENABLE_SANITIZERS)
    target_compile_options(write_protobuf_cpp_vector PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(write_protobuf_cpp_vector PRIVATE
      -fsanitize=address,undefined
    )
  endif()
  set(BIGSHARK_CPP_PROTO_VECTOR
    "${CMAKE_BINARY_DIR}/generated/fixtures/cpp-envelope.binpb"
  )
  add_test(
    NAME protobuf_cpp_vector
    COMMAND write_protobuf_cpp_vector "${BIGSHARK_CPP_PROTO_VECTOR}"
  )

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
  add_test(
    NAME protobuf_typescript
    COMMAND ${CMAKE_COMMAND}
            "-DBIGSHARK_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBIGSHARK_NPM_SCRIPT=proto:test:generated"
            "-DBIGSHARK_CPP_PROTO_VECTOR=${BIGSHARK_CPP_PROTO_VECTOR}"
            -P "${BIGSHARK_TYPESCRIPT_LOCK_SCRIPT}"
  )
  set_tests_properties(protobuf_typescript PROPERTIES
    DEPENDS protobuf_cpp_vector
    RESOURCE_LOCK typescript_dist
  )
endif()
