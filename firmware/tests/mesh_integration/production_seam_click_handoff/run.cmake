cmake_minimum_required(VERSION 3.20)

foreach(required_variable REPOSITORY_ROOT SEAM_SOURCE_DIR SEAM_BUILD_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} must be provided")
    endif()
endforeach()

if(EXISTS "${REPOSITORY_ROOT}/.venv/bin/west")
    set(west_executable "${REPOSITORY_ROOT}/.venv/bin/west")
else()
    find_program(west_executable west)
endif()
if(NOT west_executable)
    message(FATAL_ERROR "west was not found in ${REPOSITORY_ROOT}/.venv/bin or PATH")
endif()

get_filename_component(seam_build_parent "${SEAM_BUILD_DIR}" DIRECTORY)
file(MAKE_DIRECTORY "${seam_build_parent}")
file(LOCK "${SEAM_BUILD_DIR}.lock"
    GUARD PROCESS
    TIMEOUT 60
    RESULT_VARIABLE lock_result)
if(NOT lock_result STREQUAL "0")
    message(FATAL_ERROR
        "could not lock the production seam build directory: ${lock_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CCACHE_DISABLE=1
            "${west_executable}" build
            --no-sysbuild
            --pristine
            -s "${SEAM_SOURCE_DIR}"
            -b native_sim/native/64
            -d "${SEAM_BUILD_DIR}"
    WORKING_DIRECTORY "${REPOSITORY_ROOT}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
    TIMEOUT 180)
if(NOT "${build_result}" STREQUAL "0")
    message(FATAL_ERROR
        "production seam build failed (${build_result})\n${build_stdout}\n${build_stderr}")
endif()

execute_process(
    COMMAND "${SEAM_BUILD_DIR}/zephyr/zephyr.exe"
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_stdout
    ERROR_VARIABLE test_stderr
    TIMEOUT 30)
message(STATUS "${build_stdout}${build_stderr}${test_stdout}${test_stderr}")
if(NOT "${test_result}" STREQUAL "0")
    message(FATAL_ERROR "production seam test failed (${test_result})")
endif()
