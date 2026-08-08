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
    src/firmware_events.c
    src/firmware_delivery_loss.c
    src/firmware_state_click.c
    src/firmware_state_control.c
    src/firmware_state_mesh.c
    src/firmware_state_radio.c
    src/gateway_ble_transport.c
    src/protocol.c
    src/gateway_membership.c
    src/gateway_command.c
    src/uwb.c
    src/uwb_ble_courtesy.c
    src/uwb_session.c
    src/mesh.c
    src/mesh_event_owner.c
    src/mesh_event_owner_registry.c
    src/mesh_route_path.c
    src/node_comm.c
    src/node_transaction.c
    src/operation_policy.c
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
    src/semantic_digest.c
    src/survey.c
    src/survey_pair_planner.c
    src/survey_pair_round_runtime.c
    src/survey_round_control.c
    src/survey_gateway_transaction.c
    src/survey_pair_lease.c
)

target_include_directories(core PUBLIC include)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(test_firmware_events tests/test_firmware_events.c)
target_link_libraries(test_firmware_events PRIVATE core)
add_test(NAME firmware_events COMMAND test_firmware_events)
set_tests_properties(firmware_events PROPERTIES
    LABELS "mesh_integration;hardware_models;state;architecture")

add_executable(test_firmware_state_machines
    tests/test_firmware_state_machines.c)
target_link_libraries(test_firmware_state_machines PRIVATE core)
add_test(NAME firmware_state_machines COMMAND test_firmware_state_machines)
set_tests_properties(firmware_state_machines PROPERTIES
    LABELS "mesh_integration;hardware_models;state;architecture")

add_executable(test_app_clicker_event_runtime
    tests/test_app_clicker_event_runtime.c
    app/src/app_clicker_event_runtime.c)
target_include_directories(test_app_clicker_event_runtime PRIVATE app/src)
target_link_libraries(test_app_clicker_event_runtime PRIVATE core)
add_test(NAME app_clicker_event_runtime
    COMMAND test_app_clicker_event_runtime)
set_tests_properties(app_clicker_event_runtime PROPERTIES
    LABELS "mesh_integration;hardware_models;state;click")

add_executable(test_app_anchor_click_event_runtime
    tests/test_app_anchor_click_event_runtime.c
    app/src/app_anchor_click_event_runtime.c)
target_include_directories(test_app_anchor_click_event_runtime PRIVATE app/src)
target_link_libraries(test_app_anchor_click_event_runtime PRIVATE core)
add_test(NAME app_anchor_click_event_runtime
    COMMAND test_app_anchor_click_event_runtime)
set_tests_properties(app_anchor_click_event_runtime PROPERTIES
    LABELS "mesh_integration;hardware_models;state;click;custody")

add_executable(test_app_gateway_survey_event_runtime
    tests/test_app_gateway_survey_event_runtime.c
    app/src/app_gateway_survey_event_runtime.c)
target_include_directories(test_app_gateway_survey_event_runtime PRIVATE app/src)
target_link_libraries(test_app_gateway_survey_event_runtime PRIVATE core)
add_test(NAME app_gateway_survey_event_runtime
    COMMAND test_app_gateway_survey_event_runtime)
set_tests_properties(app_gateway_survey_event_runtime PROPERTIES
    LABELS "mesh_integration;hardware_models;state;survey")

add_executable(test_app_node_comm_delivery_state
    tests/test_app_node_comm_delivery_state.c)
target_link_libraries(test_app_node_comm_delivery_state PRIVATE core)
add_test(NAME app_node_comm_delivery_state
    COMMAND test_app_node_comm_delivery_state)
set_tests_properties(app_node_comm_delivery_state PROPERTIES
    LABELS "mesh_integration;hardware_models;state;communication;custody")

add_executable(test_protocol tests/test_protocol.c)
target_link_libraries(test_protocol PRIVATE core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_mesh_event_owner tests/test_mesh_event_owner.c)
target_link_libraries(test_mesh_event_owner PRIVATE core)
add_test(NAME mesh_event_owner COMMAND test_mesh_event_owner)
set_tests_properties(mesh_event_owner PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol;connection")

add_executable(test_mesh_event_owner_registry
    tests/test_mesh_event_owner_registry.c)
target_link_libraries(test_mesh_event_owner_registry PRIVATE core)
add_test(NAME mesh_event_owner_registry COMMAND test_mesh_event_owner_registry)
set_tests_properties(mesh_event_owner_registry PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol;connection;state")

add_executable(test_operation_policy tests/test_operation_policy.c)
target_link_libraries(test_operation_policy PRIVATE core)
add_test(NAME operation_policy COMMAND test_operation_policy)
set_tests_properties(operation_policy PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol")

add_executable(test_app_operation_policy
    tests/test_app_operation_policy.c
    app/src/app_operation_policy.c)
target_include_directories(test_app_operation_policy PRIVATE
    app/src
    tests/mesh_integration/zephyr_shim)
target_link_libraries(test_app_operation_policy PRIVATE core Threads::Threads)
add_test(NAME app_operation_policy COMMAND test_app_operation_policy)
set_tests_properties(app_operation_policy PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol;state")

add_executable(test_discovery_assignment tests/test_discovery_assignment.c)
target_link_libraries(test_discovery_assignment PRIVATE core)
add_test(NAME discovery_assignment COMMAND test_discovery_assignment)

add_executable(test_semantic_digest tests/test_semantic_digest.c)
target_link_libraries(test_semantic_digest PRIVATE core)
add_test(NAME semantic_digest COMMAND test_semantic_digest)
set_tests_properties(semantic_digest PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol")

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

add_executable(test_survey_round_control tests/test_survey_round_control.c)
target_link_libraries(test_survey_round_control PRIVATE core)
add_test(NAME survey_round_control COMMAND test_survey_round_control)
set_tests_properties(survey_round_control
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;protocol")

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

add_executable(test_app_watchdog_action_lease
    tests/test_app_watchdog_action_lease.c)
target_include_directories(test_app_watchdog_action_lease PRIVATE app/src)
add_test(NAME app_watchdog_action_lease
    COMMAND test_app_watchdog_action_lease)
set_tests_properties(app_watchdog_action_lease
    PROPERTIES LABELS "mesh_integration;hardware_models;watchdog;click;state")

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

add_executable(test_button_wake_recovery
    tests/test_button_wake_recovery.c)
target_include_directories(test_button_wake_recovery PRIVATE include)
add_test(NAME button_wake_recovery COMMAND test_button_wake_recovery)
set_tests_properties(button_wake_recovery PROPERTIES
    LABELS "mesh_integration;hardware_models;click;power;state")

add_executable(test_button_action_handoff
    tests/test_button_action_handoff.c)
target_include_directories(test_button_action_handoff PRIVATE include)
add_test(NAME button_action_handoff COMMAND test_button_action_handoff)
set_tests_properties(button_action_handoff PROPERTIES
    LABELS "mesh_integration;hardware_models;click;queues;state")

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

add_test(NAME assignment_claim_semantic_acceptance
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_assignment_claim_semantic_acceptance.py)
set_tests_properties(assignment_claim_semantic_acceptance PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;assignment;static_analysis")

add_executable(test_app_gateway_ble_stream
    tests/test_app_gateway_ble_stream.c
    app/src/app_gateway_ble_stream.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_app_gateway_ble_stream PRIVATE app/src)
target_link_libraries(test_app_gateway_ble_stream PRIVATE core)
add_test(NAME app_gateway_ble_stream COMMAND test_app_gateway_ble_stream)
set_tests_properties(app_gateway_ble_stream PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;gateway;ble;custody")

add_executable(test_app_gateway_command_result
    tests/test_app_gateway_command_result.c
    app/src/app_gateway_command_result.c
)
target_include_directories(test_app_gateway_command_result PRIVATE app/src)
target_link_libraries(test_app_gateway_command_result PRIVATE core)
add_test(NAME app_gateway_command_result
    COMMAND test_app_gateway_command_result)
set_tests_properties(app_gateway_command_result PROPERTIES
    LABELS "mesh_integration;hardware_models;gateway_command;ble;delivery")

add_executable(test_app_anchor_command_completion
    tests/test_app_anchor_command_completion.c
    app/src/app_anchor_command_completion.c
)
target_include_directories(test_app_anchor_command_completion PRIVATE
    app/src
    include)
add_test(NAME app_anchor_command_completion
    COMMAND test_app_anchor_command_completion)
set_tests_properties(app_anchor_command_completion PROPERTIES
    LABELS "mesh_integration;hardware_models;gateway_command;delivery;persistence;state")

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
add_test(NAME route_control_wire_source_boundary
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_route_control_wire_source_boundary.py)
set_tests_properties(route_control_wire_source_boundary
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;route;static_analysis")


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
set_tests_properties(app_mesh_c5_priority PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;timing;wake")

add_executable(test_app_mesh_scheduler_liveness
    tests/test_app_mesh_scheduler_liveness.c
)
target_include_directories(test_app_mesh_scheduler_liveness PRIVATE app/src)
target_link_libraries(test_app_mesh_scheduler_liveness PRIVATE core)
add_test(NAME app_mesh_scheduler_liveness
         COMMAND test_app_mesh_scheduler_liveness)
set_tests_properties(app_mesh_scheduler_liveness PROPERTIES
    LABELS "mesh_integration;hardware_models;communication")

add_executable(test_app_mesh_async_route_request
    tests/test_app_mesh_async_route_request.c
    app/src/app_mesh_async_route_request.c
)
target_include_directories(test_app_mesh_async_route_request PRIVATE app/src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_app_mesh_async_route_request PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME app_mesh_async_route_request
    COMMAND test_app_mesh_async_route_request)
set_tests_properties(app_mesh_async_route_request PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;route;state")

add_executable(test_app_gateway_command_ingress
    tests/test_app_gateway_command_ingress.c
    app/src/app_gateway_command_ingress.c
    app/src/app_mesh_c5_priority.c
    app/src/app_mesh_gateway_command_flow.c
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
set_tests_properties(app_wake_train_politeness PROPERTIES
    LABELS "mesh_integration;hardware_models;click;timing;wake")

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
add_test(NAME mesh_event_proposal_arbitration_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_event_proposal_arbitration_source_invariants.py)
set_tests_properties(mesh_event_proposal_arbitration_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;route;static_analysis")
add_test(NAME mesh_event_identity_replay_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_event_identity_replay_source_invariants.py)
set_tests_properties(mesh_event_identity_replay_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;route;protocol;static_analysis")
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
target_link_libraries(test_app_discovery_assignment_policy PRIVATE core)
target_include_directories(test_app_discovery_assignment_policy PRIVATE app/src)
add_test(NAME app_discovery_assignment_policy
    COMMAND test_app_discovery_assignment_policy)
target_include_directories(test_app_discovery_assignment_policy PRIVATE include)
set_tests_properties(app_discovery_assignment_policy PROPERTIES
    LABELS "mesh_integration;hardware_models;protocol;state")

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

add_executable(test_mesh_route_path tests/test_mesh_route_path.c)
target_link_libraries(test_mesh_route_path PRIVATE core)
add_test(NAME mesh_route_path COMMAND test_mesh_route_path)
set_tests_properties(mesh_route_path
    PROPERTIES LABELS "mesh_integration;hardware_models;routing")

add_executable(test_mesh_preemption tests/test_mesh_preemption.c)
target_link_libraries(test_mesh_preemption PRIVATE core)
add_test(NAME mesh_preemption COMMAND test_mesh_preemption)
set_tests_properties(mesh_preemption PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;preemption")

add_executable(test_app_mesh_preemption
    tests/test_app_mesh_preemption.c
    app/src/app_mesh_preemption.c
)
target_include_directories(test_app_mesh_preemption PRIVATE app/src)
target_link_libraries(test_app_mesh_preemption PRIVATE core)
add_test(NAME app_mesh_preemption COMMAND test_app_mesh_preemption)
set_tests_properties(app_mesh_preemption PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;preemption")



add_test(NAME gateway_survey_dynamic_deadline_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_survey_dynamic_deadline_source_invariants.py)
set_tests_properties(gateway_survey_dynamic_deadline_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;survey;timing;static_analysis")

add_test(NAME gateway_survey_receive_deadline_source_invariants
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_survey_receive_deadline_source_invariants.py)
set_tests_properties(gateway_survey_receive_deadline_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;survey;timing;communication;static_analysis")

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
set_tests_properties(app_mesh_ch9_ack PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;preemption")

add_executable(test_app_mesh_local_delivery
    tests/test_app_mesh_local_delivery.c
    app/src/app_mesh_local_delivery.c)
target_include_directories(test_app_mesh_local_delivery PRIVATE app/src)
target_link_libraries(test_app_mesh_local_delivery PRIVATE core)
add_test(NAME app_mesh_local_delivery COMMAND test_app_mesh_local_delivery)
set_tests_properties(app_mesh_local_delivery
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;persistence")


add_test(NAME transaction_digest_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_transaction_digest_source_invariants.py)
set_tests_properties(transaction_digest_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;static_analysis")



add_test(NAME survey_discovery_failure_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_discovery_failure_source_invariants.py)
set_tests_properties(survey_discovery_failure_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing")


add_test(NAME survey_pair_control_lifecycle_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_survey_pair_control_lifecycle_source_invariants.py)
set_tests_properties(survey_pair_control_lifecycle_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing;static_analysis")

add_test(NAME anchor_survey_round_barrier_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_anchor_survey_round_barrier_source_invariants.py)
set_tests_properties(anchor_survey_round_barrier_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing;static_analysis")

add_test(NAME gateway_survey_round_glue_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_gateway_survey_round_glue_source_invariants.py)
set_tests_properties(gateway_survey_round_glue_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;survey;communication;timing;static_analysis")

add_test(NAME dwm3000_receive_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_dwm3000_receive_source_invariants.py)
set_tests_properties(dwm3000_receive_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;communication;timing;watchdog")

add_test(NAME clicker_button_wake_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_clicker_button_wake_source_invariants.py)
set_tests_properties(clicker_button_wake_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;click;power;state;static_analysis")

add_test(NAME clicker_retained_idle_failure_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_clicker_retained_idle_failure_source_invariants.py)
set_tests_properties(clicker_retained_idle_failure_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;click;power;state;static_analysis")






add_test(NAME clicker_wake_deadline_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_clicker_wake_deadline_source_invariants.py)
set_tests_properties(clicker_wake_deadline_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;click;timing;wake;static_analysis")

add_test(NAME ml_clicker_role_isolation_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_ml_clicker_role_isolation_source_invariants.py)
set_tests_properties(ml_clicker_role_isolation_source_invariants
    PROPERTIES LABELS "mesh_integration;hardware_models;click;ml;memory;static_analysis")

add_executable(test_mesh_relay tests/test_mesh_relay.c)
target_link_libraries(test_mesh_relay PRIVATE core)
add_test(NAME mesh_relay COMMAND test_mesh_relay)
set_tests_properties(mesh_relay PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;routing;protocol")

add_executable(test_mesh_outbox_class_matrix
    tests/test_mesh_outbox_class_matrix.c)
target_link_libraries(test_mesh_outbox_class_matrix PRIVATE core)
add_test(NAME mesh_outbox_class_matrix COMMAND test_mesh_outbox_class_matrix)
set_tests_properties(mesh_outbox_class_matrix PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;persistence;protocol")


add_test(NAME mesh_clicker_on_demand_rx_source_invariants
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_clicker_on_demand_rx_source_invariants.py)
set_tests_properties(mesh_clicker_on_demand_rx_source_invariants PROPERTIES
    LABELS "mesh_integration;hardware_models;click;routing;power;static_analysis")

add_executable(test_mesh_route_adv_policy
    tests/test_mesh_route_adv_policy.c)
target_link_libraries(test_mesh_route_adv_policy PRIVATE core)
add_test(NAME mesh_route_adv_policy COMMAND test_mesh_route_adv_policy)
set_tests_properties(mesh_route_adv_policy PROPERTIES
    LABELS "mesh_integration;hardware_models;routing;protocol")

add_executable(test_mesh_downlink_capacity tests/test_mesh_downlink_capacity.c)
target_link_libraries(test_mesh_downlink_capacity PRIVATE core)
add_test(NAME mesh_downlink_capacity COMMAND test_mesh_downlink_capacity)
set_tests_properties(mesh_downlink_capacity PROPERTIES
    LABELS "mesh_integration;hardware_models;routing;network_stress")

add_executable(test_mesh_result_grant_retry_scenarios
    tests/mesh_integration/test_mesh_result_grant_retry_scenarios.c)
target_link_libraries(test_mesh_result_grant_retry_scenarios PRIVATE core)
add_test(NAME mesh_result_grant_retry_scenarios
    COMMAND test_mesh_result_grant_retry_scenarios)
set_tests_properties(mesh_result_grant_retry_scenarios PROPERTIES
    LABELS "mesh_integration;hardware_models;communication;network_stress")

add_executable(test_mesh_result_custody_rf_scenarios
    tests/mesh_integration/test_mesh_result_custody_rf_scenarios.c)
target_compile_options(test_mesh_result_custody_rf_scenarios PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_mesh_result_custody_rf_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_result_custody_rf_scenarios
    COMMAND test_mesh_result_custody_rf_scenarios)
set_tests_properties(mesh_result_custody_rf_scenarios PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;communication;network_stress;custody;liveness;protocol")

add_executable(test_serial_frame tests/test_serial_frame.c)
target_link_libraries(test_serial_frame PRIVATE core)
add_test(NAME serial_frame COMMAND test_serial_frame)

add_executable(test_report tests/test_report.c)
target_link_libraries(test_report PRIVATE core)
add_test(NAME report COMMAND test_report)

add_executable(test_survey tests/test_survey.c)
target_link_libraries(test_survey PRIVATE core)
add_test(NAME survey COMMAND test_survey)

add_executable(test_survey_pair_round_runtime
    tests/test_survey_pair_round_runtime.c)
target_link_libraries(test_survey_pair_round_runtime PRIVATE core)
add_test(NAME survey_pair_round_runtime
    COMMAND test_survey_pair_round_runtime)
set_tests_properties(survey_pair_round_runtime
    PROPERTIES LABELS "mesh_integration;hardware_models;survey")

add_executable(test_app_gateway_survey_round
    tests/test_app_gateway_survey_round.c
    app/src/app_gateway_survey_round.c)
target_include_directories(test_app_gateway_survey_round PRIVATE app/src)
target_link_libraries(test_app_gateway_survey_round PRIVATE core)
add_test(NAME app_gateway_survey_round
    COMMAND test_app_gateway_survey_round)
set_tests_properties(app_gateway_survey_round
    PROPERTIES LABELS "mesh_integration;hardware_models;survey")

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

add_library(mesh_sim
    sim/mesh_sim.c
    sim/mesh_sim_connection.c
    sim/mesh_sim_events.c
    sim/mesh_sim_invariants.c
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

add_executable(test_mesh_assignment_control_timing_scenarios
    tests/mesh_integration/test_mesh_assignment_control_timing_scenarios.c
)
target_include_directories(test_mesh_assignment_control_timing_scenarios
    PRIVATE app/src)
target_link_libraries(test_mesh_assignment_control_timing_scenarios
    PRIVATE mesh_sim)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_mesh_assignment_control_timing_scenarios
        PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME mesh_assignment_control_timing_scenarios
    COMMAND test_mesh_assignment_control_timing_scenarios)
set_tests_properties(mesh_assignment_control_timing_scenarios PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;assignment;gateway_command;timing")

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

add_executable(test_mesh_protocol_lifecycle_scenarios
    tests/mesh_integration/test_mesh_protocol_lifecycle_scenarios.c
)
target_include_directories(test_mesh_protocol_lifecycle_scenarios
    PRIVATE app/src)
target_link_libraries(test_mesh_protocol_lifecycle_scenarios PRIVATE mesh_sim)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_mesh_protocol_lifecycle_scenarios
        PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME mesh_protocol_lifecycle_scenarios
    COMMAND test_mesh_protocol_lifecycle_scenarios)
set_tests_properties(mesh_protocol_lifecycle_scenarios PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;protocol;assignment;survey;forcedhop;state;timing")

add_executable(test_mesh_survey_round_adversarial_lifecycle
    tests/mesh_integration/test_mesh_survey_round_adversarial_lifecycle.c
)
target_link_libraries(test_mesh_survey_round_adversarial_lifecycle
    PRIVATE mesh_sim)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_mesh_survey_round_adversarial_lifecycle
        PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME mesh_survey_round_adversarial_lifecycle
    COMMAND test_mesh_survey_round_adversarial_lifecycle)
set_tests_properties(mesh_survey_round_adversarial_lifecycle PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;protocol;survey;forcedhop;faults;state;timing;liveness")
add_test(NAME mesh_survey_round_adversarial_lifecycle_seed_zero
    COMMAND test_mesh_survey_round_adversarial_lifecycle --seed 0)
set_tests_properties(mesh_survey_round_adversarial_lifecycle_seed_zero PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;protocol;survey;forcedhop;faults;state;timing;liveness;replay")
add_test(NAME mesh_survey_round_adversarial_lifecycle_seed_max
    COMMAND test_mesh_survey_round_adversarial_lifecycle --seed 0xffffffff)
set_tests_properties(mesh_survey_round_adversarial_lifecycle_seed_max PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;protocol;survey;forcedhop;faults;state;timing;liveness;replay")

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
)
target_include_directories(test_mesh_survey_phy_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_survey_phy_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_survey_phy_scenarios COMMAND test_mesh_survey_phy_scenarios)
set_tests_properties(mesh_survey_phy_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;survey;timing")



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

add_executable(test_mesh_click_protocol_chain_scenarios
    tests/mesh_integration/test_mesh_click_protocol_chain_scenarios.c
    app/src/app_gateway_ble_stream.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_mesh_click_protocol_chain_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_click_protocol_chain_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_click_protocol_chain_scenarios
    COMMAND test_mesh_click_protocol_chain_scenarios)
set_tests_properties(mesh_click_protocol_chain_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;click;wake;protocol;communication;delivery;ble;network_stress")

add_executable(test_dwm3000_driver_handler_boundary
    tests/mesh_integration/test_dwm3000_driver_handler_boundary.c
    app/src/dwm3000_driver_handler_contract.c
    app/src/dwm3000_driver_test_seam.c
)
target_include_directories(test_dwm3000_driver_handler_boundary PRIVATE app/src)
target_link_libraries(test_dwm3000_driver_handler_boundary PRIVATE core)
add_test(NAME dwm3000_driver_handler_boundary
    COMMAND test_dwm3000_driver_handler_boundary)
set_tests_properties(dwm3000_driver_handler_boundary
    PROPERTIES LABELS "mesh_integration;hardware_models;click;dwm3000;ds_twr;protocol")

add_executable(test_mesh_gateway_ble_scenarios
    tests/mesh_integration/test_mesh_gateway_ble_scenarios.c
    app/src/app_gateway_ble_stream.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_mesh_gateway_ble_scenarios PRIVATE app/src)
target_link_libraries(test_mesh_gateway_ble_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_gateway_ble_scenarios
    COMMAND test_mesh_gateway_ble_scenarios)
set_tests_properties(mesh_gateway_ble_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;ble")

add_executable(test_gateway_ble_pressure_scenarios
    tests/mesh_integration/test_gateway_ble_pressure_scenarios.c
    app/src/app_gateway_ble_stream.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_gateway_ble_pressure_scenarios PRIVATE app/src)
target_link_libraries(test_gateway_ble_pressure_scenarios PRIVATE core)
add_test(NAME gateway_ble_pressure_scenarios
    COMMAND test_gateway_ble_pressure_scenarios)
set_tests_properties(gateway_ble_pressure_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;ble;stress;delivery")

add_executable(test_gateway_survey_host_flow_control_scenarios
    tests/mesh_integration/test_gateway_survey_host_flow_control_scenarios.c
    app/src/app_gateway_ble_stream.c
    app/src/app_gateway_command_observability.c
)
target_include_directories(test_gateway_survey_host_flow_control_scenarios
    PRIVATE app/src)
target_link_libraries(test_gateway_survey_host_flow_control_scenarios
    PRIVATE core)
add_test(NAME gateway_survey_host_flow_control_scenarios
    COMMAND test_gateway_survey_host_flow_control_scenarios)
set_tests_properties(gateway_survey_host_flow_control_scenarios
    PROPERTIES
    LABELS "mesh_integration;hardware_models;survey;ble;stress;custody;reset")

add_executable(test_mesh_app_arbitration_scenarios
    tests/mesh_integration/test_mesh_app_arbitration_scenarios.c
    app/src/app_mesh_c5_priority.c
    app/src/app_mesh_ch9_ack.c
    app/src/app_mesh_command_orchestrator.c
    app/src/app_mesh_arbitration_zephyr.c
    app/src/app_gateway_command_ingress.c
    app/src/app_gateway_command_lifecycle.c
    app/src/app_mesh_gateway_command_flow.c
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

add_executable(test_mesh_sim_fault_scenarios
    tests/mesh_integration/test_mesh_sim_fault_scenarios.c
)
target_link_libraries(test_mesh_sim_fault_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_sim_fault_scenarios
    COMMAND test_mesh_sim_fault_scenarios)
set_tests_properties(mesh_sim_fault_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;faults;state")

add_executable(test_mesh_post_operation_liveness_scenarios
    tests/mesh_integration/test_mesh_post_operation_liveness_scenarios.c
)
target_link_libraries(test_mesh_post_operation_liveness_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_post_operation_liveness_scenarios
    COMMAND test_mesh_post_operation_liveness_scenarios)
set_tests_properties(mesh_post_operation_liveness_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;faults;state;liveness;custody")

add_executable(test_mesh_event_control_rf_scenarios
    tests/mesh_integration/test_mesh_event_control_rf_scenarios.c
)
target_link_libraries(test_mesh_event_control_rf_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_event_control_rf_scenarios
    COMMAND test_mesh_event_control_rf_scenarios)
set_tests_properties(mesh_event_control_rf_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;radio;channel5;protocol;connection;forcedhop;faults;state;liveness")

add_executable(test_mesh_contact_response_scenarios
    tests/mesh_integration/test_mesh_contact_response_scenarios.c
)
target_link_libraries(test_mesh_contact_response_scenarios PRIVATE mesh_sim)
add_test(NAME mesh_contact_response_scenarios
    COMMAND test_mesh_contact_response_scenarios)
set_tests_properties(mesh_contact_response_scenarios
    PROPERTIES LABELS "mesh_integration;hardware_models;simulator;radio;channel5;control")

add_executable(mesh_stress
    tests/mesh_integration/mesh_stress_runner.c)
target_include_directories(mesh_stress PRIVATE app/src)
target_link_libraries(mesh_stress PRIVATE mesh_sim)
add_test(NAME mesh_stress_one_hop
    COMMAND mesh_stress --scenario one-hop --seed 0x51a7e551
            --packets 2 --max-steps 200 --quiet)
set_tests_properties(mesh_stress_one_hop PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;one_hop;protocol")
add_test(NAME mesh_stress_busy_line
    COMMAND mesh_stress --scenario busy-line --seed 0x51a7b551
            --packets 2 --max-steps 800 --quiet)
set_tests_properties(mesh_stress_busy_line PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;protocol")
add_test(NAME mesh_stress_busy_line_reset
    COMMAND mesh_stress --scenario busy-line --seed 0x51a7b551
            --packets 2 --reset-step 2 --max-steps 800 --quiet)
set_tests_properties(mesh_stress_busy_line_reset PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;reset;state")
add_test(NAME mesh_stress_one_hop_fault_replay
    COMMAND mesh_stress --scenario one-hop --seed 0x51a7e551
            --fault-seed 0x12345678 --packets 2 --loss 0
            --ack-loss 3000 --duplicate 3000 --delay 5000
            --max-delay-us 4000 --max-steps 300 --quiet)
set_tests_properties(mesh_stress_one_hop_fault_replay PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;one_hop;faults;replay")
add_test(NAME mesh_stress_busy_line_ack_loss_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x1000
            --fault-seed 0xf4174eed --packets 2 --loss 300
            --ack-loss 1200 --duplicate 800 --delay 2500
            --max-delay-us 6000 --max-steps 800 --quiet)
set_tests_properties(mesh_stress_busy_line_ack_loss_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;custody")
add_test(NAME mesh_stress_busy_line_route_repair_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x1076
            --fault-seed 0xf4174e9b --packets 2 --loss 300
            --ack-loss 1200 --duplicate 800 --delay 2500
            --max-delay-us 6000 --max-steps 800 --quiet)
set_tests_properties(mesh_stress_busy_line_route_repair_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;routes;liveness")
add_test(NAME mesh_stress_busy_line_runtime_cleanup_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x1089
            --fault-seed 0xf4174e64 --packets 2 --loss 0
            --ack-loss 0 --duplicate 800 --delay 2500
            --max-delay-us 6000 --max-steps 800 --quiet)
set_tests_properties(mesh_stress_busy_line_runtime_cleanup_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;radio;timing")
add_test(NAME mesh_stress_busy_line_ack_downlink_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x119c
            --fault-seed 0xf4174f71 --packets 2 --loss 50
            --ack-loss 300 --duplicate 300 --delay 500
            --max-delay-us 4000 --max-steps 300 --quiet)
set_tests_properties(mesh_stress_busy_line_ack_downlink_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;custody;routes;liveness")
add_test(NAME mesh_stress_busy_line_batch_liveness_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x1210
            --fault-seed 0xf4174cfd --packets 2 --loss 50
            --ack-loss 300 --duplicate 300 --delay 500
            --max-delay-us 4000 --max-steps 300 --quiet)
set_tests_properties(mesh_stress_busy_line_batch_liveness_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;queues;liveness")
add_test(NAME mesh_stress_busy_line_contact_reservation_regression
    COMMAND mesh_stress --scenario busy-line --seed 0x13ae
            --fault-seed 0xf4174d43 --packets 2 --loss 50
            --ack-loss 300 --duplicate 300 --delay 500
            --max-delay-us 4000 --max-steps 300 --quiet)
set_tests_properties(mesh_stress_busy_line_contact_reservation_regression PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;multi_node;faults;replay;radio;timing")
add_test(NAME mesh_stress_queue_full
    COMMAND mesh_stress --scenario queue-full --seed 0x51a7f011 --quiet)
set_tests_properties(mesh_stress_queue_full PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;queue")

add_test(NAME mesh_stress_exact_failure_replay
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_stress_replay.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_mesh_stress.py
            $<TARGET_FILE:mesh_stress>)
set_tests_properties(mesh_stress_exact_failure_replay PROPERTIES
    LABELS "mesh_integration;hardware_models;simulator;stress;replay")

add_test(NAME mesh_ble_route_monitor
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mesh_integration/test_mesh_ble_route_monitor.py)
set_tests_properties(mesh_ble_route_monitor
    PROPERTIES LABELS "mesh_integration;host_tools;observability")

# The focused protocol gate spans click wake/ranging/report delivery, direct
# probing and route advertisement, connection ownership, assignment, survey
# discovery/pair execution, result custody, persistence, fault/reset
# boundaries, and terminal cleanup.  It stays smaller than the complete
# mesh-integration label for rapid iteration.
set_property(TEST
    mesh_event_owner
    mesh_outbox_class_matrix
    discovery_assignment
    survey_round_control
    survey_gateway_drive_policy
    survey_completion_policy
    survey
    uwb
    uwb_session
    dwm3000_models
    gateway_ble_transport_model
    gateway_eack_policy
    app_gateway_eack_retry
    assignment_claim_semantic_acceptance
    app_gateway_assignment_publisher
    app_discovery_assignment_policy
    app_mesh_scheduler_liveness
    app_mesh_event_retry
    gateway_survey_dynamic_deadline_source_invariants
    gateway_survey_receive_deadline_source_invariants
    survey_discovery_failure_source_invariants
    survey_pair_control_lifecycle_source_invariants
    anchor_survey_round_barrier_source_invariants
    gateway_survey_round_glue_source_invariants
    app_node_comm_gateway_route_refresh
    mesh_direct_probe_recovery_scenarios
    mesh_integration
    mesh_production_scenarios
    mesh_runtime_priority_scenarios
    mesh_wake_scenarios
    mesh_forcedhop_connection_scenarios
    mesh_route_formation_scenarios
    mesh_discovery_scenarios
    mesh_discovery_assignment_adversarial_scenarios
    mesh_protocol_lifecycle_scenarios
    mesh_survey_round_adversarial_lifecycle
    mesh_survey_round_adversarial_lifecycle_seed_zero
    mesh_survey_round_adversarial_lifecycle_seed_max
    mesh_gateway_control_stress_scenarios
    multi_anchor_timing_invariants
    mesh_survey_phy_scenarios
    mesh_survey_topology_scenarios
    mesh_survey_direct_gateway_retry_scenarios
    survey_pair_lease
    survey_gateway_transaction
    survey_pair_round_runtime
    app_gateway_survey_round
    mesh_result_grant_retry_scenarios
    mesh_result_custody_rf_scenarios
    gateway_collection_eack
    mesh_event_control_rf_scenarios
    mesh_sim_fault_scenarios
    mesh_contact_response_scenarios
    mesh_post_operation_liveness_scenarios
    mesh_click_collision_sweep
    mesh_click_report_delivery_sweep
    mesh_click_protocol_chain_scenarios
    mesh_gateway_ble_scenarios
    gateway_ble_pressure_scenarios
    mesh_network_stress_scenarios
    mesh_watchdog_lease_scenarios
    mesh_app_arbitration_scenarios
    mesh_stress_busy_line_reset
    mesh_stress_one_hop_fault_replay
    mesh_stress_busy_line_ack_loss_regression
    mesh_stress_busy_line_ack_downlink_regression
    mesh_stress_busy_line_batch_liveness_regression
    mesh_stress_busy_line_contact_reservation_regression
    APPEND PROPERTY LABELS protocol_matrix)
