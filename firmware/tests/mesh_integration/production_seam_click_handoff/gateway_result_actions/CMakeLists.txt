cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imec2_production_seam_gateway_result_actions)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../app/src
)

target_compile_definitions(app PRIVATE
    DEVICE_ROLE=3
    CONFIG_IMEC_MESH_ROUTE_TEST=1
)

target_sources(app PRIVATE
    src/main.c
    ../../../../app/src/app_gateway_collection_recovery.c
    ../../../../app/src/app_mesh_result_handoff.c
    ../../../../src/mesh.c
    ../../../../src/mesh_relay_channel9.c
    ../../../../src/mesh_route_path.c
    ../../../../src/mesh_relay.c
    ../../../../src/discovery_assignment.c
    ../../../../src/dwm3000_timing.c
    ../../../../src/gateway_command.c
    ../../../../src/operation_policy.c
    ../../../../src/protocol.c
    ../../../../src/route.c
    ../../../../src/semantic_digest.c
)
