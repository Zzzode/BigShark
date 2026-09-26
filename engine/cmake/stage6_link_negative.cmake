# RFC 0008 stage 6 offline link-negative scan.
#
# Runs `nm` over the live test_stage6_link_negative binary (which links v0+v1
# and drives a real decision path) and fails on ANY symbol in the Itanium C++
# mangling for namespace bs::stage6 ("6stage6"), where the behavior interface,
# the uniform reference, and every later stage-6 component live. Static
# archives contribute only referenced objects, so a clean scan proves no
# offline object code reached the final live link.
if(NOT DEFINED STAGE6_BIN)
  message(FATAL_ERROR "stage6 link-negative scan: STAGE6_BIN not set")
endif()

find_program(NM_EXECUTABLE NAMES nm)
if(NOT NM_EXECUTABLE)
  message(FATAL_ERROR "stage6 link-negative scan: nm not found")
endif()

execute_process(
  COMMAND "${NM_EXECUTABLE}" "${STAGE6_BIN}"
  OUTPUT_VARIABLE nm_output
  RESULT_VARIABLE nm_result
)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "stage6 link-negative scan: nm failed")
endif()

if(nm_output MATCHES "6stage6")
  string(REPLACE "\n" "\n  " leaked "${nm_output}")
  message(FATAL_ERROR
    "RFC 0008 stage-6 offline boundary violated: the live link contains "
    "bs::stage6 symbols:\n  ${leaked}\n"
    "Offline measurement code must not be linked into v0/v1/service/host.")
endif()

message(STATUS "stage6 link-negative scan passed")
