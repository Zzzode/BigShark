add_executable(bigshark-engine main.cpp)
target_compile_features(bigshark-engine PRIVATE cxx_std_23)
set_target_properties(bigshark-engine PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(bigshark-engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(bigshark-engine
  PRIVATE bigshark_v1_protocol bigshark_v0_protocol bigshark_yyjson
)
if(BIGSHARK_ENABLE_SANITIZERS)
  target_compile_options(bigshark-engine PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
  )
  target_link_options(bigshark-engine PRIVATE -fsanitize=address,undefined)
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(engine-host-format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    COMMENT "Formatting engine host C++ sources"
    VERBATIM
  )
  add_custom_target(engine-host-format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    COMMENT "Checking engine host C++ formatting"
    VERBATIM
  )
endif()

if(BIGSHARK_PUBLISH_ENGINE)
  add_custom_command(TARGET bigshark-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bigshark-engine>
            ${CMAKE_SOURCE_DIR}/bin/bigshark-engine
    COMMENT "Publishing bigshark-engine to bin"
  )
endif()
