cmake_minimum_required(VERSION 3.20)

project(firmware_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(core
    src/protocol.c
    src/discovery.c
    src/uwb.c
    src/mesh.c
    src/mesh_relay.c
    src/report.c
    src/status.c
    src/route.c
    src/survey.c
)

target_include_directories(core PUBLIC include)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

enable_testing()

add_executable(test_protocol tests/test_protocol.c)
target_link_libraries(test_protocol PRIVATE core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_status tests/test_status.c)
target_link_libraries(test_status PRIVATE core)
add_test(NAME status COMMAND test_status)

add_executable(test_discovery tests/test_discovery.c)
target_link_libraries(test_discovery PRIVATE core)
add_test(NAME discovery COMMAND test_discovery)

add_executable(test_uwb tests/test_uwb.c)
target_link_libraries(test_uwb PRIVATE core)
add_test(NAME uwb COMMAND test_uwb)

add_executable(test_route tests/test_route.c)
target_link_libraries(test_route PRIVATE core)
add_test(NAME route COMMAND test_route)

add_executable(test_mesh tests/test_mesh.c)
target_link_libraries(test_mesh PRIVATE core)
add_test(NAME mesh COMMAND test_mesh)

add_executable(test_mesh_relay tests/test_mesh_relay.c)
target_link_libraries(test_mesh_relay PRIVATE core)
add_test(NAME mesh_relay COMMAND test_mesh_relay)

add_executable(test_report tests/test_report.c)
target_link_libraries(test_report PRIVATE core)
add_test(NAME report COMMAND test_report)

add_executable(test_survey tests/test_survey.c)
target_link_libraries(test_survey PRIVATE core)
add_test(NAME survey COMMAND test_survey)
