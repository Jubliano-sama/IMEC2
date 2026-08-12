cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imec2_production_seam_report_custody)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../app/src
)

target_compile_definitions(app PRIVATE
    DEVICE_ROLE=2
    CONFIG_IMEC_MESH_ROUTE_TEST=1
    CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE=1
    CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS=380
    CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_RX_US=3000
    CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS=3000
)

# This seam deliberately compiles the full production report owner into the
# test TU. Role/config branches leave unrelated static helpers unreferenced;
# keep Twister's global -Werror focused on diagnostics that affect the seam.
target_compile_options(app PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-const-variable
)

target_sources(app PRIVATE
    src/main.c
    ../../../../app/src/app_anchor_click_event_runtime.c
    ../../../../app/src/app_gateway_control_sequence.c
    ../../../../app/src/app_gateway_command_result.c
    ../../../../app/src/app_mesh_test.c
    ../../../../app/src/app_mesh_c5_priority.c
    ../../../../app/src/app_mesh_command_orchestrator.c
    ../../../../app/src/app_mesh_arbitration_zephyr.c
    ../../../../app/src/app_node_comm_gateway_control.c
    ../../../../app/src/app_node_comm_sync.c
    ../../../../app/src/app_node_comm_gateway_route_refresh.c
    ../../../../app/src/app_operation_policy.c
    ../../../../app/src/app_mesh_local_delivery.c
    ../../../../app/src/app_mesh_rx_policy.c
    ../../../../app/src/app_mesh_preemption.c
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
    ../../../../app/src/app_wake_train_politeness.c
    ../../../../app/src/app_node_comm.c
    ../../../../app/src/app_mesh_report_encode.c
    ../../../../app/src/app_stack_diag_record.c
    ../../../../app/src/app_stack_diag.c
    ../../../../app/src/app_stack_workload_diag.c
    ../../../../app/src/app_radio_guard.c
    ../../../../app/src/app_state.c
    ../../../../src/firmware_delivery_loss.c
    ../../../../src/debug_log.c
    ../../../../src/device_identity.c
    ../../../../src/discovery_assignment.c
    ../../../../src/dwm3000_timing.c
    ../../../../src/firmware_events.c
    ../../../../src/firmware_state_click.c
    ../../../../src/firmware_state_control.c
    ../../../../src/firmware_state_mesh.c
    ../../../../src/firmware_state_radio.c
    ../../../../src/gateway_command.c
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
