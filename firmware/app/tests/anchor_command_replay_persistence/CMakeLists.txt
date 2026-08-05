cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(anchor_command_replay_persistence_test)

target_compile_definitions(app PRIVATE DEVICE_ROLE=2)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

target_sources(app PRIVATE
    src/main.c
    ../../src/app_nvs_storage.c
    ../../src/app_mesh_persistence.c
    ../../src/app_mesh_local_delivery.c
    ../../src/app_mesh_preemption.c
    ../../src/app_mesh_result_handoff.c
    ../../../src/debug_log.c
    ../../../src/discovery_assignment.c
    ../../../src/protocol.c
    ../../../src/gateway_command.c
    ../../../src/gateway_collection_journal.c
    ../../../src/gateway_membership.c
    ../../../src/mesh.c
    ../../../src/mesh_preemption.c
    ../../../src/mesh_relay.c
    ../../../src/mesh_relay_channel9.c
    ../../../src/mesh_route_path.c
    ../../../src/operation_policy.c
    ../../../src/route.c
    ../../../src/semantic_digest.c
)
