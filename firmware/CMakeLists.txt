cmake_minimum_required(VERSION 3.20)

project(firmware_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(core
    src/debug_log.c
    src/device_identity.c
    src/discovery_assignment.c
    src/dwm3000_runtime.c
    src/dwm3000_timing.c
    src/gateway_ble_transport.c
    src/protocol.c
    src/gateway_membership.c
    src/gateway_command.c
    src/uwb.c
    src/uwb_ble_courtesy.c
    src/uwb_session.c
    src/mesh.c
    src/mesh_preemption.c
    src/mesh_runtime.c
    src/mesh_relay.c
    src/report.c
    src/serial_frame.c
    src/stack_budget.c
    src/status.c
    src/route.c
    src/survey.c
)

target_include_directories(core PUBLIC include)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(test_protocol tests/test_protocol.c)
target_link_libraries(test_protocol PRIVATE core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_discovery_assignment tests/test_discovery_assignment.c)
target_link_libraries(test_discovery_assignment PRIVATE core)
add_test(NAME discovery_assignment COMMAND test_discovery_assignment)

add_executable(test_device_identity tests/test_device_identity.c)
target_link_libraries(test_device_identity PRIVATE core)
add_test(NAME device_identity COMMAND test_device_identity)

add_executable(test_status tests/test_status.c)
target_link_libraries(test_status PRIVATE core)
add_test(NAME status COMMAND test_status)

add_executable(test_gateway_command tests/test_gateway_command.c)
target_link_libraries(test_gateway_command PRIVATE core)
add_test(NAME gateway_command COMMAND test_gateway_command)

add_executable(gateway_gui_protocol_oracle
    tests/mesh_integration/gateway_gui_protocol_oracle.c)
target_link_libraries(gateway_gui_protocol_oracle PRIVATE core)
set(GATEWAY_GUI_PYTHON ${CMAKE_CURRENT_SOURCE_DIR}/../.venv/bin/python)
if(NOT EXISTS ${GATEWAY_GUI_PYTHON})
    set(GATEWAY_GUI_PYTHON ${Python3_EXECUTABLE})
endif()
add_test(NAME gateway_gui_cross_layer
    COMMAND ${GATEWAY_GUI_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_gui_cross_layer.py
        $<TARGET_FILE:gateway_gui_protocol_oracle>)
set_tests_properties(gateway_gui_cross_layer
    PROPERTIES LABELS "mesh_integration;hardware_models;gateway_gui;deployment")

add_executable(test_gateway_command_lifecycle_invariants
    tests/mesh_integration/test_gateway_command_lifecycle_invariants.c
    app/src/app_gateway_command_ingress.c
    app/src/app_gateway_command_lifecycle.c)
target_include_directories(test_gateway_command_lifecycle_invariants PRIVATE app/src)
target_link_libraries(test_gateway_command_lifecycle_invariants PRIVATE core)
add_test(NAME gateway_command_lifecycle_invariants
    COMMAND test_gateway_command_lifecycle_invariants)
set_tests_properties(gateway_command_lifecycle_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;gateway_command;deployment")

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
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_app_gateway_ble_stream PRIVATE app/src)
target_link_libraries(test_app_gateway_ble_stream PRIVATE core)
add_test(NAME app_gateway_ble_stream COMMAND test_app_gateway_ble_stream)

add_executable(test_app_gateway_command_observability
    tests/test_app_gateway_command_observability.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_app_gateway_command_observability PRIVATE app/src)
target_link_libraries(test_app_gateway_command_observability PRIVATE core)
add_test(NAME app_gateway_command_observability
    COMMAND test_app_gateway_command_observability)
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

add_executable(test_app_mesh_coordinator_runtime
    tests/test_app_mesh_coordinator_runtime.c
    app/src/app_mesh_coordinator.c
    app/src/app_mesh_coordinator_runtime.c
)
target_include_directories(test_app_mesh_coordinator_runtime PRIVATE app/src)
target_link_libraries(test_app_mesh_coordinator_runtime PRIVATE core)
add_test(NAME app_mesh_coordinator_runtime COMMAND test_app_mesh_coordinator_runtime)

add_executable(test_app_mesh_gateway_command_priority
    tests/test_app_mesh_gateway_command_priority.c
    app/src/app_mesh_gateway_command_priority.c
)
target_include_directories(test_app_mesh_gateway_command_priority PRIVATE app/src)
target_link_libraries(test_app_mesh_gateway_command_priority PRIVATE core)
add_test(NAME app_mesh_gateway_command_priority
    COMMAND test_app_mesh_gateway_command_priority)

add_executable(test_app_gateway_command_ingress
    tests/test_app_gateway_command_ingress.c
    app/src/app_gateway_command_ingress.c
)
target_include_directories(test_app_gateway_command_ingress PRIVATE app/src)
target_link_libraries(test_app_gateway_command_ingress PRIVATE core)
add_test(NAME app_gateway_command_ingress
    COMMAND test_app_gateway_command_ingress)

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

add_executable(test_app_discovery_assignment_policy
    tests/test_app_discovery_assignment_policy.c
)
target_include_directories(test_app_discovery_assignment_policy PRIVATE app/src)
add_test(NAME app_discovery_assignment_policy
    COMMAND test_app_discovery_assignment_policy)

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

add_executable(test_dwm3000_models
    tests/mesh_integration/test_dwm3000_models.c
)
target_link_libraries(test_dwm3000_models PRIVATE core)
add_test(NAME dwm3000_models COMMAND test_dwm3000_models)
set_tests_properties(dwm3000_models PROPERTIES LABELS "hardware_models")

add_executable(test_gateway_ble_transport_model
    tests/mesh_integration/test_gateway_ble_transport_model.c
)
target_link_libraries(test_gateway_ble_transport_model PRIVATE core)
add_test(NAME gateway_ble_transport_model COMMAND test_gateway_ble_transport_model)
set_tests_properties(gateway_ble_transport_model
    PROPERTIES LABELS "hardware_models")

add_executable(test_stack_budget_model
    tests/mesh_integration/test_stack_budget_model.c
)
target_link_libraries(test_stack_budget_model PRIVATE core)
add_test(NAME stack_budget_model COMMAND test_stack_budget_model)
set_tests_properties(stack_budget_model PROPERTIES LABELS "hardware_models")

add_test(NAME verify_stack_evidence
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_verify_stack_evidence.py)
set_tests_properties(verify_stack_evidence PROPERTIES LABELS "hardware_models")

add_test(NAME flash_verified_mesh
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_flash_verified_mesh.py)
set_tests_properties(flash_verified_mesh PROPERTIES LABELS "hardware_models")

add_library(mesh_sim
    sim/mesh_sim.c
    sim/mesh_sim_scheduler.c
    sim/mesh_sim_trace.c
)
target_include_directories(mesh_sim PUBLIC sim)
target_link_libraries(mesh_sim PUBLIC core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mesh_sim PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_mesh_integration
    tests/mesh_integration/test_mesh_integration.c
)
target_link_libraries(test_mesh_integration PRIVATE mesh_sim)
add_test(NAME mesh_integration COMMAND test_mesh_integration)
set_tests_properties(mesh_integration
    PROPERTIES LABELS "mesh_integration;simulator")

add_executable(test_mesh_production_scenarios
    tests/mesh_integration/test_mesh_production_scenarios.c
)
target_link_libraries(test_mesh_production_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_production_scenarios COMMAND test_mesh_production_scenarios)
set_tests_properties(mesh_production_scenarios
    PROPERTIES LABELS "mesh_integration;simulator")

add_executable(test_mesh_runtime_priority_scenarios
    tests/mesh_integration/test_mesh_runtime_priority_scenarios.c
)
target_link_libraries(test_mesh_runtime_priority_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_runtime_priority_scenarios
    COMMAND test_mesh_runtime_priority_scenarios)
set_tests_properties(mesh_runtime_priority_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;priority")

add_executable(test_mesh_wake_scenarios
    tests/mesh_integration/test_mesh_wake_scenarios.c
)
target_link_libraries(test_mesh_wake_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_wake_scenarios COMMAND test_mesh_wake_scenarios)
set_tests_properties(mesh_wake_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;wake")

add_executable(test_mesh_discovery_scenarios
    tests/mesh_integration/test_mesh_discovery_scenarios.c
)
target_link_libraries(test_mesh_discovery_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_discovery_scenarios COMMAND test_mesh_discovery_scenarios)
set_tests_properties(mesh_discovery_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;discovery")

add_test(NAME mesh_anchor_preset
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_anchor_preset.py)
set_tests_properties(mesh_anchor_preset
    PROPERTIES LABELS "mesh_integration;hardware_models;deployment")

add_executable(test_mesh_route_recovery_scenarios
    tests/mesh_integration/test_mesh_route_recovery_scenarios.c
)
target_link_libraries(test_mesh_route_recovery_scenarios PRIVATE core)
add_test(NAME mesh_route_recovery_scenarios
    COMMAND test_mesh_route_recovery_scenarios)
set_tests_properties(mesh_route_recovery_scenarios
    PROPERTIES LABELS "mesh_integration;route;recovery")

add_executable(test_mesh_route_formation_scenarios
    tests/mesh_integration/test_mesh_route_formation_scenarios.c
)
target_link_libraries(test_mesh_route_formation_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_route_formation_scenarios
    COMMAND test_mesh_route_formation_scenarios)
set_tests_properties(mesh_route_formation_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;route;discovery")

add_executable(test_mesh_network_stress_scenarios
    tests/mesh_integration/test_mesh_network_stress_scenarios.c
)
target_link_libraries(test_mesh_network_stress_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_network_stress_scenarios
    COMMAND test_mesh_network_stress_scenarios)
set_tests_properties(mesh_network_stress_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;stress")

add_executable(test_mesh_gateway_ble_scenarios
    tests/mesh_integration/test_mesh_gateway_ble_scenarios.c
    app/src/app_gateway_ble_stream.c
)
target_include_directories(test_mesh_gateway_ble_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_gateway_ble_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_gateway_ble_scenarios
    COMMAND test_mesh_gateway_ble_scenarios)
set_tests_properties(mesh_gateway_ble_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;ble")

add_executable(test_mesh_app_arbitration_scenarios
    tests/mesh_integration/test_mesh_app_arbitration_scenarios.c
    app/src/app_mesh_c5_priority.c
    app/src/app_mesh_ch9_ack.c
    app/src/app_mesh_coordinator.c
    app/src/app_mesh_coordinator_runtime.c
    app/src/app_mesh_arbitration_zephyr.c
    app/src/app_gateway_command_ingress.c
    app/src/app_mesh_gateway_command_priority.c
    app/src/app_mesh_preemption.c
)

add_executable(test_gateway_command_observability_scenarios
    tests/mesh_integration/test_gateway_command_observability_scenarios.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_gateway_command_observability_scenarios PRIVATE app/src)
target_link_libraries(test_gateway_command_observability_scenarios PRIVATE core)
add_test(NAME gateway_command_observability_scenarios
    COMMAND test_gateway_command_observability_scenarios)
set_tests_properties(gateway_command_observability_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models")
target_include_directories(test_mesh_app_arbitration_scenarios PRIVATE
    tests/mesh_integration/zephyr_shim
    app/src
)
target_link_libraries(test_mesh_app_arbitration_scenarios PRIVATE core)
add_test(NAME mesh_app_arbitration_scenarios
    COMMAND test_mesh_app_arbitration_scenarios)
set_tests_properties(mesh_app_arbitration_scenarios
    PROPERTIES LABELS "mesh_integration;production;priority")

add_executable(test_mesh_watchdog_lease_scenarios
    tests/mesh_integration/test_mesh_watchdog_lease_scenarios.c
)
target_link_libraries(test_mesh_watchdog_lease_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_watchdog_lease_scenarios
    COMMAND test_mesh_watchdog_lease_scenarios)
set_tests_properties(mesh_watchdog_lease_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;watchdog")

add_executable(test_mesh_sim_trace_scheduler
    tests/mesh_integration/test_mesh_sim_trace_scheduler.c
)
target_link_libraries(test_mesh_sim_trace_scheduler PRIVATE mesh_sim)
add_test(NAME mesh_sim_trace_scheduler
    COMMAND test_mesh_sim_trace_scheduler)
set_tests_properties(mesh_sim_trace_scheduler
    PROPERTIES LABELS "mesh_integration;simulator;trace;scheduler")
