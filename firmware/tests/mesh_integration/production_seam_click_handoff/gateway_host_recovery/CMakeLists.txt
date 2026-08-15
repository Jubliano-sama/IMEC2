cmake_minimum_required(VERSION 3.20.0)

# Twister configures standalone test applications without a west module
# handle. The composed gateway BLE path uses TinyCrypt through Zephyr's
# Bluetooth host, so provide the minimal complete module set before Zephyr
# processes Kconfig/CMake. A normal west build is compatible with this
# explicit single-module test dependency.
set(ZEPHYR_MODULES
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../modules/crypto/tinycrypt")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imec2_production_seam_gateway_host_recovery)

# The seam intentionally includes the full role-specialized app_mesh_report
# translation unit, and imports the sequence owner without the durable-NVS
# build feature.  Those two source files consequently contain only
# role/configuration-elided statics; keep every other warning fatal.
set_source_files_properties(
    src/main.c
    ../../../../app/src/app_gateway_control_sequence.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable"
)
target_compile_options(app PRIVATE -Werror)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../app/src
)

target_compile_definitions(app PRIVATE
    DEVICE_ROLE=3
    APP_DURABLE_STATE_TESTING=1
    # Standalone Zephyr test applications do not import firmware/app/Kconfig,
    # so make the production gateway-BLE feature branch explicit here.
    CONFIG_IMEC_GATEWAY_BLE=1
    CONFIG_IMEC_MESH_ROUTE_TEST=1
    CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS=380
    CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_RX_US=3000
    CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS=3000
)

target_sources(app PRIVATE
    src/main.c
    ../../../../app/src/app_gateway_control_sequence.c
    ../../../../app/src/app_durable_state.c
    ../../../../app/src/app_gateway_assignment_publisher.c
    ../../../../app/src/app_gateway_survey_round.c
    ../../../../app/src/app_gateway_survey_observability.c
    ../../../../app/src/app_gateway_collection_eack.c
    ../../../../app/src/app_gateway_collection_recovery.c
    ../../../../app/src/app_gateway_command_observability.c
    ../../../../app/src/app_gateway_command_result.c
    ../../../../app/src/app_gateway_eack_policy.c
    ../../../../app/src/app_gateway_eack_retry.c
    ../../../../app/src/app_gateway_ble_stream.c
    ../../../../app/src/app_mesh_test.c
    ../../../../app/src/app_mesh_c5_priority.c
    ../../../../app/src/app_mesh_command_orchestrator.c
    ../../../../app/src/app_mesh_gateway_command_flow.c
    ../../../../app/src/app_mesh_arbitration_zephyr.c
    ../../../../app/src/app_mesh_preemption.c
    ../../../../app/src/app_mesh_local_delivery.c
    ../../../../app/src/app_mesh_rx_policy.c
    ../../../../app/src/app_mesh_flood.c
    ../../../../app/src/app_mesh_ch9_ack.c
    ../../../../app/src/app_mesh_direct_gateway_retry.c
    ../../../../app/src/app_mesh_collection_deferral.c
    ../../../../app/src/app_mesh_tx_handoff_gate.c
    ../../../../app/src/app_mesh_route_reply_ack.c
    ../../../../app/src/app_mesh_route_ready_handoff.c
    ../../../../app/src/app_mesh_event_retry.c
    ../../../../app/src/app_mesh_async_route_request.c
    ../../../../app/src/app_mesh_route_request_policy.c
    ../../../../app/src/app_mesh_route_wait_tx.c
    ../../../../app/src/app_mesh_rf_retry.c
    ../../../../app/src/app_mesh_gateway_ack_policy.c
    ../../../../app/src/app_mesh_result_handoff.c
    ../../../../app/src/app_mesh_smoke_fast.c
    ../../../../app/src/app_wake_train_politeness.c
    ../../../../app/src/app_node_comm.c
    ../../../../app/src/app_node_comm_gateway_control.c
    ../../../../app/src/app_node_comm_gateway_route_refresh.c
    ../../../../app/src/app_node_comm_sync.c
    ../../../../app/src/app_operation_policy.c
    ../../../../app/src/app_mesh_report_encode.c
    ../../../../app/src/app_stack_diag_record.c
    ../../../../app/src/app_stack_diag.c
    ../../../../app/src/app_stack_workload_diag.c
    ../../../../app/src/app_radio_guard.c
    ../../../../app/src/app_state.c
    ../../../../src/debug_log.c
    ../../../../src/device_identity.c
    ../../../../src/discovery_assignment.c
    ../../../../src/dwm3000_timing.c
    ../../../../src/firmware_delivery_loss.c
    ../../../../src/firmware_events.c
    ../../../../src/firmware_state_click.c
    ../../../../src/firmware_state_control.c
    ../../../../src/firmware_state_mesh.c
    ../../../../src/firmware_state_radio.c
    ../../../../src/gateway_command.c
    ../../../../src/gateway_ble_transport.c
    ../../../../src/gateway_membership.c
    ../../../../src/mesh.c
    ../../../../src/mesh_event_owner.c
    ../../../../src/mesh_event_owner_registry.c
    ../../../../src/mesh_preemption.c
    ../../../../src/mesh_relay.c
    ../../../../src/mesh_relay_channel9.c
    ../../../../src/mesh_route_path.c
    ../../../../src/node_comm.c
    ../../../../src/node_transaction.c
    ../../../../src/operation_policy.c
    ../../../../src/protocol.c
    ../../../../src/report.c
    ../../../../src/route.c
    ../../../../src/serial_frame.c
    ../../../../src/semantic_digest.c
    ../../../../src/stack_diag_transport.c
    ../../../../src/survey.c
    ../../../../src/survey_anchor_deadline.c
    ../../../../src/survey_pair_lease.c
    ../../../../src/survey_pair_planner.c
    ../../../../src/survey_pair_round_runtime.c
    ../../../../src/survey_round_control.c
    ../../../../src/survey_gateway_transaction.c
    ../../../../src/uwb.c
    ../../../../src/uwb_ble_courtesy.c
    ../../../../src/uwb_session.c
    ../../../../src/watchdog_adoption.c
)
