cmake_minimum_required(VERSION 3.20)

project(imec2_gateway_relay_specialization_trace C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT DEFINED FIRMWARE_ROOT)
    get_filename_component(FIRMWARE_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
else()
    get_filename_component(FIRMWARE_ROOT "${FIRMWARE_ROOT}" ABSOLUTE)
endif()

set(TRACE_COMMON_SOURCES
    ${FIRMWARE_ROOT}/src/mesh.c
    ${FIRMWARE_ROOT}/src/mesh_relay_channel9.c
    ${FIRMWARE_ROOT}/src/mesh_route_path.c
    ${FIRMWARE_ROOT}/src/discovery_assignment.c
    ${FIRMWARE_ROOT}/src/dwm3000_timing.c
    ${FIRMWARE_ROOT}/src/gateway_membership.c
    ${FIRMWARE_ROOT}/src/gateway_command.c
    ${FIRMWARE_ROOT}/src/operation_policy.c
    ${FIRMWARE_ROOT}/src/protocol.c
    ${FIRMWARE_ROOT}/src/route.c
    ${FIRMWARE_ROOT}/src/semantic_digest.c
    ${FIRMWARE_ROOT}/src/survey.c
    ${FIRMWARE_ROOT}/src/survey_round_control.c
)

add_library(gateway_trace_common OBJECT ${TRACE_COMMON_SOURCES})
add_library(gateway_trace_relay_generic OBJECT
    ${FIRMWARE_ROOT}/src/mesh_relay.c)
add_library(gateway_trace_relay_specialized OBJECT
    ${FIRMWARE_ROOT}/src/mesh_relay.c)

target_compile_definitions(gateway_trace_relay_generic PRIVATE
    DEVICE_ROLE=3
    CONFIG_IMEC_MESH_ROUTE_TEST=1
    IMEC_MESH_RELAY_GATEWAY_ONLY=0)
target_compile_definitions(gateway_trace_relay_specialized PRIVATE
    DEVICE_ROLE=3
    CONFIG_IMEC_MESH_ROUTE_TEST=1
    IMEC_MESH_RELAY_GATEWAY_ONLY=1)
target_compile_definitions(gateway_trace_common PRIVATE
    DEVICE_ROLE=3
    CONFIG_IMEC_MESH_ROUTE_TEST=1)

foreach(trace_object IN ITEMS
        gateway_trace_common
        gateway_trace_relay_generic
        gateway_trace_relay_specialized)
    target_include_directories(${trace_object} PRIVATE
        ${FIRMWARE_ROOT}/include)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${trace_object} PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gateway_trace_relay_specialized PRIVATE
        -Wno-unused-function)
endif()

function(add_gateway_trace_executable target_name gateway_only)
    add_executable(${target_name}
        trace_driver.c
        $<TARGET_OBJECTS:gateway_trace_common>
        $<TARGET_OBJECTS:gateway_trace_relay_${gateway_only}>)
    target_include_directories(${target_name} PRIVATE
        ${FIRMWARE_ROOT}/include)
    target_compile_definitions(${target_name} PRIVATE
        DEVICE_ROLE=3
        CONFIG_IMEC_MESH_ROUTE_TEST=1
        IMEC_MESH_RELAY_GATEWAY_ONLY=$<IF:$<STREQUAL:${gateway_only},specialized>,1,0>)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
endfunction()

add_gateway_trace_executable(gateway_trace_generic generic)
add_gateway_trace_executable(gateway_trace_specialized specialized)
