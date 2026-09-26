cmake_minimum_required(VERSION 3.20)

project(BigShark LANGUAGES C CXX)

include(CTest)

# Engine is configured before proto because its vendored Abseil must satisfy
# protobuf's Abseil dependency; reordering creates duplicate Abseil targets.
# The RFC 0002 v1 protocol target is therefore declared in proto/CMakeLists.txt,
# while its sources and tests remain under engine/.
add_subdirectory(engine)
add_subdirectory(proto)
add_subdirectory(apps/engine-host)
add_subdirectory(tools/typescript)
add_subdirectory(clients/node)
add_subdirectory(platforms/river-club)
add_subdirectory(docs)

# RFC 0008 stage 6 offline boundary: included LAST so every protected target
# and its transitive link closure exist before the configure-time assertion
# walks them.
include("${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/stage6_offline_guard.cmake")

# RFC 0008 stage-6 trainer sibling edge: trainer and eval may not link each
# other, and the trainer may never reach the deployed policy. Included last so
# both sibling targets exist before the closure walk.
include("${CMAKE_CURRENT_SOURCE_DIR}/engine/cmake/stage6_train_edge_guard.cmake")

if(TARGET engine-format AND TARGET engine-host-format)
  add_custom_target(format DEPENDS engine-format engine-host-format)
  add_custom_target(format-check DEPENDS engine-format-check engine-host-format-check)
  if(TARGET protobuf-format AND TARGET protobuf-cpp-format)
    add_dependencies(format protobuf-format protobuf-cpp-format)
    add_dependencies(format-check protobuf-format-check protobuf-cpp-format-check)
  endif()
endif()
