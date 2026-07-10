cmake_minimum_required(VERSION 3.20)

project(firmware_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(core
    src/debug_log.c
    src/discovery_assignment.c
    src/protocol.c
    src/gateway_membership.c
    src/gateway_command.c
    src/uwb.c
    src/uwb_ble_courtesy.c
    src/uwb_session.c
    src/mesh.c
    src/mesh_preemption.c
    src/mesh_relay.c
    src/report.c
    src/serial_frame.c
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

add_executable(test_discovery_assignment tests/test_discovery_assignment.c)
target_link_libraries(test_discovery_assignment PRIVATE core)
add_test(NAME discovery_assignment COMMAND test_discovery_assignment)

add_executable(test_status tests/test_status.c)
target_link_libraries(test_status PRIVATE core)
add_test(NAME status COMMAND test_status)

add_executable(test_gateway_command tests/test_gateway_command.c)
target_link_libraries(test_gateway_command PRIVATE core)
add_test(NAME gateway_command COMMAND test_gateway_command)

add_executable(test_gateway_membership tests/test_gateway_membership.c)
target_link_libraries(test_gateway_membership PRIVATE core)
add_test(NAME gateway_membership COMMAND test_gateway_membership)

add_executable(test_gateway_eack_policy
    tests/test_gateway_eack_policy.c
    app/src/app_gateway_eack_policy.c
)
target_include_directories(test_gateway_eack_policy PRIVATE app/src)
target_link_libraries(test_gateway_eack_policy PRIVATE core)
add_test(NAME gateway_eack_policy COMMAND test_gateway_eack_policy)

add_executable(test_gateway_collection_eack
    tests/test_gateway_collection_eack.c
    app/src/app_gateway_collection_eack.c
    app/src/app_gateway_eack_policy.c
)
target_include_directories(test_gateway_collection_eack PRIVATE app/src)
target_link_libraries(test_gateway_collection_eack PRIVATE core)
add_test(NAME gateway_collection_eack COMMAND test_gateway_collection_eack)

add_executable(test_app_gateway_ble_stream
    tests/test_app_gateway_ble_stream.c
    app/src/app_gateway_ble_stream.c
)
target_include_directories(test_app_gateway_ble_stream PRIVATE app/src)
target_link_libraries(test_app_gateway_ble_stream PRIVATE core)
add_test(NAME app_gateway_ble_stream COMMAND test_app_gateway_ble_stream)

add_executable(test_app_mesh_smoke_fast
    tests/test_app_mesh_smoke_fast.c
    app/src/app_mesh_smoke_fast.c
)
target_include_directories(test_app_mesh_smoke_fast PRIVATE app/src)
target_link_libraries(test_app_mesh_smoke_fast PRIVATE core)
add_test(NAME app_mesh_smoke_fast COMMAND test_app_mesh_smoke_fast)

add_executable(test_app_mesh_c5_priority
    tests/test_app_mesh_c5_priority.c
    app/src/app_mesh_c5_priority.c
)
target_include_directories(test_app_mesh_c5_priority PRIVATE app/src)
target_link_libraries(test_app_mesh_c5_priority PRIVATE core)
add_test(NAME app_mesh_c5_priority COMMAND test_app_mesh_c5_priority)

add_executable(test_app_mesh_coordinator
    tests/test_app_mesh_coordinator.c
    app/src/app_mesh_coordinator.c
)
target_include_directories(test_app_mesh_coordinator PRIVATE app/src)
target_link_libraries(test_app_mesh_coordinator PRIVATE core)
add_test(NAME app_mesh_coordinator COMMAND test_app_mesh_coordinator)

add_executable(test_app_mesh_flood
    tests/test_app_mesh_flood.c
    app/src/app_mesh_flood.c
)
target_include_directories(test_app_mesh_flood PRIVATE app/src)
target_link_libraries(test_app_mesh_flood PRIVATE core)
add_test(NAME app_mesh_flood COMMAND test_app_mesh_flood)

add_executable(test_app_wake_train_politeness
    tests/test_app_wake_train_politeness.c
    app/src/app_wake_train_politeness.c
)
target_include_directories(test_app_wake_train_politeness PRIVATE app/src)
target_link_libraries(test_app_wake_train_politeness PRIVATE core)
add_test(NAME app_wake_train_politeness COMMAND test_app_wake_train_politeness)

add_executable(test_app_mesh_rx_policy
    tests/test_app_mesh_rx_policy.c
    app/src/app_mesh_rx_policy.c
)
target_include_directories(test_app_mesh_rx_policy PRIVATE app/src)
target_link_libraries(test_app_mesh_rx_policy PRIVATE core)
add_test(NAME app_mesh_rx_policy COMMAND test_app_mesh_rx_policy)

add_executable(test_uwb tests/test_uwb.c)
target_link_libraries(test_uwb PRIVATE core)
add_test(NAME uwb COMMAND test_uwb)

add_executable(test_uwb_ble_courtesy tests/test_uwb_ble_courtesy.c)
target_link_libraries(test_uwb_ble_courtesy PRIVATE core)
add_test(NAME uwb_ble_courtesy COMMAND test_uwb_ble_courtesy)

add_executable(test_uwb_session tests/test_uwb_session.c)
target_link_libraries(test_uwb_session PRIVATE core)
add_test(NAME uwb_session COMMAND test_uwb_session)

add_executable(test_route tests/test_route.c)
target_link_libraries(test_route PRIVATE core)
add_test(NAME route COMMAND test_route)

add_executable(test_mesh tests/test_mesh.c)
target_link_libraries(test_mesh PRIVATE core)
add_test(NAME mesh COMMAND test_mesh)

add_executable(test_mesh_preemption tests/test_mesh_preemption.c)
target_link_libraries(test_mesh_preemption PRIVATE core)
add_test(NAME mesh_preemption COMMAND test_mesh_preemption)

add_executable(test_app_mesh_collection_deferral
    tests/test_app_mesh_collection_deferral.c
    app/src/app_mesh_collection_deferral.c
)
target_include_directories(test_app_mesh_collection_deferral PRIVATE app/src)
target_link_libraries(test_app_mesh_collection_deferral PRIVATE core)
add_test(NAME app_mesh_collection_deferral COMMAND test_app_mesh_collection_deferral)

add_executable(test_app_mesh_tx_handoff_gate
    tests/test_app_mesh_tx_handoff_gate.c
    app/src/app_mesh_tx_handoff_gate.c
)
target_include_directories(test_app_mesh_tx_handoff_gate PRIVATE app/src)
target_link_libraries(test_app_mesh_tx_handoff_gate PRIVATE core)
add_test(NAME app_mesh_tx_handoff_gate COMMAND test_app_mesh_tx_handoff_gate)

add_executable(test_app_mesh_route_ready_handoff
    tests/test_app_mesh_route_ready_handoff.c
    app/src/app_mesh_route_ready_handoff.c
)
target_include_directories(test_app_mesh_route_ready_handoff PRIVATE app/src)
target_link_libraries(test_app_mesh_route_ready_handoff PRIVATE core)
add_test(NAME app_mesh_route_ready_handoff COMMAND test_app_mesh_route_ready_handoff)

add_executable(test_app_mesh_route_reply_ack
    tests/test_app_mesh_route_reply_ack.c
    app/src/app_mesh_route_reply_ack.c
)
target_include_directories(test_app_mesh_route_reply_ack PRIVATE app/src)
target_link_libraries(test_app_mesh_route_reply_ack PRIVATE core)
add_test(NAME app_mesh_route_reply_ack COMMAND test_app_mesh_route_reply_ack)

add_executable(test_app_mesh_route_wait_tx
    tests/test_app_mesh_route_wait_tx.c
    app/src/app_mesh_route_wait_tx.c
)
target_include_directories(test_app_mesh_route_wait_tx PRIVATE app/src)
target_link_libraries(test_app_mesh_route_wait_tx PRIVATE core)
add_test(NAME app_mesh_route_wait_tx COMMAND test_app_mesh_route_wait_tx)

add_executable(test_app_mesh_route_request_policy
    tests/test_app_mesh_route_request_policy.c
    app/src/app_mesh_route_request_policy.c
)
target_include_directories(test_app_mesh_route_request_policy PRIVATE app/src)
target_link_libraries(test_app_mesh_route_request_policy PRIVATE core)
add_test(NAME app_mesh_route_request_policy COMMAND test_app_mesh_route_request_policy)

add_executable(test_app_mesh_gateway_ack_policy
    tests/test_app_mesh_gateway_ack_policy.c
    app/src/app_mesh_gateway_ack_policy.c
)
target_include_directories(test_app_mesh_gateway_ack_policy PRIVATE app/src)
target_link_libraries(test_app_mesh_gateway_ack_policy PRIVATE core)
add_test(NAME app_mesh_gateway_ack_policy COMMAND test_app_mesh_gateway_ack_policy)

add_executable(test_app_mesh_ch9_ack
    tests/test_app_mesh_ch9_ack.c
    app/src/app_mesh_ch9_ack.c
)
target_include_directories(test_app_mesh_ch9_ack PRIVATE app/src)
target_link_libraries(test_app_mesh_ch9_ack PRIVATE core)
add_test(NAME app_mesh_ch9_ack COMMAND test_app_mesh_ch9_ack)

add_executable(test_mesh_relay tests/test_mesh_relay.c)
target_link_libraries(test_mesh_relay PRIVATE core)
add_test(NAME mesh_relay COMMAND test_mesh_relay)

add_executable(test_serial_frame tests/test_serial_frame.c)
target_link_libraries(test_serial_frame PRIVATE core)
add_test(NAME serial_frame COMMAND test_serial_frame)

add_executable(test_report tests/test_report.c)
target_link_libraries(test_report PRIVATE core)
add_test(NAME report COMMAND test_report)

add_executable(test_survey tests/test_survey.c)
target_link_libraries(test_survey PRIVATE core)
add_test(NAME survey COMMAND test_survey)

add_executable(test_debug_log tests/test_debug_log.c)
target_link_libraries(test_debug_log PRIVATE core)
add_test(NAME debug_log COMMAND test_debug_log)
