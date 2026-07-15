cmake_minimum_required(VERSION 3.20)

project(firmware_native C)

find_package(Threads REQUIRED)

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
    src/gateway_collection_journal.c
    src/uwb.c
    src/uwb_ble_courtesy.c
    src/uwb_session.c
    src/mesh.c
    src/node_comm.c
    src/node_transaction.c
    src/watchdog_adoption.c
    src/mesh_preemption.c
    src/mesh_runtime.c
    src/mesh_relay.c
    src/mesh_relay_channel9.c
    src/report.c
    src/serial_frame.c
    src/stack_diag_transport.c
    src/stack_budget.c
    src/status.c
    src/route.c
    src/survey.c
    src/survey_gateway_transaction.c
    src/survey_pair_lease.c
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

add_executable(test_node_comm tests/test_node_comm.c)
target_link_libraries(test_node_comm PRIVATE core)
add_test(NAME node_comm COMMAND test_node_comm)
set_tests_properties(node_comm
    PROPERTIES LABELS "mesh_integration;hardware_models;communication")

add_executable(test_node_transaction tests/test_node_transaction.c)
target_link_libraries(test_node_transaction PRIVATE core)
add_test(NAME node_transaction COMMAND test_node_transaction)
set_tests_properties(node_transaction
    PROPERTIES LABELS "mesh_integration;hardware_models;communication")

add_executable(test_survey_pair_lease tests/test_survey_pair_lease.c)
target_link_libraries(test_survey_pair_lease PRIVATE core)
add_test(NAME survey_pair_lease COMMAND test_survey_pair_lease)
set_tests_properties(survey_pair_lease
    PROPERTIES LABELS "mesh_integration;hardware_models;survey")

add_executable(test_survey_gateway_transaction
    tests/test_survey_gateway_transaction.c)
target_link_libraries(test_survey_gateway_transaction PRIVATE core)
add_test(NAME survey_gateway_transaction
    COMMAND test_survey_gateway_transaction)
set_tests_properties(survey_gateway_transaction
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication")

add_executable(test_survey_gateway_drive_policy
    tests/test_survey_gateway_drive_policy.c)
target_link_libraries(test_survey_gateway_drive_policy PRIVATE core)
add_test(NAME survey_gateway_drive_policy
    COMMAND test_survey_gateway_drive_policy)
set_tests_properties(survey_gateway_drive_policy
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication")

add_executable(test_watchdog_adoption tests/test_watchdog_adoption.c)
target_link_libraries(test_watchdog_adoption PRIVATE core)
add_test(NAME watchdog_adoption COMMAND test_watchdog_adoption)
set_tests_properties(watchdog_adoption
    PROPERTIES LABELS "mesh_integration;hardware_models;watchdog")

add_executable(test_stack_diag_transport tests/test_stack_diag_transport.c)
target_link_libraries(test_stack_diag_transport PRIVATE core)
add_test(NAME stack_diag_transport COMMAND test_stack_diag_transport)
set_tests_properties(stack_diag_transport PROPERTIES
    LABELS "mesh_integration;hardware_models;stack_evidence;communication")

add_executable(test_app_node_comm_adapter
    tests/test_app_node_comm_adapter.c
    app/src/app_node_comm.c
    app/src/app_node_comm_sync.c)
target_include_directories(test_app_node_comm_adapter PRIVATE
    tests/mesh_integration/zephyr_shim
    app/src)
target_link_libraries(test_app_node_comm_adapter PRIVATE core Threads::Threads)
add_test(NAME app_node_comm_adapter COMMAND test_app_node_comm_adapter)

add_executable(test_app_node_comm_gateway_adapter
    tests/test_app_node_comm_adapter.c
    app/src/app_node_comm.c
    app/src/app_node_comm_sync.c)
target_compile_definitions(test_app_node_comm_gateway_adapter PRIVATE
    DEVICE_ROLE=ROLE_GATEWAY
    APP_NODE_COMM_GATEWAY_ROLE=1)
target_include_directories(test_app_node_comm_gateway_adapter PRIVATE
    tests/mesh_integration/zephyr_shim
    app/src)
target_link_libraries(test_app_node_comm_gateway_adapter PRIVATE
    core Threads::Threads)
add_test(NAME app_node_comm_gateway_adapter
    COMMAND test_app_node_comm_gateway_adapter)
set_tests_properties(app_node_comm_gateway_adapter PROPERTIES
    LABELS "mesh_integration;hardware_models;communication")

add_executable(test_app_node_comm_transmitter_adapter
    tests/test_app_node_comm_adapter.c
    app/src/app_node_comm.c
    app/src/app_node_comm_sync.c)
target_compile_definitions(test_app_node_comm_transmitter_adapter PRIVATE
    DEVICE_ROLE=ROLE_ANCHOR
    CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER=1)
target_include_directories(test_app_node_comm_transmitter_adapter PRIVATE
    tests/mesh_integration/zephyr_shim
    app/src)
target_link_libraries(test_app_node_comm_transmitter_adapter PRIVATE
    core Threads::Threads)
add_test(NAME app_node_comm_transmitter_adapter
    COMMAND test_app_node_comm_transmitter_adapter)
set_tests_properties(app_node_comm_transmitter_adapter PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;forcedhop")

add_executable(test_app_node_comm_gateway_route_refresh
    tests/test_app_node_comm_gateway_route_refresh.c
    app/src/app_node_comm_sync.c
    app/src/app_node_comm_gateway_route_refresh.c
    app/src/app_mesh_flood.c
)
target_include_directories(test_app_node_comm_gateway_route_refresh PRIVATE
    app/src
    tests/mesh_integration/zephyr_shim
)
target_link_libraries(test_app_node_comm_gateway_route_refresh PRIVATE
    core Threads::Threads)
add_test(NAME app_node_comm_gateway_route_refresh
    COMMAND test_app_node_comm_gateway_route_refresh)
set_tests_properties(app_node_comm_gateway_route_refresh PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;route_refresh")
set_tests_properties(app_node_comm_adapter
    PROPERTIES LABELS "mesh_integration;hardware_models;communication")

add_executable(test_device_identity tests/test_device_identity.c)
target_link_libraries(test_device_identity PRIVATE core)
add_test(NAME device_identity COMMAND test_device_identity)

add_executable(test_status tests/test_status.c)
target_link_libraries(test_status PRIVATE core)
add_test(NAME status COMMAND test_status)

add_executable(test_gateway_command tests/test_gateway_command.c)
target_link_libraries(test_gateway_command PRIVATE core)
add_test(NAME gateway_command COMMAND test_gateway_command)

add_executable(test_gateway_collection_journal
    tests/test_gateway_collection_journal.c
    src/gateway_collection_journal.c
    src/gateway_command.c
    src/gateway_membership.c
    src/protocol.c
    src/mesh.c)
target_include_directories(test_gateway_collection_journal PRIVATE include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_gateway_collection_journal PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME gateway_collection_journal COMMAND test_gateway_collection_journal)
set_tests_properties(gateway_collection_journal PROPERTIES
    LABELS "mesh_integration;hardware_models;gateway;persistence")

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

add_executable(test_app_gateway_eack_retry
    tests/test_app_gateway_eack_retry.c
    app/src/app_gateway_eack_retry.c
    app/src/app_mesh_rf_retry.c
    app/src/app_wake_train_politeness.c
)
target_include_directories(test_app_gateway_eack_retry PRIVATE app/src)
target_link_libraries(test_app_gateway_eack_retry PRIVATE core)
add_test(NAME app_gateway_eack_retry COMMAND test_app_gateway_eack_retry)
set_tests_properties(app_gateway_eack_retry PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway")

add_test(NAME gateway_eack_retry_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_eack_retry_source_invariants.py)
set_tests_properties(gateway_eack_retry_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;static_analysis")
add_test(NAME gateway_durable_accept_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_durable_accept_source_invariants.py)
set_tests_properties(gateway_durable_accept_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;survey;static_analysis")
add_test(NAME assignment_claim_semantic_acceptance
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_assignment_claim_semantic_acceptance.py)
set_tests_properties(assignment_claim_semantic_acceptance PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;assignment;static_analysis")
add_test(NAME assignment_state_serialization_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_assignment_state_serialization_source_invariants.py)
set_tests_properties(assignment_state_serialization_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;assignment;static_analysis")

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

add_executable(test_app_gateway_assignment_publisher
    tests/test_app_gateway_assignment_publisher.c
    app/src/app_gateway_assignment_publisher.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_app_gateway_assignment_publisher PRIVATE app/src)
target_link_libraries(test_app_gateway_assignment_publisher PRIVATE core)
add_test(NAME app_gateway_assignment_publisher
    COMMAND test_app_gateway_assignment_publisher)
set_tests_properties(app_gateway_assignment_publisher
    PROPERTIES LABELS "mesh_integration;hardware_models;gateway_command;enumeration")
add_test(NAME assignment_publisher_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_assignment_publisher_source_invariants.py)
set_tests_properties(assignment_publisher_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;gateway_command;enumeration")
add_test(NAME node_comm_source_boundary
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_node_comm_source_boundary.py)
set_tests_properties(node_comm_source_boundary
    PROPERTIES LABELS "mesh_integration;hardware_models;communication")
add_test(NAME route_refresh_source_boundary
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_route_refresh_source_boundary.py)
set_tests_properties(route_refresh_source_boundary
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;gateway_command;route;static_analysis")
add_test(NAME node_comm_protocol_callers
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_node_comm_protocol_callers.py)
set_tests_properties(node_comm_protocol_callers
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;survey;enumeration")

add_executable(test_app_stack_workload_diag
    tests/test_app_stack_workload_diag.c
    app/src/app_stack_workload_diag.c
)
target_compile_definitions(test_app_stack_workload_diag PRIVATE
    CONFIG_IMEC_STACK_DIAGNOSTICS=1
    CONFIG_IMEC_STACK_WORKLOAD_DIAG_TEST_HOOKS=1
    _XOPEN_SOURCE=700
)
target_include_directories(test_app_stack_workload_diag PRIVATE
    app/src
    tests/mesh_integration/zephyr_shim
)
target_link_libraries(test_app_stack_workload_diag PRIVATE core Threads::Threads)
add_test(NAME app_stack_workload_diag COMMAND test_app_stack_workload_diag)
set_tests_properties(app_stack_workload_diag PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;stack_evidence")

add_executable(test_app_stack_workload_diag_disabled
    tests/test_app_stack_workload_diag_disabled.c
    app/src/app_stack_workload_diag.c
)
target_include_directories(test_app_stack_workload_diag_disabled PRIVATE app/src)
target_link_libraries(test_app_stack_workload_diag_disabled PRIVATE core)
add_test(NAME app_stack_workload_diag_disabled COMMAND test_app_stack_workload_diag_disabled)

add_executable(test_app_stack_diag_record
    tests/test_app_stack_diag_record.c
    app/src/app_stack_diag_record.c
)
target_include_directories(test_app_stack_diag_record PRIVATE app/src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_app_stack_diag_record PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME app_stack_diag_record COMMAND test_app_stack_diag_record)
add_executable(test_app_stack_diag_commit
    tests/test_app_stack_diag_commit.c
    app/src/app_stack_diag_record.c
)
target_include_directories(test_app_stack_diag_commit PRIVATE
    app/src
    tests/mesh_integration/zephyr_shim
)
target_link_libraries(test_app_stack_diag_commit PRIVATE Threads::Threads)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_app_stack_diag_commit PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME app_stack_diag_commit COMMAND test_app_stack_diag_commit)
set_tests_properties(app_stack_diag_commit PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;stack_evidence")
add_test(NAME stack_diag_emission_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_stack_diag_emission_invariants.py)
set_tests_properties(stack_diag_emission_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;deployment")
add_test(NAME gateway_observability_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_observability_source_invariants.py)
set_tests_properties(gateway_observability_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;ble;survey;static_analysis")

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

add_executable(test_app_gateway_command_lifecycle
    tests/test_app_gateway_command_lifecycle.c
    app/src/app_gateway_command_ingress.c
    app/src/app_gateway_command_lifecycle.c
)
target_include_directories(test_app_gateway_command_lifecycle PRIVATE app/src)
target_link_libraries(test_app_gateway_command_lifecycle PRIVATE core)
add_test(NAME app_gateway_command_lifecycle
    COMMAND test_app_gateway_command_lifecycle)

add_executable(test_app_mesh_gateway_command_flow
    tests/test_app_mesh_gateway_command_flow.c
    app/src/app_mesh_gateway_command_flow.c
)
target_include_directories(test_app_mesh_gateway_command_flow PRIVATE app/src)
target_link_libraries(test_app_mesh_gateway_command_flow PRIVATE core)
add_test(NAME app_mesh_gateway_command_flow
    COMMAND test_app_mesh_gateway_command_flow)
set_tests_properties(app_mesh_gateway_command_flow
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;survey")

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

add_executable(test_app_mesh_rf_retry
    tests/test_app_mesh_rf_retry.c
    app/src/app_mesh_rf_retry.c
    app/src/app_mesh_route_request_policy.c
    app/src/app_wake_train_politeness.c)
target_include_directories(test_app_mesh_rf_retry PRIVATE app/src)
target_link_libraries(test_app_mesh_rf_retry PRIVATE core)
add_test(NAME app_mesh_rf_retry COMMAND test_app_mesh_rf_retry)
set_tests_properties(app_mesh_rf_retry PROPERTIES
    LABELS "mesh_integration;hardware_models;communication")
add_executable(test_app_mesh_event_retry
    tests/test_app_mesh_event_retry.c
    app/src/app_mesh_event_retry.c
    app/src/app_mesh_rf_retry.c
    app/src/app_wake_train_politeness.c)
target_include_directories(test_app_mesh_event_retry PRIVATE app/src)
target_link_libraries(test_app_mesh_event_retry PRIVATE core)
add_test(NAME app_mesh_event_retry COMMAND test_app_mesh_event_retry)
set_tests_properties(app_mesh_event_retry PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;route")
add_test(NAME mesh_c5_control_lane_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_c5_control_lane_invariants.py)
set_tests_properties(mesh_c5_control_lane_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;static_analysis")
add_test(NAME mesh_rf_retry_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_rf_retry_source_invariants.py)
set_tests_properties(mesh_rf_retry_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;static_analysis")

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

add_executable(test_app_mesh_preemption
    tests/test_app_mesh_preemption.c
    app/src/app_mesh_preemption.c
)
target_include_directories(test_app_mesh_preemption PRIVATE app/src)
target_link_libraries(test_app_mesh_preemption PRIVATE core)
add_test(NAME app_mesh_preemption COMMAND test_app_mesh_preemption)

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

add_executable(test_app_mesh_local_delivery
    tests/test_app_mesh_local_delivery.c
    app/src/app_mesh_local_delivery.c)
target_include_directories(test_app_mesh_local_delivery PRIVATE app/src)
target_link_libraries(test_app_mesh_local_delivery PRIVATE core)
add_test(NAME app_mesh_local_delivery COMMAND test_app_mesh_local_delivery)
set_tests_properties(app_mesh_local_delivery
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;persistence")

add_test(NAME survey_delivery_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_delivery_source_invariants.py)
set_tests_properties(survey_delivery_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;persistence")

add_test(NAME survey_discovery_failure_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_discovery_failure_source_invariants.py)
set_tests_properties(survey_discovery_failure_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing")

add_test(NAME survey_active_generation_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_active_generation_source_invariants.py)
set_tests_properties(survey_active_generation_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing")

add_test(NAME survey_pair_control_lifecycle_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_pair_control_lifecycle_source_invariants.py)
set_tests_properties(survey_pair_control_lifecycle_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing;static_analysis")

add_test(NAME dwm3000_receive_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_dwm3000_receive_source_invariants.py)
set_tests_properties(dwm3000_receive_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;timing;watchdog")

add_executable(test_mesh_relay tests/test_mesh_relay.c)
target_link_libraries(test_mesh_relay PRIVATE core)
add_test(NAME mesh_relay COMMAND test_mesh_relay)

add_executable(test_mesh_result_grant_retry_scenarios
    tests/mesh_integration/test_mesh_result_grant_retry_scenarios.c)
target_link_libraries(test_mesh_result_grant_retry_scenarios PRIVATE core)
add_test(NAME mesh_result_grant_retry_scenarios
    COMMAND test_mesh_result_grant_retry_scenarios)
set_tests_properties(mesh_result_grant_retry_scenarios PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;network_stress")

add_executable(test_serial_frame tests/test_serial_frame.c)
target_link_libraries(test_serial_frame PRIVATE core)
add_test(NAME serial_frame COMMAND test_serial_frame)

add_executable(test_report tests/test_report.c)
target_link_libraries(test_report PRIVATE core)
add_test(NAME report COMMAND test_report)

add_executable(test_survey tests/test_survey.c)
target_link_libraries(test_survey PRIVATE core)
add_test(NAME survey COMMAND test_survey)

add_executable(test_survey_completion_policy
    tests/test_survey_completion_policy.c)
target_link_libraries(test_survey_completion_policy PRIVATE core)
add_test(NAME survey_completion_policy COMMAND test_survey_completion_policy)
set_tests_properties(survey_completion_policy
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication")

add_executable(test_packet_initializers tests/test_packet_initializers.c)
target_link_libraries(test_packet_initializers PRIVATE core)
add_test(NAME packet_initializers COMMAND test_packet_initializers)

add_executable(test_debug_log tests/test_debug_log.c)
target_link_libraries(test_debug_log PRIVATE core)
add_test(NAME debug_log COMMAND test_debug_log)

add_executable(test_dwm3000_models
    tests/mesh_integration/test_dwm3000_models.c
)
target_link_libraries(test_dwm3000_models PRIVATE core)
add_test(NAME dwm3000_models COMMAND test_dwm3000_models)
set_tests_properties(dwm3000_models PROPERTIES LABELS "hardware_models")

add_executable(test_multi_anchor_timing_invariants
    tests/mesh_integration/test_multi_anchor_timing_invariants.c
)
target_link_libraries(test_multi_anchor_timing_invariants PRIVATE core)
add_test(NAME multi_anchor_timing_invariants
    COMMAND test_multi_anchor_timing_invariants)
set_tests_properties(multi_anchor_timing_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;timing;survey;wake")

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
target_include_directories(test_stack_budget_model PRIVATE app/src)
target_link_libraries(test_stack_budget_model PRIVATE core)
add_test(NAME stack_budget_model COMMAND test_stack_budget_model)
set_tests_properties(stack_budget_model PROPERTIES LABELS "hardware_models")

add_test(NAME verify_stack_evidence
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_verify_stack_evidence.py)
set_tests_properties(verify_stack_evidence PROPERTIES LABELS "hardware_models")

add_test(NAME forcedhop_ram_policy
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_forcedhop_ram_policy.py)
set_tests_properties(forcedhop_ram_policy
    PROPERTIES LABELS "mesh_integration;hardware_models;stack;forcedhop;static_analysis")

add_test(NAME mesh_report_role_storage
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_report_role_storage.py)
set_tests_properties(mesh_report_role_storage
    PROPERTIES LABELS "mesh_integration;hardware_models;stack;static_analysis")

add_test(NAME flash_verified_mesh
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_flash_verified_mesh.py)
set_tests_properties(flash_verified_mesh PROPERTIES LABELS "hardware_models")

add_test(NAME capture_stack_evidence
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_capture_stack_evidence.py)
set_tests_properties(capture_stack_evidence PROPERTIES LABELS "hardware_models")

add_test(NAME provision_mesh_anchor
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_provision_mesh_anchor.py)
set_tests_properties(provision_mesh_anchor PROPERTIES LABELS "hardware_models")

add_test(NAME mesh_deployment_policy
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_deployment_policy.py)
set_tests_properties(mesh_deployment_policy PROPERTIES LABELS "hardware_models")

add_test(NAME mesh_anchor_preset
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_anchor_preset.py)
set_tests_properties(mesh_anchor_preset
    PROPERTIES LABELS "mesh_integration;hardware_models;deployment")

add_library(mesh_sim
    sim/mesh_sim.c
    sim/mesh_sim_connection.c
    sim/mesh_sim_events.c
    sim/mesh_sim_interval.c
    sim/mesh_sim_radio.c
    sim/mesh_sim_relay.c
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

add_executable(test_mesh_discovery_assignment_adversarial_scenarios
    tests/mesh_integration/test_mesh_discovery_assignment_adversarial_scenarios.c
)
target_include_directories(test_mesh_discovery_assignment_adversarial_scenarios
    PRIVATE app/src)
target_link_libraries(test_mesh_discovery_assignment_adversarial_scenarios
    PRIVATE core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_mesh_discovery_assignment_adversarial_scenarios
        PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME mesh_discovery_assignment_adversarial_scenarios
    COMMAND test_mesh_discovery_assignment_adversarial_scenarios)
set_tests_properties(mesh_discovery_assignment_adversarial_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;assignment;network_stress")

add_executable(test_mesh_gateway_control_stress_scenarios
    tests/mesh_integration/test_mesh_gateway_control_stress_scenarios.c
    app/src/app_gateway_command_ingress.c
    app/src/app_gateway_command_lifecycle.c
    app/src/app_gateway_command_observability.c
    app/src/app_mesh_flood.c
    app/src/app_mesh_rx_policy.c
)
target_include_directories(test_mesh_gateway_control_stress_scenarios
    PRIVATE app/src)
target_link_libraries(test_mesh_gateway_control_stress_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_gateway_control_stress_scenarios
    COMMAND test_mesh_gateway_control_stress_scenarios)
set_tests_properties(mesh_gateway_control_stress_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;gateway_command;route;network_stress")

add_executable(test_node_comm_delivery_stress_scenarios
    tests/mesh_integration/test_node_comm_delivery_stress_scenarios.c)
target_link_libraries(test_node_comm_delivery_stress_scenarios PRIVATE core)
add_test(NAME node_comm_delivery_stress_scenarios
    COMMAND test_node_comm_delivery_stress_scenarios)
set_tests_properties(node_comm_delivery_stress_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;gateway_command;network_stress")

add_executable(test_mesh_survey_phy_scenarios
    tests/mesh_integration/test_mesh_survey_phy_scenarios.c
    app/src/app_mesh_c5_priority.c
    app/src/app_mesh_gateway_command_priority.c
)
target_include_directories(test_mesh_survey_phy_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_survey_phy_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_survey_phy_scenarios COMMAND test_mesh_survey_phy_scenarios)
set_tests_properties(mesh_survey_phy_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;survey;timing")

add_test(NAME survey_command_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_command_source_invariants.py)
set_tests_properties(survey_command_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;gateway_command")

add_executable(test_mesh_survey_topology_scenarios
    tests/mesh_integration/test_mesh_survey_topology_scenarios.c
    app/src/app_mesh_direct_gateway_retry.c
)
target_include_directories(test_mesh_survey_topology_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_survey_topology_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_survey_topology_scenarios
    COMMAND test_mesh_survey_topology_scenarios)
set_tests_properties(mesh_survey_topology_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;survey;network_stress")

add_executable(test_mesh_survey_direct_gateway_retry_scenarios
    tests/mesh_integration/test_mesh_survey_direct_gateway_retry_scenarios.c
    app/src/app_mesh_direct_gateway_retry.c
)
target_include_directories(test_mesh_survey_direct_gateway_retry_scenarios
    PRIVATE app/src)
target_link_libraries(test_mesh_survey_direct_gateway_retry_scenarios PRIVATE core)
add_test(NAME mesh_survey_direct_gateway_retry_scenarios
    COMMAND test_mesh_survey_direct_gateway_retry_scenarios)
set_tests_properties(mesh_survey_direct_gateway_retry_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;timing;network_stress")

add_executable(test_mesh_route_recovery_scenarios
    tests/mesh_integration/test_mesh_route_recovery_scenarios.c
)
target_link_libraries(test_mesh_route_recovery_scenarios PRIVATE core)
add_test(NAME mesh_route_recovery_scenarios
    COMMAND test_mesh_route_recovery_scenarios)
set_tests_properties(mesh_route_recovery_scenarios
    PROPERTIES LABELS "mesh_integration;route;recovery")

add_executable(test_mesh_direct_probe_recovery_scenarios
    tests/mesh_integration/test_mesh_direct_probe_recovery_scenarios.c
)
target_link_libraries(test_mesh_direct_probe_recovery_scenarios PRIVATE core)
add_test(NAME mesh_direct_probe_recovery_scenarios
    COMMAND test_mesh_direct_probe_recovery_scenarios)
set_tests_properties(mesh_direct_probe_recovery_scenarios
    PROPERTIES LABELS "mesh_integration;route;recovery")

add_executable(test_mesh_route_formation_scenarios
    tests/mesh_integration/test_mesh_route_formation_scenarios.c
)
target_link_libraries(test_mesh_route_formation_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_route_formation_scenarios
    COMMAND test_mesh_route_formation_scenarios)
set_tests_properties(mesh_route_formation_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;route;discovery")

add_executable(test_mesh_forcedhop_connection_scenarios
    tests/mesh_integration/test_mesh_forcedhop_connection_scenarios.c
    app/src/app_mesh_c5_priority.c
    app/src/app_mesh_ch9_ack.c
    app/src/app_mesh_route_request_policy.c
)
target_compile_definitions(test_mesh_forcedhop_connection_scenarios PRIVATE
    CONFIG_IMEC_MESH_ROUTE_TEST=1)
target_include_directories(test_mesh_forcedhop_connection_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_forcedhop_connection_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_forcedhop_connection_scenarios
    COMMAND test_mesh_forcedhop_connection_scenarios)
set_tests_properties(mesh_forcedhop_connection_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;route;connection;forcedhop;communication;delivery;network_stress")

add_executable(test_mesh_network_stress_scenarios
    tests/mesh_integration/test_mesh_network_stress_scenarios.c
)
target_link_libraries(test_mesh_network_stress_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_network_stress_scenarios
    COMMAND test_mesh_network_stress_scenarios)
set_tests_properties(mesh_network_stress_scenarios
    PROPERTIES LABELS "mesh_integration;simulator;stress")

add_executable(test_mesh_click_collision_sweep
    tests/mesh_integration/test_mesh_click_collision_sweep.c
)
target_link_libraries(test_mesh_click_collision_sweep PRIVATE mesh_sim)
add_test(NAME mesh_click_collision_sweep
    COMMAND test_mesh_click_collision_sweep)
set_tests_properties(mesh_click_collision_sweep
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;stress;collision;click")

add_executable(test_mesh_click_report_delivery_sweep
    tests/mesh_integration/test_mesh_click_report_delivery_sweep.c
)
target_link_libraries(test_mesh_click_report_delivery_sweep PRIVATE mesh_sim)
add_test(NAME mesh_click_report_delivery_sweep
    COMMAND test_mesh_click_report_delivery_sweep)
set_tests_properties(mesh_click_report_delivery_sweep
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;stress;click;network_stress")

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
    app/src/app_mesh_command_orchestrator.c
    app/src/app_mesh_arbitration_zephyr.c
    app/src/app_gateway_command_ingress.c
    app/src/app_gateway_command_lifecycle.c
    app/src/app_mesh_gateway_command_flow.c
    app/src/app_mesh_gateway_command_priority.c
    app/src/app_mesh_preemption.c
    app/src/app_mesh_flood.c
)

add_executable(test_gateway_command_observability_scenarios
    tests/mesh_integration/test_gateway_command_observability_scenarios.c
    app/src/app_gateway_command_observability.c
    app/src/app_gateway_survey_observability.c
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

add_executable(test_watchdog_adoption_scenarios
    tests/mesh_integration/test_watchdog_adoption_scenarios.c
)
target_link_libraries(test_watchdog_adoption_scenarios PRIVATE core)
add_test(NAME watchdog_adoption_scenarios
    COMMAND test_watchdog_adoption_scenarios)
set_tests_properties(watchdog_adoption_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;watchdog;boot")

add_test(NAME watchdog_adoption_source
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_watchdog_adoption_source.py)
set_tests_properties(watchdog_adoption_source
    PROPERTIES LABELS "mesh_integration;hardware_models;watchdog;static_analysis")

add_executable(test_mesh_sim_trace_scheduler
    tests/mesh_integration/test_mesh_sim_trace_scheduler.c
)
target_link_libraries(test_mesh_sim_trace_scheduler PRIVATE mesh_sim)
add_test(NAME mesh_sim_trace_scheduler
    COMMAND test_mesh_sim_trace_scheduler)
set_tests_properties(mesh_sim_trace_scheduler
    PROPERTIES LABELS "mesh_integration;simulator;trace;scheduler")

add_test(NAME mesh_ble_route_monitor
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_ble_route_monitor.py)
set_tests_properties(mesh_ble_route_monitor
    PROPERTIES LABELS "mesh_integration;host_tools;observability")
