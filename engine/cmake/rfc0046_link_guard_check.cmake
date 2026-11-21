# CTest-side checker for the RFC 0046 Stage-1 link isolation manifest.
#
# The configure-time rfc0046_link_guard.cmake walks the real CMake target
# dependency graph and writes each protected root's transitive closure to the
# manifest. This script gives that graph assertion CTest visibility: it fails
# if the recorded closure of any protected root contains the measurement
# library, or if a listed root is missing from the manifest entirely.
if(NOT DEFINED RFC0046_MANIFEST)
  message(FATAL_ERROR "rfc0046 link isolation check: RFC0046_MANIFEST not set")
endif()
if(NOT EXISTS "${RFC0046_MANIFEST}")
  message(FATAL_ERROR
    "rfc0046 link isolation check: manifest not found (re-run CMake configure)")
endif()

file(READ "${RFC0046_MANIFEST}" _manifest)
string(REPLACE "\n" ";" _lines "${_manifest}")
set(_roots
  bigshark-engine
  bigshark-practice
  bigshark_practice
  bigshark_practice_engine
  bigshark_resident
  bigshark-flop-library-builder
  bigshark-flop-library-pack
  bigshark-flop-library-verify-pack)

foreach(_root IN LISTS _roots)
  set(_found FALSE)
  foreach(_line IN LISTS _lines)
    string(REGEX MATCH "^${_root}:" _is_row "${_line}")
    if(_is_row)
      set(_found TRUE)
      if(_line MATCHES "bigshark_rfc0046_bench")
        message(FATAL_ERROR
          "RFC 0046 Stage-1 link isolation violated (ctest): '${_root}' "
          "recorded closure contains bigshark_rfc0046_bench.")
      endif()
    endif()
  endforeach()
  if(NOT _found)
    message(FATAL_ERROR
      "rfc0046 link isolation check: no manifest row for '${_root}'")
  endif()
endforeach()

message(STATUS "rfc0046 link isolation check passed")
