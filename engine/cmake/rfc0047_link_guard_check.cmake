# CTest-side checker for the RFC 0047 Stage-1 link isolation manifest.
if(NOT DEFINED RFC0047_MANIFEST)
  message(FATAL_ERROR "rfc0047 link isolation check: RFC0047_MANIFEST not set")
endif()
if(NOT EXISTS "${RFC0047_MANIFEST}")
  message(FATAL_ERROR
    "rfc0047 link isolation check: manifest not found (re-run CMake configure)")
endif()

file(READ "${RFC0047_MANIFEST}" _manifest)
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
      if(_line MATCHES "bigshark_rfc0047_bench")
        message(FATAL_ERROR
          "RFC 0047 Stage-1 link isolation violated (ctest): '${_root}' "
          "recorded closure contains bigshark_rfc0047_bench.")
      endif()
    endif()
  endforeach()
  if(NOT _found)
    message(FATAL_ERROR
      "rfc0047 link isolation check: no manifest row for '${_root}'")
  endif()
endforeach()

message(STATUS "rfc0047 link isolation check passed")
