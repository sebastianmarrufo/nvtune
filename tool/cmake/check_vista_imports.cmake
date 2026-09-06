# SPDX-License-Identifier: GPL-3.0-or-later
# Reject accidental UCRT or compiler DLL dependencies in the portable build.
# This supplements, but cannot replace, execution on Windows Vista.
execute_process(COMMAND "${NVTUNE_OBJDUMP}" -p "${NVTUNE_BINARY}"
    RESULT_VARIABLE result OUTPUT_VARIABLE headers ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect ${NVTUNE_BINARY}: ${diagnostic}")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" imports "${headers}")
if(NOT imports)
    message(FATAL_ERROR "No PE imports found in ${NVTUNE_BINARY}; refusing an unchecked Vista build.")
endif()
foreach(import IN LISTS imports)
    string(REPLACE "DLL Name: " "" dll "${import}")
    string(STRIP "${dll}" dll)
    string(TOLOWER "${dll}" dll)
    if(NOT dll MATCHES "^(advapi32|kernel32|msvcrt|setupapi|cfgmgr32)\\.dll$")
        message(FATAL_ERROR
            "${NVTUNE_BINARY} imports ${dll}, outside the Vista system DLL contract. "
            "Use an msvcrt mingw-w64 toolchain with NVTUNE_STATIC_RUNTIME=ON.")
    endif()
endforeach()
message(STATUS "Vista system DLL audit passed: ${NVTUNE_BINARY}")
