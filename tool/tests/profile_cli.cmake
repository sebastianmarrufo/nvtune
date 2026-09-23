# SPDX-License-Identifier: GPL-3.0-or-later

file(MAKE_DIRECTORY "${TEST_DIR}")
set(profile "${TEST_DIR}/timings.json")
set(backup "${TEST_DIR}/raw.json")
file(REMOVE "${profile}" "${backup}")

execute_process(COMMAND "${CLI}" save --device 0000:08:00.0
    --output "${backup}" --profile "${profile}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "save --profile failed: ${error}")
endif()
file(READ "${profile}" profile_json)
file(READ "${backup}" backup_json)
configure_file("${profile}" "${TEST_DIR}/timings-first.json" COPYONLY)
if(NOT profile_json MATCHES "\"fields\"" OR
   NOT profile_json MATCHES "\"RC\": 10" OR
   profile_json MATCHES "\"REFRESH_LO\"" OR
   profile_json MATCHES "\"RFCSBA\"")
    message(FATAL_ERROR "field export has wrong shape or values: ${profile_json}")
endif()
if(NOT backup_json MATCHES "nvtune-backup-1" OR
   NOT backup_json MATCHES "\"registers\"")
    message(FATAL_ERROR "raw backup was not preserved")
endif()
execute_process(COMMAND "${CLI}" apply "${profile}"
    WORKING_DIRECTORY "${TEST_DIR}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "exported profile could not be loaded by apply: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_RC=12
    "${CLI}" save --device 0000:08:00.0
    --output "${backup}" --profile "${profile}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "repeated save could not replace its own files: ${error}")
endif()
file(READ "${profile}" changed_profile)
if(NOT changed_profile MATCHES "\"RC\": 12")
    message(FATAL_ERROR "repeated save did not replace profile content")
endif()
file(READ "${profile}" profile_before_failure)
file(READ "${backup}" backup_before_failure)
execute_process(COMMAND "${CLI}" save --device 0000:08:00.0
    --output "${backup}" --profile "${backup}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "different paths")
    message(FATAL_ERROR "colliding output paths were not rejected: ${error}")
endif()
set(alias "${TEST_DIR}/raw-alias.json")
file(REMOVE "${alias}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E create_hardlink "${backup}" "${alias}"
    RESULT_VARIABLE link_status ERROR_VARIABLE link_error)
if(NOT link_status EQUAL 0)
    message(FATAL_ERROR "could not create collision test hardlink: ${link_error}")
endif()
execute_process(COMMAND "${CLI}" save --device 0000:08:00.0
    --output "${backup}" --profile "${alias}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "different paths")
    message(FATAL_ERROR "hardlinked output paths were not rejected: ${error}")
endif()
file(REMOVE "${alias}")

execute_process(COMMAND "${CLI}" daemon --profile "${backup}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "raw backup.*save --profile")
    message(FATAL_ERROR "daemon did not explain backup/profile mismatch: ${error}")
endif()
execute_process(COMMAND "${CLI}" apply "${backup}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "raw backup.*save --profile")
    message(FATAL_ERROR "apply did not explain backup/profile mismatch: ${error}")
endif()
execute_process(COMMAND "${CLI}" restore --input "${profile}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "field profile.*raw backup")
    message(FATAL_ERROR "restore did not explain profile/backup mismatch: ${error}")
endif()
execute_process(COMMAND "${CLI}" apply "${profile}"
    --device 0000:09:00.0
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "captured on 0000:08:00.0")
    message(FATAL_ERROR "profile slot mismatch was not rejected: ${error}")
endif()
file(WRITE "${TEST_DIR}/legacy.json" "{\"fields\":{\"RC\":10}}\n")
execute_process(COMMAND "${CLI}" apply "${TEST_DIR}/legacy.json"
    WORKING_DIRECTORY "${TEST_DIR}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "legacy fields profile stopped loading: ${error}")
endif()
file(WRITE "${TEST_DIR}/invalid.json" "{\"fields\":[1,2]}\n")
execute_process(COMMAND "${CLI}" apply "${TEST_DIR}/invalid.json"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "fields.*must be an object")
    message(FATAL_ERROR "invalid fields type was not rejected: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_TWO_GPUS=1
    "${CLI}" save --profile "${TEST_DIR}/multi.json"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "cannot hold multiple GPUs")
    message(FATAL_ERROR "multi-GPU explicit profile path was not rejected: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_UNKNOWN=1
    "${CLI}" save --device 0000:08:00.0 --output "${TEST_DIR}/unknown-raw.json"
    --profile "${TEST_DIR}/unknown.json"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "requires a known GPU chipset")
    message(FATAL_ERROR "unknown chipset was not rejected: ${error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_DIVERGENT=1
    "${CLI}" save --device 0000:08:00.0 --output "${backup}"
    --profile "${profile}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "differ.*single profile cannot represent")
    message(FATAL_ERROR "divergent partitions were not rejected: ${error}")
endif()
file(READ "${profile}" profile_after_failure)
file(READ "${backup}" backup_after_failure)
if(NOT profile_before_failure STREQUAL profile_after_failure OR
   NOT backup_before_failure STREQUAL backup_after_failure)
    message(FATAL_ERROR "failed profile export modified an existing file")
endif()
execute_process(COMMAND "${CLI}" save --device 0000:08:00.0
    --output "${TEST_DIR}/partition-raw.json"
    --profile "${TEST_DIR}/partition.json" --fbpa 1
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "partition-scoped profile export is not supported")
    message(FATAL_ERROR "partition profile was not rejected: ${error}")
endif()

set(stock_dir "${TEST_DIR}/stock")
file(MAKE_DIRECTORY "${stock_dir}")
file(REMOVE "${stock_dir}/0000_08_00.0.stock.json"
            "${stock_dir}/stock-profile.json")
execute_process(COMMAND "${CLI}" save --profile stock-profile.json
    WORKING_DIRECTORY "${stock_dir}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "first default stock capture failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_RC=13
    "${CLI}" save --profile stock-profile.json
    WORKING_DIRECTORY "${stock_dir}"
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "second default stock capture failed: ${error}")
endif()
file(READ "${stock_dir}/0000_08_00.0.stock.json" stock_json)
file(READ "${stock_dir}/stock-profile.json" stock_profile)
if(NOT stock_json MATCHES "0x0108100A" OR
   stock_json MATCHES "0x0108100D" OR
   NOT stock_profile MATCHES "\"RC\": 13")
    message(FATAL_ERROR "repeated capture clobbered stock or missed current fields")
endif()

set(daemon_dir "${TEST_DIR}/daemon")
file(MAKE_DIRECTORY "${daemon_dir}")
file(REMOVE "${daemon_dir}/0000_08_00.0.stock.json" "${daemon_dir}/ops.log")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env NVTUNE_FAKE_RC=9
    NVTUNE_FAKE_STOP_AFTER_CYCLE=1 NVTUNE_FAKE_LOG=${daemon_dir}/ops.log
    "${CLI}" daemon --profile "${TEST_DIR}/timings-first.json" --interval 0.1
    WORKING_DIRECTORY "${daemon_dir}"
    RESULT_VARIABLE daemon_status OUTPUT_VARIABLE daemon_output
    ERROR_VARIABLE daemon_error)
if(NOT daemon_status EQUAL 0)
    message(FATAL_ERROR "save->daemon roundtrip failed: ${daemon_error}")
endif()
file(READ "${daemon_dir}/ops.log" daemon_log)
if(NOT daemon_log MATCHES "WRITE 0000:08:00.0 0x009A0290 0x0108100A" OR
   NOT daemon_log MATCHES "WRITE 0000:08:00.0 0x009A0290 0x01081009" OR
   NOT daemon_log MATCHES "FINAL 0000:08:00.0 0x009A0290 0x01081009")
    message(FATAL_ERROR "daemon did not apply and restore exact fake RC: ${daemon_log}")
endif()
string(REGEX MATCHALL "FINAL [^\n]+" final_words "${daemon_log}")
list(LENGTH final_words final_count)
if(NOT final_count EQUAL 21)
    message(FATAL_ERROR "expected 21 final fake register values, got ${final_count}")
endif()
foreach(line IN LISTS final_words)
    if(line MATCHES "0x009A0290|0x00900290|0x00904290")
        if(NOT line MATCHES "0x01081009$")
            message(FATAL_ERROR "CONFIG0 was not restored: ${line}")
        endif()
    elseif(NOT line MATCHES "0x0108100A$")
        message(FATAL_ERROR "non-CONFIG0 register changed: ${line}")
    endif()
endforeach()
