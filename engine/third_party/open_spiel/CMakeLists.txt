# Minimal vendored OpenSpiel core: only the pieces needed to build an
# infostate-tree sequence-form Nash solver (no game registry/spiel.cc, no
# OR-Tools, no Python). Abseil is built from source as a subdirectory.
#
# Layout under this dir:
#   ./open_spiel/*.h,*.cc          core types
#   ./open_spiel/algorithms/       infostate_tree
#   ./open_spiel/abseil-cpp/       vendored abseil
#   ./open_spiel/json/             nlohmann json

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF)

add_subdirectory(open_spiel/abseil-cpp EXCLUDE_FROM_ALL)

add_library(open_spiel_min STATIC
  open_spiel/spiel_utils.cc
  open_spiel/game_parameters.cc
  open_spiel/observer.cc
  open_spiel/policy.cc
  open_spiel/action_view.cc
  open_spiel/spiel.cc
  open_spiel/algorithms/infostate_tree.cc
  open_spiel/utils/status.cc
  open_spiel/utils/usage_logging.cc
)
target_include_directories(open_spiel_min SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/open_spiel/abseil-cpp
)
target_link_libraries(open_spiel_min PUBLIC
  absl::strings
  absl::raw_hash_set
  absl::str_format_internal
  absl::time_zone
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(open_spiel_min PRIVATE -Wno-everything)
else()
  target_compile_options(open_spiel_min PRIVATE -w)
endif()
