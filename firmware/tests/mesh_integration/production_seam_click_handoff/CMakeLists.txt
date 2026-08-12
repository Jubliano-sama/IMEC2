cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imec2_production_seam_click_handoff)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../../app/src
)

target_compile_definitions(app PRIVATE
    DEVICE_ROLE=2
    CONFIG_IMEC_MESH_ROUTE_TEST=1
)

target_sources(app PRIVATE
    src/main.c
    ../../../app/src/app_radio_guard.c
    ../../../app/src/app_node_comm.c
    ../../../app/src/app_node_comm_sync.c
    ../../../app/src/app_mesh_ch9_ack.c
    ../../../app/src/app_mesh_c5_priority.c
    ../../../src/firmware_events.c
    ../../../src/firmware_state_mesh.c
    ../../../src/firmware_state_radio.c
    ../../../src/mesh.c
    ../../../src/node_comm.c
    ../../../src/protocol.c
    ../../../src/semantic_digest.c
)
