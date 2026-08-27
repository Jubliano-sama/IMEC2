#ifndef STACK_BUDGET_H
#define STACK_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STACK_BUDGET_APP_MIN_FREE_BYTES 1024u
#define STACK_BUDGET_SERVICE_MIN_FREE_BYTES 256u
#define STACK_BUDGET_MIN_FREE_PERCENT 20u
#define STACK_BUDGET_LARGE_LOCAL_FRAME_MAX_BYTES 8192u
#define STACK_BUDGET_GATEWAY_LOG_PROCESSOR_BYTES 1536u
#define STACK_BUDGET_GATEWAY_LOG_PROCESSOR_MEASURED_MAX_BYTES 480u
#define STACK_BUDGET_GATEWAY_LOG_PROCESSOR_MEASURED_MIN_FREE_BYTES 1024u

#if STACK_BUDGET_GATEWAY_LOG_PROCESSOR_BYTES - \
        STACK_BUDGET_GATEWAY_LOG_PROCESSOR_MEASURED_MAX_BYTES < \
    STACK_BUDGET_GATEWAY_LOG_PROCESSOR_MEASURED_MIN_FREE_BYTES
#error "gateway logging stack must retain its measured 1 KiB free margin"
#endif

/*
 * This table is the review baseline for the production-candidate presets.
 * Keep it machine-readable: firmware/scripts/verify_stack_evidence.py parses
 * these rows and compares them with each exact Zephyr build's generated data.
 * Anchor enumeration now retains a 456-byte semantic custody lane in BSS
 * across radio-owner handoffs. Its 8.5 KiB static reserve remains separate
 * from every measured stack margin and is still enforced before flashing.
 */
/* STACK_BUDGET_POLICY_BEGIN */
#define STACK_BUDGET_DEPLOYABLE_PRESET_POLICY(X)                              \
    X(CLICKER, "mesh_clicker", 4096u, 6144u, 6784u, 8192u, 320u, 2048u, 1536u,   \
      1024u, 24576u, true, true, true, true, false)                           \
    X(ANCHOR, "mesh_anchor", 5120u, 5376u, 9472u, 8192u, 320u, 2048u, 0u,        \
      0u, 8704u, true, true, true, true, false)                               \
    X(GATEWAY, "mesh_gateway", 4416u, 8512u, 8384u, 8192u, 320u, 1536u, 1536u,  \
      1536u, 4096u, true, true, true, true, false)

#if STACK_BUDGET_GATEWAY_LOG_PROCESSOR_BYTES != 1536u
#error "gateway logging stack constant and deployable policy diverged"
#endif

#define STACK_BUDGET_BENCH_PRESET_POLICY(X)                                   \
    X(ANCHOR_FORCEDHOP, "mesh_anchor_forcedhop", 5120u, 5376u, 9472u, 8192u, \
      320u, 2048u, 0u, 0u, 8704u, true, true, true, true, false)              \
    X(TRANSMITTER, "mesh_transmitter", 4096u, 8192u, 9216u, 8192u, 320u, 0u, 0u, \
      0u, 18432u, true, true, true, true, false)                              \
    X(TRANSMITTER_FORCEDHOP, "mesh_transmitter_forcedhop", 4096u, 8192u, \
      9216u, 8192u, 320u, 0u, 0u, 0u, 18432u, true, true, true, true, false)

#define STACK_BUDGET_PRESET_POLICY(X) \
    STACK_BUDGET_DEPLOYABLE_PRESET_POLICY(X) \
    STACK_BUDGET_BENCH_PRESET_POLICY(X)
/* STACK_BUDGET_POLICY_END */

/*
 * Hardware qualification workloads are preset-specific.  Each row is:
 * preset, typed workload, semantic owner, minimum successful runs, and
 * whether those runs must form an ordered retained-click sequence.
 * Keep this table machine-readable; the verifier rejects a capture that
 * substitutes another role's workload or owner.
 */
/* STACK_BUDGET_WORKLOAD_POLICY_BEGIN */
#define STACK_BUDGET_DEPLOYABLE_WORKLOAD_POLICY(X)                           \
    X("mesh_clicker", "click_activity", "clicker_action", 1u, false)      \
    X("mesh_anchor", "anchor_scan", "anchor_uwb_scan", 1u, false) \
    X("mesh_gateway", "gateway_report_ingress", "mesh_route", 1u, false) \
    X("mesh_gateway", "gateway_priority_control", "system_workqueue", 1u, false) \
    X("mesh_gateway", "ble_backpressure", "system_workqueue", 1u, false)
/* STACK_BUDGET_WORKLOAD_POLICY_END */

/*
 * Every linked application function is attributed by compiler IPA call graph
 * reachability from these actual Zephyr execution entries.  These are roots,
 * not function-by-function exemptions: the verifier follows every direct
 * compiler-proved edge and charges a function reached by several roots to
 * every applicable configured stack.  A callback that is only address-taken
 * needs a root row because C cannot prove the kernel's indirect invocation.
 *
 * Never add a source-wide, role-wide, or largest-stack fallback here.  The
 * verifier rejects unrooted, ambiguous, and unsupported linked symbols.
 * The fatal override, Bluetooth CCC hook, and DWM3000 SDK port functions are
 * explicit ABI boundaries whose callers live in Zephyr or the imported vendor
 * library and therefore cannot appear in the application IPA graph. The fatal
 * aggregate is bounded by the smallest configured execution stack, including
 * idle; the remaining rows enumerate only their reviewed synchronous owners.
 */
/* STACK_BUDGET_THREAD_ROOTS_BEGIN */
#define STACK_BUDGET_THREAD_ROOTS(X)                                             \
    X("main.c", "main", "main")                                                \
    X("main.c", "k_sys_fatal_error_handler", "fatal_context")                  \
    X("dwm3000_sdk_port.c", "writetospiwithcrc", "main")                       \
    X("dwm3000_sdk_port.c", "writetospiwithcrc", "system_workqueue")           \
    X("dwm3000_sdk_port.c", "writetospi", "main")                              \
    X("dwm3000_sdk_port.c", "writetospi", "system_workqueue")                  \
    X("dwm3000_sdk_port.c", "readfromspi", "main")                             \
    X("dwm3000_sdk_port.c", "readfromspi", "system_workqueue")                 \
    X("dwm3000_sdk_port.c", "deca_usleep", "main")                             \
    X("dwm3000_sdk_port.c", "deca_usleep", "system_workqueue")                 \
    X("app_clicker.c", "clicker_action_work_handler", "clicker_action")     \
    X("app_clicker.c", "clicker_action_submit_retry_work_handler", "system_workqueue") \
    X("app_clicker.c", "click_button_release_work_handler", "system_workqueue") \
    X("app_clicker.c", "click_button_rearm_work_handler", "system_workqueue") \
    X("app_clicker.c", "click_button_work_handler", "system_workqueue")       \
    X("app_clicker.c", "self_test_arm_timeout_handler", "system_workqueue")   \
    X("app_clicker_rtt_control.c", "app_clicker_rtt_poll_work_handler", "system_workqueue") \
    X("app_battery_indicator.c", "battery_indicator_work_handler", "system_workqueue") \
    X("app_clicker.c", "click_button_isr", "isr")                              \
    X("app_anchor.c", "anchor_discovery_claim_work_handler", "system_workqueue") \
    X("app_anchor.c", "anchor_collection_result_work_handler", "system_workqueue") \
    X("app_anchor.c", "anchor_collection_result_work_handler", "mesh_route") \
    X("app_anchor.c", "anchor_reboot_work_handler", "system_workqueue")       \
    X("app_anchor.c", "anchor_reboot_work_handler", "mesh_route")            \
    X("app_anchor.c", "anchor_heartbeat_work_handler", "system_workqueue")    \
    X("app_anchor.c", "anchor_heartbeat_work_handler", "mesh_route")         \
    X("app_anchor.c", "anchor_command_execute_work_handler", "system_workqueue") \
    X("app_anchor.c", "anchor_discovery_ack_liveness_work_handler", "system_workqueue") \
    X("app_anchor.c", "anchor_uwb_scan_work_handler", "anchor_uwb_scan")     \
    X("app_anchor.c", "anchor_note_uwb_awake_since", "system_workqueue")      \
    X("app_anchor.c", "anchor_click_handoff_work_handler", "anchor_uwb_scan") \
    X("app_anchor.c", "anchor_handle_mesh_click_wake_claim", "mesh_route") \
    X("app_mesh_test.c", "mesh_test_tx_thread_entry", "mesh_test")       \
    X("app_anchor.c", "anchor_handle_local_command", "system_workqueue")      \
    X("app_anchor.c", "gateway_discovery_assignment_publish_work_handler", "system_workqueue") \
    X("app_anchor.c", "gateway_discovery_assignment_finalize_work_handler", "system_workqueue") \
    X("app_anchor.c", "gateway_host_command_retry_work_handler", "system_workqueue") \
    X("app_anchor.c", "gateway_host_abort_work_handler", "system_workqueue") \
    X("app_anchor.c", "gateway_host_abort_route_work_handler", "mesh_route") \
    X("app_anchor.c", "gateway_host_command_work_handler", "mesh_route") \
    X("app_survey.c", "anchor_work_handler", "anchor_uwb_scan") \
    X("app_survey.c", "gateway_work_handler", "system_workqueue") \
    X("app_mesh_arbitration_zephyr.c", "retry_work_handler", "system_workqueue") \
    X("app_mesh_report.c", "mesh_route_discovery_work_handler", "mesh_route") \
    X("app_mesh_report.c", "mesh_click_preempt_work_handler", "mesh_route") \
    X("app_node_comm_gateway_route_refresh.c", "refresh_work_handler", "mesh_route") \
    X("app_node_comm.c", "app_node_comm_lifecycle_watchdog_handler", "system_workqueue") \
    X("app_node_comm.c", "app_node_comm_delivery_due_kick_handler", "system_workqueue") \
    X("app_node_comm.c", "app_node_comm_delivery_work_handler", "mesh_route") \
    X("app_node_comm.c", "app_node_comm_gateway_scan_restart_work_handler", "system_workqueue") \
    X("app_mesh_report.c", "mesh_uwb_rx_rearm_work_handler", "system_workqueue") \
    X("app_mesh_report.c", "mesh_c5_flood_work_handler", "mesh_route") \
    X("app_mesh_report.c", "report_tx_work_handler", "mesh_route")      \
    X("app_mesh_report.c", "mesh_route_request_action_work_handler", "mesh_route") \
    X("app_mesh_report.c", "mesh_rx_work_handler", "mesh_route")        \
    X("app_mesh_report.c", "mesh_tx_timeout_handler", "mesh_route")     \
    X("app_mesh_report.c", "mesh_route_waiting_work_handler", "mesh_route") \
    X("app_mesh_report.c", "mesh_event_negotiation_retry_work_handler", "mesh_route") \
    X("app_mesh_report.c", "mesh_uwb_rx_work_handler", "mesh_route")    \
    X("app_mesh_report.c", "mesh_gateway_host_delivery_retry_work_handler", "mesh_route") \
    X("app_mesh_report.c", "mesh_node_comm_cancel_work_handler", "mesh_route") \
    X("app_gateway_ble.c", "gateway_persistence_retry_work_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_collection_eack_work_handler", "mesh_route") \
    X("app_gateway_ble.c", "gateway_command_result_timeout_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_command_timeout_side_effect_handler", "mesh_route") \
    X("app_gateway_ble.c", "gateway_command_timeout_side_effect_retry_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_ble_rx_work_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_ble_stream_work_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_ble_host_receipt_timeout_work_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_ble_recovery_work_handler", "system_workqueue") \
    X("app_gateway_control_sequence.c", "gateway_control_sequence_maintenance_handler", "system_workqueue") \
    X("app_gateway_ble.c", "gateway_ble_tx_complete", "bt_rx")                \
    X("app_gateway_ble.c", "gateway_ble_connected", "bt_rx")                  \
    X("app_gateway_ble.c", "gateway_ble_le_param_updated", "bt_rx")           \
    X("app_gateway_ble.c", "gateway_ble_disconnected", "bt_rx")                \
    X("app_gateway_ble.c", "gateway_ble_packet_ccc_changed", "bt_rx")          \
    X("app_board.c", "status1_debug_pulse_restore_handler", "system_workqueue") \
    X("app_board.c", "status0_debug_pulse_restore_handler", "system_workqueue") \
    X("app_board.c", "status0_power_blink_handler", "system_workqueue")       \
    X("app_watchdog.c", "system_progress_work_handler", "system_workqueue")   \
    X("app_watchdog.c", "watchdog_timer_handler", "isr")                    \
    X("app_stack_diag.c", "stack_diag_boot_init", "main")                       \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_release", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_cir_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_cir_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_cir_release", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_relay_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_relay_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_relay_release", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_release", "bt_rx") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_release_all", "bt_rx") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_admit_with_pressure", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_sample_with_pressure", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_terminal_with_pressure", "bt_rx") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_ble_release_all_with_pressure", "bt_rx") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_activity_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_activity_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_click_activity_release", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_gateway_report_cycle", "mesh_route") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_gateway_control_admit", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_gateway_control_sample", "system_workqueue") \
    X("app_stack_workload_diag.c", "app_stack_workload_diag_gateway_control_release", "system_workqueue")
/* STACK_BUDGET_THREAD_ROOTS_END */

enum stack_budget_role {
    STACK_BUDGET_ROLE_CLICKER = 0,
    STACK_BUDGET_ROLE_ANCHOR,
    STACK_BUDGET_ROLE_GATEWAY,
    STACK_BUDGET_ROLE_ANCHOR_FORCEDHOP,
    STACK_BUDGET_ROLE_TRANSMITTER,
    STACK_BUDGET_ROLE_TRANSMITTER_FORCEDHOP,
    STACK_BUDGET_ROLE_COUNT,
};

enum stack_budget_owner {
    STACK_BUDGET_OWNER_MAIN = 0,
    STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
    STACK_BUDGET_OWNER_MESH_ROUTE,
    STACK_BUDGET_OWNER_ISR,
    STACK_BUDGET_OWNER_IDLE,
    STACK_BUDGET_OWNER_LOG_PROCESSOR,
    STACK_BUDGET_OWNER_BT_HCI_TX,
    STACK_BUDGET_OWNER_BT_RX,
    STACK_BUDGET_OWNER_DEDICATED_APP,
};

struct stack_budget_role_config {
    uint32_t main_bytes;
    uint32_t system_workqueue_bytes;
    uint32_t mesh_route_bytes;
    uint32_t isr_bytes;
    uint32_t idle_bytes;
    uint32_t log_processor_bytes;
    uint32_t bt_hci_tx_bytes;
    uint32_t bt_rx_bytes;
    bool hw_stack_protection;
    bool mpu_stack_guard;
    bool thread_stack_info;
    bool init_stacks;
    bool stack_sentinel;
    uint32_t minimum_static_ram_headroom_bytes;
};

struct stack_budget_result {
    uint32_t configured_bytes;
    uint32_t measured_bytes;
    uint32_t indirect_reserve_bytes;
    uint32_t required_free_bytes;
    uint32_t remaining_bytes;
    bool passes;
};

struct stack_budget_combined_scenario {
    uint32_t root_frame_bytes;
    uint32_t shared_mesh_bytes;
    uint32_t ds_twr_bytes;
    uint32_t cir_pack_bytes;
    uint32_t transit_retry_bytes;
    uint32_t gateway_command_bytes;
    uint32_t ble_fragment_bytes;
    uint32_t indirect_reserve_bytes;
    uint8_t sequential_click_count;
};

struct stack_budget_combined_usage {
    uint32_t measured_chain_bytes;
    uint32_t indirect_reserve_bytes;
    uint32_t total_bytes;
};

int stack_budget_role_baseline(enum stack_budget_role role,
                               struct stack_budget_role_config *config);
const char *stack_budget_role_preset_name(enum stack_budget_role role);
int stack_budget_validate_role_config(
    enum stack_budget_role role,
    const struct stack_budget_role_config *config,
    uint32_t static_ram_headroom_bytes);
uint32_t stack_budget_configured_for_owner(
    const struct stack_budget_role_config *config,
    enum stack_budget_owner owner);
uint32_t stack_budget_required_free(uint32_t configured_bytes,
                                    enum stack_budget_owner owner);
int stack_budget_evaluate(uint32_t configured_bytes,
                          uint32_t measured_bytes,
                          uint32_t indirect_reserve_bytes,
                          enum stack_budget_owner owner,
                          struct stack_budget_result *result);
bool stack_budget_large_local_allowed(size_t frame_bytes);
int stack_budget_calculate_combined(
    const struct stack_budget_combined_scenario *scenario,
    struct stack_budget_combined_usage *usage);

#ifdef __cplusplus
}
#endif

#endif
