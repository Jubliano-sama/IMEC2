cmake_minimum_required(VERSION 3.20)

foreach(required_variable REPOSITORY_ROOT TRACE_SOURCE_DIR TRACE_BUILD_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

if(DEFINED FIRMWARE_ROOT)
    get_filename_component(trace_firmware_root "${FIRMWARE_ROOT}" ABSOLUTE)
else()
    get_filename_component(trace_firmware_root
        "${REPOSITORY_ROOT}/firmware" ABSOLUTE)
endif()

get_filename_component(trace_build_parent "${TRACE_BUILD_DIR}" DIRECTORY)
file(MAKE_DIRECTORY "${trace_build_parent}")
file(LOCK "${TRACE_BUILD_DIR}.lock"
    GUARD PROCESS
    TIMEOUT 60
    RESULT_VARIABLE lock_result)
if(NOT lock_result STREQUAL "0")
    message(FATAL_ERROR
        "could not lock the gateway specialization trace build: ${lock_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CCACHE_DISABLE=1
            "${CMAKE_COMMAND}"
            -S "${TRACE_SOURCE_DIR}"
            -B "${TRACE_BUILD_DIR}"
            -DFIRMWARE_ROOT=${trace_firmware_root}
    WORKING_DIRECTORY "${REPOSITORY_ROOT}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
    TIMEOUT 60)
if(NOT configure_result STREQUAL "0")
    message(FATAL_ERROR
        "gateway specialization trace configure failed (${configure_result})\n"
        "${configure_stdout}${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CCACHE_DISABLE=1
            "${CMAKE_COMMAND}" --build "${TRACE_BUILD_DIR}"
            --target gateway_trace_generic gateway_trace_specialized
    WORKING_DIRECTORY "${REPOSITORY_ROOT}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
    TIMEOUT 180)
if(NOT build_result STREQUAL "0")
    message(FATAL_ERROR
        "gateway specialization trace build failed (${build_result})\n"
        "${configure_stdout}${configure_stderr}${build_stdout}${build_stderr}")
endif()

execute_process(
    COMMAND "${TRACE_BUILD_DIR}/gateway_trace_generic"
    RESULT_VARIABLE generic_result
    OUTPUT_VARIABLE generic_trace
    ERROR_VARIABLE generic_stderr
    TIMEOUT 30)
file(WRITE "${TRACE_BUILD_DIR}/generic.trace" "${generic_trace}")
if(NOT generic_result STREQUAL "0")
    message(FATAL_ERROR
        "generic gateway trace failed (${generic_result})\n"
        "${generic_trace}${generic_stderr}")
endif()

execute_process(
    COMMAND "${TRACE_BUILD_DIR}/gateway_trace_specialized"
    RESULT_VARIABLE specialized_result
    OUTPUT_VARIABLE specialized_trace
    ERROR_VARIABLE specialized_stderr
    TIMEOUT 30)
file(WRITE "${TRACE_BUILD_DIR}/specialized.trace" "${specialized_trace}")
if(NOT specialized_result STREQUAL "0")
    message(FATAL_ERROR
        "specialized gateway trace failed (${specialized_result})\n"
        "${specialized_trace}${specialized_stderr}")
endif()

if(NOT generic_trace STREQUAL specialized_trace)
    message(FATAL_ERROR
        "generic and gateway-only relay traces differ\n"
        "generic trace: ${TRACE_BUILD_DIR}/generic.trace\n"
        "specialized trace: ${TRACE_BUILD_DIR}/specialized.trace")
endif()

string(LENGTH "${generic_trace}" trace_length)
message(STATUS
    "generic and gateway-only relay traces match exactly (${trace_length} bytes)")
