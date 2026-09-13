cmake_minimum_required(VERSION 3.20)

project(BigShark LANGUAGES C CXX)

include(CTest)

add_subdirectory(engine)
add_subdirectory(proto)
add_subdirectory(apps/engine-host)
add_subdirectory(tools/typescript)
add_subdirectory(clients/node)
add_subdirectory(platforms/river-club)
add_subdirectory(docs)

if(TARGET engine-format AND TARGET engine-host-format)
  add_custom_target(format DEPENDS engine-format engine-host-format)
  add_custom_target(format-check DEPENDS engine-format-check engine-host-format-check)
  if(TARGET protobuf-format AND TARGET protobuf-cpp-format)
    add_dependencies(format protobuf-format protobuf-cpp-format)
    add_dependencies(format-check protobuf-format-check protobuf-cpp-format-check)
  endif()
endif()
